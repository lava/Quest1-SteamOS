// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 6DoF head tracking: tracking cameras and IMU into Monado's SLAM tracker (Basalt).
 * @ingroup drv_quest1
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "util/u_logging.h"

#include "quest1_calib.h"

#ifdef __cplusplus
extern "C" {
#endif

struct quest1_slam;

/*!
 * Starts the SLAM tracker with the driver's calibration; NULL if SLAM is not built in or the VIT
 * library (default /opt/basalt/lib/libbasalt.so, VIT_SYSTEM_LIBRARY_PATH) cannot be loaded.
 *
 * @param cam_ids Calibration ids in tracker order; the first two must be the stereo pair (0, 2).
 * @param frame_divider Only every n-th frameset of the 30 Hz stream goes to the tracker.
 */
struct quest1_slam *
quest1_slam_create(const struct quest1_calib *calib,
                   const int *cam_ids,
                   int cam_count,
                   int frame_divider,
                   enum u_logging_level log_level);

//! Rectified IMU sample (device axes, SI units) on the monotonic clock. Never blocks.
void
quest1_slam_push_imu(struct quest1_slam *s, int64_t timestamp_ns, const struct xrt_vec3 *accel, const struct xrt_vec3 *gyro);

//! A frameset (calibration id order, 640x480 Y8, mid-exposure on the monotonic clock). Never blocks.
void
quest1_slam_push_frameset(struct quest1_slam *s, int64_t timestamp_ns, const uint8_t *const pixels[QUEST1_CALIB_CAMS]);

/*!
 * Device pose (the calibration's device frame: +X right, +Y up, +Z back) in an OpenXR-style
 * world (+Y up). False until the tracker has produced its first pose.
 */
bool
quest1_slam_get_pose(struct quest1_slam *s, int64_t at_timestamp_ns, struct xrt_space_relation *out_relation);

void
quest1_slam_destroy(struct quest1_slam **s_ptr);

#ifdef __cplusplus
}
#endif
