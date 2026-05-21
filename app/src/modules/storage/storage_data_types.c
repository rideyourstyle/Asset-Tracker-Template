/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "storage.h"
#include "storage_data_types.h"

#ifdef CONFIG_APP_CLOUD
#include "tracker_record.h"
#endif

DATA_SOURCE_LIST(STORAGE_DATA_TYPE_ADD)

#ifdef CONFIG_APP_CLOUD

bool tracker_check(const struct tracker_record *msg)
{
	ARG_UNUSED(msg);
	return true;
}

void tracker_extract(const struct tracker_record *msg, struct tracker_record *data)
{
	*data = *msg;
}

#endif /* CONFIG_APP_CLOUD */
