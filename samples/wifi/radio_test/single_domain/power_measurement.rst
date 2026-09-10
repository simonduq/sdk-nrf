.. _wifi_radio_test_power_measurement_sd:

Current measurement on the nRF7120 DK
#####################################

.. contents::
   :local:
   :depth: 2

The sample can be used as a single firmware image that covers the three current
measurement modes of the nRF7120 DK: Wi-Fi® Rx, Wi-Fi Tx and System OFF.
The mode is selected at runtime from the shell, so the image does not have to
be rebuilt between measurements.

Building and flashing
*********************

Build the sample for the ``nrf7120dk/nrf7120/cpuapp`` board target:

.. code-block:: console

   west build -p -b nrf7120dk/nrf7120/cpuapp --sysbuild \
      -d build_unified_power \
      nrf/samples/wifi/radio_test/single_domain \
      -- -DSB_CONFIG_WIFI_NRF70=n

Flash it with the default runner. The nRF7120 DK requires ``--recover``, since
the application core access port is closed after every reset:

.. code-block:: console

   west flash -d build_unified_power --recover

Connect to the shell UART, for example with ``picocom -b 115200 /dev/ttyACM0``.

At boot, the firmware prints the value of ``FICR->PROTEST.CP.TIMESTAMP1``,
which identifies the chip trim revision, and a reminder of the commands that
select each mode.
Take note of the register value together with the measured currents.

Each mode has a one-word shorthand: ``rx``, ``tx`` and ``off``.
The ``rx`` and ``tx`` shorthands echo every ``wifi_radio_test`` command they
run, so the shell log still shows exactly what was configured, and the
individual commands remain available for measurements that need other
parameters.

.. note::

   Switch directly from ``rx`` to ``tx`` (or back) only after the previous
   mode has actually stopped: ``wifi_radio_test init`` tries to disable an
   ongoing Rx or Tx test for you, but that teardown call can time out on some
   RPU firmware builds (``wait_for_radio_cmd_status: Timed out``), aborting
   the new mode. If that happens, reset the board (or run
   ``wifi_radio_test rx 0`` / ``wifi_radio_test tx 0`` and retry) before
   starting the next mode.

Measuring the Wi-Fi Rx current
******************************

Run the ``rx`` shorthand, which configures the radio for reception on
channel 6 in the 2.4 GHz band:

.. code-block:: console

   rx

It runs the following commands:

.. code-block:: console

   wifi_radio_test init 0 6
   wifi_radio_test rx 1

Capture at least 500 ms of current and read out the mean value.

Measuring the Wi-Fi Tx current
******************************

Run the ``tx`` shorthand, which transmits modulated frames back-to-back on
channel 6 in the 2.4 GHz band, at 15 dBm:

.. code-block:: console

   tx

It runs the following commands:

.. code-block:: console

   wifi_radio_test init 0 6
   wifi_radio_test tx_power 15
   wifi_radio_test tx 1

Frames are occasionally separated by gaps of a few milliseconds.
Measure the mean current over a window that does not contain such a gap.

Measuring the System OFF current
********************************

The ``off`` command, also available as ``systemoff``, enters System OFF without configuring any wake-up
source and without requesting RAM retention, which is the lowest power state
the device supports.
Only a pin reset or a power cycle brings the device back.

.. code-block:: console

   off

The command stores the request in uninitialized RAM, which survives a software
reset, and resets the device.
On the next boot, the request is handled by the first initialization step, before
the console, the shell, the logging subsystem, the system clock and the Wi-Fi
driver are brought up.
The device therefore enters System OFF from the same state as the
``system_off`` sample built with ``CONFIG_GRTC_WAKEUP_ENABLE=n``,
``CONFIG_GPIO_WAKEUP_ENABLE=n``, ``CONFIG_APP_USE_RETAINED_MEM=n`` and
``CONFIG_SYS_CLOCK_DISABLE=y``.

For the lowest and most repeatable reading, run ``off`` on a freshly
reset device, before running any radio test command.
The Wi-Fi driver powers the Wi-Fi core up during boot and the driver does not
support powering it down again, so the reset performed by the command is what
brings the device back to a fully idle state.

If a reset is not wanted, for example to enter System OFF directly from a state
the radio test has set up, use the following command instead:

.. code-block:: console

   off now

This variant stops any ongoing Wi-Fi transmission or reception, stops the shell
and the logging backends, suspends the console UART and stops the system clock,
and then enters System OFF in place.
The residual current can be slightly higher than with the default variant,
because the peripherals brought up during boot are only shut down by software.

Recovering the device
*********************

Waking up from System OFF on the nRF7120 DK is subject to the reset and startup
limitation described in the nRF7120 DK bring-up documentation, so a debugger
reset is not enough to restart the application.
Flashing the image again brings the device back:

.. code-block:: console

   west flash -d build_unified_power --no-rebuild --recover
