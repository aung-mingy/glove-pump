# glove-pump

ESP32-C3 firmware that takes commands over its **native USB serial port** and drives a
pump/valve rig. Two host clients are included: a CLI and a Flask web UI.

```
host  ──USB CDC-ACM──▶  ESP32-C3  ──▶  GPIO0  valve
 glove_pump.py                  ──▶  GPIO1  compression pump
 glove_pump_ui.py               ──▶  GPIO2  suction pump
```

## Three states

The valve always follows the running pump, so only these pump-running combinations exist:

| State | GPIO0 valve | GPIO1 compression | GPIO2 suction | What it does |
|---|---|---|---|---|
| `off` | 0 | 0 | 0 | everything off |
| `suction` | 0 | 0 | 1 | valve sends the suction pump to the output, suction pump on |
| `compression` | 1 | 0→1 | 0 | valve sends the compression pump to the output, compression pump on |

Startup is `off`. **The two pumps are never both high**, not even for microseconds, and the
valve is never switched while a pump is running (everything drops, then the valve moves,
then the pump starts).

## Commands

Every line sent to the port gets exactly one line back:
`OK <state> gpio0=<0|1> gpio1=<0|1> gpio2=<0|1>` or `ERR <reason>`, where `<state>` is
`off`, `suction`, `compression`, or `raw` for a hand-set pin combination. Case-insensitive.

| Command | Effect |
|---|---|
| `off` | all pins low |
| `suction` | GPIO2 high, valve low |
| `compression` | GPIO0 and GPIO1 high |
| `toggle` | next state: off → suction → compression → off |
| `set gpio 0 high\|low` | raw valve control |
| `set gpio 1 high\|low` | raw compression pump; raising it drops GPIO2 and moves the valve high |
| `set gpio 2 high\|low` | raw suction pump; raising it drops GPIO1 and moves the valve low |
| `status` | report state and pin levels, change nothing |

The named states are the intended interface (the UI uses only those). `set gpio …` is raw
bench access: raising a pump still enforces both hardware rules, lowering one leaves the
valve where it is — which is how you reach a `raw` valve-only combination.

## Wiring

| Signal | Pin | Notes |
|---|---|---|
| Valve | GPIO0 | plain IO (analog alt: XTAL_32K_P / ADC1_CH0) |
| Compression pump | GPIO1 | plain IO (analog alt: XTAL_32K_N / ADC1_CH1) |
| Suction pump | GPIO2 | plain IO (analog alt: ADC1_CH2) |
| USB D− / D+ | GPIO18 / GPIO19 | fixed; the console and flashing ride on these |

Plug the host into the USB port wired to **GPIO18/19**. On an ESP32-C3-DevKitM-1/DevKitC-1
that is the connector silked **USB**; the other one (**UART**) goes through a USB-UART bridge
chip and does not carry this console. Host side it shows up as `/dev/ttyACM0` (CDC-ACM —
baud rate is ignored, any value works).

Two chip notes:
- GPIO2 is a boot strapping pin, but per the [ESP32-C3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
  (§3, Table 3-3) it **does not determine the boot mode** — Espressif only recommends pulling
  it up to avoid reset-time glitches. Free to use as an output once the chip is up; don't add
  a strong external pull-down.
- GPIO0/GPIO1 are `XTAL_32K_P`/`XTAL_32K_N` (§2, Table 2-6). If your board fits an external
  32.768 kHz crystal on those pins, don't drive them. The Espressif devkits don't.

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

## Host CLI

`host/glove_pump.py` — stdlib only, no dependencies.
**[Full usage docs in host/README.md](host/README.md).** Short version:

```bash
./host/glove_pump.py suction
./host/glove_pump.py compression
./host/glove_pump.py off
./host/glove_pump.py set 1 high        # raw pin access; "set gpio 1 high" also works
./host/glove_pump.py status
./host/glove_pump.py                   # interactive; ^D to exit
./host/glove_pump.py --selftest        # no hardware needed
```

Port auto-detects from `/dev/ttyACM*` then `/dev/ttyUSB*`. It opens raw (no echo, no
CR/LF translation), drains stale/boot-log lines, then sends one command. Exit status is
0 for `OK`, 2 for a command it rejected locally, 1 for `ERR`/no reply/port problems.

## Host web UI (Flask)

`host/glove_pump_ui.py` — three buttons, one per state, plus the live pin levels. It asks the
device for its state on every page load, so it shows the real pins rather than the last
click, and reloads itself every 3 s.

```bash
python3 -m venv ~/.venvs/glove-ui
~/.venvs/glove-ui/bin/pip install -r host/requirements.txt
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py            # http://127.0.0.1:8080
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py --bind 0.0.0.0   # reachable on the LAN
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py --port /dev/ttyACM1 --http-port 9000
```

Flask is the only dependency in the repo (`host/requirements.txt`). Nothing else — CLI,
firmware, tests — needs anything beyond the stdlib. `--bind 0.0.0.0` exposes the rig to
anyone on the network with no authentication; it's meant for a bench LAN.

## Tests

```bash
gcc -Wall -Wextra -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
python3 host/glove_pump.py --selftest
```

The C test links the real `main/glove_pump.c` against stubbed GPIO and checks the two
hardware rules after **every** command path (all three named states, all three `toggle`
steps, raising each pump, lowering each pump, uppercase, bad pin, bad level, unknown
command, blank line): the pumps are never both high, and the valve is never left on the
other pump's path. Verified against four deliberate mutations — a `set_pin()` that raises a
pump without dropping the other, a `set_pin()` that forgets the valve, a `write_pins()` that
never raises the valve, and a state table with the wrong valve value — each one aborts the
test. Its fake GPIO also mirrors `gpio_config()`, so it fails if the pins are configured
output-only (every readback reads 0 — that bug shipped once and was caught by the boot
selftest on hardware).

The Python selftest checks command building and the line framing over a real pty (including
that raw mode isn't mangling LF into CRLF). The Flask UI was exercised end to end over HTTP
against a fake device on a pty: all three buttons, the state round trip, bad-state rejection,
and the page reflecting the device on reload.

The firmware additionally self-checks the real pins at boot: it walks the named states and
the raw pump path, verifying the pads land where the table says, then logs
`selftest PASS — off/suction/compression land on their pins`. It blips the lines for
microseconds — delete the `selftest()` call in `app_main()` if your load must not see any
blip at power-on.

## Notes

- Power-on state is `off` (all pins low).
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
- Neither client keeps the rig in a state of its own: the firmware is the source of truth and
  reports actual pad levels, so a UI page left open and a CLI command can't disagree.
