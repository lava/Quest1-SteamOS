// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*
 * q1record: records the Quest 1 tracking cameras and the headset IMU from Monado's quest1 driver
 * (shared memory, quest1_cameras_shm.h) as a EuRoC-style dataset for offline SLAM work:
 *
 *   DIR/mav0/cam{0..3}/data/<t>.pgm   640x480 Y8, index = calibration Id
 *   DIR/mav0/cam{0..3}/data.csv       #timestamp [ns],filename
 *   DIR/mav0/imu0/data.csv            #timestamp [ns],w_x,w_y,w_z [rad s^-1],a_x,a_y,a_z [m s^-2]
 *   DIR/mav0/frames.csv               per frameset: shutter time, ISP time, clock offset, exposure, gain
 *   DIR/mav0/imu0/temperature.csv     IMU temperature, once per second
 *
 * Timestamps are on the IMU clock (SyncBoss MCU time in ns): IMU samples as delivered, frames at
 * their SyncBoss shutter time. The IMU values are raw (sensor axes, no factory calibration).
 * Framesets without a matching shutter record are skipped and counted.
 *
 * Usage: q1record [-t SECONDS] [-c CAMS] DIR    (CAMS e.g. 0,2; default all four)
 * Needs root (the shared memory is root-only) and monado-service running. Stop with Ctrl-C.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "quest1_cameras_shm.h"

#define LOG(...) fprintf(stderr, "q1record: " __VA_ARGS__)

static volatile sig_atomic_t quit_requested;

static void
on_signal(int sig)
{
	(void)sig;
	quit_requested = 1;
}

static int64_t
monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void
heartbeat(struct q1cam_shm *shm)
{
	__atomic_store_n(&shm->demand_ns, (uint64_t)monotonic_ns(), __ATOMIC_RELEASE);
}

static struct q1cam_shm *
open_shm(void)
{
	int fd = shm_open(Q1CAM_SHM_NAME, O_RDWR, 0);
	if (fd < 0) {
		LOG("cannot open %s: %s (monado-service running? root?)\n", Q1CAM_SHM_NAME, strerror(errno));
		return NULL;
	}
	void *p = mmap(NULL, sizeof(struct q1cam_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		LOG("mmap: %s\n", strerror(errno));
		return NULL;
	}
	struct q1cam_shm *shm = p;
	if (__atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) != Q1CAM_MAGIC || shm->version != Q1CAM_VERSION) {
		LOG("%s has version %u, expected %u (old monado-service?)\n", Q1CAM_SHM_NAME, shm->version,
		    Q1CAM_VERSION);
		munmap(p, sizeof(struct q1cam_shm));
		return NULL;
	}
	return shm;
}

static void
mkdir_or_die(const char *path)
{
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		LOG("mkdir %s: %s\n", path, strerror(errno));
		exit(1);
	}
}

static FILE *
open_or_die(const char *path)
{
	FILE *f = fopen(path, "w");
	if (f == NULL) {
		LOG("cannot write %s: %s\n", path, strerror(errno));
		exit(1);
	}
	return f;
}

//! Copies IMU sample n; false if the writer has overwritten it.
static bool
read_imu(const struct q1cam_shm *shm, uint64_t n, struct q1cam_imu_sample *out)
{
	const struct q1cam_imu_sample *e = &shm->imu[n % Q1CAM_IMU_RING];
	if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != n) {
		return false;
	}
	memcpy(out, e, sizeof(*out));
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&e->seq, __ATOMIC_RELAXED) == n;
}

//! Copies frameset n; false if the writer has overwritten it.
static bool
read_slot(const struct q1cam_shm *shm, uint64_t n, struct q1cam_slot *out)
{
	const struct q1cam_slot *s = &shm->slot[n % Q1CAM_SLOTS];
	if (__atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != n) {
		return false;
	}
	memcpy(out, s, sizeof(*out));
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&s->seq, __ATOMIC_RELAXED) == n;
}

static bool
write_pgm(const char *path, const uint8_t *pixels)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		return false;
	}
	fprintf(f, "P5\n%d %d\n255\n", Q1CAM_W, Q1CAM_H);
	bool ok = fwrite(pixels, 1, Q1CAM_W * Q1CAM_H, f) == Q1CAM_W * Q1CAM_H;
	return fclose(f) == 0 && ok;
}

int
main(int argc, char **argv)
{
	double seconds = 0;
	unsigned cams = (1u << Q1CAM_N) - 1;
	int opt;
	while ((opt = getopt(argc, argv, "t:c:")) != -1) {
		if (opt == 't') {
			seconds = atof(optarg);
		} else if (opt == 'c') {
			cams = 0;
			for (const char *p = optarg; *p; p++) {
				if (*p >= '0' && *p < '0' + Q1CAM_N) {
					cams |= 1u << (*p - '0');
				}
			}
		} else {
			optind = argc + 1;
			break;
		}
	}
	if (optind != argc - 1 || cams == 0) {
		fprintf(stderr, "usage: %s [-t SECONDS] [-c CAMS] DIR\n", argv[0]);
		return 2;
	}
	const char *dir = argv[optind];

	struct q1cam_shm *shm = open_shm();
	if (shm == NULL) {
		return 1;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	char path[4096];
	mkdir_or_die(dir);
	snprintf(path, sizeof(path), "%s/mav0", dir);
	mkdir_or_die(path);
	FILE *cam_csv[Q1CAM_N] = {0};
	for (int i = 0; i < Q1CAM_N; i++) {
		if (!(cams & (1u << i))) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/mav0/cam%d", dir, i);
		mkdir_or_die(path);
		snprintf(path, sizeof(path), "%s/mav0/cam%d/data", dir, i);
		mkdir_or_die(path);
		snprintf(path, sizeof(path), "%s/mav0/cam%d/data.csv", dir, i);
		cam_csv[i] = open_or_die(path);
		fprintf(cam_csv[i], "#timestamp [ns],filename\n");
	}
	snprintf(path, sizeof(path), "%s/mav0/imu0", dir);
	mkdir_or_die(path);
	snprintf(path, sizeof(path), "%s/mav0/imu0/data.csv", dir);
	FILE *imu_csv = open_or_die(path);
	fprintf(imu_csv, "#timestamp [ns],w_RS_S_x [rad s^-1],w_RS_S_y [rad s^-1],w_RS_S_z [rad s^-1],"
	                 "a_RS_S_x [m s^-2],a_RS_S_y [m s^-2],a_RS_S_z [m s^-2]\n");
	snprintf(path, sizeof(path), "%s/mav0/imu0/temperature.csv", dir);
	FILE *temp_csv = open_or_die(path);
	fprintf(temp_csv, "#timestamp [ns],temperature [C]\n");
	snprintf(path, sizeof(path), "%s/mav0/frames.csv", dir);
	FILE *frames_csv = open_or_die(path);
	fprintf(frames_csv, "#shutter [ns],isp_monotonic [ns],imu_to_monotonic [ns],sync_id,"
	                    "exposure0 [us],exposure1,exposure2,exposure3,gain0 [x16],gain1,gain2,gain3\n");

	struct q1cam_slot *slot = malloc(sizeof(*slot));
	if (slot == NULL) {
		return 1;
	}
	heartbeat(shm);
	uint64_t imu_next = __atomic_load_n(&shm->imu_latest, __ATOMIC_ACQUIRE) + 1;
	uint64_t frame_last = __atomic_load_n(&shm->latest, __ATOMIC_ACQUIRE);
	uint64_t n_imu = 0, imu_lost = 0, n_frames = 0, frames_lost = 0, no_shutter = 0, write_errors = 0;
	int64_t last_temp_ns = 0, last_shutter_ns = 0;
	int64_t start = monotonic_ns(), last_report = start;
	LOG("recording to %s (cameras 0x%x)%s\n", dir, cams, shm->streaming ? "" : ", waiting for the cameras");

	while (!quit_requested && (seconds <= 0 || monotonic_ns() - start < (int64_t)(seconds * 1e9))) {
		heartbeat(shm);

		uint64_t imu_latest = __atomic_load_n(&shm->imu_latest, __ATOMIC_ACQUIRE);
		if (imu_latest >= imu_next + Q1CAM_IMU_RING) {
			uint64_t skip = imu_latest + 1 - Q1CAM_IMU_RING / 2;
			imu_lost += skip - imu_next;
			imu_next = skip;
		}
		for (; imu_next <= imu_latest; imu_next++) {
			struct q1cam_imu_sample e;
			if (!read_imu(shm, imu_next, &e)) {
				imu_lost++;
				continue;
			}
			fprintf(imu_csv, "%" PRId64 ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", e.timestamp_ns, e.gyro[0],
			        e.gyro[1], e.gyro[2], e.accel[0], e.accel[1], e.accel[2]);
			if (e.timestamp_ns - last_temp_ns >= 1000000000) {
				fprintf(temp_csv, "%" PRId64 ",%.2f\n", e.timestamp_ns, e.temperature);
				last_temp_ns = e.timestamp_ns;
			}
			n_imu++;
		}

		uint64_t latest = __atomic_load_n(&shm->latest, __ATOMIC_ACQUIRE);
		if (latest > frame_last) {
			frames_lost += latest - frame_last - 1;
			frame_last = latest;
			if (!read_slot(shm, latest, slot)) {
				frames_lost++;
			} else if (slot->shutter_ns == 0 || slot->shutter_ns <= last_shutter_ns) {
				no_shutter++;
			} else {
				last_shutter_ns = slot->shutter_ns;
				int64_t t = slot->shutter_ns;
				for (int i = 0; i < Q1CAM_N; i++) {
					if (!(cams & (1u << i))) {
						continue;
					}
					snprintf(path, sizeof(path), "%s/mav0/cam%d/data/%" PRId64 ".pgm", dir, i, t);
					if (!write_pgm(path, slot->pixels[i])) {
						write_errors++;
						continue;
					}
					fprintf(cam_csv[i], "%" PRId64 ",%" PRId64 ".pgm\n", t, t);
				}
				fprintf(frames_csv, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u\n", t,
				        slot->timestamp_ns, __atomic_load_n(&shm->imu_to_monotonic_ns, __ATOMIC_RELAXED),
				        slot->sync_id, slot->exposure_us[0], slot->exposure_us[1], slot->exposure_us[2],
				        slot->exposure_us[3], slot->gain_x16[0], slot->gain_x16[1], slot->gain_x16[2],
				        slot->gain_x16[3]);
				n_frames++;
			}
		}

		int64_t now = monotonic_ns();
		if (now - last_report >= 5000000000) {
			LOG("%.0f s: %" PRIu64 " framesets, %" PRIu64 " IMU samples; lost %" PRIu64 " framesets, %" PRIu64
			    " IMU, %" PRIu64 " without shutter, %" PRIu64 " write errors\n",
			    (now - start) / 1e9, n_frames, n_imu, frames_lost, imu_lost, no_shutter, write_errors);
			last_report = now;
		}
		usleep(2000);
	}

	for (int i = 0; i < Q1CAM_N; i++) {
		if (cam_csv[i] != NULL) {
			fclose(cam_csv[i]);
		}
	}
	fclose(imu_csv);
	fclose(temp_csv);
	fclose(frames_csv);
	LOG("done: %" PRIu64 " framesets, %" PRIu64 " IMU samples; lost %" PRIu64 " framesets, %" PRIu64
	    " IMU, %" PRIu64 " without shutter, %" PRIu64 " write errors\n",
	    n_frames, n_imu, frames_lost, imu_lost, no_shutter, write_errors);
	return write_errors != 0;
}
