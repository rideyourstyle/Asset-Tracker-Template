/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Software-based activity/inactivity detection for ADXL367.
 *
 * The ADXL367 hardware interrupt (INT1) does not work reliably on the
 * Thingy:91 X — a known issue (devzone.nordicsemi.com/f/nordic-q-a/126611).
 * We poll the sensor every CONFIG_APP_MOTION_POLL_INTERVAL_MS and compare
 * the current XYZ vector against the last known reference.
 *
 * Activity:   |delta| > ACT_THRESHOLD_MG for ACT_CONSEC_SAMPLES consecutive polls
 * Inactivity: |delta| < INACT_THRESHOLD_MG for INACT_CONSEC_SAMPLES consecutive polls
 *
 * After an INACTIVITY event the reference is frozen at the current position
 * so the next movement is compared against the stationary baseline.
 * After an ACTIVITY event the reference is updated each poll so that
 * sustained movement keeps the device "active" and the counter resets.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/zbus/zbus.h>

#include "app_common.h"
#include "motion.h"

LOG_MODULE_REGISTER(motion, CONFIG_APP_MOTION_LOG_LEVEL);

ZBUS_CHAN_DEFINE(motion_chan,
		 struct motion_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer_lp));

/* Thresholds in milli-g (1 mg = 0.00981 m/s²) */
#define ACT_THRESHOLD_MG        300   /* change > 300 mg = activity */
#define INACT_THRESHOLD_MG      150   /* change < 150 mg = stationary */

/* Number of consecutive polls required to confirm state change.
 * Divide the desired time window by the poll interval (both in ms).
 * MAX(1,...) ensures at least one poll even when interval > window.
 */
#define ACT_CONSEC_SAMPLES   MAX(1, (3000 / CONFIG_APP_MOTION_POLL_INTERVAL_MS))
#define INACT_CONSEC_SAMPLES MAX(1, (CONFIG_APP_MOTION_INACTIVITY_SECONDS \
				     * 1000 / CONFIG_APP_MOTION_POLL_INTERVAL_MS))

/* Sensor_value in µm/s² — convert one component to mg */
static inline int32_t sv_to_mg(const struct sensor_value *sv)
{
	/* val1 m/s², val2 µm/s² fraction → total in µm/s² → mg */
	int64_t umps2 = (int64_t)sv->val1 * 1000000LL + sv->val2;

	return (int32_t)(umps2 / 9810LL);
}

static struct sensor_value ref_x, ref_y, ref_z;
static bool ref_valid;
/* Start active so inactivity detection begins immediately after boot.
 * The device was presumably just handled/installed — treat that as the
 * "active" state and sleep once it has been stationary for long enough.
 */
static bool is_active = true;
static int  consec_count;
/* Counts consecutive medium-band (INACT..ACT) samples. When this reaches
 * RESETTLED_CONSEC_SAMPLES the device has settled at a new orientation and
 * the reference is advanced so inactivity detection can converge. */
static int  resettled_count;
#define RESETTLED_CONSEC_SAMPLES MAX(1, (10000 / CONFIG_APP_MOTION_POLL_INTERVAL_MS))

static void poll_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poll_work, poll_work_fn);

static void publish(bool active)
{
	is_active = active;

	const struct motion_msg msg = {
		.type = active ? MOTION_ACTIVITY : MOTION_INACTIVITY,
	};

	LOG_INF("%s", active ? "Motion detected" : "Inactivity detected");

	int err = zbus_chan_pub(&motion_chan, &msg, PUB_TIMEOUT);

	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void poll_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	struct sensor_value x, y, z;

	if (sensor_sample_fetch(accel) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_X, &x) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Y, &y) != 0 ||
	    sensor_channel_get(accel, SENSOR_CHAN_ACCEL_Z, &z) != 0) {
		LOG_WRN("sensor read failed");
		goto reschedule;
	}

	if (!ref_valid) {
		ref_x = x;
		ref_y = y;
		ref_z = z;
		ref_valid = true;
		goto reschedule;
	}

	int32_t dx = sv_to_mg(&x) - sv_to_mg(&ref_x);
	int32_t dy = sv_to_mg(&y) - sv_to_mg(&ref_y);
	int32_t dz = sv_to_mg(&z) - sv_to_mg(&ref_z);

	/* Use the largest axis delta to avoid sqrt */
	int32_t adx = dx < 0 ? -dx : dx;
	int32_t ady = dy < 0 ? -dy : dy;
	int32_t adz = dz < 0 ? -dz : dz;
	int32_t delta = adx > ady ? (adx > adz ? adx : adz)
				  : (ady > adz ? ady : adz);

	LOG_DBG("delta=%d mg (x=%d y=%d z=%d)", delta, dx, dy, dz);

	if (is_active) {
		/* Looking for inactivity.
		 * Three bands:
		 *   delta > ACT_THRESHOLD_MG:   strong movement — advance reference, reset counter
		 *   INACT_THRESHOLD_MG < delta: medium movement (e.g. car ride) — reset counter only
		 *   delta <= INACT_THRESHOLD_MG: near-stillness — increment inactivity counter
		 */
		if (delta > ACT_THRESHOLD_MG) {
			/* Significant movement — advance reference, reset counters */
			ref_x = x;
			ref_y = y;
			ref_z = z;
			consec_count = 0;
			resettled_count = 0;
		} else if (delta > INACT_THRESHOLD_MG) {
			/* Medium movement — device is not still, reset inactivity counter.
			 * If we stay in this band for RESETTLED_CONSEC_SAMPLES polls the
			 * device has settled at a new orientation; advance the reference so
			 * the inactivity detection can converge rather than being stuck. */
			consec_count = 0;
			resettled_count++;
			if (resettled_count >= RESETTLED_CONSEC_SAMPLES) {
				resettled_count = 0;
				ref_x = x;
				ref_y = y;
				ref_z = z;
				LOG_DBG("re-settle: reference advanced");
			}
		} else {
			resettled_count = 0;
			consec_count++;
			LOG_DBG("inact count %d/%d", consec_count, INACT_CONSEC_SAMPLES);
			if (consec_count >= INACT_CONSEC_SAMPLES) {
				consec_count = 0;
				ref_x = x;
				ref_y = y;
				ref_z = z;
				publish(false);
			}
		}
	} else {
		/* Looking for activity (device is sleeping) */
		if (delta > ACT_THRESHOLD_MG) {
			consec_count++;
			if (consec_count >= ACT_CONSEC_SAMPLES) {
				consec_count = 0;
				ref_x = x;
				ref_y = y;
				ref_z = z;
				publish(true);
			}
		} else {
			consec_count = 0;
		}
	}

reschedule:
	k_work_reschedule(&poll_work, K_MSEC(CONFIG_APP_MOTION_POLL_INTERVAL_MS));
}

static int motion_init(void)
{
	if (!device_is_ready(accel)) {
		LOG_ERR("Accelerometer device not ready");
		SEND_FATAL_ERROR();
		return -ENODEV;
	}

	k_work_reschedule(&poll_work, K_SECONDS(2));

	LOG_INF("Motion module initialized (software polling every %d ms, "
		"inactivity after %d s)",
		CONFIG_APP_MOTION_POLL_INTERVAL_MS,
		CONFIG_APP_MOTION_INACTIVITY_SECONDS);
	return 0;
}

SYS_INIT(motion_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
