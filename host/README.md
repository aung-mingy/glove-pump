# glove_pump.py — host control script

Drives the ESP32-C3 firmware in this repo over its USB serial port. Sends one command,
prints the device's reply, exits with a status you can test in a shell.

```bash
./glove_pump.py toggle
./glove_pump.py set 1 high
./glove_pump.py set gpio 2 low      # same thing, either spelling
./glove_pump.py status
./glove_pump.py                     # interactive: type commands, ^D to exit
./glove_pump.py --selftest          # run the script's own checks, no hardware
```

## Requirements

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

Case-insensitive. `gpio` is optional, and `gpio1` / `gpio2` are accepted as one word.

| You type | Wire command | Result |
|---|---|---|
| `toggle` | `toggle` | next state: both low → GPIO1 high → GPIO2 high → both low |
| `set 1 high` | `set gpio 1 high` | GPIO1 high, GPIO2 dropped if it was high |
| `set gpio 2 high` | `set gpio 2 high` | GPIO2 high, GPIO1 dropped if it was high |
| `set gpio1 low` | `set gpio 1 low` | GPIO1 low, GPIO2 left as it is |
| `set 2 low` | `set gpio 2 low` | GPIO2 low, GPIO1 left as it is |
| `status` | `status` | report state, change nothing |

Both pins low is a valid state (off); both high is not, and no command can produce it.
`toggle` from off goes to GPIO1 first.

Anything else is rejected locally, before the port is opened:

```
$ ./glove_pump.py set 3 high; echo $?
bad command: set 3 high (want: toggle | set 1|2 high|low | status)
2
```

Because the device always answers with the full state, you never need `status` to know
where the pair ended up — every `OK` line carries both pins.

## Output and exit codes

Every command prints exactly one line, the device's reply:

```
$ ./glove_pump.py toggle
OK gpio1=1 gpio2=0
```

| Exit | When |
|---|---|
| `0` | device replied `OK gpio1=… gpio2=…` |
| `1` | no reply, or the port could not be opened |
| `2` | the command words did not make sense (nothing was sent) |

So `./glove_pump.py toggle && echo done` works as expected, and a `goto` in a script
only needs the exit code.

## Interactive mode

Run it with no command words on a terminal and it prompts with `> `; type commands, `quit`
or `exit` or `^D` to leave. The port is opened once and stays open, so this is the cheapest
way to do a lot of small steps by hand:

```
$ ./glove_pump.py
> toggle
OK gpio1=1 gpio2=0
> set 2 high
OK gpio1=0 gpio2=1
> set 2 low
OK gpio1=0 gpio2=0
> title
bad command: title (want: toggle | set 1|2 high|low | status)
> ^D
```

When stdin is not a terminal (a pipe, a file) there is no prompt, so replies stay clean
for parsing.

## Scripting

Piped input works, one command per line:

```bash
printf 'set 1 high\nset 2 high\ntoggle\n' | ./glove_pump.py
sleep 2
printf 'set 1 high\n' | ./glove_pump.py
```

Capture the reply and branch on it:

```bash
reply=$(./glove_pump.py toggle) || { echo "pump did not answer: $reply" >&2; exit 1; }
case "$reply" in
  "OK gpio1=1 gpio2=0") echo "now driving side 1" ;;
  "OK gpio1=0 gpio2=1") echo "now driving side 2" ;;
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
for cmd in ("toggle", "set gpio 2 high", "status"):
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
| Reply arrives but the pump doesn't move | pins are driven — check the wiring against the GPIO1/GPIO2 table in the top-level README |
| Works after a manual reset, not after flashing | the port was reopened by the flasher mid-run; just re-run the command |

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
