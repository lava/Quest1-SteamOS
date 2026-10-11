// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 calibration: /persist/calibration JSON, Fisheye62 to KB4, IMU rectification.
 *
 * The same conversion as tools/quest1-basalt-prep.py, which was validated offline with Basalt.
 *
 * @ingroup drv_quest1
 */

#include "quest1_calib.h"

#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_logging.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CALIB_DIR
#define CALIB_DIR "/persist/calibration"
#endif
#define CALIB_ERROR(ll, ...) U_LOG_IFL_E(ll, "calibration: " __VA_ARGS__)
#define CALIB_INFO(ll, ...) U_LOG_IFL_I(ll, "calibration: " __VA_ARGS__)


/*
 *
 * Fisheye62 -> KB4.
 *
 */

//! Meta's Fisheye62 (as rift_s_util.cpp): pixel of the ray at angle theta from the axis, azimuth phi.
static void
fisheye62_project(const struct quest1_cam_calib *c, double theta, double phi, double out[2])
{
	const double *d = c->fisheye62;
	double t2 = theta * theta;
	double radial = 1 + t2 * (d[0] + t2 * (d[1] + t2 * (d[2] + t2 * (d[3] + t2 * (d[4] + t2 * d[5])))));
	double xp = theta * cos(phi), yp = theta * sin(phi);
	double p1 = d[6], p2 = d[7];
	double dx = 2 * p1 * xp * yp + p2 * (t2 + 2 * xp * xp);
	double dy = 2 * p2 * xp * yp + p1 * (t2 + 2 * yp * yp);
	out[0] = c->f * (xp * radial + dx) + c->cx;
	out[1] = c->f * (yp * radial + dy) + c->cy;
}

//! Solves the 4x4 system a x = b in place (Gaussian elimination, partial pivoting).
static bool
solve4(double a[4][4], double b[4], double x[4])
{
	for (int col = 0; col < 4; col++) {
		int piv = col;
		for (int r = col + 1; r < 4; r++) {
			if (fabs(a[r][col]) > fabs(a[piv][col])) {
				piv = r;
			}
		}
		if (fabs(a[piv][col]) < 1e-12) {
			return false;
		}
		for (int k = 0; k < 4; k++) {
			double t = a[col][k];
			a[col][k] = a[piv][k];
			a[piv][k] = t;
		}
		double t = b[col];
		b[col] = b[piv];
		b[piv] = t;
		for (int r = col + 1; r < 4; r++) {
			double m = a[r][col] / a[col][col];
			for (int k = col; k < 4; k++) {
				a[r][k] -= m * a[col][k];
			}
			b[r] -= m * b[col];
		}
	}
	for (int r = 3; r >= 0; r--) {
		double s = b[r];
		for (int k = r + 1; k < 4; k++) {
			s -= a[r][k] * x[k];
		}
		x[r] = s / a[r][r];
	}
	return true;
}

#define FIT_THETA_STEPS 200
#define FIT_PHI_STEPS 72
#define FIT_THETA_MAX (100.0 * M_PI / 180.0)

/*!
 * KB4 with the same f, cx, cy, fitted to the Fisheye62 pixels of the rays that land in the image
 * (and only while the projection still grows with theta). With f fixed the model is linear in
 * k1..k4: pixel = c + f * theta * (1 + k1 t^2 + k2 t^4 + k3 t^6 + k4 t^8) * (cos phi, sin phi).
 */
static bool
fit_kb4(struct quest1_cam_calib *c)
{
	double ata[4][4] = {{0}}, atb[4] = {0};
	// Residuals need a second pass; keep the samples.
	static double samples[FIT_THETA_STEPS * FIT_PHI_STEPS][4]; // theta, phi, target x, y
	int n = 0;
	for (int j = 0; j < FIT_PHI_STEPS; j++) {
		double phi = -M_PI + 2.0 * M_PI * j / (FIT_PHI_STEPS - 1);
		double last_r = -1.0;
		for (int i = 0; i < FIT_THETA_STEPS; i++) {
			double theta = FIT_THETA_MAX * i / (FIT_THETA_STEPS - 1);
			double px[2];
			fisheye62_project(c, theta, phi, px);
			double r = hypot(px[0] - c->cx, px[1] - c->cy);
			if (!(r > last_r)) {
				break; // not monotonic from here on
			}
			last_r = r;
			if (px[0] < 0 || px[0] > c->width - 1 || px[1] < 0 || px[1] > c->height - 1) {
				continue;
			}
			double *s = samples[n++];
			s[0] = theta;
			s[1] = phi;
			s[2] = px[0];
			s[3] = px[1];
		}
	}
	for (int s = 0; s < n; s++) {
		double theta = samples[s][0], phi = samples[s][1];
		double t2 = theta * theta;
		double pw[4] = {t2, t2 * t2, t2 * t2 * t2, t2 * t2 * t2 * t2};
		double dir[2] = {cos(phi), sin(phi)};
		double base[2] = {c->cx, c->cy};
		for (int axis = 0; axis < 2; axis++) {
			double g = c->f * theta * dir[axis];
			double row[4] = {g * pw[0], g * pw[1], g * pw[2], g * pw[3]};
			double rhs = samples[s][2 + axis] - base[axis] - g;
			for (int a = 0; a < 4; a++) {
				for (int b = 0; b < 4; b++) {
					ata[a][b] += row[a] * row[b];
				}
				atb[a] += row[a] * rhs;
			}
		}
	}
	if (n < 10 || !solve4(ata, atb, c->kb4)) {
		return false;
	}
	double sum2 = 0, worst = 0;
	for (int s = 0; s < n; s++) {
		double theta = samples[s][0], phi = samples[s][1];
		double t2 = theta * theta;
		const double *k = c->kb4;
		double rt = theta * (1 + t2 * (k[0] + t2 * (k[1] + t2 * (k[2] + t2 * k[3]))));
		double ex = c->f * rt * cos(phi) + c->cx - samples[s][2];
		double ey = c->f * rt * sin(phi) + c->cy - samples[s][3];
		double e2 = ex * ex + ey * ey;
		sum2 += e2;
		worst = e2 > worst ? e2 : worst;
	}
	c->kb4_rms_px = sqrt(sum2 / n);
	c->kb4_max_px = sqrt(worst);
	return true;
}


/*
 *
 * JSON.
 *
 */

static bool
parse_offset(const cJSON *json, struct quest1_imu_offset *o)
{
	memset(o, 0, sizeof(*o));
	char model[64] = {0};
	u_json_get_string_into_array(u_json_get(json, "Model"), model, sizeof(model));
	if (strcmp(model, "Constant") == 0) {
		return u_json_get_float_array(u_json_get(json, "ConstantOffset"), o->offset_at_0c, 3) == 3;
	}
	if (strcmp(model, "LinearTemperatureDependence") == 0) {
		return u_json_get_float_array(u_json_get(json, "OffsetAtZeroDegC"), o->offset_at_0c, 3) == 3 &&
		       u_json_get_float_array(u_json_get(json, "OffsetTemperatureCoefficient"), o->temp_coeff, 3) == 3;
	}
	if (strcmp(model, "TemperaturePiecewiseLinear") == 0) {
		char cs[32] = "Raw";
		u_json_get_string_into_array(u_json_get(json, "CoordinateSystem"), cs, sizeof(cs));
		if (strcmp(cs, "Raw") != 0) {
			return false;
		}
		const cJSON *pair;
		cJSON_ArrayForEach(pair, u_json_get(json, "TemperatureOffsetPairs"))
		{
			if (o->pair_count == QUEST1_CALIB_MAX_PAIRS ||
			    u_json_get_float_array(pair, o->pairs[o->pair_count], 4) != 4) {
				return false;
			}
			o->pair_count++;
		}
		// insertion sort by temperature
		for (int i = 1; i < o->pair_count; i++) {
			for (int j = i; j > 0 && o->pairs[j][0] < o->pairs[j - 1][0]; j--) {
				float t[4];
				memcpy(t, o->pairs[j], sizeof(t));
				memcpy(o->pairs[j], o->pairs[j - 1], sizeof(t));
				memcpy(o->pairs[j - 1], t, sizeof(t));
			}
		}
		return o->pair_count > 0;
	}
	return false;
}

static bool
parse_imu(const cJSON *imu, struct quest1_imu_calib *c)
{
	const cJSON *acc = u_json_get(imu, "Accelerometer"), *gyr = u_json_get(imu, "Gyroscope");
	return u_json_get_float_array(u_json_get(imu, "DeviceFromImu"), c->device_from_imu, 16) == 16 &&
	       u_json_get_float_array(u_json_get(acc, "RectificationMatrix"), c->accel_rect, 9) == 9 &&
	       u_json_get_float_array(u_json_get(gyr, "RectificationMatrix"), c->gyro_rect, 9) == 9 &&
	       parse_offset(u_json_get(acc, "Offset"), &c->accel_offset) &&
	       parse_offset(u_json_get(gyr, "Offset"), &c->gyro_offset);
}

static bool
parse_cameras(const cJSON *cams, struct quest1_calib *out, enum u_logging_level ll)
{
	unsigned found = 0;
	const cJSON *cam;
	cJSON_ArrayForEach(cam, cams)
	{
		char id[8] = {0}, proj_model[32] = {0}, dist_model[32] = {0};
		u_json_get_string_into_array(u_json_get(cam, "Id"), id, sizeof(id));
		int i = atoi(id);
		if (i < 0 || i >= QUEST1_CALIB_CAMS) {
			continue;
		}
		struct quest1_cam_calib *c = &out->cam[i];
		const cJSON *proj = u_json_get(cam, "Projection"), *dist = u_json_get(cam, "Distortion");
		u_json_get_string_into_array(u_json_get(proj, "Model"), proj_model, sizeof(proj_model));
		u_json_get_string_into_array(u_json_get(dist, "Model"), dist_model, sizeof(dist_model));
		double p[3];
		int size[2] = {640, 480};
		u_json_get_int_array(u_json_get(cam, "ImageSize"), size, 2);
		if (strcmp(proj_model, "PinholeSymmetric") != 0 || strcmp(dist_model, "Fisheye62") != 0 ||
		    u_json_get_float_array(u_json_get(cam, "DeviceFromCamera"), c->device_from_camera, 16) != 16 ||
		    u_json_get_double_array(u_json_get(proj, "Coefficients"), p, 3) != 3 ||
		    u_json_get_double_array(u_json_get(dist, "Coefficients"), c->fisheye62, 8) != 8) {
			CALIB_ERROR(ll, "camera %d: unexpected layout (%s, %s)", i, proj_model, dist_model);
			return false;
		}
		c->f = p[0];
		c->cx = p[1];
		c->cy = p[2];
		c->width = size[0];
		c->height = size[1];
		if (!fit_kb4(c)) {
			CALIB_ERROR(ll, "camera %d: KB4 fit failed", i);
			return false;
		}
		found |= 1u << i;
	}
	return found == (1u << QUEST1_CALIB_CAMS) - 1;
}

static cJSON *
read_json(const char *path)
{
	char *text = u_file_read_content_from_path(path, NULL);
	if (text == NULL) {
		return NULL;
	}
	cJSON *root = cJSON_Parse(text);
	free(text);
	return root;
}

//! Newest regular file in CALIB_DIR/online, by modification time.
static bool
newest_online(char *path, size_t size)
{
	DIR *d = opendir(CALIB_DIR "/online");
	if (d == NULL) {
		return false;
	}
	time_t best = 0;
	bool found = false;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char p[512];
		struct stat st;
		snprintf(p, sizeof(p), CALIB_DIR "/online/%s", e->d_name);
		if (e->d_name[0] == '.' || stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		if (!found || st.st_mtime > best) {
			best = st.st_mtime;
			snprintf(path, size, "%s", p);
			found = true;
		}
	}
	closedir(d);
	return found;
}

static bool
load_online(struct quest1_calib *out, enum u_logging_level ll)
{
	char path[512];
	if (!newest_online(path, sizeof(path))) {
		return false;
	}
	cJSON *root = read_json(path);
	bool ok = root != NULL && parse_cameras(u_json_get(root, "CameraCalibration"), out, ll) &&
	          parse_imu(u_json_get(root, "ImuCalibration"), &out->imu);
	cJSON_Delete(root);
	if (ok) {
		snprintf(out->source, sizeof(out->source), "%s", path);
	} else {
		CALIB_ERROR(ll, "cannot use %s, trying the factory calibration", path);
	}
	return ok;
}

static bool
load_factory(struct quest1_calib *out, enum u_logging_level ll)
{
	cJSON *cams = read_json(CALIB_DIR "/camera_calibration_v2.json");
	cJSON *imu = read_json(CALIB_DIR "/imu_calibration.json");
	bool ok = cams != NULL && imu != NULL && parse_cameras(u_json_get(cams, "CameraCalibration"), out, ll) &&
	          parse_imu(u_json_get(imu, "ImuCalibration"), &out->imu);
	cJSON_Delete(cams);
	cJSON_Delete(imu);
	if (ok) {
		snprintf(out->source, sizeof(out->source), "factory");
	}
	return ok;
}

bool
quest1_calib_load(struct quest1_calib *out, enum u_logging_level ll)
{
	memset(out, 0, sizeof(*out));
	if (!load_online(out, ll)) {
		memset(out, 0, sizeof(*out));
		if (!load_factory(out, ll)) {
			CALIB_ERROR(ll, "no usable calibration in " CALIB_DIR);
			return false;
		}
	}
	CALIB_INFO(ll, "using %s", out->source);
	for (int i = 0; i < QUEST1_CALIB_CAMS; i++) {
		const struct quest1_cam_calib *c = &out->cam[i];
		CALIB_INFO(ll, "camera %d: f %.2f c (%.2f, %.2f), KB4 [%.6f %.6f %.6f %.6f], fit rms %.3f px, max %.3f px", i,
		           c->f, c->cx, c->cy, c->kb4[0], c->kb4[1], c->kb4[2], c->kb4[3], c->kb4_rms_px, c->kb4_max_px);
	}
	return true;
}


/*
 *
 * IMU.
 *
 */

static void
offset_at(const struct quest1_imu_offset *o, float t, float out[3])
{
	if (o->pair_count == 0) {
		for (int i = 0; i < 3; i++) {
			out[i] = o->offset_at_0c[i] + t * o->temp_coeff[i];
		}
		return;
	}
	// Linear interpolation, clamped at the ends (as numpy.interp).
	const float(*p)[4] = o->pairs;
	int n = o->pair_count;
	if (t <= p[0][0] || n == 1) {
		memcpy(out, &p[0][1], 3 * sizeof(float));
		return;
	}
	if (t >= p[n - 1][0]) {
		memcpy(out, &p[n - 1][1], 3 * sizeof(float));
		return;
	}
	int k = 1;
	while (p[k][0] < t) {
		k++;
	}
	float span = p[k][0] - p[k - 1][0];
	float a = span > 0 ? (t - p[k - 1][0]) / span : 0.0f;
	for (int i = 0; i < 3; i++) {
		out[i] = p[k - 1][1 + i] + a * (p[k][1 + i] - p[k - 1][1 + i]);
	}
}

static struct xrt_vec3
rectify(const float r[9], const struct quest1_imu_offset *o, float t, const float raw[3])
{
	float off[3], v[3];
	offset_at(o, t, off);
	for (int i = 0; i < 3; i++) {
		v[i] = raw[i] - off[i];
	}
	return (struct xrt_vec3){
	    r[0] * v[0] + r[1] * v[1] + r[2] * v[2],
	    r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
	    r[6] * v[0] + r[7] * v[1] + r[8] * v[2],
	};
}

void
quest1_calib_rectify_imu(const struct quest1_imu_calib *c,
                         float temperature,
                         const float raw_accel[3],
                         const float raw_gyro[3],
                         struct xrt_vec3 *out_accel,
                         struct xrt_vec3 *out_gyro)
{
	*out_accel = rectify(c->accel_rect, &c->accel_offset, temperature, raw_accel);
	*out_gyro = rectify(c->gyro_rect, &c->gyro_offset, temperature, raw_gyro);
}
