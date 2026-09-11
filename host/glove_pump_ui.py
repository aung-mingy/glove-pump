#!/usr/bin/env python3
"""Flask UI for the glove-pump: three states, three buttons.

    python3 -m venv ~/.venvs/glove-ui
    ~/.venvs/glove-ui/bin/pip install -r host/requirements.txt
    ~/.venvs/glove-ui/bin/python host/glove_pump_ui.py

    ./glove_pump_ui.py                          # http://127.0.0.1:8080
    ./glove_pump_ui.py --bind 0.0.0.0           # reachable from the LAN / a phone

Off          everything off, all pins low
Suction      GPIO2 (suction pump) high, valve low -> suction pump to output
Compression  GPIO0 (valve) + GPIO1 (compression pump) high

The page asks the device for its state on every load (and reloads itself every
3 s), so it shows what the pins actually are, not what the last click intended.
"""

import argparse
import logging
import os
import sys
import threading

from flask import Flask, abort, redirect, render_template_string, request, url_for

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glove_pump as gp          # same directory; noqa: E402

STATES = ("off", "suction", "compression")
BLURB = {
    "off": "everything off",
    "suction": "suction pump on, valve on the suction path",
    "compression": "compression pump on, valve on the compression path",
}

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="3">
<title>glove-pump</title>
<style>
 :root { color-scheme: dark }
 body { font: 16px/1.5 system-ui, sans-serif; margin: 0; padding: 2rem; background: #111;
        color: #eee; min-height: 80vh; display: grid; align-content: center; gap: 1.5rem }
 h1 { font-size: .875rem; font-weight: 600; letter-spacing: .1em; text-transform: uppercase;
      color: #888; margin: 0 }
 .now { font-size: 2rem; font-weight: 600; margin: 0 }
 .now small { display: block; font-size: .875rem; font-weight: 400; color: #888 }
 form { display: grid; gap: .75rem; grid-template-columns: repeat(auto-fit, minmax(9rem, 1fr)) }
 button { font: inherit; font-weight: 600; padding: 1.5rem .75rem; border-radius: .75rem;
          border: 1px solid #333; background: #1c1c1c; color: #eee; cursor: pointer }
 button:hover:not([disabled]) { background: #262626; border-color: #555 }
 button[aria-current="true"] { background: #0c8; border-color: #0c8; color: #031; cursor: default }
 .pins { font-family: ui-monospace, monospace; font-size: .875rem; color: #888; margin: 0 }
 .err { color: #f77; font-weight: 600; margin: 0 }
</style>
</head>
<body>
<h1>glove-pump</h1>
<p class="now">{{ state }}<small>{{ blurb }}</small></p>
<form method="post" action="{{ url_for('set_state') }}">
{%- for s in states %}
  <button name="state" value="{{ s }}"{% if s == state %} aria-current="true" disabled{% endif %}>{{ s|capitalize }}</button>
{%- endfor %}
</form>
<p class="pins">{{ pins }}</p>
{%- if error %}<p class="err">{{ error }}</p>{% endif %}
</body>
</html>
"""

app = Flask(__name__)

serial_lock = threading.Lock()      # the dev server is threaded; one fd, one user
last_ok = "OK off gpio0=0 gpio1=0 gpio2=0"
fd = -1


def send(command):
    """One command to the device, serialised. Returns the reply line."""
    global last_ok
    with serial_lock:
        reply = gp.command(fd, command)
    if reply.startswith("OK "):
        last_ok = reply
    return reply


def pins_line():
    values = dict(p.split("=", 1) for p in last_ok.split() if "=" in p)
    return "valve %s · compression %s · suction %s" % (
        values.get("gpio0", "?"), values.get("gpio1", "?"), values.get("gpio2", "?"))


@app.route("/")
def index():
    reply = send("status")
    parts = last_ok.split()
    state = parts[1] if len(parts) > 1 else "unknown"
    if reply.startswith("ERR"):
        error = "device refused it: %s" % reply
    elif not reply.startswith("OK "):
        error = "no reply from the device — is the board plugged in?"
    else:
        error = ""
    return render_template_string(PAGE, state=state,
                                  blurb=BLURB.get(state, "pin combination set by hand"),
                                  states=STATES, pins=pins_line(), error=error)


@app.post("/state")
def set_state():
    name = request.form.get("state", "")
    if name not in STATES:
        abort(400, "unknown state")
    with serial_lock:
        gp.command(fd, name)
    return redirect(url_for("index"))


def main():
    global fd
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/ttyACM*, /dev/ttyUSB*)")
    ap.add_argument("--bind", default="127.0.0.1", help="address to listen on")
    ap.add_argument("--http-port", type=int, default=8080, help="HTTP port (default 8080)")
    args = ap.parse_args()

    logging.getLogger("werkzeug").setLevel(logging.WARNING)   # no 3 s refresh chatter

    fd = gp.open_port(gp.find_port(args.port))
    print("device: %s" % (send("status") or "no reply — is the firmware flashed and running?"))
    print("UI: http://%s:%d   (^C to stop)" % (args.bind, args.http_port))
    # debug/reloader off on purpose: the reloader would open the serial port twice
    app.run(host=args.bind, port=args.http_port, debug=False, threaded=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
