#!/usr/bin/env python3
"""Desktop app for the glove-pump: three buttons, one per state. Stdlib only.

    python3 host/glove_pump_app.py
    python3 host/glove_pump_app.py --port /dev/ttyACM1

Off          everything off, all pins low
Suction      GPIO2 (suction pump) high, valve low -> suction pump to output
Compression  GPIO0 (valve) + GPIO1 (compression pump) high

The window polls the device once a second and shows the state the pins are
actually in, not what the last click intended.
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
POLL_MS = 1000
REPLY_TIMEOUT = 0.3     # short: a dead device must not freeze the window


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


class App:
    def __init__(self, root, fd, port):
        self.root = root
        self.fd = fd
        self.buttons = {}

        style = ttk.Style(root)
        style.theme_use("clam")              # lets the active button take a colour
        style.configure("State.TButton", font=(None, 15), padding=(18, 22),
                        background="#e8e8e8", foreground="#111")
        style.map("State.TButton", background=[("active", "#d8d8d8")])
        style.configure("Active.TButton", font=(None, 15, "bold"), padding=(18, 22),
                        background="#0c8", foreground="#031")

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

        for col, state in enumerate(STATES):
            button = ttk.Button(frame, text=state.capitalize(), style="State.TButton",
                                command=lambda s=state: self.send(s))
            button.grid(row=3, column=col, padx=(0 if col == 0 else 8, 0), sticky="ew")
            frame.columnconfigure(col, weight=1)
            self.buttons[state] = button

        self.pins_label = ttk.Label(frame, text="", font=("monospace", 10),
                                    foreground="#666")
        self.pins_label.grid(row=4, column=0, columnspan=3, sticky="w", pady=(16, 0))
        self.error_label = ttk.Label(frame, text="", foreground="#c00",
                                     font=(None, 10, "bold"))
        self.error_label.grid(row=5, column=0, columnspan=3, sticky="w")
        ttk.Label(frame, text=port, font=(None, 8), foreground="#999").grid(
            row=6, column=0, columnspan=3, sticky="w", pady=(12, 0))

        self.poll()

    def send(self, command):
        try:
            reply = gp.command(self.fd, command, REPLY_TIMEOUT)
        except OSError as e:                 # board unplugged mid-session
            self.show("unknown", {}, "device gone: %s" % e)
            return
        self.show(*parse_status(reply))

    def show(self, state, pins, error):
        self.state_label.config(text=state)
        self.blurb_label.config(text=BLURB.get(state, "pin combination set by hand"
                                                if state == "raw" else ""))
        self.pins_label.config(text=pins_text(pins) if pins else "")
        self.error_label.config(text=error)
        for name, button in self.buttons.items():
            button.configure(style="Active.TButton" if name == state else "State.TButton")

    def poll(self):
        self.send("status")
        self.root.after(POLL_MS, self.poll)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/ttyACM*, /dev/ttyUSB*)")
    args = ap.parse_args()

    fd = gp.open_port(gp.find_port(args.port))
    root = tk.Tk()
    App(root, fd, args.port or gp.find_port())
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
