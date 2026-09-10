/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief Measurement mode shorthands for the unified power test firmware.
 *
 * The Wi-Fi Rx and Wi-Fi Tx current measurements each need a fixed sequence of
 * radio test commands. The "rx" and "tx" shorthands run those sequences, and
 * echo each command as it runs so that the shell log still shows exactly what
 * was configured. The third mode, System OFF, is the "off" command implemented
 * in system_off.c.
 *
 * The sequences are executed by a worker thread rather than directly from the
 * command handler, because shell_execute_cmd() must not be called from command
 * context: it reuses the shell command buffer and takes the shell transport
 * lock, both of which are busy while a command handler runs.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_SERIES_NRF71)
/* FICR->PROTEST.CP.TIMESTAMP1, not exposed by the FICR devicetree node. */
#define FICR_PROTEST_CP_TIMESTAMP1_ADDR 0x00FFC3CCUL
#endif

/* The worker runs at the shell thread priority, and the shell thread only
 * yields once it has finished the command and is waiting for input again. The
 * radio test command handlers run on this stack, so size it like the shell's.
 */
#define MODE_THREAD_STACK_SIZE CONFIG_SHELL_STACK_SIZE
#define MODE_THREAD_PRIORITY   K_LOWEST_APPLICATION_THREAD_PRIO

/* Extra margin on top of the priority argument above, in case the shell is
 * preempted between returning from the handler and releasing its lock.
 */
#define MODE_START_DELAY K_MSEC(20)

/* Wi-Fi Rx at 2.4 GHz, channel 6. */
static const char *const rx_mode_cmds[] = {
	"wifi_radio_test init 0 6",
	"wifi_radio_test rx 1",
};

/* Wi-Fi Tx at 2.4 GHz, channel 6, 15 dBm, modulated frames back-to-back. */
static const char *const tx_mode_cmds[] = {
	"wifi_radio_test init 0 6",
	"wifi_radio_test tx_power 15",
	"wifi_radio_test tx 1",
};

struct measurement_mode {
	const char *name;
	const char *const *cmds;
	size_t cmd_count;
};

static const struct measurement_mode rx_mode = {
	.name = "Wi-Fi Rx",
	.cmds = rx_mode_cmds,
	.cmd_count = ARRAY_SIZE(rx_mode_cmds),
};

static const struct measurement_mode tx_mode = {
	.name = "Wi-Fi Tx",
	.cmds = tx_mode_cmds,
	.cmd_count = ARRAY_SIZE(tx_mode_cmds),
};

static const struct measurement_mode *requested_mode;
static const struct shell *requesting_shell;
static atomic_t mode_busy;
static K_SEM_DEFINE(mode_request, 0, 1);

static void measurement_mode_run(void *unused1, void *unused2, void *unused3)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	while (true) {
		const struct measurement_mode *mode;
		const struct shell *sh;
		int err = 0;

		k_sem_take(&mode_request, K_FOREVER);

		mode = requested_mode;
		sh = requesting_shell;

		k_sleep(MODE_START_DELAY);

		for (size_t i = 0; i < mode->cmd_count; i++) {
			shell_print(sh, "> %s", mode->cmds[i]);

			err = shell_execute_cmd(sh, mode->cmds[i]);
			if (err) {
				shell_error(sh, "%s mode aborted: \"%s\" failed (%d)",
					    mode->name, mode->cmds[i], err);
				break;
			}
		}

		if (!err) {
			shell_print(sh, "%s mode running. Capture the current now.",
				    mode->name);
		}

		atomic_clear(&mode_busy);
	}
}

K_THREAD_DEFINE(measurement_mode_thread, MODE_THREAD_STACK_SIZE,
		measurement_mode_run, NULL, NULL, NULL,
		MODE_THREAD_PRIORITY, 0, 0);

static int measurement_mode_request(const struct shell *sh,
				    const struct measurement_mode *mode)
{
	if (atomic_set(&mode_busy, 1)) {
		shell_error(sh, "A measurement mode is still starting up, try again");
		return -EBUSY;
	}

	requested_mode = mode;
	requesting_shell = sh;

	k_sem_give(&mode_request);

	return 0;
}

static int cmd_rx_mode(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return measurement_mode_request(sh, &rx_mode);
}

static int cmd_tx_mode(const struct shell *sh, size_t argc, char *argv[])
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return measurement_mode_request(sh, &tx_mode);
}

SHELL_CMD_REGISTER(rx, NULL,
		   "Start the Wi-Fi Rx current measurement mode.\n"
		   "Receives on channel 6 in the 2.4 GHz band, by running:\n"
		   "  wifi_radio_test init 0 6\n"
		   "  wifi_radio_test rx 1",
		   cmd_rx_mode);

SHELL_CMD_REGISTER(tx, NULL,
		   "Start the Wi-Fi Tx current measurement mode.\n"
		   "Transmits modulated frames back-to-back on channel 6 in the\n"
		   "2.4 GHz band at 15 dBm, by running:\n"
		   "  wifi_radio_test init 0 6\n"
		   "  wifi_radio_test tx_power 15\n"
		   "  wifi_radio_test tx 1",
		   cmd_tx_mode);

static int measurement_modes_banner(void)
{
#if defined(CONFIG_SOC_SERIES_NRF71)
	printk("FICR->PROTEST.CP.TIMESTAMP1 (0x%08lX) = 0x%08x\n",
	       FICR_PROTEST_CP_TIMESTAMP1_ADDR,
	       *(volatile uint32_t *)FICR_PROTEST_CP_TIMESTAMP1_ADDR);
#endif

	printk("\n"
	       "Unified power test firmware. One command per measurement mode:\n"
	       "  rx    Wi-Fi Rx current, 2.4 GHz channel 6\n"
	       "  tx    Wi-Fi Tx current, 2.4 GHz channel 6, 15 dBm\n"
	       "  off   System OFF current, the lowest power mode\n"
	       "\n"
	       "\"rx\" and \"tx\" echo the wifi_radio_test commands they run, so the\n"
	       "individual commands remain available. \"off\" resets first and enters\n"
	       "System OFF before anything is initialized; recover the device with\n"
	       "\"west flash --recover\". Run \"<command> -h\" for details.\n"
	       "\n");

	return 0;
}

/* Runs after the radio test shell initialization, which is registered at the
 * APPLICATION level with CONFIG_APPLICATION_INIT_PRIORITY.
 */
SYS_INIT(measurement_modes_banner, APPLICATION, 99);
