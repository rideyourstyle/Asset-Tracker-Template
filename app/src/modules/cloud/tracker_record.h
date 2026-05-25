/*
 * Compact record of all fields needed for one API PUT request.
 * Captured at GNSS sampling time (sensor values snapshotted from cache).
 * sizeof(struct tracker_record) == 48 bytes (battery_soc sits in former padding).
 */

#ifndef TRACKER_RECORD_H_
#define TRACKER_RECORD_H_

#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tracker_record {
	/* Position */
	double   latitude;
	double   longitude;
	float    accuracy_m;

	/* Sensors (0 if not available at sampling time) */
	int32_t  temperature_c;
	int32_t  pressure_pa;
	int32_t  battery_mv;

	/* Timestamp (from GNSS or RTC) */
	uint16_t year;
	uint16_t ms;
	uint8_t  month;
	uint8_t  day;
	uint8_t  hour;
	uint8_t  minute;
	uint8_t  second;
	uint8_t  timestamp_valid;  /* 1 = real clock, 0 = unknown */

	/* Placed last so earlier fields stay at the same offsets as before.
	 * -1 = not available, 0–100 = State of Charge in %. */
	int8_t   battery_soc;
};

/* Published by cloud.c when a new GNSS fix is combined with the sensor cache.
 * Subscribed by the storage module to persist the record to flash.
 */
ZBUS_CHAN_DECLARE(tracker_record_chan);

#ifdef __cplusplus
}
#endif

#endif /* TRACKER_RECORD_H_ */
