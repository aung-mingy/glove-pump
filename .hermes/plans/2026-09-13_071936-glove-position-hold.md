# Glove hold — as built

**Goal:** keep the sensor near a target the host sets by pulsing the on/off pumps. ±1 kΩ is
acceptable. `hold <ohms>` / `hold 12k` / `hold off`, or the app's Hold/Release buttons.

**Approach:** deadband + dwell on the board, one direction at a time. No PID (the actuator is
on/off, so PID would just be PWM + PI with three gains to tune instead of two thresholds), no
probe run, no PWM, no host-side loop.

```
err = R - target                      (+ = too closed -> suction lowers R)
in band                        -> pumps off
bite shorter than MIN_ON       -> keep going
bite at MAX_ON or in band      -> stop
stopped < MIN_OFF_MS ago       -> stay off
else                           -> suction if err > 0, else compression
```

The leak is free actuation: over-vacuuming decays back on its own, so only the toward-target
pump ever fires and the valve doesn't flap. Error is bounded by the band by construction.

**Where:** `main/glove_pump.c` — `hold_next()` (pure, tested directly), `hold_step()` (the
100 Hz-equivalent tick at 50 ms: ADC read, decision, pins, under a mutex shared with `status()`
so the console can't interleave with a bite), `hold_task()` at 50 ms. Console commands cancel
the hold. Host: `host/glove_pump.py` (`build_command` + `parse_ohms`), `host/glove_pump_app.py`
(target entry, Hold, Release, readout, `hold_text()`).

**Knobs** (top of `main/glove_pump.c`): `HOLD_DEADBAND_OHMS 1000`, `HOLD_MIN_ON_MS 100`,
`HOLD_MIN_OFF_MS 200`, `HOLD_MAX_ON_MS 400`, `HOLD_STALL_MS 15000`, `HOLD_TICK_MS 50`.
Constaint to respect: a minimum bite must move less than the band and more than the pump's
spin-up, i.e. `MIN_ON × flow < DEADBAND`.

**Safety:** unreadable sensor → stop, pins low, `hold=stalled`; no progress while outside the
band for 15 s → same; targets limited to 1 kΩ–100 kΩ.

**Done and verified:** firmware builds clean on esp32c3; C test passes with the decision-table
walk plus a simulated glove through the real `hold_step()` (settles at 12.68 kΩ for a 12 kΩ
target, pumps on 53/400 ticks, worst error 1040 Ω = band + one tick of leak), plus stall and
sensor-fail paths ending with pins low; CLI selftest and app tests pass; app driven under Xvfb
against a fake board (Hold/Release/bad input/stalled label/screenshot).

**Bugs the tests caught on the way** (all were real, all fixed): `toggle` read its position
*after* the hold cancel dropped the pins, so it was stuck going to suction; `hold_cancel()`
clobbered the pins when no hold was running, undoing a manual `set gpio 1 low`; the stall guard
treated a settled hold (error oscillating inside the band, no new best) as no progress and shut
the loop off after 15 s.

**If it can't hold tightly enough:** lower `MIN_ON` (down to the pump's spin-up), or open the
band. If the pump is fast enough that even a minimum bite overshoots, that's the point where
PWM at a fixed frequency (duty ∝ filtered error) or hardware (bleed orifice, vacuum reservoir,
proportional valve) is the answer — not a different control law.

**Open questions:** the pump's flow rate and spin-up, and the leak rate, are unknown until it
runs on the rig. They set `MIN_ON`. Measure with `glove_pump.py status` while pulsing by hand.
