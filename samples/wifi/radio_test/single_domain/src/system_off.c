/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief Runtime-selectable System OFF mode for the unified power test firmware.
 *
 * The radio test sample and the Zephyr system_off sample normally select their
 * behavior at build time. This file adds a "systemoff" shell command so that a
 * single image can be used for Wi-Fi Rx, Wi-Fi Tx and System OFF current
 * measurements, without rebuilding.
 *
 * Two entry paths are provided:
 *
 * - "off" (or "systemoff"): request System OFF and warm reset. The request is
 *   kept in uninitialized RAM, which survives a software reset, and is acted
 *   upon at the very first initialization step of the next boot. Nothing is
 *   running at that point: no console, no shell, no logging, no system clock
 *   and no Wi-Fi driver. This reproduces the conditions of the
 *   "No Wakeup Configured, No Ram Retention, Sysclock is disabled" build of the
 *   system_off sample.
 *
 * - "off now": tear down the running firmware (radio test, shell,
 *   logging, console, system clock) and enter System OFF in place. Use it when
 *   a warm reset is not wanted; the residual current may be slightly higher
 *   because peripherals brought up by this boot are only shut down by software.
 *
 * No wake-up source is configured and no RAM retention is requested in either
 * path, so the only way out of System OFF is a pin reset or a power cycle.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/toolchain.h>

#if defined(CONFIG_NRF71_RADIO_TEST)
#include <radio_test/main.h>
#include <radio_test/fmac_api.h>
#endif

/* Kept out of the initialized data sections on purpose: the value has to
 * survive the warm reset triggered by the "systemoff" command.
 */
#define SYSTEM_OFF_REQUEST_MAGIC 0x5ff0ff5aUL

static __noinit uint32_t system_off_request;

/* Time given to the console to drain the last messages before it is suspended
 * or the device is reset.
 */
#define CONSOLE_DRAIN_TIME K_MSEC(100)

static void system_off_enter(bool stop_system_clock)
{
	/* z_sys_poweroff() only clears the reset reason on the nRF54L Series,
	 * so do it here to keep the reset cause reported after wake-up clean.
	 */
	(void)hwinfo_clear_reset_cause();

	if (stop_system_clock) {
		/* Stop the system clock, and with it the GRTC, so that nothing
		 * is left running in System OFF.
		 */
		sys_clock_disable();
	}

	sys_poweroff();

	CODE_UNREACHABLE;
}

/* Runs before any other initialization step, including the system clock
 * driver (registered at EARLY, priority 1), the console and the Wi-Fi driver.
 */
static int system_off_early_entry(void)
{
	if (system_off_request != SYSTEM_OFF_REQUEST_MAGIC) {
		/* Not a System OFF request: make sure a random RAM pattern is
		 * not mistaken for one on a later boot.
		 */
		system_off_request = 0;
		return 0;
	}

	/* Clear the request first: waking up from System OFF is a cold boot,
	 * but a pin reset that happens before this point must not enter
	 * System OFF again.
	 */
	system_off_request = 0;

	/* The system clock driver has not run yet, so the GRTC and the LFCLK
	 * were never started and must not be released here.
	 */
	system_off_enter(false);

	return 0;
}

SYS_INIT(system_off_early_entry, EARLY, 0);

static void logging_stop(void)
{
	/* Flush whatever is pending, then take the backends down so that no
	 * late log message can wake the console up again.
	 */
	log_panic();

	for (int i = 0; i < log_backend_count_get(); i++) {
		const struct log_backend *backend = log_backend_get(i);

		if (log_backend_is_active(backend)) {
			log_backend_disable(backend);
		}
	}
}

#if defined(CONFIG_NRF71_RADIO_TEST)
static void radio_test_stop(const struct shell *sh)
{
	struct nrf_wifi_rt_drv_ctx *rt_ctx = &rt_drv_priv.drv_ctx;

	if (rt_ctx->rpu_ctx == NULL) {
		return;
	}

	/* Stop an ongoing transmission or reception, so that the radio is not
	 * left running while the rest of the firmware is shut down.
	 */
	if (rt_ctx->conf_params.tx) {
		rt_ctx->conf_params.tx = 0;

		if (nrf_wifi_rt_fmac_prog_tx(rt_ctx->rpu_ctx, &rt_ctx->conf_params) !=
		    NRF_WIFI_STATUS_SUCCESS) {
			shell_warn(sh, "Failed to stop the ongoing transmission");
		}
	}

	if (rt_ctx->conf_params.rx) {
		rt_ctx->conf_params.rx = 0;

		if (nrf_wifi_rt_fmac_prog_rx(rt_ctx->rpu_ctx, &rt_ctx->conf_params) !=
		    NRF_WIFI_STATUS_SUCCESS) {
			shell_warn(sh, "Failed to stop the ongoing reception");
		}
	}
}
#else
static void radio_test_stop(const struct shell *sh)
{
	ARG_UNUSED(sh);
}
#endif /* defined(CONFIG_NRF71_RADIO_TEST) */

static int system_off_in_place(const struct shell *sh)
{
	const struct device *const cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	radio_test_stop(sh);

	shell_print(sh, "Entering System OFF in place. Reset the board to wake up.");
	k_sleep(CONSOLE_DRAIN_TIME);

	/* Kill the shell: it stops processing input and stops printing, and
	 * its logging backend is disabled.
	 */
	(void)shell_stop(sh);

	logging_stop();

	/* Suspending the console disables the UART peripheral and releases its
	 * pins. There is nobody left to report a failure to, and a failure only
	 * costs current, so the result is ignored.
	 */
	if (device_is_ready(cons)) {
		(void)pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	}

	system_off_enter(true);

	return 0;
}

static int system_off_after_reset(const struct shell *sh)
{
	radio_test_stop(sh);

	shell_print(sh, "Entering System OFF after reset. Reset the board to wake up.");
	k_sleep(CONSOLE_DRAIN_TIME);

	system_off_request = SYSTEM_OFF_REQUEST_MAGIC;

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static int cmd_system_off(const struct shell *sh, size_t argc, char *argv[])
{
	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
			shell_help(sh);
			return SHELL_CMD_HELP_PRINTED;
		}

		if (strcmp(argv[1], "now") != 0) {
			shell_error(sh, "Unknown argument \"%s\"", argv[1]);
			shell_help(sh);
			return -EINVAL;
		}

		return system_off_in_place(sh);
	}

	return system_off_after_reset(sh);
}

/* The command is registered twice: "off" is the shorthand used alongside the
 * "rx" and "tx" measurement modes, "systemoff" is kept as the explicit name.
 */
#define SYSTEM_OFF_CMD_HELP \
	"Start the System OFF current measurement mode, the lowest power mode.\n" \
	"Resets first, then enters System OFF as the very first initialization\n" \
	"step, before the console, shell, logging, system clock and Wi-Fi driver\n" \
	"are brought up. No wake-up source and no RAM retention are configured,\n" \
	"so nothing wakes the device: recover it with \"west flash --recover\".\n" \
	"Usage: off|systemoff [now]\n" \
	"  (no argument)  Reset first, then enter System OFF. Lowest current.\n" \
	"  now            Enter System OFF in place, without resetting: stops\n" \
	"                 Wi-Fi Tx/Rx, the shell, logging and the console. Use it\n" \
	"                 to measure from a state the radio test set up; the\n" \
	"                 current can be slightly higher."

SHELL_CMD_ARG_REGISTER(off, NULL, SYSTEM_OFF_CMD_HELP, cmd_system_off, 1, 1);

SHELL_CMD_ARG_REGISTER(systemoff, NULL, SYSTEM_OFF_CMD_HELP, cmd_system_off, 1, 1);
