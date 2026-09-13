# host tools — CLI and desktop app

Two clients for the ESP32-C3 firmware in this repo, both over its USB serial port:

| File | What |
|---|---|
| `glove_pump.py` | CLI: one command per invocation, prints the reply, exits with a usable status |
| `glove_pump_app.py` | tkinter desktop app: three buttons, one per state, plus the live pin levels |

```bash
./glove_pump.py suction
./glove_pump.py compression
./glove_pump.py off
./glove_pump.py set 1 high          # raw pin access
./glove_pump.py set gpio 2 low      # same thing, either spelling
./glove_pump.py status
./glove_pump.py                     # interactive: type commands, ^D to exit
./glove_pump.py --selftest          # run the script's own checks, no hardware
```

The desktop app is documented in its own section at the end. Everything here is stdlib — no
`pip install` anywhere.

## Requirements (CLI)

- **Python 3.6+** (tested on 3.10). **No dependencies** — `argparse`, `glob`, `select`,
  `termios`, `pty` are all stdlib. There is no `pip install` step.
- The firmware flashed and running (see the top-level README).
- **Linux / macOS / BSD.** It uses `termios` for raw mode, so it does not run on Windows.
- Read/write access to the port. On Linux that usually means membership of `dialout`
  (`sudo usermod -aG dialout $USER`, then log out and back in). On macOS use `uucp`.

## Picking the port

Left to itself, the script takes the first match from `/dev/ttyACM*`, then
`/dev/ttyUSB*`. With one board attached that is the right port. To be explicit, or when
something else (a modem, another dev board, a serial console) is also on the bus:

```bash
./glove_pump.py --port /dev/ttyACM1 toggle
./glove_pump.py --port /dev/cu.usbmodem14201 toggle     # macOS
```

If it finds nothing: `no serial port found (looked at /dev/ttyACM*, /dev/ttyUSB*) — pass --port`.

## Options

| Option | Meaning |
|---|---|
| `--port PATH` | serial port; default: first `/dev/ttyACM*`, then `/dev/ttyUSB*` |
| `--timeout SECS` | seconds to wait for the reply, default `1.0` |
| `--selftest` | run the script's own checks (pty framing, command parsing, timeout); no hardware, no port |
| `-h`, `--help` | usage |

Options may go before or after the command words.

## Command words

Case-insensitive. The three state names are passed straight through; `gpio` is optional on
`set`, and `gpio0` / `gpio1` / `gpio2` are accepted as one word.

| You type | Wire command | Result |
|---|---|---|
| `off` | `off` | all pins low |
| `suction` | `suction` | GPIO2 high, valve low |
| `compression` | `compression` | GPIO0 (valve) and GPIO1 (compression pump) high |
| `toggle` | `toggle` | next state: off → suction → compression → off |
| `set 1 high` | `set gpio 1 high` | GPIO1 high, GPIO2 dropped, valve moved high |
| `set gpio 2 high` | `set gpio 2 high` | GPIO2 high, GPIO1 dropped, valve moved low |
| `set gpio1 low` | `set gpio 1 low` | GPIO1 low, valve and GPIO2 left as they are |
| `set 0 low` | `set gpio 0 low` | valve low |
| `status` | `status` | report state and pin levels, change nothing |

The pumps are never both high and the valve is never left on the other pump's path; no
command can produce either. `set gpio …` is raw bench access — prefer the three state names.

Anything else is rejected locally, before the port is opened:

```
$ ./glove_pump.py set 9 high; echo $?
bad command: set 9 high (want: off | suction | compression | toggle | set 0|1|2 high|low | status)
2
```

Because the device always answers with the full state, you never need `status` to know
where the rig ended up — every `OK` line carries the state name and all three pins.

## Output and exit codes

Every command prints exactly one line, the device's reply:

```
$ ./glove_pump.py suction
OK suction gpio0=0 gpio1=0 gpio2=1 adc=1674 mv=1350 r=9000
```

| Exit | When |
|---|---|
| `0` | device replied `OK …` |
| `1` | no reply, or the port could not be opened |
| `2` | the command words did not make sense (nothing was sent) |

So `./glove_pump.py suction && echo done` works as expected, and a `goto` in a script
only needs the exit code.

## Interactive mode

Run it with no command words on a terminal and it prompts with `> `; type commands, `quit`
or `exit` or `^D` to leave. The port is opened once and stays open, so this is the cheapest
way to do a lot of small steps by hand:

```
$ ./glove_pump.py
> suction
OK suction gpio0=0 gpio1=0 gpio2=1
> compression
OK compression gpio0=1 gpio1=1 gpio2=0
> off
OK off gpio0=0 gpio1=0 gpio2=0
> title
bad command: title (want: off | suction | compression | toggle | set 0|1|2 high|low | status)
> ^D
```

When stdin is not a terminal (a pipe, a file) there is no prompt, so replies stay clean
for parsing.

## Scripting

Piped input works, one command per line:

```bash
printf 'suction\ncompression\noff\n' | ./glove_pump.py
sleep 2
printf 'suction\n' | ./glove_pump.py
```

Capture the reply and branch on it:

```bash
reply=$(./glove_pump.py toggle) || { echo "pump did not answer: $reply" >&2; exit 1; }
case "$reply" in
  "OK suction"*)     echo "pulling" ;;
  "OK compression"*) echo "pushing" ;;
  "OK off"*)         echo "idle" ;;
esac
```

Poll repeatedly (each call opens and closes the port; the firmware keeps no connection
state, so this is cheap and safe):

```bash
while sleep 1; do ./glove_pump.py toggle >/dev/null || exit; done
```

Note that each CLI invocation is one command — open, send, read, close. There is no
persistent connection except in interactive mode.

## Use as a Python module

The pieces are importable if you want to drive it from a longer-running program and keep
one port open for many commands:

```python
import sys
sys.path.insert(0, "host")
from glove_pump import open_port, find_port, command

fd = open_port(find_port())                 # or open_port("/dev/ttyACM1")
for cmd in ("off", "suction", "compression", "status"):
    print(cmd, "->", command(fd, cmd))
```

| Function | Does |
|---|---|
| `find_port(explicit=None)` | resolve the port (or `sys.exit` with a message) |
| `open_port(path)` | open raw (no echo, no CR/LF translation), return an fd |
| `command(fd, cmd, timeout=1.0)` | send one command, return the reply line (`""` on silence) |
| `read_line(fd, timeout=1.0)` | read one line, `""` on timeout |
| `build_command(tokens)` | CLI words → wire command; raises `ValueError` |
| `run(fd, cmd, timeout=1.0)` | `command()` + print + 0/1 status |

`command()` drains anything already waiting (boot log, an earlier reply) before it writes,
then skips lines that are not `OK`/`ERR`, so a stale line can't be mistaken for the answer.
A silent device costs one `--timeout` and returns `""`.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no serial port found` | cable/hub, or the board enumerates as something else — check `ls /dev/tty{ACM,USB}*` and pass `--port` |
| `cannot open /dev/ttyACM0: Permission denied` | add yourself to `dialout` (Linux) / `uucp` (macOS), re-login |
| `/dev/null is not a serial port: (25, 'Inappropriate ioctl for device')` | `--port` points at something that isn't a tty |
| `no reply from device (wrong port, or firmware not running)` | wrong port (see the UART caveat below), firmware not flashed, or the board is held in reset. Raise `--timeout` if the device is slow, lower it if you want fast failures |
| Reply arrives but the pump doesn't move | pins are driven — check the wiring against the GPIO table in the top-level README |
| `r=9000`…`r=20000` never changes | sensor not on GPIO3 (an ADC1 pin), or the wire isn't making contact — see the bring-up checks in the top-level README |
| `mv` / `r` move when you press Suction or Compression | the sensor is sharing a pin with a pump output (on the C3 the SPI MISO default is GPIO2, our suction line). Move the wire |
| Works after a manual reset, not after flashing | the port was reopened by the flasher mid-run; just re-run the command |
| `device disconnected: [Errno 5] Input/output error` | the board vanished mid-command (cable, power, hub). Re-run; the desktop app flags and recovers from this on its own |
| `OSError: [Errno 16] Device or resource busy` | something else holds the port — a serial monitor, or a second copy of the CLI / app. One process per port |

**Wrong-port trap:** on an ESP32-C3-DevKitM-1/DevKitC-1, the connector silked **UART** goes
through a USB-UART bridge chip and is *not* where the firmware's console lives. The firmware
only talks on the port wired to GPIO18/19, silked **USB**. On the UART port you get a port
that opens fine and never answers.

## Verifying the script itself

```bash
./glove_pump.py --selftest
```

Checks command parsing (including rejecting bad input), the line framing round trip over a
real pty, that raw mode doesn't turn LF into CRLF, and that `--timeout` actually reaches
the read loop. Prints `selftest PASS` and exits 0; any failure raises an `AssertionError`.
No board required.

## Desktop app (tkinter) — glove_pump_app.py

One window, three buttons, one per state, the live pin levels, and the glove reading. It polls
the device once a second and renders the answer, so the window shows what the pins actually
are rather than what the last click intended — move the rig from the CLI and the window
follows.

```
┌────────────────────────────────────────────────┐
│ GLOVE-PUMP                                     │
│ compression                                    │
│ compression pump on, valve on the …            │
│ ┌────────┐ ┌─────────┐ ┌─────────────────────┐ │
│ │  Off   │ │ Suction │ │     Compression     │ │  <- current state filled green,
│ └────────┘ └─────────┘ └─────────────────────┘ │     others still clickable
│ valve 1 · compression 1 · suction 0            │
│ glove 13.1 kΩ · 37% closed (1655 mV)           │  <- from the GPIO3 divider
│ connected · /dev/ttyACM0      [Search for device] │
└────────────────────────────────────────────────┘
```

`glove_pump_app.py` holds `GLOVE_OPEN_OHMS` / `GLOVE_CLOSED_OHMS` (9000 / 20000 by default) —
those are the "fully open" and "fully closed" resistances that the percentage is scaled
between, so measure your own ends and put them there. The sensor's own maths (mV → ohms) runs
on the firmware; the app only formats it and flags the two degenerate cases: `r=-1` ("no
reading — check the GPIO3 wiring") and anything above 100 kΩ ("open circuit").

When the board goes away the same line turns red and the buttons grey out:

```
│ —                                              │  <- no state left to show
│ ┌────────┐ ┌─────────┐ ┌─────────────────────┐ │
│ │  Off   │ │ Suction │ │     Compression     │ │  <- all greyed, clicks ignored
│ └────────┘ └─────────┘ └─────────────────────┘ │
│ disconnected — searching for the device…  [Search for device] │
```

Run:

```bash
python3 host/glove_pump_app.py                      # auto-detect the port
python3 host/glove_pump_app.py --port /dev/ttyACM1
```

| Option | Meaning |
|---|---|
| `--port PATH` | port to try first; default: first `/dev/ttyACM*`, then `/dev/ttyUSB*` |

Notes:

- Stdlib only (`tkinter`) — no `pip install`. Needs a display, and `python3-tk`
  (`sudo apt install python3-tk` on Debian/Ubuntu; it is included in the python.org and
  Homebrew builds). Headless box? The CLI covers everything except the buttons.
- Only three states are offered — the raw `set gpio …` commands stay CLI-only on purpose.
- A button sends its state name and displays the reply; it never assumes the command worked.
  An unanswered device shows an amber line instead of freezing the window (0.3 s reply
  timeout, because the poll runs on the Tk thread).
- **Disconnects are flagged and recovered automatically.** A vanished port (unplug, brownout,
  re-enumeration) is noticed on the next poll: the window goes red, the buttons grey out, and
  the app searches about every second for `AUTO_SEARCH_ATTEMPTS` tries (~10 s) — trying its
  last port first, since a re-enumeration can move it to a different number — then stops.
  A port that is still open but stops answering gets one poll of grace first, so one slow reply
  doesn't drop the connection. `AUTO_SEARCH_ATTEMPTS` is a module constant: raise it for a
  longer hunt, or set it very high to keep searching for ever.
- **Search for device** searches immediately and restarts the automatic attempts (it also
  re-reads the state when it reconnects). While already connected it just refreshes the
  readout — deliberately, because re-opening a live CDC-ACM port can trip the board's reset
  lines and stop your pump.
- The status line is one line of text in three states: `connected · /dev/ttyACM0`,
  `disconnected — searching for the device…` while the automatic hunt runs, and plain
  `disconnected` once it has stopped.
- `Link` (the serial side: fd, disconnect detection, port re-scan) contains no Tk, so it runs
  and is tested headless — `test/test_app_link.py`. Same for `parse_status()`/`conn_text()`.
  See the Tests section of the top-level README.
- One process per serially-attached board: stop the CLI or a serial monitor before starting
  the app, and vice versa.
