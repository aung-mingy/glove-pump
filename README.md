# glove-pump

ESP32-C3 firmware that takes commands over its **native USB serial port** and drives a
pump/valve rig. Two host clients are included: a CLI and a desktop app.

```
host  ──USB CDC-ACM──▶  ESP32-C3  ──▶  GPIO0  valve
 glove_pump.py                  ──▶  GPIO1  compression pump
 glove_pump_app.py              ──▶  GPIO2  suction pump
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
`OK <state> gpio0=<0|1> gpio1=<0|1> gpio2=<0|1> adc=<raw> mv=<mV> r=<ohms>` or
`ERR <reason>`, where `<state>` is `off`, `suction`, `compression`, or `raw` for a hand-set
pin combination. Case-insensitive.

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
| Glove sensor | GPIO3 | must be an **ADC1** pin (GPIO0–GPIO4); the C3's ADC2 is unusable |
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

## Glove sensor

A variable resistor across a fixed 13 kΩ, tapped by the ADC:

```
3V3 ──[13 kΩ]──┬── GPIO3 (ADC1_CH3)
               │
             [R_var]        R_var ≈ 9 kΩ  glove fully open  → ~1350 mV
               │            R_var ≈ 20 kΩ glove fully closed → ~2000 mV
              GND
```

    mv = 3300 · R / (13000 + R)        R = 13000 · mv / (3300 − mv)

The firmware reports all three numbers on every reply — `adc=<raw counts> mv=<mV>
r=<ohms>` — so the chain can be checked against a multimeter at any point, and the app turns
`r` into `glove 13.1 kΩ · 37% closed (1655 mV)`. Because the taps are the only thing the app
knows about your rig, they live in `host/glove_pump_app.py` as `GLOVE_OPEN_OHMS` /
`GLOVE_CLOSED_OHMS` — measure your own ends and set them there. `r` saturates at 999999 Ω
(open circuit / no sensor) and is −1 if the ADC read itself failed.

**The sensor has to land on an ADC1 pin: GPIO0–GPIO4.** On the ESP32-C3 the ADC driver
supports unit 1 only. The C3's own `soc_caps.h` says so —
`SOC_ADC_DIG_SUPPORTED_UNIT(UNIT)` is `((UNIT == 0) ? 1 : 0)` — and IDF's ADC example skips
ADC2 for this target with *"On ESP32C3, ADC2 is no longer supported, due to its HW
limitation."* The datasheet agrees: ADC2 is not factory-calibrated, and the errata list
"ADC2 of some chip revisions is not operable".

So **GPIO5 — the pin an ESP32-C3 Super Mini labels `IO5 / A5 · MISO` — cannot be read at
all**, however correctly it is wired. `adc_oneshot_new_unit()` fails with
`ESP_ERR_INVALID_ARG: adc unit not supported`, and the firmware reports
`adc=-1 mv=-1 r=-1` with a `sensor: adc unit 2 unavailable` warning in the boot log. The `A`
labels are the board vendor's, not the driver's. Nothing about that pin is a wiring fault,
and there is no workaround: ADC2 is unusable on this chip.

GPIO0/1/2 are the valve and pumps, so the sensor's home here is **GPIO3 (`A3`)**. GPIO4
(`A4`) would also work but is `MTMS`, so a JTAG probe would fight it. Both of the mistakes
that cost a debugging session are now build errors: an unsupported unit trips an `#error`, and
putting the sensor on a pump pin trips a `_Static_assert`. Putting it on GPIO0-2 would be
worse than useless anyway — the pad is driven there, and `adc_oneshot_config_channel()` would
disable that output and leave the pump line floating.

Bring-up check, in order:
1. `glove_pump.py status` with the glove open, then closed — `mv` should land near 1350 → 2000
   and `r` near 9000 → 20000. Compare `mv` with a multimeter at the tap; they should agree
   within ~50 mV.
2. Press Suction and Compression. `r` and `mv` must **not move**. If they do, the sensor is
   sharing a pin with a pump output — move the wire.
3. `adc=-1 mv=-1 r=-1` in the reply (with the `adc unit … unavailable` warning at boot) means
   the sensor pin is not on a readable ADC unit — check it against the GPIO table above, not
   the silkscreen. `mv` frozen at 0 or 3300 means the divider isn't connected: check the 3V3
   end and the ground.

Notes:
- ADC1 is not shared with the radio, so the reading stays valid with Wi-Fi on (this build
  never starts a radio anyway).
- The reading is an average of 8 samples, and the ADC uses the chip's eFuse curve-fitting
  calibration (the scheme differs per chip — that's why the include is `adc_cali_scheme.h`
  plus a `#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED`). Without burnt eFuse values it falls
  back to `raw · 3300 / 4095` and says so in the boot log.
- Jittery reading? A 100 nF from the tap to GND (next to the ADC pin) is the usual fix — it
  shunts the ADC's switched-capacitor sample-and-hold charge spikes. Your divider's source
  impedance is 13 kΩ ∥ R ≈ 8 kΩ, which is fine, but wire length isn't.
- Don't reconfigure this pin as a GPIO. `adc_oneshot_config_channel()` calls
  `gpio_config_as_analog()`, which *disables* the pin's input and output drivers and leaves it
  floating — that's why there's no "scan every ADC pin" bring-up command in the firmware: it
  would silently release GPIO0/1/2 and let the pump driver inputs float.

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

## Host desktop app (tkinter)

`host/glove_pump_app.py` — one window, three buttons, one per state, plus the live pin
levels. It polls the device once a second and shows the state the pins are actually in, so
if the CLI moves the rig the window follows. Stdlib only (`tkinter`), like everything else.

```bash
python3 host/glove_pump_app.py                     # auto-detect the port
python3 host/glove_pump_app.py --port /dev/ttyACM1
```

The current state's button is filled green; the others stay clickable. Each of the three
sends that state name to the firmware and shows the reply — the button never assumes the
command worked, and an unanswered device turns into an amber "no reply" line instead of a
frozen window (0.3 s reply timeout, so a dead board can't lock the UI).

**If the board disappears** (flaky cable, USB re-enumeration, brownout), the window flags it
in red and greys the state buttons out. It searches for the board every second for
`AUTO_SEARCH_ATTEMPTS` tries (~10 s) — trying the port it last used first, since the port
number can change across a re-enumeration, then whatever is enumerated — and re-enables the
buttons when it lands. The status line says `disconnected — searching for the device…` while
that hunt is running, and drops to plain `disconnected` once it has stopped, so the window
never claims to be searching when it isn't. **Search for device** searches immediately and
restarts the automatic attempts. A board that is still open but stops answering gets one poll
of grace before the app treats it as gone, so a single slow reply doesn't drop the connection.
After a reconnect the readout shows the board's real state — if it reset on the way, that will
be `off`, all pins low.

Needs a display and `python3-tk` (`sudo apt install python3-tk` on Debian/Ubuntu — it ships
with most Python installs, including the python.org and Homebrew ones). For a headless box,
the CLI does everything except the buttons.

## Tests

```bash
gcc -Wall -Wextra -o /tmp/test_glove_pump test/test_glove_pump.c -Itest/stubs && /tmp/test_glove_pump
python3 host/glove_pump.py --selftest
python3 test/test_app_link.py        # the desktop app's reconnect logic, no display needed
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

The Python selftest checks command building, that `list_ports()`/`open_port()` raise instead
of exiting (the app calls them from Tk callbacks and its reconnect loop), and the line
framing over a real pty (including that raw mode isn't mangling LF into CRLF).

`test/test_app_link.py` covers the app's non-Tk logic against fake boards on ptys, so it needs
no display: the parse/format helpers, the three status-line wordings, connecting and sending a
state, a silent-but-open port (flagged on the first poll, given up on the second), a vanished
port, the automatic search finding a board that comes back on a new port number, the search
stopping after `AUTO_SEARCH_ATTEMPTS`, no recovery without the button once it has stopped, and
the button restarting it. Five mutations of the app module were checked against it — never
giving up, a hardcoded give-up bound, a Search button that doesn't reset the counter, a
reconnect that ignores the give-up, and `searching()` always true — each fails a check.

Widget rendering ("disconnected — searching…" vs plain "disconnected", buttons greyed out) was
confirmed by running the real app under Xvfb, unplugging the board under it, and screenshotting
on both sides of the give-up.

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
