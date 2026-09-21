/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief Front-panel button control for the unified power test firmware.
 *
 * Replaces the earlier persisted-marker auto-cycling of measurement modes
 * across reboots: sw0..sw3 now select a mode directly, at any time, without
 * a reset. The buttons themselves are described in devicetree ("gpio-keys",
 * see nrf7120dk_nrf7120-common.dtsi), pull-up and active-low included.
 *
 * Mapping:
 *   sw0  System OFF, on release (equivalent to the "systemoff" shell command)
 *   sw1  Wi-Fi Rx, on press    ("rx")
 *   sw2  Wi-Fi Tx, on press    ("tx")
 *
 * sw0 triggers on release rather than press so that holding it down (e.g.
 * while inspecting the board) does not immediately power the device off.
 *
 * NOTE: on this engineering sample, GPIO port P4 (where sw0..sw3 and
 * led0..led3 live) has no GPIOTE instance wired up in the SoC devicetree
 * (see the gpio4 node in nrf7120_enga.dtsi, which is missing the
 * "gpiote-instance" property that gpio1/gpio3 have), so pin interrupts are
 * not available on this port. Buttons are therefore handled by polling the
 * raw pin state from a dedicated thread instead of via
 * gpio_add_callback()/ISR.
 *
 * This file also implements CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_* (see the
 * sample's Kconfig): a build-time choice that starts one of the three modes
 * automatically at boot, equivalent to pressing sw0..sw2 right after boot,
 * for setups where a shell connection or physical button access is not
 * convenient (e.g. automated current measurement).
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/printk.h>

enum button_action {
	BUTTON_ACTION_SYSTEM_OFF,
	BUTTON_ACTION_RX,
	BUTTON_ACTION_TX,
};

/* Edge to trigger the action on, in terms of the button's logical state
 * as returned by gpio_pin_get_dt() (1 = pressed, 0 = released).
 */
enum button_edge {
	BUTTON_EDGE_PRESS,
	BUTTON_EDGE_RELEASE,
};

struct button {
	struct gpio_dt_spec spec;
	enum button_action action;
	enum button_edge edge;
	const char *name;
	int last_state;
};

static struct button buttons[] = {
	{
		.spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios),
		.action = BUTTON_ACTION_SYSTEM_OFF,
		.edge = BUTTON_EDGE_RELEASE,
		.name = "sw0 (System OFF)",
		.last_state = INT32_MIN,
	},
	{
		.spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios),
		.action = BUTTON_ACTION_RX,
		.edge = BUTTON_EDGE_PRESS,
		.name = "sw1 (Wi-Fi Rx)",
		.last_state = INT32_MIN,
	},
	{
		.spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios),
		.action = BUTTON_ACTION_TX,
		.edge = BUTTON_EDGE_PRESS,
		.name = "sw2 (Wi-Fi Tx)",
		.last_state = INT32_MIN,
	},
};

static int action_run(enum button_action action)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	switch (action) {
	case BUTTON_ACTION_SYSTEM_OFF:
		return shell_execute_cmd(sh, "systemoff");
	case BUTTON_ACTION_RX:
		return shell_execute_cmd(sh, "rx");
	case BUTTON_ACTION_TX:
		return shell_execute_cmd(sh, "tx");
	default:
		return -EINVAL;
	}
}

static void button_action_run(struct button *button)
{
	int ret;

	printk("%s %s\n", button->name,
	       button->edge == BUTTON_EDGE_RELEASE ? "released" : "pressed");

	ret = action_run(button->action);
	if (ret != 0) {
		printk("%s: command failed (%d)\n", button->name, ret);
	}
}

/* CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_* selects a mode to start
 * automatically at boot, equivalent to pressing sw0..sw2 right after boot,
 * without requiring a shell command or a button press. Runs after the
 * measurement modes banner (SYS_INIT APPLICATION priority 99, see
 * measurement_modes.c) so the banner is still printed first.
 */
#if defined(CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_SYSTEMOFF) || \
	defined(CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_RX) || \
	defined(CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_TX)
static int default_mode_run(void)
{
	int ret;

#if defined(CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_SYSTEMOFF)
	printk("Default boot mode: System OFF\n");
	ret = action_run(BUTTON_ACTION_SYSTEM_OFF);
#elif defined(CONFIG_NRF71_POWER_TEST_DEFAULT_MODE_RX)
	printk("Default boot mode: Wi-Fi Rx\n");
	ret = action_run(BUTTON_ACTION_RX);
#else
	printk("Default boot mode: Wi-Fi Tx\n");
	ret = action_run(BUTTON_ACTION_TX);
#endif

	if (ret != 0) {
		printk("Default boot mode: command failed (%d)\n", ret);
	}

	return 0;
}

SYS_INIT(default_mode_run, APPLICATION, 100);
#endif

static void button_poll_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		struct button *button = &buttons[i];

		if (!gpio_is_ready_dt(&button->spec)) {
			printk("%s: device not ready\n", button->name);
			continue;
		}

		if (gpio_pin_configure_dt(&button->spec, GPIO_INPUT) != 0) {
			printk("%s: failed to configure\n", button->name);
		}
	}

	while (1) {
		for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
			struct button *button = &buttons[i];
			int val;

			if (!gpio_is_ready_dt(&button->spec)) {
				continue;
			}

			val = gpio_pin_get_dt(&button->spec);
			if (val < 0) {
				continue;
			}

			if (val != button->last_state &&
			    button->last_state != INT32_MIN) {
				bool pressed = (val == 1);
				bool trigger = (button->edge == BUTTON_EDGE_PRESS) ?
					       pressed : !pressed;

				if (trigger) {
					button_action_run(button);
				}
			}

			button->last_state = val;
		}

		k_msleep(20);
	}
}

K_THREAD_DEFINE(button_poll, 1024, button_poll_thread, NULL, NULL, NULL, 7, 0, 0);
