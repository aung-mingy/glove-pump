"""The app's serial side: parse/format helpers and Link's reconnect behaviour.

    python3 test/test_app_link.py

No hardware and no display (no Tk), so it runs anywhere:
  gcc + test_glove_pump.c -> the firmware's rules and the sensor maths
  host/glove_pump.py --selftest -> the CLI and its line framing
  this file -> the app's parsing, its readouts, and its reconnect logic
"""
import os
import pty
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "host"))
import glove_pump_app as app                      # noqa: E402

STATES = {"off": (0, 0, 0), "suction": (0, 0, 1), "compression": (1, 1, 0)}
fails = []


def check(label, got, want):
    ok = got == want
    if not ok:
        fails.append(label)
    print("%s %-52s %r" % ("ok  " if ok else "FAIL", label, got) +
          ("" if ok else "  want %r" % (want,)))


def state(name, valve, comp, suct, raw=1674, mv=1350, ohms=9000):
    """What Link.tick() returns for one reply: (connected, state, pins, error)."""
    return (True, name, {"gpio0": str(valve), "gpio1": str(comp), "gpio2": str(suct),
                         "adc": str(raw), "mv": str(mv), "r": str(ohms)}, "")


def offline():
    return (False, None, {}, "")


class Fake:
    """A fake board on a pty. close() is an unplug."""

    def __init__(self):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.pins = (0, 0, 0)
        self.raw, self.mv, self.ohms = 1674, 1350, 9000
        self.seen = []
        self.stop = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        buf = b""
        while not self.stop.is_set():
            try:
                data = os.read(self.master, 256)
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                cmd = line.decode(errors="replace").strip()
                if cmd:
                    self.seen.append(cmd)
                    try:
                        os.write(self.master, (self._reply(cmd) + "\n").encode())
                    except OSError:
                        return

    def _reply(self, cmd):
        toks = cmd.split()
        if toks[0] in STATES:
            self.pins = STATES[toks[0]]
        elif len(toks) == 4 and toks[0] == "set" and toks[1] == "gpio":
            pin, high = int(toks[2]), toks[3] == "high"
            v, c, s = self.pins
            if high:
                v, c, s = (1, 1, 0) if pin == 1 else (0, 0, 1) if pin == 2 else (1, c, s)
            else:
                v, c, s = (0, c, s) if pin == 0 else (v, 0, s) if pin == 1 else (v, c, 0)
            self.pins = (v, c, s)
        elif toks[0] != "status":
            return "ERR unknown command"
        name = next((n for n, v in STATES.items() if v == self.pins), "raw")
        return "OK %s gpio0=%d gpio1=%d gpio2=%d adc=%d mv=%d r=%d" % (
            (name,) + self.pins + (self.raw, self.mv, self.ohms))

    def close(self):
        self.stop.set()
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


# --- pure helpers ------------------------------------------------------
check("parse OK", app.parse_status("OK suction gpio0=0 gpio1=0 gpio2=1 "
                                   "adc=1674 mv=1350 r=9000"),
      ("suction", {"gpio0": "0", "gpio1": "0", "gpio2": "1",
                   "adc": "1674", "mv": "1350", "r": "9000"}, ""))
check("parse ERR", app.parse_status("ERR unknown command"),
      ("unknown", {}, "ERR unknown command"))
check("parse silence", app.parse_status("")[2].startswith("no reply"), True)
check("pins text", app.pins_text({"gpio0": "1", "gpio1": "1", "gpio2": "0"}),
      "valve 1 · compression 1 · suction 0")
check("conn text: connected", app.conn_text(True, "/dev/ttyACM0", False),
      "connected · /dev/ttyACM0")
check("conn text: searching", app.conn_text(False, "/dev/ttyACM0", True),
      "disconnected — searching for the device…")
check("conn text: search stopped", app.conn_text(False, "/dev/ttyACM0", False),
      "disconnected")

# --- the hold readout -------------------------------------------------
check("hold off", app.hold_text({"hold": "off", "err": "+0"}), "hold off")
check("holding", app.hold_text({"hold": "12000", "err": "+320"}),
      "holding at 12.0 kΩ · error +320 Ω")
check("holding, too open", app.hold_text({"hold": "9500", "err": "-1100"}),
      "holding at 9.5 kΩ · error -1100 Ω")
check("stalled is called out", app.hold_text({"hold": "stalled", "err": "+2400"}),
      "hold stalled — pumps stopped (check the line, then Hold again)")
check("no hold fields", app.hold_text({"gpio0": "0"}), "")

# --- the glove readout, against the wiring's two ends ------------------
check("glove fully open (9k)", app.sensor_text({"r": "9000", "mv": "1350"}),
      "glove 9.0 kΩ · 0% closed (1350 mV)")
check("glove fully closed (20k)", app.sensor_text({"r": "20000", "mv": "2000"}),
      "glove 20.0 kΩ · 100% closed (2000 mV)")
check("glove half way", app.sensor_text({"r": "14500", "mv": "1655"}),
      "glove 14.5 kΩ · 50% closed (1655 mV)")
check("glove rounds", app.sensor_text({"r": "12400", "mv": "1560"}),
      "glove 12.4 kΩ · 31% closed (1560 mV)")
check("below the open end clamps to 0%", app.sensor_text({"r": "5000", "mv": "950"}),
      "glove 5.0 kΩ · 0% closed (950 mV)")
check("above the closed end clamps to 100%",
      app.sensor_text({"r": "44000", "mv": "2540"}),
      "glove 44.0 kΩ · 100% closed (2540 mV)")
check("unreadable sensor", app.sensor_text({"r": "-1", "mv": "-1"}),
      "glove — no reading (check the GPIO4 wiring)")
check("open circuit", app.sensor_text({"r": "999999", "mv": "3300"}),
      "glove — open circuit (sensor disconnected?)")
check("no sensor fields in the reply", app.sensor_text({"gpio0": "0"}), "")

# --- connect, command, disconnect -------------------------------------
board = Fake()
link = app.Link(board.path)
check("connects on the first tick", link.tick(), state("off", 0, 0, 0))
check("sends a state", link.tick("compression"), state("compression", 1, 1, 0))
check("board saw it", board.seen, ["status", "compression"])
check("reconnect cadence and search window",
      (app.POLL_MS, app.AUTO_SEARCH_ATTEMPTS), (1000, 10))

# a port that stays open but goes quiet: one poll of grace, then it's gone
quiet_master, quiet_slave = pty.openpty()
silent = app.Link(os.ttyname(quiet_slave))
check("silent board: 1st poll flags it",
      silent.tick(), (True, None, {}, "no reply from the device"))
check("silent board: 2nd poll gives up on it", silent.tick(), offline())
check("silent board: fd closed", silent.fd, -1)
for fd in (quiet_master, quiet_slave):
    os.close(fd)

# --- the automatic search gives up ------------------------------------
board.close()
link.tick()                                     # notices the port going quiet
check("offline within 2 polls", link.tick(), offline())
check("still searching", link.searching(), True)
check("re-scan tries the lost port first", link.candidates()[0], board.path)
for _ in range(app.AUTO_SEARCH_ATTEMPTS):
    link.tick()
check("search stops after the attempts run out", link.searching(), False)
check("label drops the searching wording",
      app.conn_text(False, link.port, link.searching()), "disconnected")

# a board that comes back on its own is NOT picked up once we've given up...
late = Fake()
app.gp.list_ports = lambda: [late.path]
check("no auto-recovery after giving up", link.tick(), offline())
check("...and it did not touch the new board", late.seen, [])

# ...until the Search button, which restarts the automatic attempts
check("Search button reconnects", link.search(), state("off", 0, 0, 0))
check("searching again (counter reset)", link.searching(), True)
check("label back to connected",
      app.conn_text(True, link.port, link.searching()), "connected · %s" % late.path)

# recovery inside the window needs no button at all
late.close()
link.tick()
link.tick()
check("offline again", link.fd, -1)
check("searching again after a fresh loss", link.searching(), True)
earlier = Fake()
app.gp.list_ports = lambda: [earlier.path]
check("reconnects unaided within the window", link.tick()[0], True)
check("board is the new one", link.port, earlier.path)

for f in (late, earlier, board):
    f.close()
print("FAILURES: %d" % len(fails))
sys.exit(1 if fails else 0)
