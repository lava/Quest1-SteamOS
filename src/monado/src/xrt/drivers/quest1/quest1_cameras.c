// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 tracking cameras: the src/qcam capture sequence on the driver's SyncBoss handle,
 *         HEADSET frames published to shared memory (quest1_cameras_shm.h) while readers ask for them.
 *
 * libsyncboss powers and syncs the four OV7251 sensors, libqcameraoculushal (Qualcomm camera_v2
 * mini-driver) streams them; both are bionic libraries loaded with libhybris. The IMU thread keeps
 * pumping the SyncBoss stream, which the camera commands need. Prototypes and the call order are
 * documented in src/qcam/qcam.c and docs/tracking-6dof.md.
 *
 * @ingroup drv_quest1
 */

#include "quest1_interface.h"
#include "quest1_cameras_shm.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern void *
android_dlopen(const char *filename, int flag);
extern void *
android_dlsym(void *handle, const char *symbol);

DEBUG_GET_ONCE_NUM_OPTION(quest1_camera_exposure, "QUEST1_CAMERA_EXPOSURE_US", 0) // 0 = auto
DEBUG_GET_ONCE_FLOAT_OPTION(quest1_camera_gain, "QUEST1_CAMERA_GAIN", 4.0f)

#define CAM_ERROR(c, ...) U_LOG_IFL_E(c->log_level, "cameras: " __VA_ARGS__)
#define CAM_INFO(c, ...) U_LOG_IFL_I(c->log_level, "cameras: " __VA_ARGS__)
#define CAM_DEBUG(c, ...) U_LOG_IFL_D(c->log_level, "cameras: " __VA_ARGS__)

#define CAM_PERIOD_US 33333  // 60 Hz mains; HEADSET and CONTROLLER frames alternate within it
#define CAM_NBUFS 4
#define CAM_DEQ_TIMEOUT_MS 100
#define CAM_TAG_HEADSET 1
#define CAM_EXP_MIN_US 100
#define CAM_EXP_MAX_US 14000 // below the 16.7 ms frame slot
#define CAM_GAIN_MAX_X16 128 // 8x
#define CAM_TARGET_MEAN 90.0f
#define CAM_DEMAND_TIMEOUT_NS (2 * (int64_t)U_TIME_1S_IN_NS) // after the last reader heartbeat
#define CAM_IDLE_POLL_NS (100 * U_TIME_1MS_IN_NS)
#define CAM_RETRY_NS (5 * (int64_t)U_TIME_1S_IN_NS)
#define CAM_CALIB_PATH "/persist/calibration/camera_calibration_v2.json"


/*
 *
 * libqcameraoculushal and the libsyncboss camera calls.
 *
 */

//! qcamera_query_sensor_info output (0x8c bytes written), with slack.
struct qc_sensor_info
{
	uint32_t width, height; // without the metadata line
	uint32_t format;        // 0x70 = Y8
	uint32_t bpp, pixel_proc, code_bits, px_per_code, packed, bayer, sensor_id;
	uint8_t rest[140 - 40];
	uint8_t slack[116];
};

struct qc_frame
{
	int64_t ts_sec, ts_nsec; // CLOCK_MONOTONIC
	const void *fmt;
	uint32_t sequence, pad0;
	uint64_t zero[2];
	uint8_t *data; // metadata line first
	int32_t buffer_fd, pad1;
	uint32_t poison;
};

struct sb_cam_egt
{
	uint16_t exposure; // us
	uint16_t gain;     // gain * 16
	uint8_t tag;
	uint8_t pad;
};

struct sb_frame_meta
{
	uint16_t exposure;
	uint8_t gain_x16;
	uint8_t b3;
	uint8_t tag; // 1 = HEADSET, 0 = CONTROLLER
	uint8_t sync_id;
	uint8_t pad[10];
};

struct cam_api
{
	void *(*open)(int bpp_mode);
	int (*num_sensors)(void *hal);
	void *(*get_sensor)(void *hal, int idx);
	int (*query_info)(void *sensor, struct qc_sensor_info *out);
	int (*start_sensor)(void *sensor, const uint32_t *dims, const void *fmt28, int nbufs, const int *fds, int meta);
	struct qc_frame *(*dequeue)(void *sensor, int timeout_ms);
	int (*enqueue)(void *sensor, struct qc_frame *f);
	int (*stop_sensor)(void *sensor);
	void (*release_sensor)(void *sensor);
	void (*close)(void *hal);

	int (*probe)(void *sb, uint8_t *mask);
	int (*release)(void *sb);
	int (*init)(void *sb, uint8_t n);
	int (*deinit)(void *sb, uint8_t n);
	int (*start_streaming)(void *sb, uint8_t n);
	int (*stop_streaming)(void *sb, uint8_t n);
	int (*set_bpp)(void *sb, uint8_t bpp);
	int (*set_tag_mode)(void *sb, uint8_t mode);
	int (*set_frame_rate)(void *sb, uint32_t period_us);
	int (*params)(void *sb, void *out);
	int (*set_exp_gain_tag)(void *sb, const struct sb_cam_egt *cfg4);
	int (*decode_meta)(const void *line, struct sb_frame_meta *out, const void *params);
};

struct quest1_cameras
{
	enum u_logging_level log_level;
	void *sb;
	struct cam_api api;
	struct os_thread_helper oth;

	void *hal;
	int n;
	void *sensor[Q1CAM_N];
	bool started[Q1CAM_N];
	bool probed, inited, streaming;
	uint8_t params[256];

	struct q1cam_shm *shm;
	int exposure_us;
	uint16_t gain_x16, gain_min_x16;
	bool auto_exposure;
};

#define LOAD(lib, field, name)                                                                                         \
	do {                                                                                                           \
		*(void **)&c->api.field = android_dlsym(lib, name);                                                    \
		if (c->api.field == NULL) {                                                                            \
			CAM_ERROR(c, "missing symbol %s", name);                                                       \
			return false;                                                                                  \
		}                                                                                                      \
	} while (false)

static bool
load_api(struct quest1_cameras *c)
{
	void *lsb = android_dlopen("libsyncboss.so", RTLD_NOW);
	void *lqc = android_dlopen("libqcameraoculushal.so", RTLD_NOW);
	if (lsb == NULL || lqc == NULL) {
		CAM_ERROR(c, "cannot load libsyncboss.so / libqcameraoculushal.so");
		return false;
	}
	LOAD(lqc, open, "qcamera_open");
	LOAD(lqc, num_sensors, "qcamera_num_sensors");
	LOAD(lqc, get_sensor, "qcamera_get_sensor");
	LOAD(lqc, query_info, "qcamera_query_sensor_info");
	LOAD(lqc, start_sensor, "qcamera_start_sensor");
	LOAD(lqc, dequeue, "qcamera_dequeue");
	LOAD(lqc, enqueue, "qcamera_enqueue");
	LOAD(lqc, stop_sensor, "qcamera_stop_sensor");
	LOAD(lqc, release_sensor, "qcamera_release_sensor");
	LOAD(lqc, close, "qcamera_close");
	LOAD(lsb, probe, "syncboss_camera_probe");
	LOAD(lsb, release, "syncboss_camera_release");
	LOAD(lsb, init, "syncboss_camera_init");
	LOAD(lsb, deinit, "syncboss_camera_deinit");
	LOAD(lsb, start_streaming, "syncboss_camera_start_streaming");
	LOAD(lsb, stop_streaming, "syncboss_camera_stop_streaming");
	LOAD(lsb, set_bpp, "syncboss_camera_set_bpp");
	LOAD(lsb, set_tag_mode, "syncboss_camera_set_frame_tag_mode");
	LOAD(lsb, set_frame_rate, "syncboss_camera_set_frame_rate");
	LOAD(lsb, params, "syncboss_camera_params");
	LOAD(lsb, set_exp_gain_tag, "syncboss_camera_set_exposure_gain_tag");
	LOAD(lsb, decode_meta, "syncboss_camera_decode_metadata");
	return true;
}


/*
 *
 * Calibration and shared memory.
 *
 */

static void
load_calibration(struct quest1_cameras *c)
{
	char *text = u_file_read_content_from_path(CAM_CALIB_PATH, NULL);
	if (text == NULL) {
		CAM_ERROR(c, "cannot read %s", CAM_CALIB_PATH);
		return;
	}
	cJSON *root = cJSON_Parse(text);
	free(text);
	const cJSON *cams = u_json_get(root, "CameraCalibration");
	const cJSON *cam;
	cJSON_ArrayForEach(cam, cams)
	{
		char id[8] = {0};
		u_json_get_string_into_array(u_json_get(cam, "Id"), id, sizeof(id));
		int i = atoi(id);
		if (i < 0 || i >= Q1CAM_N) {
			continue;
		}
		struct q1cam_calib *k = &c->shm->calib[i];
		float proj[3], dist[8];
		if (u_json_get_float_array(u_json_get(cam, "DeviceFromCamera"), k->device_from_camera, 16) != 16 ||
		    u_json_get_float_array(u_json_get(u_json_get(cam, "Projection"), "Coefficients"), proj, 3) != 3 ||
		    u_json_get_float_array(u_json_get(u_json_get(cam, "Distortion"), "Coefficients"), dist, 8) != 8) {
			CAM_ERROR(c, "camera %d: unexpected calibration layout", i);
			continue;
		}
		k->f = proj[0];
		k->cx = proj[1];
		k->cy = proj[2];
		memcpy(k->k, dist, sizeof(k->k));
		memcpy(k->p, dist + 6, sizeof(k->p));
		c->shm->calib_valid |= 1u << i;
	}
	cJSON_Delete(root);
}

static bool
create_shm(struct quest1_cameras *c)
{
	int fd = shm_open(Q1CAM_SHM_NAME, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0 || ftruncate(fd, sizeof(struct q1cam_shm)) != 0) {
		CAM_ERROR(c, "cannot create shared memory %s", Q1CAM_SHM_NAME);
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	c->shm = mmap(NULL, sizeof(struct q1cam_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (c->shm == MAP_FAILED) {
		c->shm = NULL;
		return false;
	}
	c->shm->ncams = Q1CAM_N;
	c->shm->width = Q1CAM_W;
	c->shm->height = Q1CAM_H;
	c->shm->slots = Q1CAM_SLOTS;
	c->shm->version = Q1CAM_VERSION;
	load_calibration(c);
	__atomic_store_n(&c->shm->magic, Q1CAM_MAGIC, __ATOMIC_RELEASE);
	return true;
}


/*
 *
 * Capture.
 *
 */

static void
set_exposure(struct quest1_cameras *c)
{
	struct sb_cam_egt cfg[4];
	memset(cfg, 0, sizeof(cfg));
	for (int i = 0; i < 4; i++) {
		cfg[i].exposure = (uint16_t)c->exposure_us;
		cfg[i].gain = c->gain_x16;
		cfg[i].tag = CAM_TAG_HEADSET;
	}
	c->api.set_exp_gain_tag(c->sb, cfg);
}

//! The qcam sequence (MontereyCameraProvider order), up to streaming.
static bool
start_cameras(struct quest1_cameras *c)
{
	struct cam_api *a = &c->api;
	c->hal = a->open(0); // 8 bpp
	if (c->hal == NULL) {
		CAM_ERROR(c, "qcamera_open failed");
		return false;
	}
	c->n = a->num_sensors(c->hal) & 0xff;
	uint8_t mask = 0;
	int r = a->probe(c->sb, &mask);
	if (r != 0) {
		CAM_ERROR(c, "syncboss_camera_probe: %d", r);
		return false;
	}
	c->probed = true;
	if (c->n != Q1CAM_N || __builtin_popcount(mask) != Q1CAM_N) {
		CAM_ERROR(c, "expected 4 cameras: HAL %d, SyncBoss mask 0x%02x", c->n, mask);
		return false;
	}
	a->set_bpp(c->sb, 8);
	r = a->init(c->sb, Q1CAM_N);
	if (r != 0) {
		CAM_ERROR(c, "syncboss_camera_init: %d", r);
		return false;
	}
	c->inited = true;

	struct qc_sensor_info info[Q1CAM_N];
	memset(info, 0, sizeof(info));
	for (int i = 0; i < Q1CAM_N; i++) {
		c->sensor[i] = a->get_sensor(c->hal, i);
		if (c->sensor[i] == NULL) {
			CAM_ERROR(c, "qcamera_get_sensor(%d) failed", i);
			return false;
		}
		a->query_info(c->sensor[i], &info[i]);
		if (info[i].width != Q1CAM_W || info[i].height != Q1CAM_H || info[i].bpp != 8) {
			CAM_ERROR(c, "camera %d: unexpected %ux%u, %u bpp", i, info[i].width, info[i].height, info[i].bpp);
			return false;
		}
	}
	a->set_frame_rate(c->sb, CAM_PERIOD_US);
	if (a->params(c->sb, c->params) != 0) {
		CAM_ERROR(c, "syncboss_camera_params failed");
		return false;
	}
	for (int i = 0; i < Q1CAM_N; i++) {
		r = a->start_sensor(c->sensor[i], &info[i].width, &info[i].format, CAM_NBUFS, NULL, 1);
		if (r != 0) {
			CAM_ERROR(c, "qcamera_start_sensor(%d): %d", i, r);
			return false;
		}
		c->started[i] = true;
	}
	a->set_tag_mode(c->sb, 1);
	r = a->start_streaming(c->sb, Q1CAM_N);
	c->streaming = true; // stop it even if the call reported an error
	if (r != 0) {
		CAM_ERROR(c, "syncboss_camera_start_streaming: %d", r);
		return false;
	}
	set_exposure(c);
	return true;
}

static void
stop_cameras(struct quest1_cameras *c)
{
	struct cam_api *a = &c->api;
	if (c->streaming) {
		a->stop_streaming(c->sb, Q1CAM_N);
		c->streaming = false;
	}
	for (int i = 0; i < Q1CAM_N; i++) {
		if (c->started[i]) {
			a->stop_sensor(c->sensor[i]);
			c->started[i] = false;
		}
	}
	for (int i = 0; i < Q1CAM_N; i++) {
		if (c->sensor[i] != NULL) {
			a->release_sensor(c->sensor[i]);
			c->sensor[i] = NULL;
		}
	}
	if (c->hal != NULL) {
		a->close(c->hal);
		c->hal = NULL;
	}
	if (c->inited) {
		a->deinit(c->sb, Q1CAM_N);
		c->inited = false;
	}
	if (c->probed) {
		a->release(c->sb);
		c->probed = false;
	}
}

//! Simple auto exposure on the mean of a pixel grid over all cameras.
static void
update_exposure(struct quest1_cameras *c, const struct q1cam_slot *s)
{
	uint64_t sum = 0, count = 0;
	for (int i = 0; i < Q1CAM_N; i++) {
		for (int y = 8; y < Q1CAM_H; y += 16) {
			for (int x = 8; x < Q1CAM_W; x += 16) {
				sum += s->pixels[i][y * Q1CAM_W + x];
				count++;
			}
		}
	}
	float mean = (float)sum / (float)count;
	float ratio = CAM_TARGET_MEAN / (mean < 1.0f ? 1.0f : mean);
	ratio = ratio < 0.7f ? 0.7f : (ratio > 1.4f ? 1.4f : ratio);
	// Exposure first; gain (from QUEST1_CAMERA_GAIN up to CAM_GAIN_MAX_X16) only at the exposure limit.
	int exp = c->exposure_us, gain = c->gain_x16;
	if (ratio > 1.0f && exp >= CAM_EXP_MAX_US) {
		gain = (int)((float)gain * ratio);
	} else if (ratio < 1.0f && gain > c->gain_min_x16) {
		gain = (int)((float)gain * ratio);
		gain = gain < c->gain_min_x16 ? c->gain_min_x16 : gain;
	} else {
		exp = (int)((float)exp * ratio);
	}
	exp = exp < CAM_EXP_MIN_US ? CAM_EXP_MIN_US : (exp > CAM_EXP_MAX_US ? CAM_EXP_MAX_US : exp);
	gain = gain > CAM_GAIN_MAX_X16 ? CAM_GAIN_MAX_X16 : gain;
	if (abs(exp - c->exposure_us) * 20 > c->exposure_us || abs(gain - c->gain_x16) * 20 > c->gain_x16) {
		CAM_DEBUG(c, "mean %.1f, exposure %d -> %d us, gain %d -> %d /16", mean, c->exposure_us, exp,
		          c->gain_x16, gain);
		c->exposure_us = exp;
		c->gain_x16 = (uint16_t)gain;
		set_exposure(c);
	}
}

//! Dequeues one frameset; HEADSET framesets are published. False on a dequeue timeout.
static bool
capture_frameset(struct quest1_cameras *c, uint64_t *seq)
{
	struct q1cam_slot *s = &c->shm->slot[(*seq + 1) % Q1CAM_SLOTS];
	struct qc_frame *f[Q1CAM_N];
	bool ok = true, headset = false;
	for (int i = 0; i < Q1CAM_N; i++) {
		f[i] = c->api.dequeue(c->sensor[i], CAM_DEQ_TIMEOUT_MS);
		if (f[i] == NULL || f[i]->data == NULL) {
			ok = false;
			continue;
		}
		struct sb_frame_meta m;
		memset(&m, 0, sizeof(m));
		c->api.decode_meta(f[i]->data, &m, c->params);
		if (i == 0) {
			headset = m.tag == CAM_TAG_HEADSET;
			s->timestamp_ns = f[i]->ts_sec * U_TIME_1S_IN_NS + f[i]->ts_nsec;
		}
		if (headset) {
			memcpy(s->pixels[i], f[i]->data + Q1CAM_W, Q1CAM_W * Q1CAM_H); // skip the metadata line
			s->exposure_us[i] = m.exposure;
			s->gain_x16[i] = m.gain_x16;
		}
	}
	for (int i = 0; i < Q1CAM_N; i++) {
		if (f[i] != NULL) {
			c->api.enqueue(c->sensor[i], f[i]);
		}
	}
	if (ok && headset) {
		(*seq)++;
		__atomic_store_n(&s->seq, *seq, __ATOMIC_RELEASE);
		__atomic_store_n(&c->shm->latest, *seq, __ATOMIC_RELEASE);
		if (c->auto_exposure && *seq % 8 == 0) {
			update_exposure(c, s);
		}
	}
	return ok;
}

static bool
demanded(struct quest1_cameras *c)
{
	int64_t demand = (int64_t)__atomic_load_n(&c->shm->demand_ns, __ATOMIC_ACQUIRE);
	return demand != 0 && os_monotonic_get_ns() - demand < CAM_DEMAND_TIMEOUT_NS;
}

//! Idle until a reader asks for frames, stream while asked, power the cameras down afterwards.
static void *
camera_thread(void *ptr)
{
	struct quest1_cameras *c = ptr;
	os_thread_helper_name(&c->oth, "Quest1 cameras");

	uint64_t seq = 0;
	int64_t retry_after_ns = 0;
	os_thread_helper_lock(&c->oth);
	while (os_thread_helper_is_running_locked(&c->oth)) {
		os_thread_helper_unlock(&c->oth);

		if (!demanded(c) || os_monotonic_get_ns() < retry_after_ns) {
			os_nanosleep(CAM_IDLE_POLL_NS);
		} else if (!start_cameras(c)) {
			stop_cameras(c);
			retry_after_ns = os_monotonic_get_ns() + CAM_RETRY_NS;
		} else {
			CAM_INFO(c, "streaming (exposure %s %d us, gain %.1f)", c->auto_exposure ? "auto" : "fixed",
			         c->exposure_us, c->gain_x16 / 16.0f);
			__atomic_store_n(&c->shm->streaming, 1, __ATOMIC_RELEASE);
			uint64_t first = seq, timeouts = 0;
			while (demanded(c) && os_thread_helper_is_running(&c->oth)) {
				if (!capture_frameset(c, &seq) && ++timeouts % 50 == 0) {
					CAM_ERROR(c, "%llu dequeue timeouts", (unsigned long long)timeouts);
				}
			}
			__atomic_store_n(&c->shm->streaming, 0, __ATOMIC_RELEASE);
			stop_cameras(c);
			CAM_INFO(c, "stopped after %llu framesets", (unsigned long long)(seq - first));
		}

		os_thread_helper_lock(&c->oth);
	}
	os_thread_helper_unlock(&c->oth);
	return NULL;
}


/*
 *
 * Exported functions.
 *
 */

struct quest1_cameras *
quest1_cameras_start(void *sb_handle, enum u_logging_level log_level)
{
	struct quest1_cameras *c = U_TYPED_CALLOC(struct quest1_cameras);
	c->log_level = log_level;
	c->sb = sb_handle;
	int64_t exp = debug_get_num_option_quest1_camera_exposure();
	c->auto_exposure = exp <= 0;
	c->exposure_us = c->auto_exposure ? 8000 : (int)exp;
	c->gain_x16 = (uint16_t)(debug_get_float_option_quest1_camera_gain() * 16.0f + 0.5f);
	c->gain_min_x16 = c->gain_x16;
	os_thread_helper_init(&c->oth);

	if (!load_api(c) || !create_shm(c) || os_thread_helper_start(&c->oth, camera_thread, c) != 0) {
		quest1_cameras_destroy(&c);
		return NULL;
	}
	return c;
}

void
quest1_cameras_destroy(struct quest1_cameras **c_ptr)
{
	struct quest1_cameras *c = *c_ptr;
	if (c == NULL) {
		return;
	}
	os_thread_helper_destroy(&c->oth); // the thread stops the cameras
	if (c->shm != NULL) {
		munmap(c->shm, sizeof(struct q1cam_shm));
		shm_unlink(Q1CAM_SHM_NAME);
	}
	free(c);
	*c_ptr = NULL;
}
