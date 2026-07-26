# Rebuilding a dead Litter-Robot 2 control board

**Doc:** LR2-Redux / Revision Notes · **Rev.** 2.0 · **Subject board:** LITTER-ROBOT Rev.2C (2003, Intensa Inc.) · **Status:** Complete — running in the real unit · **Repo:** [github.com/micgresham/lr2-redux](https://github.com/micgresham/lr2-redux)

The original control board died — not the sensors, not the switches, not the motor. Instead of hunting for a 20-year-old replacement for a dead PIC microcontroller, this project replaces the whole control board with an ESP32 and reuses every other stock part exactly as it was: the gearmotor, the hall sensors, the weight switch, the anti-pinch switch, the wiring harness itself. This page is the story of how that went, in the order it actually happened — including the parts that didn't work the first time. As of this revision, the custom PCB is built, installed, and running the real robot end to end. Full source is on GitHub at [micgresham/lr2-redux](https://github.com/micgresham/lr2-redux).

| | |
|---|---|
| Harness pinout | **Fully resolved** |
| Hall sensors | **Installed in chassis** |
| Weight / anti-pinch split | **Done, bench-verified** |
| Motor driver | **DRV8871, hardware-verified** |
| Firmware | **Builds clean, both targets** |
| Custom PCB | **Fabricated, installed — working** |
| Real-hardware cycle timing | **Confirmed working (placeholders kept as-is)** |
| WiFi `ASSOC_LEAVE` ghost (§09) | **Resolved — hasn't recurred since PCB install** |
| **Project** | **Complete** |

## Contents

1. [The problem](#01-the-problem)
2. [Reading the corpse](#02-reading-the-corpse)
3. [The series-wiring trap](#03-the-series-wiring-trap)
4. [Design decisions](#04-design-decisions)
5. [Picking the parts](#05-picking-the-parts)
6. [Bring-up, and the harness split](#06-bring-up-and-the-harness-split)
7. [Firmware, and its bugs](#07-firmware-and-its-bugs)
8. [The dashboard](#08-the-dashboard)
9. [Chasing a WiFi ghost](#09-chasing-a-wifi-ghost)
10. [Where it stands](#10-where-it-stands)

---

## 01 · The problem

The stock board is stamped **"LITTER-ROBOT Rev.2C, Copyright (C) 2003 Intensa Inc."** — a PIC16C622A-based controller that simply died. Not the Home/Dump hall-effect position sensors, not the weight or anti-pinch switches, not the gearmotor, not the harness connecting them all — those were all still good. The brain was gone.

The original chip is old, unsupported, and effectively irreplaceable without reverse-engineering its firmware from scratch. Rather than try to source or clone a 20-year-old PIC controller, the plan became: replace the whole control board with an ESP32 and an H-bridge motor driver, and wire every still-good stock part into it exactly as it was — the 12V gearmotor, the hall sensors and their existing magnets on the globe, the cat-weight switch, the anti-pinch switch, the wiring harness itself.

## 02 · Reading the corpse

Before any of the new electronics could go in, the stock harness's actual wiring had to be established — not assumed. Early attempts to find a pinout online turned up two AI-generated summaries that contradicted each other (one literally hedged *"Pin 1: Positive/Negative"*) and didn't match anything verifiable. Those were discarded outright.

What actually resolved it was a multimeter, going pin by pin across both connectors on the physical board, checked against someone else's teardown of the same board family (a 2020 student reverse-engineering writeup that turned out to describe the exact same connector, down to the wire colors).

| Connector | Carries | Resolved via |
|---|---|---|
| 4-pin | Motor (to driver IC "U5") | Continuity trace |
| 7-pin | Hall sensors + weight/anti-pinch switches | Continuity trace + bench test |

Opening the hall sensor package itself settled the last unknown: two **Allegro A1101EUA-T** hall-effect ICs, mounted facing each other, open-drain outputs pulled up through a 330Ω network on the board. A part number, not a guess.

## 03 · The series-wiring trap

Pins 6 and 7 of the 7-pin connector looked, at first, like two independent switch inputs: the weight switch and the anti-pinch switch, each presumably grounded on its own. Wiring pin 6 alone to a pulled-up GPIO read *nothing*.

Tying pin 7 to ground made pin 6 start working — which meant the two switches weren't independent at all. They were wired in **series**, on one shared loop, with the original PIC almost certainly using one of its own pins as a firmware-driven "virtual ground" for the whole loop rather than true board ground.

> **Why this matters, not just a wiring curiosity**
>
> Cycling only ever starts after the cat has already left — the weight switch is already open by the time the globe moves. On a shared series loop, a mid-cycle pinch produces **zero observable change** on that one signal. It's not an ambiguous read; a truth table proves no amount of clever firmware can tell "cat gone, cycling normally" apart from "cat stuck under the globe" on a single combined wire.

The same failure mode, independently, in someone else's teardown of this exact board family:

> **Independently confirmed, 2020 teardown of the same board**
>
> *"...totally stupid, [it makes it] impossible to distinguish a cat in the litter from a cat stuck under the globe."*

Motor current/stall sensing was considered as a software-only workaround and rejected — too failure-prone on an old, uncalibrated gearbox without real bench work to back it up. The real fix was physical: open the case, find the internal jumper joining the two switches, cut it, and give each switch its own independent ground return. See §06 for how that actually went.

## 04 · Design decisions

A few choices made early on show up everywhere else in this project:

- **WiFi/MQTT are runtime config, not compile-time secrets.** Set once through a captive setup portal and stored in NVS — the same firmware image works on any unit, no per-device rebuild.
- **The browser never speaks MQTT.** The dashboard was originally going to connect to the MQTT broker directly (MQTT-over-WebSockets), but that needed a broker websocket listener and pulled in polyfills for no real benefit. It talks to a plain WebSocket server running on the ESP32 itself instead — simpler network setup, zero broker dependency for the dashboard.
- **Setup mode is reboot-based.** Hot-swapping between "AP + captive portal" and "STA + dashboard" inside one running process is fiddly with two async servers. Instead: flag it in NVS, restart, let boot branch cleanly. Boring, and it works.
- **Home and Dump are defined by GPIO, not by which stock sensor is which.** The original board's "sensor A / sensor B" identity never actually mattered for a rebuild — whichever physical sensor ends up mounted at each position just wires to that position's GPIO.

## 05 · Picking the parts

None of this needed to be exotic. The plan was always to keep the gearmotor, the switches, the sensors, and replace only the dead brain — so the new-parts list stayed short, and mostly boring on purpose.

| Part | Why this one |
|---|---|
| ESP32, mini form factor | Has to physically fit where the old board sat. A full-size DevKit board — which is what earlier bench testing actually used — doesn't fit the original enclosure and mounting points; the mini does. |
| DRV8871 H-bridge | Production motor driver as of 2026-07-07, switched from an L298N (see below). Drives the single stock 12V gearmotor directly off IN1/IN2 with no separate enable pin — one less GPIO to route on the eventual custom PCB, and integrated flyback protection instead of relying on a breakout board having its own diodes. |
| 12V → 5V buck converter | A clean, isolated logic supply for the ESP32 and hall sensors, deliberately kept off the motor driver's own noisy 12V-derived rail — so motor switching noise doesn't ride into the logic side. |

**Switched from an L298N to a DRV8871, 2026-07-07.** The L298N was the original choice — already on hand, and it drove the gearmotor without any trouble on the bench. The DRV8871 replaced it on production boards specifically because it needs no separate enable/PWM pin, simplifying the eventual custom PCB's routing by one GPIO; firmware now PWMs whichever of IN1/IN2 is the active direction directly, reproducing the same softened speed the old ENA line gave. The breadboard/diagnostic bring-up setup still uses the L298N, on purpose — no reason to touch a working bench tool. Installed on the real unit and confirmed working the same day.

That custom PCB — sized to match the original controller's physical footprint, so the new board mounts exactly where the old one did instead of sitting somewhere else on loose wires — came back from fabrication looking great: clean routing, correct footprint, no post-fab rework needed. See §10 for how it performed once installed in the actual unit.

## 06 · Bring-up, and the harness split

Before trusting the real state machine with a live gearmotor, a standalone diagnostic firmware image came first — its own SoftAP, no WiFi credentials or MQTT needed, one page showing live raw and debounced state for every sensor input plus manual motor jog controls (with safety interlocks: auto-stop on a tripped weight switch, a dropped browser tab, or a lost WebSocket). It's what actually proved the series-wiring theory from §03 on the bench, and what verified the fix afterward.

The fix itself: open the case, locate the jumper wire bridging the weight and anti-pinch switches, cut it, run a new ground return to each. The harness's outward wiring didn't need to change — the same two conductors that always went to the connector now simply carry two independent signals instead of one shared loop.

*Fig. 1 — internal jumper cut, independent ground returns wired to each switch. (Photo placeholder — not yet supplied.)*

Bench-verified afterward with all three test cases — weight alone, anti-pinch alone, both together — each reading independently on the diagnostic tool, exactly as the truth table predicted they should.

> **A second bug, caught the same day**
>
> The anti-pinch switch turned out to be **normally-closed, opening on trip** — reading "on" at rest and "off" when actually pinched, the reverse of every other input in the project. Missed, this would have made the safety interlock trigger constantly during normal operation while missing a real pinch entirely — about as bad as a safety bug gets. Fixed with a per-input polarity flag rather than assuming one global convention.

## 07 · Firmware, and its bugs

The state machine grew from a simple two-phase cycle into something closer to the original board's actual behavior: `BOOT_HOMING → IDLE ⇄ CAT_PRESENT → WAIT_TIMER → CYCLE_TO_DUMP → CYCLE_DUMP_PAUSE → CYCLE_DUMP_SHAKE → CYCLE_TO_HOME → CYCLE_HOME_OVERSHOOT → CYCLE_HOME_SETTLE → IDLE`, plus `SAFETY_STOP` and `FAULT`. Real hardware testing found real bugs in it:

> **The motor never actually reversed**
>
> The first real MQTT-triggered cycle test: "it just kept turning until it found home and that caused it to jump over the stop for dump." The Dump→Home transition changed a state variable without ever stopping or reversing the motor — it sailed straight through Dump and only stopped by chance, finding Home again on the far side of a full rotation.

> **Homing overshot, even after that fix**
>
> `BOOT_HOMING` ran forward with no idea which direction Home actually was from an arbitrary starting position. Redesigned to seek Dump first — the same direction as a normal cycle's first leg — then reverse to Home from there. That works no matter where the globe happens to be sitting, and it reuses direction logic that was already proven correct instead of trying something new and unproven.

The status LEDs went through two designs before landing on the right one: a bi-color (2-pin) LED faking amber by blinking red and green together, corrected once the actual hardware turned out to be three fully discrete LEDs on three separate GPIOs. The corrected version replicates the original board's documented light language exactly — solid green for standby, solid yellow while cycling, solid red for an interruption, flashing red for an overweight globe, flashing yellow for a detected pinch.

## 08 · The dashboard

The ESP32 hosts its own web dashboard over LittleFS — no separate server, no app to install. It grew from a single status page into three tabs: live status and actions, a usage-analytics view (visit history, average time between visits split into day and night), and settings covering WiFi/MQTT/behavior plus browser-based firmware updates.

One recurring theme: anything involving *time* needed care. The board's clock is deliberately UTC-only (no DST math to get wrong) — which means "today" can never be computed on the device itself without knowing the viewer's timezone. Visit counts and day/night classification are computed client-side, in the browser, against each visit's local time — not trusted from the device's own rolling-window math, which quietly counted a visit from the previous evening as "today" depending on what hour it happened to be when the page loaded.

## 09 · Chasing a WiFi ghost

The most stubborn bug so far wasn't in the litter-robot logic at all — it was WiFi refusing to associate, intermittently, logging nothing more informative than `ASSOC_LEAVE`. Each theory got tested and, mostly, ruled out rather than assumed:

- **Dirty resets left the network stack corrupted** — plausible, and a real fix (tearing down WiFi/lwIP after a brownout/watchdog/panic reset) — but it recurred on a genuinely clean power-on reset, which has nothing "dirty" to clean up. Not the (whole) answer.
- **Motor driver noise during homing** — the motor runs immediately on boot, independent of WiFi, so the timing lined up on paper. Directly checked against the status LED: no correlation. Ruled out.
- **Power supply** — tested on a buck converter and plain USB both; failed on both at different times, with no brownout message ever logged. Inconclusive at best.

Rather than guess at a fourth explanation, it seemed better to add logging instead of another theory: signal strength now shows up on the dashboard, the reset cause prints on every boot (not just suspicious ones), WiFi connect attempts get logged with timing, and — on the chance the home network has more than one access point sharing an SSID — the board now scans first and connects to whichever AP actually has the strongest signal, instead of whichever one it happens to see first.

> **A separate, related bug found along the way**
>
> mDNS (`lr2redux.local`) turned out to initialize exactly once, ever — the first successful connect after boot — and never re-arm itself after a reconnect. Direct-IP access kept working the whole time, which is what made it look "sporadic" rather than broken: two features that don't share any state, one silently going stale while the other kept working.

The root cause was never pinned down to a single confirmed mechanism — but the issue has not recurred since the custom PCB (§05) replaced the breadboard wiring. Whatever combination of cleaner grounding, decoupling, or simply eliminating loose bench jumpers was responsible, `ASSOC_LEAVE` hasn't shown up again. This section, once a standing mystery, is now closed.

## 10 · Where it stands

The custom PCB (§05) came back from fabrication looking great — clean routing, correct footprint, dropped straight into the original enclosure where the dead board used to sit. It's now installed in the actual Litter-Robot 2, wired to every stock part this project set out to reuse: the gearmotor, both hall sensors, the weight switch, the anti-pinch switch, the manual button, all three status LEDs.

Every piece of it works. Homing finds Dump and then Home correctly on boot. A normal cycle runs the full sequence — forward to Dump, pause, shake, reverse to Home, overshoot, settle — with none of the motor-direction or timing bugs found earlier in testing (§07) surviving into real use. The weight switch and anti-pinch switch behave as two fully independent signals, exactly as the hardware split (§06) was meant to guarantee. The dashboard, MQTT/Home Assistant integration, and OTA updates all work over WiFi that, notably, has stayed connected — the `ASSOC_LEAVE` ghost from §09 hasn't reappeared since this board replaced the breadboard wiring.

The cycle-phase timing constants (dump pause, shake, home overshoot, segment timeouts) were never re-measured with a stopwatch — they're the same placeholder values guessed early in the project. They didn't need to change: the placeholders turned out to be good enough, and the real unit cycles correctly with them left exactly as they were.

**The project is complete.** A Litter-Robot 2 whose original 2003 control board had died is now running on a custom ESP32-based replacement, reusing every stock mechanical and sensing part exactly as it was, with a working dashboard, Home Assistant integration, and OTA update path — installed, wired, and cycling on its own in the real unit.

---

*LR2-Redux — revision notes, compiled from the project's own build log. Source: [github.com/micgresham/lr2-redux](https://github.com/micgresham/lr2-redux). See `README.md` in the repo for full technical detail.*
