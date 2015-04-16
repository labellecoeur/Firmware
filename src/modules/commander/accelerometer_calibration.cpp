/****************************************************************************
 *
 *   Copyright (c) 2013-2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file accelerometer_calibration.cpp
 *
 * Implementation of accelerometer calibration.
 *
 * Transform acceleration vector to true orientation, scale and offset
 *
 * ===== Model =====
 * accel_corr = accel_T * (accel_raw - accel_offs)
 *
 * accel_corr[3] - fully corrected acceleration vector in body frame
 * accel_T[3][3] - accelerometers transform matrix, rotation and scaling transform
 * accel_raw[3]  - raw acceleration vector
 * accel_offs[3] - acceleration offset vector
 *
 * ===== Calibration =====
 *
 * Reference vectors
 * accel_corr_ref[6][3] = [  g  0  0 ]     // nose up
 *                        | -g  0  0 |     // nose down
 *                        |  0  g  0 |     // left side down
 *                        |  0 -g  0 |     // right side down
 *                        |  0  0  g |     // on back
 *                        [  0  0 -g ]     // level
 * accel_raw_ref[6][3]
 *
 * accel_corr_ref[i] = accel_T * (accel_raw_ref[i] - accel_offs), i = 0...5
 *
 * 6 reference vectors * 3 axes = 18 equations
 * 9 (accel_T) + 3 (accel_offs) = 12 unknown constants
 *
 * Find accel_offs
 *
 * accel_offs[i] = (accel_raw_ref[i*2][i] + accel_raw_ref[i*2+1][i]) / 2
 *
 * Find accel_T
 *
 * 9 unknown constants
 * need 9 equations -> use 3 of 6 measurements -> 3 * 3 = 9 equations
 *
 * accel_corr_ref[i*2] = accel_T * (accel_raw_ref[i*2] - accel_offs), i = 0...2
 *
 * Solve separate system for each row of accel_T:
 *
 * accel_corr_ref[j*2][i] = accel_T[i] * (accel_raw_ref[j*2] - accel_offs), j = 0...2
 *
 * A * x = b
 *
 * x = [ accel_T[0][i] ]
 *     | accel_T[1][i] |
 *     [ accel_T[2][i] ]
 *
 * b = [ accel_corr_ref[0][i] ]	// One measurement per side is enough
 *     | accel_corr_ref[2][i] |
 *     [ accel_corr_ref[4][i] ]
 *
 * a[i][j] = accel_raw_ref[i][j] - accel_offs[j], i = 0;2;4, j = 0...2
 *
 * Matrix A is common for all three systems:
 * A = [ a[0][0]  a[0][1]  a[0][2] ]
 *     | a[2][0]  a[2][1]  a[2][2] |
 *     [ a[4][0]  a[4][1]  a[4][2] ]
 *
 * x = A^-1 * b
 *
 * accel_T = A^-1 * g
 * g = 9.80665
 *
 * ===== Rotation =====
 *
 * Calibrating using model:
 * accel_corr = accel_T_r * (rot * accel_raw - accel_offs_r)
 *
 * Actual correction:
 * accel_corr = rot * accel_T * (accel_raw - accel_offs)
 *
 * Known: accel_T_r, accel_offs_r, rot
 * Unknown: accel_T, accel_offs
 *
 * Solution:
 * accel_T_r * (rot * accel_raw - accel_offs_r) = rot * accel_T * (accel_raw - accel_offs)
 * rot^-1 * accel_T_r * (rot * accel_raw - accel_offs_r) = accel_T * (accel_raw - accel_offs)
 * rot^-1 * accel_T_r * rot * accel_raw - rot^-1 * accel_T_r * accel_offs_r = accel_T * accel_raw - accel_T * accel_offs)
 * => accel_T = rot^-1 * accel_T_r * rot
 * => accel_offs = rot^-1 * accel_offs_r
 *
 * @author Anton Babushkin <anton.babushkin@me.com>
 */

// FIXME: Can some of these headers move out with detect_ move?

#include "accelerometer_calibration.h"
#include "calibration_messages.h"
#include "calibration_routines.h"
#include "commander_helper.h"

#include <px4_posix.h>
#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <math.h>
#include <float.h>
#include <mathlib/mathlib.h>
#include <string.h>
#include <drivers/drv_hrt.h>
#include <drivers/drv_accel.h>
#include <geo/geo.h>
#include <conversion/rotation.h>
#include <systemlib/param/param.h>
#include <systemlib/err.h>
#include <mavlink/mavlink_log.h>

/* oddly, ERROR is not defined for c++ */
#ifdef ERROR
# undef ERROR
#endif
static const int ERROR = -1;

static const char *sensor_name = "accel";

int do_accel_calibration_measurements(int mavlink_fd, float (&accel_offs)[max_accel_sens][3], float (&accel_T)[max_accel_sens][3][3], unsigned *active_sensors);
int read_accelerometer_avg(int (&subs)[max_accel_sens], float (&accel_avg)[max_accel_sens][detect_orientation_side_count][3], unsigned orient, unsigned samples_num);
int mat_invert3(float src[3][3], float dst[3][3]);
int calculate_calibration_values(unsigned sensor, float (&accel_ref)[max_accel_sens][detect_orientation_side_count][3], float (&accel_T)[max_accel_sens][3][3], float (&accel_offs)[max_accel_sens][3], float g);
int accel_calibration_worker(detect_orientation_return orientation, void* worker_data);

/// Data passed to calibration worker routine
typedef struct  {
	int		mavlink_fd;
	unsigned	done_count;
	int		subs[max_accel_sens];
	float		accel_ref[max_accel_sens][detect_orientation_side_count][3];
} accel_worker_data_t;

int do_accel_calibration(int mavlink_fd)
{
	int fd;
	int32_t device_id[max_accel_sens];

	mavlink_and_console_log_info(mavlink_fd, CAL_STARTED_MSG, sensor_name);

	struct accel_scale accel_scale = {
		0.0f,
		1.0f,
		0.0f,
		1.0f,
		0.0f,
		1.0f,
	};

	int res = OK;

	char str[30];

	/* reset all sensors */
	for (unsigned s = 0; s < max_accel_sens; s++) {
		sprintf(str, "%s%u", ACCEL_BASE_DEVICE_PATH, s);
		/* reset all offsets to zero and all scales to one */
		fd = px4_open(str, 0);

		if (fd < 0) {
			continue;
		}

		device_id[s] = px4_ioctl(fd, DEVIOCGDEVICEID, 0);

		res = px4_ioctl(fd, ACCELIOCSSCALE, (long unsigned int)&accel_scale);
		px4_close(fd);

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_RESET_CAL_MSG, s);
		}
	}

	float accel_offs[max_accel_sens][3];
	float accel_T[max_accel_sens][3][3];
	unsigned active_sensors;

	if (res == OK) {
		/* measure and calculate offsets & scales */
		res = do_accel_calibration_measurements(mavlink_fd, accel_offs, accel_T, &active_sensors);
	}

	if (res != OK || active_sensors == 0) {
		mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SENSOR_MSG);
		return ERROR;
	}

	/* measurements completed successfully, rotate calibration values */
	param_t board_rotation_h = param_find("SENS_BOARD_ROT");
	int32_t board_rotation_int;
	param_get(board_rotation_h, &(board_rotation_int));
	enum Rotation board_rotation_id = (enum Rotation)board_rotation_int;
	math::Matrix<3, 3> board_rotation;
	get_rot_matrix(board_rotation_id, &board_rotation);
	math::Matrix<3, 3> board_rotation_t = board_rotation.transposed();

	for (unsigned i = 0; i < active_sensors; i++) {

		/* handle individual sensors, one by one */
		math::Vector<3> accel_offs_vec(accel_offs[i]);
		math::Vector<3> accel_offs_rotated = board_rotation_t * accel_offs_vec;
		math::Matrix<3, 3> accel_T_mat(accel_T[i]);
		math::Matrix<3, 3> accel_T_rotated = board_rotation_t * accel_T_mat * board_rotation;

		accel_scale.x_offset = accel_offs_rotated(0);
		accel_scale.x_scale = accel_T_rotated(0, 0);
		accel_scale.y_offset = accel_offs_rotated(1);
		accel_scale.y_scale = accel_T_rotated(1, 1);
		accel_scale.z_offset = accel_offs_rotated(2);
		accel_scale.z_scale = accel_T_rotated(2, 2);
		
		bool failed = false;

		/* set parameters */
		(void)sprintf(str, "CAL_ACC%u_XOFF", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.x_offset)));
		(void)sprintf(str, "CAL_ACC%u_YOFF", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.y_offset)));
		(void)sprintf(str, "CAL_ACC%u_ZOFF", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.z_offset)));
		(void)sprintf(str, "CAL_ACC%u_XSCALE", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.x_scale)));
		(void)sprintf(str, "CAL_ACC%u_YSCALE", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.y_scale)));
		(void)sprintf(str, "CAL_ACC%u_ZSCALE", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(accel_scale.z_scale)));
		(void)sprintf(str, "CAL_ACC%u_ID", i);
		failed |= (OK != param_set_no_notification(param_find(str), &(device_id[i])));
		
		if (failed) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SET_PARAMS_MSG, i);
			return ERROR;
		}

		sprintf(str, "%s%u", ACCEL_BASE_DEVICE_PATH, i);
		fd = px4_open(str, 0);

		if (fd < 0) {
			mavlink_and_console_log_critical(mavlink_fd, "sensor does not exist");
			res = ERROR;
		} else {
			res = px4_ioctl(fd, ACCELIOCSSCALE, (long unsigned int)&accel_scale);
			px4_close(fd);
		}

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_APPLY_CAL_MSG, i);
		}
	}

	if (res == OK) {
		/* auto-save to EEPROM */
		res = param_save_default();

		if (res != OK) {
			mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_SAVE_PARAMS_MSG);
		}

		mavlink_and_console_log_info(mavlink_fd, CAL_DONE_MSG, sensor_name);

	} else {
		mavlink_and_console_log_critical(mavlink_fd, CAL_FAILED_MSG, sensor_name);
	}

	return res;
}

int accel_calibration_worker(detect_orientation_return orientation, void* data)
{
	const unsigned samples_num = 3000;
	accel_worker_data_t* worker_data = (accel_worker_data_t*)(data);
	
	mavlink_and_console_log_info(worker_data->mavlink_fd, "Hold still, starting to measure %s side", detect_orientation_str(orientation));
	
	read_accelerometer_avg(worker_data->subs, worker_data->accel_ref, orientation, samples_num);
	
	mavlink_and_console_log_info(worker_data->mavlink_fd, "%s side result: [ %8.4f %8.4f %8.4f ]", detect_orientation_str(orientation),
				     (double)worker_data->accel_ref[0][orientation][0],
				     (double)worker_data->accel_ref[0][orientation][1],
				     (double)worker_data->accel_ref[0][orientation][2]);
	
	worker_data->done_count++;
	mavlink_and_console_log_info(worker_data->mavlink_fd, CAL_PROGRESS_MSG, sensor_name, 17 * worker_data->done_count);
	
	return OK;
}

int do_accel_calibration_measurements(int mavlink_fd, float (&accel_offs)[max_accel_sens][3], float (&accel_T)[max_accel_sens][3][3], unsigned *active_sensors)
{
	int result = OK;
	
	*active_sensors = 0;
	
	accel_worker_data_t worker_data;
	
	worker_data.mavlink_fd = mavlink_fd;
	worker_data.done_count = 0;

	bool data_collected[detect_orientation_side_count] = { false, false, false, false, false, false };

	// Initialize subs to error condition so we know which ones are open and which are not
	for (size_t i=0; i<max_accel_sens; i++) {
		worker_data.subs[i] = -1;
	}

	uint64_t timestamps[max_accel_sens];

	for (unsigned i = 0; i < max_accel_sens; i++) {
		worker_data.subs[i] = orb_subscribe_multi(ORB_ID(sensor_accel), i);
		if (worker_data.subs[i] < 0) {
			result = ERROR;
			break;
		}
		
		/* store initial timestamp - used to infer which sensors are active */
		struct accel_report arp = {};
		(void)orb_copy(ORB_ID(sensor_accel), worker_data.subs[i], &arp);
		timestamps[i] = arp.timestamp;
	}

	if (result == OK) {
		result = calibrate_from_orientation(mavlink_fd, data_collected, accel_calibration_worker, &worker_data);
	}

	/* close all subscriptions */
	for (unsigned i = 0; i < max_accel_sens; i++) {
		if (worker_data.subs[i] >= 0) {
			/* figure out which sensors were active */
			struct accel_report arp = {};
			(void)orb_copy(ORB_ID(sensor_accel), worker_data.subs[i], &arp);
			if (arp.timestamp != 0 && timestamps[i] != arp.timestamp) {
				(*active_sensors)++;
			}
			px4_close(worker_data.subs[i]);
		}
	}

	if (result == OK) {
		/* calculate offsets and transform matrix */
		for (unsigned i = 0; i < (*active_sensors); i++) {
			result = calculate_calibration_values(i, worker_data.accel_ref, accel_T, accel_offs, CONSTANTS_ONE_G);

			if (result != OK) {
				mavlink_and_console_log_critical(mavlink_fd, "ERROR: calibration values calculation error");
				break;
			}
		}
	}

	return result;
}

/*
 * Read specified number of accelerometer samples, calculate average and dispersion.
 */
int read_accelerometer_avg(int (&subs)[max_accel_sens], float (&accel_avg)[max_accel_sens][detect_orientation_side_count][3], unsigned orient, unsigned samples_num)
{
	px4_pollfd_struct_t fds[max_accel_sens];

	for (unsigned i = 0; i < max_accel_sens; i++) {
		fds[i].fd = subs[i];
		fds[i].events = POLLIN;
	}

	unsigned counts[max_accel_sens] = { 0 };
	float accel_sum[max_accel_sens][3];
	memset(accel_sum, 0, sizeof(accel_sum));

	unsigned errcount = 0;

	/* use the first sensor to pace the readout, but do per-sensor counts */
	while (counts[0] < samples_num) {
		int poll_ret = px4_poll(&fds[0], max_accel_sens, 1000);

		if (poll_ret > 0) {

			for (unsigned s = 0; s < max_accel_sens; s++) {
				bool changed;
				orb_check(subs[s], &changed);

				if (changed) {

					struct accel_report arp;
					orb_copy(ORB_ID(sensor_accel), subs[s], &arp);

					accel_sum[s][0] += arp.x;
					accel_sum[s][1] += arp.y;
					accel_sum[s][2] += arp.z;

					counts[s]++;
				}
			}

		} else {
			errcount++;
			continue;
		}

		if (errcount > samples_num / 10) {
			return ERROR;
		}
	}

	for (unsigned s = 0; s < max_accel_sens; s++) {
		for (unsigned i = 0; i < 3; i++) {
			accel_avg[s][orient][i] = accel_sum[s][i] / counts[s];
		}
	}

	return OK;
}

int mat_invert3(float src[3][3], float dst[3][3])
{
	float det = src[0][0] * (src[1][1] * src[2][2] - src[1][2] * src[2][1]) -
		    src[0][1] * (src[1][0] * src[2][2] - src[1][2] * src[2][0]) +
		    src[0][2] * (src[1][0] * src[2][1] - src[1][1] * src[2][0]);

	if (fabsf(det) < FLT_EPSILON) {
		return ERROR;        // Singular matrix
	}

	dst[0][0] = (src[1][1] * src[2][2] - src[1][2] * src[2][1]) / det;
	dst[1][0] = (src[1][2] * src[2][0] - src[1][0] * src[2][2]) / det;
	dst[2][0] = (src[1][0] * src[2][1] - src[1][1] * src[2][0]) / det;
	dst[0][1] = (src[0][2] * src[2][1] - src[0][1] * src[2][2]) / det;
	dst[1][1] = (src[0][0] * src[2][2] - src[0][2] * src[2][0]) / det;
	dst[2][1] = (src[0][1] * src[2][0] - src[0][0] * src[2][1]) / det;
	dst[0][2] = (src[0][1] * src[1][2] - src[0][2] * src[1][1]) / det;
	dst[1][2] = (src[0][2] * src[1][0] - src[0][0] * src[1][2]) / det;
	dst[2][2] = (src[0][0] * src[1][1] - src[0][1] * src[1][0]) / det;

	return OK;
}

int calculate_calibration_values(unsigned sensor, float (&accel_ref)[max_accel_sens][detect_orientation_side_count][3], float (&accel_T)[max_accel_sens][3][3], float (&accel_offs)[max_accel_sens][3], float g)
{
	/* calculate offsets */
	for (unsigned i = 0; i < 3; i++) {
		accel_offs[sensor][i] = (accel_ref[sensor][i * 2][i] + accel_ref[sensor][i * 2 + 1][i]) / 2;
	}

	/* fill matrix A for linear equations system*/
	float mat_A[3][3];
	memset(mat_A, 0, sizeof(mat_A));

	for (unsigned i = 0; i < 3; i++) {
		for (unsigned j = 0; j < 3; j++) {
			float a = accel_ref[sensor][i * 2][j] - accel_offs[sensor][j];
			mat_A[i][j] = a;
		}
	}

	/* calculate inverse matrix for A */
	float mat_A_inv[3][3];

	if (mat_invert3(mat_A, mat_A_inv) != OK) {
		return ERROR;
	}

	/* copy results to accel_T */
	for (unsigned i = 0; i < 3; i++) {
		for (unsigned j = 0; j < 3; j++) {
			/* simplify matrices mult because b has only one non-zero element == g at index i */
			accel_T[sensor][j][i] = mat_A_inv[j][i] * g;
		}
	}

	return OK;
}
