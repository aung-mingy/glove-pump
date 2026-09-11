# glove-pump

ESP32-C3 firmware that takes commands over its **native USB serial port** and drives
**GPIO1 / GPIO2 as a complementary pair** — exactly one high at any instant.

```
host  ──USB CDC-ACM──▶  ESP32-C3  ──▶  GPIO1 ┐
 (glove_pump.py)                ──▶  GPIO2 ┘  one high, one low, always
```

## Commands

Every line sent to the port gets exactly one line back: `OK gpio1=<0|1> gpio2=<0|1>`
or `ERR <reason>`. Commands are case-insensitive.

| Command | Effect |
|---|---|
| `toggle` | swap which pin is high |
| `set gpio 1 high` | GPIO1 high, GPIO2 low |
| `set gpio 2 high` | GPIO2 high, GPIO1 low |
| `set gpio 1 low` | GPIO1 low → **GPIO2 goes high** (see below) |
| `set gpio 2 low` | GPIO2 low → **GPIO1 goes high** |
| `status` | report state, change nothing |

`set ... low` cannot leave both pins low — the pair is complementary by construction, so
asking for low on one pin drives the other high. `set gpio 1 high` and `set gpio 2 low`
are the same operation. Switches are break-before-make: both drop before one rises, so
the pins are never both high, not even for a few microseconds.

## Wiring

| Signal | Pin | Notes |
|---|---|---|
| Output A | GPIO1 | plain IO (analog alt: XTAL_32K_N / ADC1_CH1) |
| Output B | GPIO2 | plain IO (analog alt: ADC1_CH2) |
| USB D− / D+ | GPIO18 / GPIO19 | fixed; the console and flashing ride on these |

Plug the host into the USB port wired to **GPIO18/19**. On an ESP32-C3-DevKitM-1/DevKitC-1
that is the connector silked **USB**; the other one (**UART**) goes through a USB-UART bridge
chip and does not carry this console. Host side it shows up as `/dev/ttyACM0` (CDC-ACM —
baud rate is ignored, any value works).

GPIO2 is a boot strapping pin, but per the [ESP32-C3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
(§3, Table 3-3) it **does not determine the boot mode** — Espressif only recommends pulling
it up to avoid reset-time glitches. It is free to use as an output once the chip is up.
If you add an external pull-down on GPIO2, don't make it strong.

## Build and flash

```bash
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.10_env
source ~/.espressif/v5.5.4/esp-idf/export.sh
idf.py set-target esp32c3          # fresh clone only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Console config lives in `sdkconfig.defaults`: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
(GPIO18/19, no bridge chip). To use the bridged UART port instead, swap that for
`CONFIG_ESP_CONSOLE_UART_DEFAULT=y` — the firmware code is unchanged.

## Host script

`host/glove_pump.py`, stdlib only, no dependencies — **[full usage docs in host/README.md](host/README.md)**.
Short version:

```bash
./host/glove_pump.py toggle
./host/glove_pump.py set 1 high        # or: set gpio 1 high
./host/glove_pump.py status
./host/glove_pump.py                   # interactive; ^D to exit
./host/glove_pump.py --port /dev/ttyACM1 toggle
./host/glove_pump.py --selftest        # no hardware needed
```

Port auto-detects from `/dev/ttyACM*` then `/dev/ttyUSB*`. It opens raw (no echo, no
CR/LF translation), drains stale/boot-log lines, then sends one command. Exit status is
0 for `OK`, 2 for a command it rejected locally, 1 for `ERR`/no reply/port problems.

## Tests

```bash
gcc -Wall -Wextra -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
python3 host/glove_pump.py --selftest
```

The C test links the real `main/glove_pump.c` against stubbed GPIO and asserts the
invariant after **every** command path: toggle, both spellings of `set`, uppercase,
bad pin, bad level, unknown command, blank line. Its fake GPIO mirrors `gpio_config()`,
so it also fails if the pins are configured output-only (which makes every readback 0 —
that bug shipped once and was caught by the boot selftest on hardware). The Python
selftest checks command building and the line framing over a real pty (including that raw
mode isn't mangling LF into CRLF).

The firmware additionally runs a self-check on the real pins at every boot and logs
`selftest PASS gpio1/gpio2 complementary`. It toggles the lines a few times in
microseconds — delete the `selftest()` call in `app_main()` if your load must not see
any blip at power-on.

## Notes

- Power-on state is GPIO1 high, GPIO2 low.
- Lines may end in `\n` (scripts) or `\r` (terminals). The RX line-ending mode is set to
  `ESP_LINE_ENDINGS_CR` because the IDF default (CRLF) stalls the console read on a bare CR.
- **The pins are configured `GPIO_MODE_INPUT_OUTPUT`, not `GPIO_MODE_OUTPUT`.** An
  output-only pin has its input buffer disabled (`gpio_config()` → `gpio_input_disable()`),
  so `gpio_get_level()` reads back 0 for ever and the boot selftest fails. Both directions
  are enabled so `status` reports the real pad level. `test_glove_pump.c` fails if this is
  reverted.
- **The USB-Serial-JTAG driver is installed explicitly** (`usb_serial_jtag_driver_install()`
  + `usb_serial_jtag_vfs_use_driver()`), plus `fcntl(fileno(stdin), F_SETFL, 0)`. Without
  them the VFS read path polls the RX FIFO once and returns `EWOULDBLOCK`, so `fgets()`
  sees EOF immediately and the command loop exits during boot. This mirrors ESP-IDF's
  `examples/system/console/advanced`.
- 4 MB flash is configured in `sdkconfig.defaults`; otherwise the ROM warns that the image
  header says 2 MB on a 4 MB part. Editing `sdkconfig.defaults` does **not** update an
  existing `sdkconfig` — `rm -rf build sdkconfig` and re-run `idf.py set-target esp32c3`
  after changing it.
- If stdin errors or hits EOF, the firmware clears the error and retries rather than idling
  for ever, so closing the host port doesn't need a board reset.
