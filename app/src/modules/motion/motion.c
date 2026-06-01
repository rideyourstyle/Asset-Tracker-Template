/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Snapshot-based motion detection for ADXL367.
 *
 * Called once per sampling cycle from main.c at LOCATION_SEARCH_DONE.
 * Compares consecutive ACC snapshots — a stationary device reads near-zero
 * delta regardless of mounting angle, eliminating the reference-drift problem
 * that affected continuous polling.
 *
 * The ADXL367 hardware interrupt (INT1) is not used — known issue on Thingy:91 X:
 * devzone.nordicsemi.com/f/nordic-q-a/126611
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

#include "app_common.h"
#include "motion.h"

LOG_MODULE_REGISTER(motion, CONFIG_APP_MOTION_LOG_LEVEL);

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer_lp));

/* Consecutive-snapshot delta below which the device is considered stationary.
 * 150 mg covers sensor noise and minor vibrations; a moving vehicle produces
 * deltas well above this value between 30-second snapshots. */
#define INACT_THRESHOLD_MG 150

static inline int32_t sv_to_mg(const struct sensor_value *sv)
{
	int64_t umps2 = (int64_t)sv->val1 * 1000000LL + sv->val2;

	return (int32_t)(umps2 / 9810LL);
}

static struct sensor_value ref_x, ref_y, ref_z;
static bool ref_valid;

bool motion_snapshot_check(void)
{
	struct sensor_value x, y, z;

	if (!device_is_ready(accel)) {
		return true; /* assume moving on error — fail-safe: don't sleep */
	}

	if (sensor_sample_fetch(accel) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_X, &x) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Y, &y) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Z, &z) != 0) {
		LOG_WRN("ACC read failed");
		return true;
	}

	if (!ref_valid) {
		ref_x = x;
		ref_y = y;
		ref_z = z;
		ref_valid = true;
		return true; /* no baseline yet — report moving */
	}

	int32_t dx = sv_to_mg(&x) - sv_to_mg(&ref_x);
	int32_t dy = sv_to_mg(&y) - sv_to_mg(&ref_y);
	int32_t dz = sv_to_mg(&z) - sv_to_mg(&ref_z);
	int32_t adx = dx < 0 ? -dx : dx;
	int32_t ady = dy < 0 ? -dy : dy;
	int32_t adz = dz < 0 ? -dz : dz;
	int32_t delta = adx > ady ? (adx > adz ? adx : adz)
				  : (ady > adz ? ady : adz);

	/* Always advance the reference so the next call compares the next window */
	ref_x = x;
	ref_y = y;
	ref_z = z;

	LOG_INF("ACC snapshot: delta=%d mg (%s)", delta,
		delta <= INACT_THRESHOLD_MG ? "stationary" : "moving");

	return delta > INACT_THRESHOLD_MG;
}

static int motion_init(void)
{
	if (!device_is_ready(accel)) {
		LOG_ERR("Accelerometer device not ready");
		SEND_FATAL_ERROR();
		return -ENODEV;
	}

	LOG_INF("Motion module initialized (snapshot mode, threshold=%d mg)",
		INACT_THRESHOLD_MG);
	return 0;
}

SYS_INIT(motion_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
