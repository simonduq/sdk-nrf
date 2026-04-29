#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(BOARD MATCHES "^nrf7002dk")
  set(SB_CONF_FILE "${APP_DIR}/sysbuild_nrf7002dk_nrf5340_cpuapp_ns.conf")
endif()

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)

