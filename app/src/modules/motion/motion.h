/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MOTION_H_
#define _MOTION_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Channels provided by this module */
ZBUS_CHAN_DECLARE(
	motion_chan
);

enum motion_msg_type {
	/* The device has started moving.
	 * Published when the ADXL367 activity threshold is exceeded.
	 */
	MOTION_ACTIVITY = 0x1,

	/* The device has been stationary.
	 * Published when all axes stay below the inactivity threshold for the configured time.
	 */
	MOTION_INACTIVITY,
};

struct motion_msg {
	enum motion_msg_type type;
};

#ifdef __cplusplus
}
#endif

#endif /* _MOTION_H_ */
