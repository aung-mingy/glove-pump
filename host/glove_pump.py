#!/usr/bin/env python3
"""Control the glove-pump ESP32-C3 over its USB CDC-ACM port. Stdlib only.

    ./glove_pump.py suction
    ./glove_pump.py compression
    ./glove_pump.py off
    ./glove_pump.py toggle
    ./glove_pump.py set 1 high           # raw pin access
    ./glove_pump.py set gpio 2 low       # same thing, either spelling
    ./glove_pump.py status
    ./glove_pump.py                      # interactive: type commands, ^D to exit
    ./glove_pump.py --selftest           # no hardware needed

Port is auto-picked from /dev/ttyACM* then /dev/ttyUSB*; override with --port.
Baud rate is irrelevant: CDC-ACM ignores it.
"""

import argparse
import glob
import os
import select
import sys
import termios
import time

PROMPT = "> "


def list_ports():
    """Candidate ports, best first: /dev/ttyACM* then /dev/ttyUSB*."""
    return sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))


def find_port(explicit=None):
    if explicit:
        return explicit
    ports = list_ports()
    if ports:
        return ports[0]
    sys.exit("no serial port found (looked at /dev/ttyACM*, /dev/ttyUSB*) — pass --port")


def open_port(path):
    """Open raw: no echo, no CR/LF translation, no line discipline.

    Raises OSError if the path isn't an openable serial port — never exits, so a
    caller with a UI (or a reconnect loop) can try the next candidate.
    """
    try:
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as e:
        raise OSError("cannot open %s: %s" % (path, e.strerror)) from e
    try:
        a = termios.tcgetattr(fd)
    except termios.error as e:                      # termios.error may lack .strerror
        os.close(fd)
        raise OSError("%s is not a serial port: %s" % (path, e)) from e
    a[0] = 0                                        # iflag: no ICRNL/IXON/INLCR
    a[1] = 0                                        # oflag: no ONLCR
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0                                        # lflag: no ICANON/ECHO
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def read_line(fd, timeout=1.0):
    """One newline-terminated line, or '' on timeout."""
    buf = bytearray()
    end = time.monotonic() + timeout
    while True:
        left = end - time.monotonic()
        if left <= 0:
            break
        if not select.select([fd], [], [], left)[0]:
            break
        chunk = os.read(fd, 4096)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            break
    return bytes(buf).decode(errors="replace").split("\n")[0].strip()


def build_command(tokens):
    """CLI words -> wire command. Raises ValueError on anything else."""
    t = [x.lower() for x in tokens]
    if len(t) > 1 and t[1] == "gpio":
        t.pop(1)
    if t in (["toggle"], ["status"], ["off"], ["suction"], ["compression"]):
        return t[0]
    if len(t) == 3 and t[0] == "set" \
            and t[1] in ("0", "1", "2", "gpio0", "gpio1", "gpio2") \
            and t[2] in ("high", "low"):
        return "set gpio %s %s" % (t[1][-1], t[2])
    if len(t) == 2 and t[0] == "hold":       # len() first: t may be empty
        if t[1] == "off":
            return "hold off"
        ohms = parse_ohms(t[1])
        if ohms is not None:
            return "hold %d" % ohms
    raise ValueError("bad command: %s (want: off | suction | compression | toggle | "
                     "set 0|1|2 high|low | hold <ohms>|12k|off | status)"
                     % " ".join(tokens))


def parse_ohms(word):
    """'12000' or '12k' -> ohms, or None. Rejects the firmware's own range check."""
    if word.endswith("k"):
        word, scale = word[:-1], 1000
    else:
        scale = 1
    try:
        return int(round(float(word) * scale))
    except ValueError:
        return None


def command(fd, cmd, timeout=1.0):
    """Send one command, return the device's OK/ERR line ('' on silence)."""
    while read_line(fd, 0.05):
        pass                                        # drop boot log / stale replies
    os.write(fd, (cmd + "\n").encode())
    line = ""
    for _ in range(10):                             # skip anything that isn't a reply
        line = read_line(fd, timeout)
        if not line or line.startswith(("OK", "ERR")):
            break
    return line


def run(fd, cmd, timeout=1.0):
    try:
        out = command(fd, cmd, timeout)
    except OSError as e:                            # board unplugged mid-session
        print("device disconnected: %s" % e, file=sys.stderr)
        return 1
    print(out or "no reply from device (wrong port, or firmware not running)")
    return 0 if out.startswith("OK") else 1


def run_selftest():
    assert build_command(["toggle"]) == "toggle"
    assert build_command(["status"]) == "status"
    assert build_command(["off"]) == "off"
    assert build_command(["Suction"]) == "suction"
    assert build_command(["COMPRESSION"]) == "compression"
    assert build_command(["set", "1", "HIGH"]) == "set gpio 1 high"
    assert build_command(["set", "0", "low"]) == "set gpio 0 low"
    assert build_command(["set", "gpio", "2", "low"]) == "set gpio 2 low"
    assert build_command(["set", "gpio0", "low"]) == "set gpio 0 low"
    # hold: the target is normalized to whole ohms before it goes on the wire
    assert build_command(["hold", "off"]) == "hold off"
    assert build_command(["hold", "12000"]) == "hold 12000"
    assert build_command(["hold", "12k"]) == "hold 12000"
    assert build_command(["HOLD", "9.5k"]) == "hold 9500"
    assert parse_ohms("20k") == 20000
    assert parse_ohms("1350") == 1350
    assert parse_ohms("x") is None
    for bad in ([], ["reboot"], ["set", "3", "high"], ["set", "1", "sideways"],
                ["set", "1"], ["off", "now"], ["hold"], ["hold", "soon"],
                ["hold", "1k", "2k"]):
        try:
            build_command(bad)
            raise AssertionError("accepted bad command %r" % (bad,))
        except ValueError:
            pass

    # Port discovery and open failures must return/raise, never exit the process:
    # the desktop app calls these from a Tk callback and from its reconnect loop.
    assert isinstance(list_ports(), list)
    assert all(isinstance(p, str) for p in list_ports())
    for bad in ("/dev/null", "/dev/definitely-not-here"):
        try:
            open_port(bad)
            raise AssertionError("open_port accepted %s" % bad)
        except OSError:
            pass

    # Framing round trip over a real tty: open_port + write + read_line.
    import pty
    import threading

    master, slave = pty.openpty()

    def fake_device():
        try:
            while True:
                data = os.read(master, 128)
                if not data:
                    return
                if b"toggle" in data:
                    os.write(master, b"OK suction gpio0=0 gpio1=0 gpio2=1\n")
                else:
                    os.write(master, b"ERR unknown command\n")
        except OSError:
            pass                                    # master closed at end of test

    threading.Thread(target=fake_device, daemon=True).start()
    fd = open_port(os.ttyname(slave))
    assert command(fd, "toggle") == "OK suction gpio0=0 gpio1=0 gpio2=1", "line framing broken"
    assert command(fd, "bogus") == "ERR unknown command"
    # raw-mode check: the device sent LF only, so nothing may turn it into CRLF
    os.write(fd, b"toggle\n")
    assert select.select([fd], [], [], 1.0)[0], "no reply"
    raw = os.read(fd, 4096)
    assert raw == b"OK suction gpio0=0 gpio1=0 gpio2=1\n", "line discipline not raw: %r" % raw
    os.close(fd)
    os.close(master)
    os.close(slave)

    # --timeout must actually reach the read loop (it silently didn't once)
    quiet_master, quiet_slave = pty.openpty()
    fd = open_port(os.ttyname(quiet_slave))
    started = time.monotonic()
    assert command(fd, "toggle", 0.2) == ""
    elapsed = time.monotonic() - started
    assert elapsed < 0.6, "--timeout ignored: silent device took %.2fs" % elapsed
    os.close(fd)
    os.close(quiet_master)
    os.close(quiet_slave)

    print("selftest PASS")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    # nargs="*", not REMAINDER: REMAINDER would swallow "--port X" typed after
    # the command words, turning it into part of the command.
    ap.add_argument("cmd", nargs="*", help="off | suction | compression | toggle | "
                    "set 0|1|2 high|low | hold <ohms>|12k|off | status")
    ap.add_argument("--port", help="serial port (default: first /dev/ttyACM*, /dev/ttyUSB*)")
    ap.add_argument("--timeout", type=float, default=1.0, help="reply timeout, seconds")
    ap.add_argument("--selftest", action="store_true", help="run checks, no hardware")
    args = ap.parse_args()

    if args.selftest:
        run_selftest()
        return 0

    try:
        cmd = build_command(args.cmd) if args.cmd else None
    except ValueError as e:
        print(e, file=sys.stderr)
        return 2

    fd = find_port(args.port)
    try:
        fd = open_port(fd)
    except OSError as e:
        print(e, file=sys.stderr)
        return 1

    if cmd:
        return run(fd, cmd, args.timeout)

    for line in sys.stdin:                          # interactive
        if sys.stdin.isatty():
            print(PROMPT, end="", flush=True)       # piped stdin stays clean
        words = line.split()
        if not words:
            continue
        if words[0] in ("quit", "exit"):
            break
        try:
            cmd = build_command(words)
        except ValueError as e:
            print(e, file=sys.stderr)
            continue
        run(fd, cmd, args.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
