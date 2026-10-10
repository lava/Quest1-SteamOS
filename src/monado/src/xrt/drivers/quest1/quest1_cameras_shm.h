// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared-memory layout of the tracking camera frames that the quest1 driver publishes
 *         (QUEST1_CAMERAS=0 turns it off). Plain C, no Monado headers: clients include it directly.
 *
 * The writer fills slot (n % Q1CAM_SLOTS), sets its seq to n, then stores latest = n (release).
 * A reader loads latest (acquire), copies that slot and checks that its seq is still n.
 *
 * The cameras run only on demand: a reader stores CLOCK_MONOTONIC ns into demand_ns at least every
 * 0.5 s (the region is root-only, open it O_RDWR). The driver powers the cameras up when demand_ns
 * is fresh (startup takes about a second) and down 2 s after the last heartbeat.
 *
 * @ingroup drv_quest1
 */

#pragma once

#include <stdint.h>

#define Q1CAM_SHM_NAME "/quest1-cameras"
#define Q1CAM_MAGIC 0x314d4351u // "QCM1"
#define Q1CAM_VERSION 2
#define Q1CAM_N 4
#define Q1CAM_W 640
#define Q1CAM_H 480
#define Q1CAM_SLOTS 3

//! Factory calibration from /persist/calibration/camera_calibration_v2.json.
struct q1cam_calib
{
	//! Row-major 4x4. Device frame: +X right, +Y up, +Z back; camera frame: OpenCV (+Z forward).
	float device_from_camera[16];
	float f, cx, cy; //!< PinholeSymmetric, pixels
	float k[6];      //!< Fisheye62 radial: theta_d = theta * (1 + k0 theta^2 + ... + k5 theta^12)
	float p[2];      //!< Fisheye62 tangential
	float pad;
};

struct q1cam_slot
{
	uint64_t seq;
	int64_t timestamp_ns; //!< CLOCK_MONOTONIC capture time (ISP timestamp of camera 0)
	uint16_t exposure_us[Q1CAM_N];
	uint8_t gain_x16[Q1CAM_N];
	uint8_t pad[4];
	uint8_t pixels[Q1CAM_N][Q1CAM_W * Q1CAM_H]; //!< Y8, index = calibration Id
};

struct q1cam_shm
{
	uint32_t magic, version;
	uint32_t ncams, width, height, slots;
	uint32_t calib_valid; //!< bit i: calib[i] was loaded
	uint32_t streaming;   //!< 1 while the cameras run
	struct q1cam_calib calib[Q1CAM_N];
	uint64_t latest;    //!< 0 = no frame yet
	uint64_t demand_ns; //!< written by readers: CLOCK_MONOTONIC of their last heartbeat
	uint64_t pad2[6];
	struct q1cam_slot slot[Q1CAM_SLOTS];
};
