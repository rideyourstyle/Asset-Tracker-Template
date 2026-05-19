#
# Copyright (c) 2025
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cloud.c)
target_include_directories(app PRIVATE .)

# Include the FOTA module header directory so cloud.c can include fota.h even
# when CONFIG_APP_FOTA=n (fota module not compiled, but header still needed to
# define fota_chan and publish FOTA_MODULE_READY).
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../fota)
