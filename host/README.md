# host tools — CLI and Flask web UI

Two clients for the ESP32-C3 firmware in this repo, both over its USB serial port:

| File | What |
|---|---|
| `glove_pump.py` | CLI: one command per invocation, prints the reply, exits with a usable status |
| `glove_pump_ui.py` | Flask web UI: three buttons, one per state, plus the live pin levels |

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

The web UI is documented in its own section at the end; it is the only thing in this repo
that needs a dependency (Flask).

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
OK suction gpio0=0 gpio1=0 gpio2=1
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
| Works after a manual reset, not after flashing | the port was reopened by the flasher mid-run; just re-run the command |
| `OSError: [Errno 16] Device or resource busy` | something else holds the port — a serial monitor, or a second copy of the CLI / UI. One process per port |

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

## Web UI (Flask) — glove_pump_ui.py

Three buttons, one per state, and the live pin levels. Every page load asks the device for
`status`, so the page shows what the pins actually are rather than what the last click
intended — if the CLI moved the rig, the UI follows on its next reload (every 3 s).

```
┌──────────────────────────────┐
│ GLOVE-PUMP                   │
│ compression                  │
│ compression pump on, valve … │
│ [ Off ] [ Suction ] [ ▓▓Comp ]│   <- current state is highlighted and disabled
│ valve 1 · compression 1 · suction 0
└──────────────────────────────┘
```

Setup — Flask is the repo's only dependency:

```bash
python3 -m venv ~/.venvs/glove-ui
~/.venvs/glove-ui/bin/pip install -r host/requirements.txt
```

Run:

```bash
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py                          # 127.0.0.1:8080
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py --bind 0.0.0.0           # reachable on the LAN
~/.venvs/glove-ui/bin/python host/glove_pump_ui.py --port /dev/ttyACM1 --http-port 9000
```

| Option | Meaning |
|---|---|
| `--port PATH` | serial port; default: first `/dev/ttyACM*`, then `/dev/ttyUSB*` |
| `--bind ADDR` | address to listen on, default `127.0.0.1` |
| `--http-port N` | HTTP port, default `8080` |

Notes:

- Only three states are offered, and the server rejects anything else with `400` — the raw
  `set gpio …` commands are CLI-only on purpose.
- The serial port is guarded by a lock, so a double-clicked button can't interleave two
  commands on one fd.
- The reloader is off (`debug=False`) by design: Flask's auto-reloader would fork a second
  process and open the serial port twice.
- One process per serially-attached board. Stop the CLI/monitor before starting the UI, and
  vice versa.
- `--bind 0.0.0.0` puts an unauthenticated rig controller on the network. Bench LAN only.
- If you unplug and replug the board, restart the UI — the open fd does not survive the
  device disappearing.
