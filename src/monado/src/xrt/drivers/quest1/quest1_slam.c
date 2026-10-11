// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 6DoF head tracking through Monado's SLAM tracker (t_slam, VIT, Basalt).
 *
 * Frames: Basalt's IMU frame is the frame of the rectified IMU samples, i.e. the calibration's
 * device frame (+X right, +Y up, +Z back; DeviceFromImu is a pure translation). Cameras are
 * given as T_imu_cam = inv(DeviceFromImu) * DeviceFromCamera. Basalt reports T_world_imu with
 * the world +Z up (gravity along -Z, yaw from the first IMU sample). OpenXR wants +Y up, so the
 * pose is rotated by -90 deg about X, then moved from the IMU to the device origin:
 * world_xr_from_device = Rx(-90) * T_world_imu * inv(DeviceFromImu).
 *
 * The SyncBoss IMU thread and the camera thread hand their data to two feeder threads here, one
 * for IMU samples and one for frames, which push it into the tracker: Basalt's bounded input
 * queues block when it falls behind, and the IMU thread also pumps the SyncBoss stream for the
 * controllers and the cameras. One feeder for both deadlocks once Basalt lags: blocked on a full
 * image queue it stops delivering the IMU samples that Basalt's VIO waits for.
 *
 * @ingroup drv_quest1
 */

#include "quest1_slam.h"

#include "xrt/xrt_config_build.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_tracking.h"

#include "math/m_api.h"
#include "math/m_space.h"
#include "math/m_vec3.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_frame.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"

#ifdef XRT_FEATURE_SLAM
#include "tracking/t_tracking.h"
#endif

#include <stdlib.h>
#include <string.h>

#define SLAM_ERROR(s, ...) U_LOG_IFL_E(s->log_level, "slam: " __VA_ARGS__)
#define SLAM_WARN(s, ...) U_LOG_IFL_W(s->log_level, "slam: " __VA_ARGS__)
#define SLAM_INFO(s, ...) U_LOG_IFL_I(s->log_level, "slam: " __VA_ARGS__)
#define SLAM_DEBUG(s, ...) U_LOG_IFL_D(s->log_level, "slam: " __VA_ARGS__)

#define SLAM_IMU_RING 1024 // 1 s at 1 kHz
#define SLAM_CAM_HZ 30.0
#define SLAM_IMU_HZ 1000.0
#define SLAM_STATS_PERIOD_NS (10 * (int64_t)U_TIME_1S_IN_NS)
#define SLAM_DEFAULT_VIT "/opt/basalt/lib/libbasalt.so"
// Basalt has no failure signal: VIO that lost its landmarks drifts away with the IMU, quadratically.
// Head motion stays far below these; beyond them the driver resets the tracker (new origin) and
// reports 3DoF until the poses are plausible again.
#define SLAM_MAX_SPEED_M_S 3.0f
#define SLAM_MAX_DISTANCE_M 10.0f
#define SLAM_RESET_INTERVAL_NS (3 * (int64_t)U_TIME_1S_IN_NS)

// IMU noise: the Rift S driver's values (rift_s_tracker.c), validated offline on Quest 1 datasets.
#define SLAM_ACCEL_NOISE 0.016
#define SLAM_ACCEL_BIAS 0.001
#define SLAM_GYRO_NOISE 0.000282
#define SLAM_GYRO_BIAS 0.0001

#ifdef XRT_FEATURE_SLAM

struct quest1_slam
{
	enum u_logging_level log_level;
	struct xrt_frame_context xfctx;
	struct xrt_tracked_slam *tracker;
	struct xrt_slam_sinks *sinks;
	struct t_slam_calibration calib;
	int cam_count;
	int cam_ids[QUEST1_CALIB_CAMS];
	int width, height;
	int frame_divider;
	uint64_t frameset_counter; //!< only the camera thread
	struct xrt_pose imu_from_device;

	//! IMU feeder; its mutex guards the ring.
	struct os_thread_helper imu_oth;
	struct xrt_imu_sample imu[SLAM_IMU_RING];
	uint32_t imu_head, imu_tail; //!< written at head, read at tail
	int64_t last_imu_in_ts;

	//! Frame feeder; its mutex guards the pending frameset.
	struct os_thread_helper frame_oth;
	struct xrt_frame *pending[QUEST1_CALIB_CAMS]; //!< newest unsent frameset, NULL = none
	int64_t pending_ts;

	//! Updated with __atomic builtins.
	struct
	{
		uint64_t imu_in, imu_dropped, framesets_in, framesets_dropped, framesets_pushed;
	} stats;

	//! Only the frame feeder.
	int64_t last_frame_ts, last_stats_ns;

	//! Serialises t_slam_get_tracked_pose (it caches the last request).
	struct os_mutex pose_mutex;
	int64_t last_reset_ns; //!< protected by pose_mutex
	uint32_t resets;       //!< protected by pose_mutex
};

static void
fill_calibration(struct quest1_slam *s, const struct quest1_calib *calib)
{
	struct t_slam_imu_calibration *imu = &s->calib.imu;
	memset(imu, 0, sizeof(*imu));
	imu->frequency = SLAM_IMU_HZ;
	// The samples arrive rectified: identity transforms, zero offsets.
	for (int i = 0; i < 3; i++) {
		imu->base.accel.transform[i][i] = 1.0;
		imu->base.gyro.transform[i][i] = 1.0;
		imu->base.accel.noise_std[i] = SLAM_ACCEL_NOISE;
		imu->base.accel.bias_std[i] = SLAM_ACCEL_BIAS;
		imu->base.gyro.noise_std[i] = SLAM_GYRO_NOISE;
		imu->base.gyro.bias_std[i] = SLAM_GYRO_BIAS;
	}

	struct xrt_matrix_4x4 m;
	struct xrt_pose device_from_imu;
	math_matrix_4x4_transpose((const struct xrt_matrix_4x4 *)calib->imu.device_from_imu, &m); // to column-major
	math_pose_from_isometry(&m, &device_from_imu);
	math_pose_invert(&device_from_imu, &s->imu_from_device);

	s->calib.cam_count = s->cam_count;
	for (int n = 0; n < s->cam_count; n++) {
		const struct quest1_cam_calib *c = &calib->cam[s->cam_ids[n]];
		struct xrt_pose device_from_cam, imu_from_cam;
		math_matrix_4x4_transpose((const struct xrt_matrix_4x4 *)c->device_from_camera, &m);
		math_pose_from_isometry(&m, &device_from_cam);
		math_pose_transform(&s->imu_from_device, &device_from_cam, &imu_from_cam);

		struct t_slam_camera_calibration *sc = &s->calib.cams[n];
		memset(sc, 0, sizeof(*sc));
		sc->frequency = SLAM_CAM_HZ / s->frame_divider;
		math_matrix_4x4_isometry_from_pose(&imu_from_cam, &sc->T_imu_cam);
		sc->base.image_size_pixels.w = c->width;
		sc->base.image_size_pixels.h = c->height;
		sc->base.intrinsics[0][0] = c->f;
		sc->base.intrinsics[1][1] = c->f;
		sc->base.intrinsics[0][2] = c->cx;
		sc->base.intrinsics[1][2] = c->cy;
		sc->base.intrinsics[2][2] = 1.0;
		sc->base.distortion_model = T_DISTORTION_FISHEYE_KB4;
		sc->base.kb4.k1 = c->kb4[0];
		sc->base.kb4.k2 = c->kb4[1];
		sc->base.kb4.k3 = c->kb4[2];
		sc->base.kb4.k4 = c->kb4[3];
		SLAM_INFO(s, "cam%d = camera %d, T_imu_cam p [%.4f %.4f %.4f] q [%.4f %.4f %.4f %.4f]", n, s->cam_ids[n],
		          imu_from_cam.position.x, imu_from_cam.position.y, imu_from_cam.position.z,
		          imu_from_cam.orientation.x, imu_from_cam.orientation.y, imu_from_cam.orientation.z,
		          imu_from_cam.orientation.w);
	}
}

static void
log_stats(struct quest1_slam *s, int64_t now)
{
	typeof(s->stats) st;
	st.imu_in = __atomic_load_n(&s->stats.imu_in, __ATOMIC_RELAXED);
	st.imu_dropped = __atomic_load_n(&s->stats.imu_dropped, __ATOMIC_RELAXED);
	st.framesets_in = __atomic_load_n(&s->stats.framesets_in, __ATOMIC_RELAXED);
	st.framesets_dropped = __atomic_load_n(&s->stats.framesets_dropped, __ATOMIC_RELAXED);
	st.framesets_pushed = __atomic_load_n(&s->stats.framesets_pushed, __ATOMIC_RELAXED);

	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	bool ok = quest1_slam_get_pose(s, now, &rel);
	os_mutex_lock(&s->pose_mutex);
	uint32_t resets = s->resets;
	os_mutex_unlock(&s->pose_mutex);
	SLAM_INFO(s,
	          "framesets %llu in, %llu pushed, %llu dropped; IMU %llu in, %llu dropped; resets %u; "
	          "pose %s p [%.3f %.3f %.3f] q [%.3f %.3f %.3f %.3f]",
	          (unsigned long long)st.framesets_in, (unsigned long long)st.framesets_pushed,
	          (unsigned long long)st.framesets_dropped, (unsigned long long)st.imu_in,
	          (unsigned long long)st.imu_dropped, resets, ok ? "tracked" : "none", rel.pose.position.x, rel.pose.position.y,
	          rel.pose.position.z, rel.pose.orientation.x, rel.pose.orientation.y, rel.pose.orientation.z,
	          rel.pose.orientation.w);
}

static void *
imu_feeder(void *ptr)
{
	struct quest1_slam *s = ptr;
	os_thread_helper_name(&s->imu_oth, "Quest1 SLAM IMU");
	struct xrt_imu_sample batch[64];

	os_thread_helper_lock(&s->imu_oth);
	while (os_thread_helper_is_running_locked(&s->imu_oth)) {
		if (s->imu_head == s->imu_tail) {
			os_thread_helper_wait_locked(&s->imu_oth);
			continue;
		}
		uint32_t n = 0;
		while (s->imu_tail != s->imu_head && n < ARRAY_SIZE(batch)) {
			batch[n++] = s->imu[s->imu_tail++ % SLAM_IMU_RING];
		}
		os_thread_helper_unlock(&s->imu_oth);
		for (uint32_t i = 0; i < n; i++) {
			xrt_sink_push_imu(s->sinks->imus[0], &batch[i]);
		}
		os_thread_helper_lock(&s->imu_oth);
	}
	os_thread_helper_unlock(&s->imu_oth);
	return NULL;
}

static void *
frame_feeder(void *ptr)
{
	struct quest1_slam *s = ptr;
	os_thread_helper_name(&s->frame_oth, "Quest1 SLAM cam");

	os_thread_helper_lock(&s->frame_oth);
	while (os_thread_helper_is_running_locked(&s->frame_oth)) {
		if (s->pending[0] == NULL) {
			os_thread_helper_wait_locked(&s->frame_oth);
			continue;
		}
		struct xrt_frame *frames[QUEST1_CALIB_CAMS];
		memcpy(frames, s->pending, sizeof(frames));
		memset(s->pending, 0, sizeof(s->pending));
		int64_t frame_ts = s->pending_ts;
		os_thread_helper_unlock(&s->frame_oth);

		if (frame_ts > s->last_frame_ts) {
			s->last_frame_ts = frame_ts;
			for (int i = 0; i < s->cam_count; i++) {
				xrt_sink_push_frame(s->sinks->cams[i], frames[i]);
			}
			__atomic_add_fetch(&s->stats.framesets_pushed, 1, __ATOMIC_RELAXED);
		}
		for (int i = 0; i < s->cam_count; i++) {
			xrt_frame_reference(&frames[i], NULL);
		}

		int64_t now = os_monotonic_get_ns();
		if (now - s->last_stats_ns > SLAM_STATS_PERIOD_NS) {
			s->last_stats_ns = now;
			log_stats(s, now);
		}

		os_thread_helper_lock(&s->frame_oth);
	}
	os_thread_helper_unlock(&s->frame_oth);
	return NULL;
}

struct quest1_slam *
quest1_slam_create(const struct quest1_calib *calib,
                   const int *cam_ids,
                   int cam_count,
                   int frame_divider,
                   enum u_logging_level log_level)
{
	if (cam_count < 2 || cam_count > QUEST1_CALIB_CAMS) {
		U_LOG_IFL_E(log_level, "slam: need 2 to %d cameras, got %d", QUEST1_CALIB_CAMS, cam_count);
		return NULL;
	}
	struct quest1_slam *s = U_TYPED_CALLOC(struct quest1_slam);
	s->log_level = log_level;
	s->cam_count = cam_count;
	s->frame_divider = frame_divider < 1 ? 1 : frame_divider;
	memcpy(s->cam_ids, cam_ids, sizeof(int) * cam_count);
	s->width = calib->cam[cam_ids[0]].width;
	s->height = calib->cam[cam_ids[0]].height;
	s->last_imu_in_ts = INT64_MIN;
	s->last_frame_ts = INT64_MIN;
	os_thread_helper_init(&s->imu_oth);
	os_thread_helper_init(&s->frame_oth);
	os_mutex_init(&s->pose_mutex);
	fill_calibration(s, calib);

	struct t_slam_tracker_config config;
	t_slam_fill_default_config(&config);
	if (getenv("VIT_SYSTEM_LIBRARY_PATH") == NULL) {
		config.vit_system_library_path = SLAM_DEFAULT_VIT;
	}
	config.cam_count = cam_count;
	config.slam_calib = &s->calib;
	config.submit_from_start = true;
	if (config.slam_config != NULL) {
		SLAM_WARN(s, "SLAM_CONFIG=%s replaces the driver's calibration", config.slam_config);
	}

	if (t_slam_create(&s->xfctx, &config, &s->tracker, &s->sinks) != 0 || t_slam_start(s->tracker) != 0) {
		SLAM_ERROR(s, "cannot start the SLAM tracker (VIT library %s)", config.vit_system_library_path);
		quest1_slam_destroy(&s);
		return NULL;
	}
	if (os_thread_helper_start(&s->imu_oth, imu_feeder, s) != 0 ||
	    os_thread_helper_start(&s->frame_oth, frame_feeder, s) != 0) {
		quest1_slam_destroy(&s);
		return NULL;
	}
	SLAM_INFO(s, "tracker running with %d cameras at %.0f Hz (%s)", cam_count, SLAM_CAM_HZ / s->frame_divider,
	          config.vit_system_library_path);
	return s;
}

void
quest1_slam_push_imu(struct quest1_slam *s, int64_t ts, const struct xrt_vec3 *accel, const struct xrt_vec3 *gyro)
{
	if (s == NULL) {
		return;
	}
	os_thread_helper_lock(&s->imu_oth);
	__atomic_add_fetch(&s->stats.imu_in, 1, __ATOMIC_RELAXED);
	// Strictly increasing, as the tracker requires; the clock offset filter can step back slightly.
	if (ts <= s->last_imu_in_ts) {
		ts = s->last_imu_in_ts + 1;
	}
	s->last_imu_in_ts = ts;
	if (s->imu_head - s->imu_tail >= SLAM_IMU_RING) {
		s->imu_tail++; // the tracker fell behind: drop the oldest
		__atomic_add_fetch(&s->stats.imu_dropped, 1, __ATOMIC_RELAXED);
	}
	struct xrt_imu_sample *e = &s->imu[s->imu_head++ % SLAM_IMU_RING];
	e->timestamp_ns = ts;
	e->accel_m_s2 = (struct xrt_vec3_f64){accel->x, accel->y, accel->z};
	e->gyro_rad_secs = (struct xrt_vec3_f64){gyro->x, gyro->y, gyro->z};
	os_thread_helper_signal_locked(&s->imu_oth);
	os_thread_helper_unlock(&s->imu_oth);
}

void
quest1_slam_push_frameset(struct quest1_slam *s, int64_t ts, const uint8_t *const pixels[QUEST1_CALIB_CAMS])
{
	if (s == NULL || s->frameset_counter++ % s->frame_divider != 0) {
		return;
	}
	struct xrt_frame *frames[QUEST1_CALIB_CAMS] = {0};
	for (int i = 0; i < s->cam_count; i++) {
		u_frame_create_one_off(XRT_FORMAT_L8, s->width, s->height, &frames[i]);
		memcpy(frames[i]->data, pixels[s->cam_ids[i]], (size_t)s->width * s->height);
		frames[i]->timestamp = ts;
		frames[i]->source_timestamp = ts;
		frames[i]->source_sequence = (uint64_t)ts;
	}

	struct xrt_frame *old[QUEST1_CALIB_CAMS] = {0};
	__atomic_add_fetch(&s->stats.framesets_in, 1, __ATOMIC_RELAXED);
	os_thread_helper_lock(&s->frame_oth);
	if (s->pending[0] != NULL) {
		memcpy(old, s->pending, sizeof(old)); // the tracker fell behind: replace the unsent frameset
		__atomic_add_fetch(&s->stats.framesets_dropped, 1, __ATOMIC_RELAXED);
	}
	memcpy(s->pending, frames, sizeof(frames));
	s->pending_ts = ts;
	os_thread_helper_signal_locked(&s->frame_oth);
	os_thread_helper_unlock(&s->frame_oth);

	for (int i = 0; i < s->cam_count; i++) {
		xrt_frame_reference(&old[i], NULL);
	}
}

//! Basalt's T_world_imu (+Z up) to the device pose in a +Y up world.
bool
quest1_slam_get_pose(struct quest1_slam *s, int64_t at_ns, struct xrt_space_relation *out)
{
	if (s == NULL) {
		return false;
	}
	struct xrt_space_relation imu_rel = XRT_SPACE_RELATION_ZERO;
	os_mutex_lock(&s->pose_mutex);
	xrt_tracked_slam_get_tracked_pose(s->tracker, at_ns, &imu_rel);
	if ((imu_rel.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0) {
		os_mutex_unlock(&s->pose_mutex);
		return false;
	}
	float speed = m_vec3_len(imu_rel.linear_velocity), dist = m_vec3_len(imu_rel.pose.position);
	if (!(speed < SLAM_MAX_SPEED_M_S) || !(dist < SLAM_MAX_DISTANCE_M)) {
		int64_t now = os_monotonic_get_ns();
		if (now - s->last_reset_ns > SLAM_RESET_INTERVAL_NS) {
			s->last_reset_ns = now;
			s->resets++;
			SLAM_WARN(s, "tracking diverged (%.1f m/s, %.1f m from the origin): reset %u", speed, dist,
			          s->resets);
			t_slam_reset(s->tracker);
		}
		os_mutex_unlock(&s->pose_mutex);
		return false;
	}
	os_mutex_unlock(&s->pose_mutex);
	imu_rel.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	    (imu_rel.relation_flags &
	     (XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT)));

	const struct xrt_pose z_up_to_y_up = {{-0.70710678f, 0.0f, 0.0f, 0.70710678f}, {0.0f, 0.0f, 0.0f}};
	struct xrt_relation_chain xrc = {0};
	m_relation_chain_push_pose(&xrc, &s->imu_from_device);
	m_relation_chain_push_relation(&xrc, &imu_rel);
	m_relation_chain_push_pose(&xrc, &z_up_to_y_up);
	m_relation_chain_resolve(&xrc, out);
	return true;
}

void
quest1_slam_destroy(struct quest1_slam **s_ptr)
{
	struct quest1_slam *s = *s_ptr;
	if (s == NULL) {
		return;
	}
	// A feeder blocked in one of Basalt's full queues returns once the running tracker drains it.
	// The IMU feeder goes first: Basalt only consumes IMU samples while frames keep coming.
	// Then the tracker, which must not be destroyed under a feeder.
	os_thread_helper_destroy(&s->imu_oth);
	os_thread_helper_destroy(&s->frame_oth);
	xrt_frame_context_destroy_nodes(&s->xfctx);
	for (int i = 0; i < QUEST1_CALIB_CAMS; i++) {
		xrt_frame_reference(&s->pending[i], NULL);
	}
	os_mutex_destroy(&s->pose_mutex);
	free(s);
	*s_ptr = NULL;
}

#else // XRT_FEATURE_SLAM

struct quest1_slam *
quest1_slam_create(const struct quest1_calib *calib,
                   const int *cam_ids,
                   int cam_count,
                   int frame_divider,
                   enum u_logging_level log_level)
{
	U_LOG_IFL_I(log_level, "slam: Monado was built without XRT_FEATURE_SLAM, 3DoF only");
	return NULL;
}

void
quest1_slam_push_imu(struct quest1_slam *s, int64_t ts, const struct xrt_vec3 *accel, const struct xrt_vec3 *gyro)
{}

void
quest1_slam_push_frameset(struct quest1_slam *s, int64_t ts, const uint8_t *const pixels[QUEST1_CALIB_CAMS])
{}

bool
quest1_slam_get_pose(struct quest1_slam *s, int64_t at_ns, struct xrt_space_relation *out)
{
	return false;
}

void
quest1_slam_destroy(struct quest1_slam **s_ptr)
{}

#endif // XRT_FEATURE_SLAM
