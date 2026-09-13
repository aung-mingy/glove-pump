#!/usr/bin/env python3
"""Desktop app for the glove-pump: three buttons, one per state. Stdlib only.

    python3 host/glove_pump_app.py
    python3 host/glove_pump_app.py --port /dev/ttyACM1

Off          everything off, all pins low
Suction      GPIO2 (suction pump) high, valve low -> suction pump to output
Compression  GPIO0 (valve) + GPIO1 (compression pump) high

The window polls the device once a second and shows the state the pins are
actually in, not what the last click intended. If the board disappears — a
flaky cable or a USB re-enumeration — the window flags it and hunts for the
board again, once a second, for AUTO_SEARCH_ATTEMPTS tries; the status line says
so while that hunt is running and just "disconnected" once it has stopped.
"Search for device" searches immediately and restarts the hunt.
"""

import argparse
import os
import sys
import tkinter as tk
from tkinter import ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glove_pump as gp          # same directory; noqa: E402

STATES = ("off", "suction", "compression")
BLURB = {
    "off": "everything off",
    "suction": "suction pump on, valve on the suction path",
    "compression": "compression pump on, valve on the compression path",
}
POLL_MS = 1000                  # also the reconnect cadence: a lost board is
REPLY_TIMEOUT = 0.3             # searched for on the next poll, ~1 s later
MISSES_BEFORE_RECONNECT = 2     # a silent board gets two chances before we re-scan
AUTO_SEARCH_ATTEMPTS = 10       # ~10 s of hunting, then it stops and says so —
                                # Search for device (or raise this) to try again

# The glove sensor, from the wiring: 9 kOhm fully open, 20 kOhm fully closed.
# Those two numbers are the calibration knobs — measure your own ends and change
# them here. Above SENSOR_MAX_OHMS we assume nothing is connected.
GLOVE_OPEN_OHMS = 9000
GLOVE_CLOSED_OHMS = 20000
SENSOR_MAX_OHMS = 100000


def parse_status(reply):
    """Device reply -> (state, {pin: level}, error). Pure, so it's testable
    without a display."""
    if reply.startswith("OK "):
        parts = reply.split()
        state = parts[1] if len(parts) > 1 else "unknown"
        return state, dict(p.split("=", 1) for p in parts if "=" in p), ""
    if reply.startswith("ERR"):
        return "unknown", {}, reply
    return "unknown", {}, "no reply from the device — is the board plugged in?"


def pins_text(pins):
    return "valve %s · compression %s · suction %s" % (
        pins.get("gpio0", "?"), pins.get("gpio1", "?"), pins.get("gpio2", "?"))


def sensor_text(pins):
    """The glove readout, from the adc/mv/r fields. Pure, and the maths lives on
    the firmware side — this only formats and sanity-checks."""
    if "r" not in pins:
        return ""
    try:
        ohms = int(pins["r"])
    except ValueError:
        return "sensor ?"
    if ohms < 0:
        return "glove — no reading (check the GPIO3 wiring)"
    if ohms > SENSOR_MAX_OHMS:
        return "glove — open circuit (sensor disconnected?)"
    closed = max(0.0, min(100.0, (ohms - GLOVE_OPEN_OHMS) * 100.0 /
                              (GLOVE_CLOSED_OHMS - GLOVE_OPEN_OHMS)))
    return "glove %.1f kΩ · %.0f%% closed (%s mV)" % (
        ohms / 1000.0, closed, pins.get("mv", "?"))


def manual_command(pin, high):
    """The wire command for a manual toggle of GPIO `pin` (0 valve, 1 compression,
    2 suction). Straight through: the board owns the rules."""
    return "set gpio %d %s" % (pin, "high" if high else "low")


def hold_text(pins):
    """The hold readout, from the hold/err fields. Pure, like the rest."""
    target = pins.get("hold")
    if target is None:
        return ""
    if target == "off":
        return "hold off"
    if target == "stalled":
        return "hold stalled — pumps stopped (check the line, then Hold again)"
    try:
        err = int(pins.get("err", "0"))
    except ValueError:
        err = 0
    return "holding at %.1f kΩ · error %+d Ω" % (int(target) / 1000.0, err)


def conn_text(connected, port, searching):
    """The status line. Pure, because the wording is the part that changes."""
    if connected:
        return "connected · %s" % port
    if searching:
        return "disconnected — searching for the device…"
    return "disconnected"


class Link:
    """The serial side of the app: holds the fd, notices the board going away,
    and finds it again. No Tk in here, so it's testable without a display."""

    def __init__(self, port=None):
        self.port = port            # last port used, or the one --port named
        self.fd = -1
        self.misses = 0
        self.search_tries = 0

    def searching(self):
        """True while the automatic search is still running."""
        return self.search_tries < AUTO_SEARCH_ATTEMPTS

    def search(self):
        """Search now: what the button does. Restarts the automatic attempts."""
        self.search_tries = 0
        return self.tick()

    def candidates(self):
        """The port we had first — a re-enumeration can move it — then whatever
        is enumerated now."""
        out = [self.port] if self.port else []
        return out + [p for p in gp.list_ports() if p not in out]

    def close(self):
        if self.fd >= 0:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = -1

    def reconnect(self):
        """Close and look for the board again. True when a port opened.

        Stops trying after AUTO_SEARCH_ATTEMPTS failed scans, so a board that
        stays away doesn't get its ports poked every second for ever.
        """
        if not self.searching():
            return False
        self.close()
        self.search_tries += 1
        for path in self.candidates():
            try:
                self.fd = gp.open_port(path)
            except OSError:
                continue            # not this one; try the next candidate
            self.port = path
            self.misses = 0
            self.search_tries = 0
            return True
        return False

    def tick(self, command="status"):
        """One cycle: find the board if it's missing, then send `command`.
        Returns (connected, state, pins, error)."""
        if self.fd < 0:
            self.reconnect()
        if self.fd < 0:
            return False, None, {}, ""
        try:
            reply = gp.command(self.fd, command, REPLY_TIMEOUT)
        except OSError:             # unplugged: read/write on a dead fd
            self.close()
            return False, None, {}, ""
        if not reply:
            self.misses += 1
            if self.misses >= MISSES_BEFORE_RECONNECT:
                self.close()        # silent board: re-scan on the next tick
                self.misses = 0
                return False, None, {}, ""
            return True, None, {}, "no reply from the device"
        self.misses = 0
        state, pins, error = parse_status(reply)
        return True, state, pins, error


class App:
    def __init__(self, root, link):
        self.root = root
        self.link = link

        style = ttk.Style(root)
        style.theme_use("clam")              # lets the active button take a colour
        style.configure("State.TButton", font=(None, 15), padding=(18, 22),
                        background="#e8e8e8", foreground="#111")
        style.map("State.TButton", background=[("active", "#d8d8d8")])
        style.configure("Active.TButton", font=(None, 15, "bold"), padding=(18, 22),
                        background="#0c8", foreground="#031")
        style.configure("Search.TButton", font=(None, 9), padding=(8, 3))

        root.title("glove-pump")
        frame = ttk.Frame(root, padding=20)
        frame.grid(sticky="nsew")
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)

        ttk.Label(frame, text="GLOVE-PUMP", font=(None, 9, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w")
        self.state_label = ttk.Label(frame, text="…", font=(None, 26, "bold"))
        self.state_label.grid(row=1, column=0, columnspan=3, sticky="w")
        self.blurb_label = ttk.Label(frame, text="", foreground="#666")
        self.blurb_label.grid(row=2, column=0, columnspan=3, sticky="w", pady=(0, 16))

        self.buttons = {}
        for col, state in enumerate(STATES):
            button = ttk.Button(frame, text=state.capitalize(), style="State.TButton",
                                command=lambda s=state: self.tick(s))
            button.grid(row=3, column=col, padx=(0 if col == 0 else 8, 0), sticky="ew")
            frame.columnconfigure(col, weight=1)
            self.buttons[state] = button

        self.pins_label = ttk.Label(frame, text="", font=("monospace", 10),
                                    foreground="#666")
        self.pins_label.grid(row=5, column=0, columnspan=3, sticky="w", pady=(16, 0))

        # Manual per-pin control. These mirror what the board reports, not what was
        # clicked, and the firmware enforces the one real rule: raising a pump drops
        # the other one and pairs the valve. No copy of that rule lives here.
        self.pin_vars = {}
        self.manual_widgets = []
        manual = ttk.Frame(frame)
        manual.grid(row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))
        ttk.Label(manual, text="Manual:").pack(side="left")
        for pin, label in ((0, "Valve"), (1, "Compression"), (2, "Suction")):
            var = tk.IntVar(value=0)
            box = ttk.Checkbutton(manual, text=label, variable=var,
                                  command=lambda p=pin: self.manual(p))
            box.pack(side="left", padx=(10, 0))
            self.pin_vars[pin] = var
            self.manual_widgets.append(box)

        self.sensor_label = ttk.Label(frame, text="", font=("monospace", 11))
        self.sensor_label.grid(row=6, column=0, columnspan=3, sticky="w", pady=(6, 0))
        self.hold_label = ttk.Label(frame, text="", font=("monospace", 10),
                                    foreground="#666")
        self.hold_label.grid(row=7, column=0, columnspan=3, sticky="w")

        # Hold target, in kOhm. The loop itself runs on the board, so this only
        # sends a target and shows the result.
        hold_row = ttk.Frame(frame)
        hold_row.grid(row=8, column=0, columnspan=3, sticky="w", pady=(10, 0))
        ttk.Label(hold_row, text="Hold at").pack(side="left")
        self.target_var = tk.StringVar(value="12.0")
        ttk.Entry(hold_row, textvariable=self.target_var, width=6).pack(side="left", padx=6)
        ttk.Label(hold_row, text="kΩ").pack(side="left")
        ttk.Button(hold_row, text="Hold", style="Search.TButton",
                   command=self.hold).pack(side="left", padx=(10, 0))
        ttk.Button(hold_row, text="Release", style="Search.TButton",
                   command=lambda: self.tick("hold off")).pack(side="left", padx=6)

        self.error_label = ttk.Label(frame, text="", foreground="#c60",
                                     font=(None, 10, "bold"))
        self.error_label.grid(row=9, column=0, columnspan=3, sticky="w")

        # width reserves room for the longest status line, so the label can't
        # grow over the Search button when the text changes
        self.conn_label = ttk.Label(frame, text="", font=(None, 9), width=44)
        self.conn_label.grid(row=10, column=0, columnspan=2, sticky="w", pady=(12, 0))
        ttk.Button(frame, text="Search for device", style="Search.TButton",
                   command=self.search).grid(row=10, column=2, sticky="e",
                                             padx=(16, 0), pady=(12, 0))

        self.tick()
        self.poll()

    def tick(self, command="status"):
        """One poll cycle, or a button press (which sends that state instead)."""
        self.render(*self.link.tick(command))

    def manual(self, pin):
        """A manual pin toggle: send what the box now says. The board enforces the
        rule (raising a pump drops the other and pairs the valve), and its reply
        re-syncs the boxes, so they show the rig and not the click."""
        self.tick(manual_command(pin, bool(self.pin_vars[pin].get())))

    def hold(self):
        """Send the setpoint from the entry, in kOhm. Bad input stays local."""
        try:
            ohms = int(round(float(self.target_var.get()) * 1000))
        except ValueError:
            self.error_label.config(text="hold needs a number of kΩ, e.g. 12.0")
            return
        if not 1000 <= ohms <= 100000:
            self.error_label.config(text="hold target must be 1–100 kΩ")
            return
        self.tick("hold %d" % ohms)

    def search(self):
        """The Search button: look for the board now, and start trying again if
        the automatic search had given up."""
        self.render(*self.link.search())

    def render(self, connected, state, pins, error):
        self.state_label.config(text=state or "—")
        self.blurb_label.config(text=BLURB.get(state or "", ""))
        self.pins_label.config(text=pins_text(pins) if pins else "")
        self.sensor_label.config(text=sensor_text(pins) if pins else "")
        self.hold_label.config(text=hold_text(pins) if pins else "")
        self.error_label.config(text="" if not connected else error)
        self.conn_label.config(text=conn_text(connected, self.link.port,
                                              self.link.searching()),
                               foreground="#2a2" if connected else "#c00")
        for name, button in self.buttons.items():
            button.state(["!disabled"] if connected else ["disabled"])
            button.configure(style="Active.TButton" if name == state else "State.TButton")
        for pin, var in self.pin_vars.items():
            var.set(int(pins.get("gpio%d" % pin, 0)) if pins else 0)
        for widget in self.manual_widgets:
            widget.state(["!disabled"] if connected else ["disabled"])

    def poll(self):
        self.tick()
        self.root.after(POLL_MS, self.poll)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port to try first (default: first "
                                   "/dev/ttyACM*, /dev/ttyUSB*)")
    args = ap.parse_args()

    root = tk.Tk()
    App(root, Link(args.port))
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
