#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wifi_ble_radio_test)

target_include_directories(app PRIVATE ${ZEPHYR_NRF_MODULE_DIR}/drivers/wifi/nrf71/inc)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/early_shell_start.c
  src/measurement_modes.c
  src/system_off.c
  src/buttons.c
)
# NORDIC SDK APP END
