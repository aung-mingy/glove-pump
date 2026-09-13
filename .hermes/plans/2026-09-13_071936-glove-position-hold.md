# Glove position hold — implementation plan

> **For Hermes:** plan mode output. No code written yet. Start at Phase 0; the controller
> constants come from Phase 0 measurements, not from guesses.

**Goal:** Hold the glove at a commanded position (commanded as a sensor resistance) despite the
air leak, by pulsing the on/off pumps — closed loop, running on the board.

**Architecture:** Bang-bang with hysteresis (deadband) + dwell times, not PID. The loop runs on
the ESP32-C3 at 100 Hz; the host only sets the target and displays. Every constant comes from a
measured probe run (Phase 0).

**Hardware:** C3 SuperMini, valve GPIO0 / compression GPIO1 / suction GPIO2, sensor GPIO4 (A4)
(ADC1_CH4). Two on/off diaphragm pumps, 3-way valve. Sensor: 13 kΩ series, R_var 9 kΩ (open) to
20 kΩ (closed), 1350→2000 mV.

---
## Why not PID

Not because it's hard — because it doesn't fit this plant:

1. **The actuator is on/off.** There is no proportional valve, so any PID output becomes a duty
   cycle. "PID" here means PWM + PI, at which point the P and I terms are just choosing a pulse
   width and a pulse rate.
2. **The D term is unusable.** R is nonlinear in the ADC reading (dR/dV ≈ 11 Ω/mV at 9 kΩ, 25 Ω/mV
   at 20 kΩ) and noisy. D amplifies exactly that, so it needs a filter — after which it's too
   slow to matter at these rates.
3. **I is the only term that matters** (it's what holds against a leak), and with a bang-bang
   actuator a slow integrator limit-cycles. You end up adding anti-windup *and* dwell limits —
   i.e. the hysteresis controller you should have written.
4. **Pulse timing is the real control effort**, and that's expressible directly as a deadband
   plus min/max on and off times. Three gains to tune → two thresholds and two timers measured
   from the plant.

PID becomes the right tool if a proportional actuator appears (PWM driver on the pump, or a
proportional valve). That's Phase 3, with a trigger condition, not a default.

## The structural insight: the leak is free actuation in one direction

The leak only ever pushes R one way at a given end. So the loop does not need to fight it: when a
pulse overshoots, the leak brings R back into the band by itself. Consequences:

- Only the *toward-the-target* direction ever fires. No valve switching per cycle → no transient
  from moving the valve, no audible chatter, half the wear.
- The hold error is bounded by the deadband by construction. Nothing to tune to get stability;
  the only question is whether the resulting pulse cadence is acceptable (audible? wears pumps?).
- The output of the loop is effectively "compensate the leak": if measurements say the leak costs
  X Ω/s at 9 kΩ and one 100 ms pulse buys Y Ω, the steady-state pump duty is X/Y. If that duty is
  high, the fix is hardware (bleed orifice, vacuum reservoir, check valve), not code.

## Where the loop runs

**Firmware.** The host is a 1 Hz Tk poll over USB serial — not a control loop, and it must not
own one: if the laptop sleeps, USB re-enumerates (we already built that recovery) or the process
dies, the glove would go slack. On-device also means the loop interval is reliable (a 100 Hz tick)
rather than dependent on USB scheduling.

Split: firmware loops and holds; host sets `hold <ohms>` / `hold off` and reports. The reply gains
`hold=<ohms|off> err=<Ω>`; the probe derives pump duty offline from logged state, so the firmware
doesn't need to compute it.

## Phase 0 — measure the plant (no firmware change)

Deliverable: `host/glove_probe.py`, stdlib only, runs against the *current* firmware (it just
sends `status` in a tight loop and can drive the pumps like the CLI does).

It must produce these numbers, because they are the controller's constants:

| # | Measurement | How | Becomes |
|---|---|---|---|
| 1 | Noise: σ(R), peak-peak at rest | 10 s of `status` at ~50 Hz, pumps off, at each end | `DEADBAND` (start ≥ 3σ) |
| 2 | Leak rate and direction: dR/dt with pumps off | 20–30 s at 9 kΩ, then at 20 kΩ | pulse cadence, and whether one direction suffices |
| 3 | Dead time: pump-on command → first ΔR beyond noise | N pulses of 50/100/200/400 ms, timestamped | `MIN_ON_MS` (must exceed dead time or pulses do nothing) |
| 4 | Gain: ΔR per ms of pump-on, and the decay after it | same pulses | pulse width for a given step; travel timeout |
| 5 | Hysteresis: R at a position approached from each side | go to 12 kΩ from both directions | whether the deadband must exceed a mechanical hysteresis |

Task 1 — `host/glove_probe.py` skeleton and stats (with `--selftest`)
- Create `host/glove_probe.py`: reuse `glove_pump.find_port/open_port/command` for I/O.
- `--selftest` feeds synthetic samples through the stats functions and asserts the numbers
  (drift, σ, dead time on a step) — the smallest thing that fails if the maths is wrong.
- Verify: `python3 host/glove_probe.py --selftest` → `selftest PASS`.
- Commit.

Task 2 — measurement modes
- `--mode noise|leak|step|all`, `--seconds`, `--pulse-ms`, CSV to stdout (and `--csv FILE`).
- `--mode step` drives pumps itself via `glove_pump.build_command` (`suction`/`compression`/`off`).
- Verify: run against a fake device on a pty (a throwaway script, like earlier verifications) and
  confirm the printed numbers match the fake's known behaviour.
- Commit.

Task 3 — run on hardware, write the numbers down
- Flash, run `--mode all` at both ends. Record the table above into `README.md` (new "Hold tuning"
  section) and into the constants at Task 5.
- Verify: the wiring sanity checks in README hold (`r` moves 9 kΩ↔20 kΩ, doesn't follow pump
  state); manual fallback if the probe is inconvenient: drive with the CLI and time with a
  stopwatch.
- Commit.

**Gate:** do not write the controller until 1–4 exist. If dead time is larger than the time R
spends crossing the band, bang-bang cannot hold tightly and Phase 3's PWM comes first.

## Phase 1 — firmware hold mode

Task 4 — `hold` command, plumbing only
- Modify `main/glove_pump.c`: `hold <ohms>` / `hold 9k` / `hold off`; store `target` + `enabled`;
  extend the reply: `OK ... hold=9000 err=+120`. No control logic yet; `hold` just records.
- Extend the grammar table in the file header, `README.md` Commands table, and
  `host/glove_pump.py::build_command` (+ `--selftest` cases).
- Test: `test/test_glove_pump.c` — `hold 12k` → `hold=12000`, `hold off` → `hold=off`,
  bad input → `ERR`.
- Verify: `gcc ... && /tmp/test_glove_pump` → PASS; `python3 host/glove_pump.py --selftest` → PASS;
  `idf.py build` clean.
- Commit.

Task 5 — the controller as a pure function
- Add to `main/glove_pump.c`:

```c
typedef enum { ACT_OFF, ACT_SUCTION, ACT_COMPRESSION } action_t;

typedef struct {                 /* all from the Phase 0 table */
    int target;                  /* ohms */
    int deadband;                /* ohms, >= 3 sigma of the noise */
    int min_on_ms;               /* > pump dead time */
    int min_off_ms;              /* dwell: no chatter */
    int max_on_ms;               /* over-travel guard */
} hold_cfg_t;

typedef struct {
    action_t action;
    long action_started_ms;
    long last_change_ms;
} hold_state_t;

/* Pure: no I/O, no globals - the host test drives this directly.
 * err = R - target, so err > 0 means "more closed than asked" -> suction
 * (suction drops R toward 9 kOhm, compression raises it toward 20 kOhm). */
static action_t hold_decide(const hold_cfg_t *cfg, hold_state_t *st, int ohms, long now)
{
    int err = ohms - cfg->target;
    long in_action = st->action == ACT_OFF ? 0 : now - st->action_started_ms;

    if (st->action != ACT_OFF) {
        if (in_action < cfg->min_on_ms) return st->action;      /* finish the bite */
        if (in_action >= cfg->max_on_ms) return ACT_OFF;        /* over-travel guard */
        if (abs(err) <= cfg->deadband) return ACT_OFF;          /* arrived */
        if ((st->action == ACT_SUCTION) != (err > 0)) return ACT_OFF;  /* wrong way */
        return st->action;
    }
    if (abs(err) <= cfg->deadband) return ACT_OFF;
    if (now - st->last_change_ms < cfg->min_off_ms) return ACT_OFF;
    return err > 0 ? ACT_SUCTION : ACT_COMPRESSION;
}
```

- Test (`test/test_glove_pump.c`): in band → OFF; err > band then `min_on` → SUCTION and stays
  SUCTION until `min_on` elapses even if the band is entered; `min_off` blocks an immediate
  restart; `max_on` forces OFF; a sign flip cancels; and the pump/valve invariant
  (`!(comp && suct)`, valve follows) holds through every action the function can return.
- Verify: PASS + `idf.py build` clean.
- Commit.

Task 6 — run it: tick task, filter, wiring into the pins
- Add `hold_task`: `vTaskDelayUntil` at `HOLD_TICK_MS` (10 ms). Each tick: `sense_read()`,
  EMA filter (`r += (raw - r)/4`), `hold_decide()`, then apply via the existing `write_pins()`
  (valve follows the pump — already enforced).
- Concurrency: the console loop and the task both touch the pins/state. One FreeRTOS mutex
  (`hold_lock`) around `write_pins` + hold state; **do not** restructure the console loop (a
  non-blocking select rewrite would put the USB-Serial-JTAG EOF/EWOULDBLOCK handling back on the
  table, and that cost an evening already).
- Manual commands (`off`/`suction`/`compression`/`toggle`/`set`) cancel hold (set `enabled=false`),
  so the buttons in the app keep meaning what they say.
- Verify: `idf.py build`; on hardware, `hold 12k` from the CLI and watch `err` fall to within the
  deadband, `off` returns to manual.
- Commit.

Task 7 — safety, because a stuck loop with a live pump is the failure that matters
- `HOLD_DUTY_WINDOW_MS` (5 s) with a max on-fraction (e.g. 60 %): a leak that a pump can never win
  must not run a pump continuously.
- Stall: if `|err|` hasn't improved by `deadband/2` within `HOLD_STALL_MS` while pumping →
  `ERR stalled` + all pins low (blocked tube, dead pump, kinked line).
- Travel timeout per commanded move (`HOLD_TRAVEL_TIMEOUT_MS`) → same.
- Sensor fail (`r < 0` or `r > SENSOR_MAX`) → hold disabled, all pins low, `ERR`; never pump blind.
- Test: extend the pure-function test with the budget/stall inputs; assert pumps end low in each
  abort path.
- Commit.

## Phase 2 — host

Task 8 — CLI: `hold 12k`, `hold off`, exit codes as today; `status` already prints the fields.
- Verify: `--selftest` + the pty round trip.
- Commit.

Task 9 — app: target entry (kΩ) + Hold/Release, and a readout showing target, R, err, and the
deadband; poll 1 s normally, 200 ms while holding (display only — the loop is on the board, so
this is cosmetic). Extend `test/test_app_link.py` for the new parse/format.
- Verify: `python3 test/test_app_link.py` FAILURES: 0, plus a real Xvfb run + screenshot.
- Commit.

## Phase 3 — only if the deadband can't hold (trigger, not default)

Trigger: measured peak-to-peak R at hold > 2× what the application needs, or the pulse cadence is
audible/objectionable, or duty > ~50 %.

1. **PWM the pump at fixed frequency, duty ∝ filtered err** (that's the proportional controller;
   add I for the leak, anti-windup, keep D off). ~20 lines given Task 5's structure.
2. **Hardware first, honestly:** a bleed orifice turns the leak into a known constant (steady
   state with no limit cycle); a small vacuum reservoir + check valve makes the pump run rarely
   and quietly; shorter/wider tubing cuts the dead time. Any of these is usually a better
   purchase than another gain.

## Files likely to change

- Create: `host/glove_probe.py`, `.hermes/plans/…` (this file)
- Modify: `main/glove_pump.c`, `host/glove_pump.py`, `host/glove_pump_app.py`,
  `test/test_glove_pump.c`, `test/test_app_link.py`, `README.md`, `host/README.md`

## Tests / validation

- `test/test_glove_pump.c` — parser, pump/valve invariant, sensor maths, and now the whole
  `hold_decide` decision table including its abort paths.
- `test/test_app_link.py` — parsing/formatting of the hold fields.
- `host/glove_probe.py --selftest` — stats on synthetic curves.
- A **simulated plant** check (host, no hardware): R moves by flow·pulse and decays at the measured
  leak rate; assert the controller holds inside the band for 60 s of simulated time and that duty
  is sane. This is the only way to validate the control logic before touching the rig; it belongs
  in the C test as a second pass over `hold_decide`.
- Hardware: probe run at both ends; then a 5-minute hold at 12 kΩ logging R (peak-peak), pulse
  count, and duty.

## Risks, tradeoffs, open questions

1. **Dead time vs deadband.** Tubing volume and pump spin-up can put the response delay above the
   time R spends crossing the band → overshoot each pulse, and no hysteresis setting fixes that.
   Phase 0 #3 answers it before any controller code is written.
2. **Sensor noise floor sets the deadband**, and the mapping halves the resolution at the closed
   end (25 Ω/mV vs 11 Ω/mV). If the application needs ±50 Ω at 9 kΩ, the ADC at 12 dB atten will
   not deliver it — that needs a different front end (more averaging, lower atten + re-ranging, or
   an external ADC), not better control.
3. **Glove mechanics dominate at the ends.** If R is flat near fully open (the glove bottoms out
   mechanically) then "hold at 9 kΩ" is open-loop in that region — the number to hold is the
   pressure, and the sensor can't see it. Phase 0 #5 (hysteresis) exposes this.
4. **Pump life and noise**: 10 ms dwell limits mean hundreds of pulses a minute at steady state.
   If that's audible, the answer is the reservoir/bleed in Phase 3, not slower control.
5. **Wear on the valve**: the design deliberately fires one direction at a time to avoid valve
   chatter, but a target change still switches under load.

Open questions for the operator:
- What band is acceptable (in ohms), and what is the glove used for? (Sets whether the ADC
  resolution is even adequate.)
- When pressure is released, does the glove rest open, closed, or in between? (Sets whether one
  end self-holds, and which direction "leak" pushes.)
- Tubing length/inner diameter, and pump model — or just measure dead time with the probe.
- Is the glove worn during holds (skin contact, mechanical load changes)? That changes the
  effective leak and makes the tuning conservative.
