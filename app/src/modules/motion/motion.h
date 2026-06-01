/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef _MOTION_H_
#define _MOTION_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Take one ACC snapshot and compare it to the previous sample.
 *
 * Returns true  if the device appears to be moving (delta > threshold).
 * Returns false if the device appears to be stationary.
 *
 * Call once at the end of each sampling cycle (LOCATION_SEARCH_DONE).
 * The internal reference is always advanced after each call, so consecutive
 * calls compare adjacent sampling windows.
 *
 * A parked-but-tilted device is correctly detected as stationary because
 * two consecutive snapshots at the same orientation are identical.
 */
bool motion_snapshot_check(void);

#ifdef __cplusplus
}
#endif

#endif /* _MOTION_H_ */
