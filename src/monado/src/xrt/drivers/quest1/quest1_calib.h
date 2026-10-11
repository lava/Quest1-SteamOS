// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 camera and IMU calibration from /persist/calibration (Rift S JSON schema).
 * @ingroup drv_quest1
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "util/u_logging.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QUEST1_CALIB_CAMS 4
#define QUEST1_CALIB_MAX_PAIRS 64

//! IMU offset in raw sensor coordinates as a function of the temperature.
struct quest1_imu_offset
{
	int pair_count;                         //!< 0: constant (offset_at_0c + temp_coeff * T)
	float offset_at_0c[3], temp_coeff[3];   //!< Constant and LinearTemperatureDependence
	float pairs[QUEST1_CALIB_MAX_PAIRS][4]; //!< TemperaturePiecewiseLinear: T, x, y, z (sorted by T)
};

struct quest1_imu_calib
{
	//! Row-major 4x4. Device frame: +X right, +Y up, +Z back.
	float device_from_imu[16];
	float accel_rect[9], gyro_rect[9]; //!< row-major; rectified = R * (raw - offset(T))
	struct quest1_imu_offset accel_offset, gyro_offset;
};

struct quest1_cam_calib
{
	//! Row-major 4x4; camera frame: OpenCV (+X right, +Y down, +Z forward in the image).
	float device_from_camera[16];
	int width, height;
	double f, cx, cy;     //!< PinholeSymmetric
	double fisheye62[8];  //!< k1..k6, p1, p2
	double kb4[4];        //!< Kannala-Brandt fit with the same f, cx, cy
	double kb4_rms_px, kb4_max_px;
};

struct quest1_calib
{
	char source[512]; //!< file the cameras and IMU came from
	struct quest1_imu_calib imu;
	struct quest1_cam_calib cam[QUEST1_CALIB_CAMS]; //!< index = calibration Id
};

/*!
 * Loads the newest online calibration (/persist/calibration/online/<id>, refined by Meta's
 * tracking), falling back to the factory files, and fits KB4 to each camera's Fisheye62.
 */
bool
quest1_calib_load(struct quest1_calib *out, enum u_logging_level log_level);

//! Raw IMU sample to device axes, with the temperature-dependent offsets removed.
void
quest1_calib_rectify_imu(const struct quest1_imu_calib *c,
                         float temperature,
                         const float raw_accel[3],
                         const float raw_gyro[3],
                         struct xrt_vec3 *out_accel,
                         struct xrt_vec3 *out_gyro);

#ifdef __cplusplus
}
#endif
