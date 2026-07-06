# lr2-redux

Replacement control board for a **Litter Robot 2** (stock board: "LITTER-ROBOT
Rev.2C, Copyright (C) 2003 Intensa Inc.") — the original control board itself
died. The hall-effect Home/Dump position sensors, weight switch, anti-pinch
switch, gearmotor, and wiring harness were all still good — **not** what
failed — and this project reuses every one of them as-is. (An earlier
version of this file incorrectly said the hall sensors themselves had
failed; corrected 2026-07-06 per the user.) Rebuild target: ESP32-WROOM-32 +
L298N H-bridge, reusing the stock 12V gearmotor, stock cat-weight switch,
and stock wiring harness wherever possible.

Full technical detail (BOM, wiring, pinout, state machine) lives in
`README.md` and `web/README.md` — this file is oriented instead toward *why*
things are the way they are, and what's still open, so a session on a new
machine doesn't have to re-derive it.

## Status: firmware now build-verified (2026-07-04); web app check in progress

Every dev session before 2026-07-04 had **no compiler or Node.js available**.
As of 2026-07-04, both showed up on the machine (PlatformIO 6.1.19 with the
espressif32 platform + toolchain already fully downloaded; Node v26.4.0 /
npm 11.17.0) — **still worth re-checking at the start of a new session** in
case it's a different machine, but don't assume absence anymore without
checking `which pio node npm` first.

`pio run -e esp32dev` (the production firmware) and `pio run -e diagnostic`
(the new hardware bring-up tool, see below) both **build clean, zero
warnings, zero errors** as of 2026-07-04. One local setup step was required
and is *not* a code problem: `include/secrets.h` didn't exist yet (it's
gitignored, per the README's own step 1) — copied from
`secrets.h.example` to unblock the build. Flash usage: esp32dev 73.8%,
diagnostic 63.5%; RAM ~13-15% either way — comfortable headroom on both.

The web dashboard also build-verified clean the same session: `npm install`,
`npx tsc --noEmit`, and `npm run build` (`tsc -b && vite build`) all
succeeded with zero type errors, output `dist/index.html` +
`dist/assets/index-*.js` (415KB / 125KB gzipped). `npm install` reported 2
audit vulnerabilities (1 moderate, 1 high, transitive deps) — not addressed,
just noting they exist; re-run `npm audit` for details if that matters
before a real deployment.

**So as of 2026-07-04, the whole codebase is build-verified**: firmware
(both environments) and web dashboard all compile clean. This doesn't mean
correct-on-hardware — no physical assembly has happened yet — but "should
work, unverified" from earlier sessions can be upgraded to "builds clean,
behavior unverified on real hardware."

## Architecture

**Firmware** (`src/`, `include/`), PlatformIO + Arduino framework:
- `main.cpp` — state machine (`BOOT_HOMING → IDLE ⇄ CAT_PRESENT → WAIT_TIMER
  → CYCLE_TO_DUMP → CYCLE_TO_HOME → IDLE`, plus `SAFETY_STOP` and `FAULT`),
  MQTT (Home Assistant only), WebSocket dashboard server, OTA, NVS-persisted
  cycle/drawer counters, mode branching (normal vs. setup).
- `config.h/cpp` — `DeviceConfig` struct (wifiSsid/Pass, mqttHost/Port/User/Pass,
  waitTimerSec), stored in NVS namespace `lr2net`. Also the force-setup flag
  + `ESP.restart()` helper used to cleanly transition into setup mode.
- `config_api.h/cpp` — shared HTTP routes (`/scan`, `/config`, `/save`),
  registered on **both** the setup-mode portal server and the normal-mode
  dashboard server. Saving always reboots — deliberately simpler than trying
  to hot-swap WiFi/MQTT/server state at runtime.
- `setup_portal.h/cpp` — SoftAP (`LR2-Redux-Setup` / `lr2setup`) + DNS
  captive-portal redirect + a hand-written, self-contained HTML page (no
  external requests possible — there's no internet in AP mode).
- `diagnostic_main.cpp` — **standalone hardware bring-up firmware**, added
  2026-07-04, its own PlatformIO environment (`pio run -e diagnostic -t
  upload`), not part of the production image. Purpose: verify wiring and
  pin identity on the breadboard *before* trusting the real state machine
  with it — see "Diagnostic tool" below.

### Diagnostic tool (`diagnostic_main.cpp`, `env:diagnostic`)

A second, independent firmware image for hardware bring-up — zero
dependency on WiFi credentials, MQTT, or LittleFS. It boots straight into
its own SoftAP (`LR2-Redux-Diag` / `lr2diag123`) and serves one
self-contained page (embedded in PROGMEM, no build step) at
`http://192.168.4.1/` showing:
- Live sensor state for all 5 digital inputs — hall Home (GPIO34), hall
  Dump (GPIO35), weight switch (GPIO32), manual button (GPIO33), and a
  **temporary aux test input (GPIO14)** — each showing both the raw
  instantaneous reading and the debounced/stable state plus time-since-last-change,
  so you can tell a clean signal from a floating/noisy pin.
- Motor jog controls (forward/reverse, press-and-hold, adjustable PWM
  0-255) plus a stop button.

Why `PIN_AUX_TEST` (GPIO14) exists: it's there specifically to help close
the harness worksheet's open item about whether the stock 7-pin connector's
pins 6/7 are one combined weight/anti-pinch circuit or two independent
switches (see the harness worksheet artifact). Wire whichever of 6/7 isn't
already on GPIO32 to GPIO14 temporarily, actuate each switch separately, and
watch whether they change independently on this page. Safe to leave
unconnected otherwise. (Originally GPIO13 — moved to GPIO14 in 2026-07-06
when the project switched to an ESP32 mini board whose header doesn't
break out GPIO13; see the dated entry further down.)

Safety interlocks, since this tool jogs a real gearmotor outside the state
machine's normal guardrails: the motor auto-stops if the weight switch
trips (mirrors production `SAFETY_STOP`), if the browser stops sending its
~200ms keepalive for >500ms (covers a closed tab, dropped WiFi, or a stuck
button), or on WebSocket disconnect.

Build/flash independently of the production firmware:
`pio run -e diagnostic -t upload`. Reflash `-e esp32dev` to return to normal
operation — they're separate images, not a runtime mode switch.

**Web dashboard** (`web/`), Vite + React + TypeScript + Grommet:
- `hooks/useLr2Socket.ts` — plain browser `WebSocket` to `/ws` (no MQTT
  library in the browser at all — that was an explicit architecture change
  mid-project, see "Key decisions" below).
- `hooks/useDeviceConfigApi.ts` — talks to the same `/scan` `/config` `/save`
  HTTP routes the setup portal uses.
- `components/SettingsCard.tsx` — WiFi/MQTT/wait-timer config UI + scan.
- Built via `npm run build:device` (outputs to `../data`) and flashed to the
  board's LittleFS partition (`pio run -t uploadfs`) — **the ESP32 serves
  the dashboard itself**, at `http://lr2redux.local/`. It auto-detects
  being self-hosted via `window.location` and needs no configuration in
  that case; `npm run dev` still works standalone for development.

## Key decisions (the non-obvious "why")

- **MQTT is ESP32↔broker only; the browser never speaks MQTT.** Originally
  the dashboard connected to the MQTT broker directly via MQTT-over-WebSockets
  (mqtt.js), which required a Mosquitto websocket listener and pulled in
  Buffer/global polyfills. Moved to a plain WebSocket server on the ESP32
  itself (`ESPAsyncWebServer` + `ArduinoJson`) at the user's explicit request
  — simpler network setup, no broker dependency for the dashboard at all.
- **WiFi/MQTT are runtime config, not compile-time secrets.** `secrets.h`
  now holds only `OTA_PASSWORD`. Everything else lives in NVS, set through
  the setup portal. This means the same firmware image works on any unit —
  no per-device rebuild needed.
- **Setup mode is reboot-based, not a runtime mode switch.** Hot-swapping
  between STA+dashboard-server and AP+portal-server within one running
  process is fiddly and easy to get subtly wrong with two async servers.
  Instead: set an NVS flag, `ESP.restart()`, and let `setup()` branch cleanly
  on boot. Simpler, more reliable, easy to reason about.
- **Hall sensors were bench-tested on the breadboard before permanent
  install; both are now finalized and installed in the actual LR2 chassis
  (2026-07-06), wired through the stock 7-pin connector like the weight/
  anti-pinch switches always were.** Superseded: don't reintroduce the
  "hall sensors are bench-only, wired separately from the harness"
  framing if it shows up in older context — that described the bring-up
  phase only. See the "Home vs. Dump" entry below for the final pin 2/3 →
  GPIO mapping.
- **The weight switch and manual button use internal pull-ups (`INPUT_PULLUP`
  on GPIO32/33); the hall sensors need external 10kΩ pull-ups** because
  GPIO34/35 are input-only pins with no internal pull capability in
  hardware. This was actually a real bug caught and fixed mid-project — an
  earlier version of `DebouncedInput::begin()` used plain `INPUT` for
  everything, silently leaving the weight switch/button floating.
- **Pull-up resistors for the hall sensors go to 3.3V, not 5V** — ESP32
  GPIOs aren't 5V-tolerant, but the A3144-class hall ICs need ≥4.5V to
  operate. Power the sensor from 5V, pull its open-collector output to
  3.3V. Documented in the breadboard diagram and README.
- **The weight switch and anti-pinch switch get physically split apart,
  not read as the stock shared circuit.** The stock harness wires them in
  series on one 2-wire loop — bench-confirmed, and independently confirmed
  by a third party's teardown of this same board family. This isn't a
  cosmetic ambiguity: since cycling only happens after the cat has already
  left (weight switch open by design), a mid-cycle pinch produces **zero
  observable change** on that combined signal, not just an ambiguous one.
  Software-only fixes were seriously considered and rejected (see "Pins 6/7
  topology" below) — toggling which pin drives/senses doesn't work
  (proven with a truth table, it's a hard information-theoretic limit of a
  series loop, not a code problem), and motor current/stall sensing as a
  workaround was rejected for now as too failure-prone with an old
  motor/gearbox without real calibration work. The chosen fix is physical:
  cut the internal jumper connecting the two switches and give each its own
  ground return. Firmware (`PIN_ANTI_PINCH`, independent `SAFETY_STOP`
  interlock) plus the hardware split itself are both done and
  bench-verified as of 2026-07-05 — including catching and fixing a real
  polarity bug along the way (the anti-pinch switch is active-HIGH, not
  active-low like everything else; see "Pins 6/7 topology" below).
- **`WiFi.mode(WIFI_STA)` must be called before `server.begin()` in normal
  mode.** Real crash, caught 2026-07-05 on first real hardware boot after
  completing WiFi setup: `assert failed: tcpip_api_call ... (Invalid
  mbox)`, boot-looping forever. Root cause: `setup()` called
  `server.begin()` (AsyncWebServer/AsyncTCP) while WiFi hadn't been put
  into any mode yet — `WiFi.mode(WIFI_STA)` + `WiFi.begin()` only happened
  later, inside `connectWifiIfNeeded()` in `loop()`. Starting the async
  TCP stack before any network interface exists trips this assert.
  `diagnostic_main.cpp` never hit this because it calls
  `WiFi.mode(WIFI_AP)` *before* `server.begin()` in its own `setup()` —
  that ordering difference is what gave away the root cause. Fixed by
  adding `WiFi.mode(WIFI_STA);` in `main.cpp`'s `setup()` right before the
  async server block; `connectWifiIfNeeded()` still owns the actual
  `WiFi.begin()` call and reconnect loop, unchanged. **Lesson**: any
  ESPAsyncWebServer/AsyncTCP `server.begin()` needs a WiFi mode set first,
  even if the actual connection attempt happens later — check this first
  if a similar boot-loop assert shows up again.
- **mDNS was already implemented** (`MDNS.begin("lr2redux")` in
  `setupOtaIfNeeded()`, gated on first successful WiFi connect) but was
  never reachable before the above crash was fixed, since WiFi never
  connected. Also added `MDNS.addService("http", "tcp", 80)` (proper
  mDNS-SD service advertisement, previously just hostname resolution) and
  a `Serial.printf` of the IP address on connect, since mDNS/Bonjour isn't
  reliable on every network (guest WiFi, some enterprise networks block
  multicast, Windows needs Bonjour installed) — the serial-printed IP is a
  robust fallback that always works. **Superseded 2026-07-06**: this was
  gated on a one-time latch that never re-armed mDNS after a reconnect,
  which turned out to be a real, separate bug — see the dated entry
  further down.
- **`CYCLE_SEGMENT_TIMEOUT_MS` raised twice, 15s → 90s → 180s (2026-07-05),
  still pending a real measurement.** First real end-to-end MQTT cycle test
  (via Home Assistant, WiFi crash above already fixed) faulted after
  exactly 15s. Confirmed via the diagnostic tool that all hall sensors are
  mounted in the actual LR2 and behave correctly (only active right at
  Home/Dump, off everywhere in between — not a sensor problem). Confirmed
  with the user that the globe **did** physically rotate during the
  attempt, it just didn't reach Dump in time — so the timeout value itself
  (never validated against real hardware before this week) was simply too
  short. Raised to 90s; **that also faulted, at exactly 90s elapsed** —
  raised again to 180s at the user's request ("I needs longer than 90
  seconds... we can dial it back later"). 180s is a deliberately generous
  placeholder to unblock testing, not a measured value — tighten it once
  an actual segment time is timed with a stopwatch, and note the motor
  direction bug below may have been part of why early attempts ran long
  (reversing at the wrong time / not at all could easily eat 60-90s before
  timing out on its own). `BOOT_HOMING` uses 2x this (now 360s).
- **Real bug found the same day: the motor never actually reversed
  direction.** `main.cpp` only ever had `motorRunForward()` — the
  `CYCLE_TO_DUMP → CYCLE_TO_HOME` transition just changed the state
  variable when `hallDump` triggered, without stopping or reversing the
  motor, so it kept spinning forward straight through the Dump position
  and only stopped once it happened to find Home again on the far side of
  a full rotation. The user caught this from an actual MQTT-triggered
  cycle: "it just kept turning until it found home and that caused it to
  jump over the stop for dump." Correct behavior, confirmed by the user:
  forward Home→Dump, stop, **reverse** Dump→Home — not a continuous
  one-way rotation as an earlier (unvalidated) assumption in this file and
  the README claimed. Fixed by adding `motorRunReverse()` and calling
  `motorStop(); motorRunReverse();` at the `CYCLE_TO_DUMP→CYCLE_TO_HOME`
  transition. Also fixed a latent consequence: `SAFETY_STOP`'s resume logic
  always called `motorRunForward()` regardless of which segment it
  interrupted — now picks direction based on `stateBeforeSafetyStop`
  (reverse if resuming into `CYCLE_TO_HOME`, forward otherwise). Both
  build-verified clean, **not yet hardware-verified** — next cycle test
  should confirm it actually stops at Dump and returns to Home correctly.
- **`BOOT_HOMING` redesigned the same day, after the reverse-direction fix
  above turned out not to be the whole story.** Reflashed with that fix,
  the user still saw it overshoot on homing: "it goes forward past the
  dump position to find home. It should go forward until it finds the
  dump position and then reverse back to home." The old `BOOT_HOMING` just
  ran forward until `hallHome` triggered, with no idea which direction
  Home actually was from an arbitrary starting position — meaning if the
  globe wasn't already near Home, forward might not reach it directly at
  all, and worse, continuing to assume that's always safe risks stalling
  the motor against a mechanical hard stop if one exists on that side
  (unconfirmed either way, hence treating it as a real risk rather than
  assuming a full 360° is always safe). Redesigned to two phases within
  the same state: seek Dump first (forward, identical direction to a
  normal cycle's first leg), then reverse to Home from there — deterministic
  regardless of starting position, and reuses direction logic already
  validated for normal cycling rather than a new, unproven strategy.
  Tracked via a `bootHomingFoundDump` flag, reset only on a genuine fresh
  start (initial boot, or `FAULT`'s `reset_fault` path) — **not** on a
  `SAFETY_STOP` resume back into `BOOT_HOMING`, which needed its own fix
  (see next point) since that's a continuation, not a restart. Also
  extended the weight/anti-pinch `SAFETY_STOP` interlock to cover
  `BOOT_HOMING`, since it now does a real multi-phase motor sequence just
  like a normal cycle, not a single simple spin — it wasn't covered before.
  `SAFETY_STOP`'s resume-direction logic was also updated: it now checks
  `bootHomingFoundDump` to know whether an interrupted `BOOT_HOMING` should
  resume forward (still seeking Dump) or reverse (already past Dump,
  heading to Home) — this was a necessary companion fix, not optional,
  since getting it wrong would silently resume in the wrong direction.
  Build-verified clean, **not yet hardware-verified**.
- **`waitTimerSec` now has a hard-enforced 2-minute minimum** (2026-07-05),
  at the user's request — reasoning being it needs to reliably give the cat
  time to actually leave before a cycle starts, not just clear the weight
  switch briefly. Enforced in two places: `config_api.cpp`'s `/save` handler
  (authoritative — rejects `minutes < 2` silently, per existing style of
  ignoring invalid values rather than erroring) and `SettingsCard.tsx`
  (client-side check with a visible error message before the request is
  even sent). The wait-timer field also moved into a new collapsible
  "Advanced settings" section in the dashboard, per the user's request —
  previously it sat in a plain row next to MQTT port.
- **`pio run -e esp32dev` now builds and deploys the web dashboard
  automatically** (2026-07-05), via `scripts/build_web.py` as a PlatformIO
  `pre:` extra_script. Runs `npm run build:device` (`web/` → `./data`)
  before every build/upload/uploadfs on this environment, and chains an
  automatic `uploadfs` run onto `upload` — so `pio run -e esp32dev -t
  upload` alone now puts both firmware and dashboard on the device. Fails
  loudly if Node is present but the web build itself errors; fails
  *quietly* (a warning, not a crash) if npm/node aren't installed at all,
  since tool availability has varied session-to-session on this project —
  a firmware-only build should still work without Node. Scoped to
  `env:esp32dev` only; `env:diagnostic` is untouched (verified both build
  clean after this change). Confirmed working: `data/` was freshly rebuilt
  and the plain firmware build still succeeded end-to-end.
- **Cycle sequence expanded with real dwell/shake/overshoot phases and a
  second LED, all at the user's request (2026-07-05), none yet
  hardware-verified.** Added three new `State` values on each side of the
  old two-phase cycle: `CYCLE_TO_DUMP → CYCLE_DUMP_PAUSE (5s dwell) →
  CYCLE_DUMP_SHAKE (~2.4s oscillating) → CYCLE_TO_HOME →
  CYCLE_HOME_OVERSHOOT (3s past Home) → CYCLE_HOME_SETTLE (forward back to
  Home, this is what actually ends the cycle now, not the first Home hit).
  All timing constants (`DUMP_PAUSE_MS`, `DUMP_SHAKE_STEP_MS`,
  `DUMP_SHAKE_TOTAL_MS`, `HOME_OVERSHOOT_MS`) are placeholders, same
  "unvalidated against real hardware" caveat as `CYCLE_SEGMENT_TIMEOUT_MS`.
  This touched a lot of surrounding logic that had to stay in sync:
  - `isMotionState()` helper added so the `SAFETY_STOP` interlock and
    resume-direction logic have one shared definition of "the motor is
    moving or about to," rather than repeating an ad-hoc state list in two
    places (which is exactly what caused the original motor-direction bug
    earlier this file documents).
  - `SAFETY_STOP`'s resume-direction switch now covers all the new phases:
    forward for `CYCLE_TO_DUMP`/`CYCLE_HOME_SETTLE`, reverse for
    `CYCLE_TO_HOME`/`CYCLE_HOME_OVERSHOOT`, no motor command needed for
    `CYCLE_DUMP_PAUSE`/`CYCLE_DUMP_SHAKE` (pause has nothing to resume;
    shake re-derives its own direction from `elapsed` restarting at 0).
  - **Weight-switch cooldown, at the user's explicit request**: "if the
    weight sensor is triggered during a cycle it should stop for 2
    minutes, not just resume." Anti-pinch clearing is still immediate (a
    mechanical condition, not a cat-presence question); the weight switch
    now needs `WEIGHT_SAFETY_COOLDOWN_MS` (2 min) of continuous clearness
    via a `weightClearedAt` timestamp before motion actually resumes.
    Reset to 0 on fresh `SAFETY_STOP` entry.
  - Only `CYCLE_TO_DUMP`/`CYCLE_TO_HOME`/`CYCLE_HOME_SETTLE` are
    sensor-wait-bounded and need the `FAULT` timeout; the three new
    time-bounded phases always advance on their own and don't need it.
  - **Second LED** (`PIN_WAIT_ISSUE_LED` = GPIO16): solid during
    `WAIT_TIMER`, fast blink during `FAULT`/`SAFETY_STOP`, off otherwise —
    a dedicated "waiting or issues" signal distinct from the primary
    status LED, per the user's request. Primary status LED's blink
    condition extended to cover all the new cycling sub-phases too, so it
    doesn't misleadingly go solid mid-dwell/shake/overshoot.
  All build-verified clean; the whole sequence needs real hardware
  verification before trusting the placeholder timings.
- **Usage analytics added (2026-07-05)**: a 30-day rolling hourly visit log
  (`visitLog[720]`, one byte per hour), persisted to NVS, indexed by
  absolute hours-since-epoch modulo 720 so it self-ages without an
  explicit prune step. "Visit" = `IDLE`→`CAT_PRESENT` specifically (not
  `WAIT_TIMER`→`CAT_PRESENT`, which is a re-engagement of the same visit,
  not a new one) — `recordVisit()` is called at exactly that one
  transition. Needs real wall-clock time to bucket correctly, so NTP sync
  (`pool.ntp.org`, UTC, no DST) was added, gated on first WiFi connect;
  `recordVisit()` is a no-op until `ntpTimeSynced` to avoid corrupting the
  log by bucketing into `time()`'s ~0 default pre-sync. New `/analytics`
  HTTP route returns `hourly24`/`daily30` arrays plus today/week/30-day
  totals, computed on demand (not pushed over the WS telemetry stream,
  since visit counts change far too rarely to justify that). Dashboard
  side: new `UsageCard.tsx` + `useAnalytics.ts` hook (polls every 60s),
  with a hand-built 30-day daily bar chart (plain HTML/CSS, no charting
  library) — went through the dataviz skill's procedure: single series so
  no categorical palette/validator needed, reused the app's existing
  `state-cycle`/`accent-1` theme colors rather than introducing new ones,
  verified visually in both light and dark mode via Playwright before
  calling it done (screenshots, not just typecheck). Known limitation,
  documented in README: if the board is powered off more than 30
  consecutive days, a slot not touched since could transiently show
  stale data from over a month ago — accepted rather than adding
  per-slot timestamps to engineer around a rare, low-stakes case.
- **Dashboard header/URL-bar overlap fixed (2026-07-05)**: the "Litter
  Robot 2" heading was visually crowding the device-URL connect bar right
  below it (user-reported via screenshot). Fixed with explicit
  margin/border spacing in `App.tsx` (bottom border + padding on the
  title block, tightened heading margin) rather than relying on the
  parent `Box`'s `gap` alone. Verified visually via Playwright
  screenshot before/after, not just by reasoning about the CSS.
- **Status LED redesigned around three discrete LEDs (green/yellow/red),
  replicating the original stock board's exact language (2026-07-05).**
  The user provided the original manual's status-light text verbatim:
  solid green (standby), solid yellow (cycling), solid red (cycle
  interrupted / cat sensor active), flashing red (overweight / cat sensor
  >2min), flashing yellow (pinch detect). First implementation attempt
  used a bi-color (2-channel) LED, combining red+green to fake yellow -
  the user then clarified the actual hardware: three separate physical
  LEDs on three separate GPIOs (`gpio 4 - green, gpio 16 - yellow, gpio 17
  - red`), not a bi-color part. Corrected to
  `PIN_LED_GREEN`=GPIO4, `PIN_LED_YELLOW`=GPIO16, `PIN_LED_RED`=GPIO17 (new
  pin - the bi-color version only needed 2 GPIOs, three discrete LEDs need
  a third). `updateLeds()` rewritten to drive three independent booleans
  instead of combining two - notably simpler than the bi-color version's
  "blink both together to look amber" hack, since yellow now has its own
  dedicated pin. See README's "Status LED" section for the full table.
  This fully supersedes/retires both the original two-LED ad-hoc design
  AND the bi-color LED design that briefly replaced it - don't
  reintroduce either if revisiting old context.
- **Manual-reset behavior for SAFETY_STOP, at the user's explicit design
  call**: "by default autoresume on everything except the anti-pinch
  sensor. Give me an option for the original manual intervention
  behaviour in the web settings." Implemented as:
  - Anti-pinch involvement in a `SAFETY_STOP` **always** requires manual
    reset (hard default, not configurable) - tracked via
    `safetyStopAntiPinchInvolved`, set at trigger time and upgradeable
    later if anti-pinch becomes active mid-episode even if it wasn't the
    original trigger (better to require confirmation than risk an
    incorrect auto-resume).
  - New `DeviceConfig.requireManualReset` (persisted, default `false`)
    extends manual-reset to weight-only interruptions too, matching the
    original stock board exactly when enabled. Dashboard: a checkbox in
    Advanced Settings.
  - Manual reset is satisfied by either the physical manual button
    (`manualButton.isActive()` while in `SAFETY_STOP`) or a new `resume`
    MQTT/WS command - both set the same `manualResumeRequested` flag,
    matching "press any button to reset" from the original manual.
  - New `/analytics`-style telemetry field `needsManualReset` (WS only,
    true only while `state == SAFETY_STOP` and manual reset applies) drives
    a new "Resume" button in the dashboard's Actions card, parallel to the
    existing "Reset fault" button.
- **Cat-present warning timeout, at the user's request**, matching the
  original board's documented "activated for more than 2 minutes" flashing-
  red signal: new `DeviceConfig.catPresentWarningSec` (persisted, default
  120s/2min, **enforced minimum 2 minutes, can be set higher** - same
  minimum-only pattern as `waitTimerSec`). Purely a warning, not a state
  transition - computed inline in `updateLeds()` (flashing red once
  `CAT_PRESENT`'s elapsed time exceeds it) and exposed via a new
  `catPresentWarning` WS telemetry field the dashboard uses to flag the
  "Cat present" row in red with "(a while)" appended.
- All of the above build-verified clean (firmware both envs, web
  typecheck) and the new dashboard UI (Resume button, cat-present warning
  styling, Advanced Settings checkbox + new field) verified visually via
  Playwright screenshots. **None of the new firmware behavior is
  hardware-verified** - same standing caveat as the rest of this session's
  cycle-sequence work.
- **Dashboard restructured into tabs, and a web-based OTA upload added
  (2026-07-05), both at the user's request.** `App.tsx` now wraps its
  content in Grommet `Tabs`: "Dashboard" (Status/Drawer/Actions/Usage, the
  frequently-glanced-at cards) and "Settings" (`SettingsCard` +
  the new `FirmwareUpdateCard`) - the device-URL bar and header stay
  outside the tabs since they're relevant regardless of which tab is
  active. New `POST /update` route in `main.cpp` accepts a multipart
  firmware upload and flashes it via the ESP32 `Update` library - the same
  mechanism `pio run -t upload --upload-port` uses under the hood, just
  reachable from the browser instead of the command line. Reuses
  `OTA_PASSWORD` from `secrets.h` (sent as an `X-OTA-Password` request
  header, since multipart form data doesn't cleanly carry it alongside the
  file) rather than adding a second credential - checked on the first
  upload chunk (`index==0`), tracked via a static `otaUploadAuthorized`
  flag for the rest of that upload's chunks. Stops the motor first, same
  precaution as the existing `ArduinoOTA.onStart()` handler. Dashboard
  side uses `XMLHttpRequest` rather than `fetch` specifically for upload
  progress events (fetch doesn't expose them). **Caught mid-session**: a
  Playwright screenshot of the dashboard unexpectedly connected to the
  user's actual physical device (`lr2redux.local` resolved via mDNS from
  this machine) rather than a mock - read-only, no commands sent, but
  worth remembering that this project's `lr2redux.local` may resolve to
  real hardware from sessions running on the user's own machine, not just
  in a sandboxed/mocked context.
- **IP address exposed on the dashboard (2026-07-05).** Small addition:
  `buildStateJson()` now includes `ipAddress` (`WiFi.localIP().toString()`,
  empty string if not connected), shown as a new row in the Status card.
  Useful as a fallback when `lr2redux.local` doesn't resolve (see the mDNS
  reliability note earlier in this file).
- **Board switched to an ESP32 mini form factor (2026-07-06)**, which
  doesn't break out GPIO13 on its header — `PIN_ANTI_PINCH` (and the
  diagnostic tool's mirrored `PIN_AUX_TEST`) moved to GPIO14, a plain GPIO
  with no strapping/UART/flash conflicts and no analog use in this
  project (its ADC2 channel would conflict with WiFi if read via
  `analogRead()`, but nothing here does that - it's a plain digital
  `INPUT_PULLUP` read). WiFi briefly appeared broken (`ASSOC_LEAVE`, reason
  8, repeating every `WIFI_RECONNECT_INTERVAL_MS`) right after this move,
  traced to a physical side-effect of the rewiring itself (a disturbed
  connection near the new pin, not GPIO14 or the firmware) rather than
  anything electrical about GPIO14 - confirmed once the page came back up
  with the wiring reseated. All three worksheet artifacts, their PDFs, and
  the README pinout table were updated to GPIO14 - see the "Original
  harness pinout" section below for the full pin-by-pin table.
- **Real bug found the same day: anti-pinch was silently ignored outside
  motion states.** The global safety interlock in `runStateMachine()` was
  gated behind `isMotionState(state)` for *both* the weight switch and
  anti-pinch, so a pinch condition while `IDLE`, `CAT_PRESENT`, or
  `WAIT_TIMER` never registered at all - caught from the user physically
  testing it ("the pinch sensor is not working when in idle state or when
  waiting"). That gating is correct for the weight switch (its state means
  cat presence in those states, not a safety abort) but wrong for
  anti-pinch, which is a real safety condition regardless of whether the
  motor happens to be moving. Fixed by splitting the interlock into two
  independent conditions: `weightMotionInterlock` (unchanged, still gated
  on `isMotionState()`) and `antiPinchInterlock` (checked in every state
  except `SAFETY_STOP`/`FAULT` themselves, to avoid re-triggering into
  itself every loop iteration while the pinch stays active) - either one
  now enters `SAFETY_STOP`. `stateBeforeSafetyStop` already had a `default:
  break;` in its resume-direction switch for non-motion states, so
  resuming correctly returns to `IDLE`/`CAT_PRESENT`/`WAIT_TIMER` with no
  motor command needed - that part required no change. Build-verified
  clean; **not yet hardware-verified** that a pinch during idle/waiting
  now actually triggers `SAFETY_STOP` as intended.
- **Drawer-full threshold made a runtime setting (2026-07-06), at the
  user's request.** Previously `DRAWER_FULL_CYCLES` was a compile-time
  `const` in `main.cpp` (10 cycles, "tune to taste" only meant editing and
  reflashing). Moved into `DeviceConfig.drawerFullCycles` (default 10,
  enforced minimum of 1 in `config_api.cpp` - 0 would mean "always full"),
  same persisted-in-NVS/exposed-via-`/config`-`/save` pattern as every
  other setting, with a new "Cycles until drawer full" field in
  `SettingsCard.tsx`'s Advanced settings section. Reasoning: how many
  cycles a drawer actually holds depends on litter type and number of
  cats, so it's a per-household tuning value the user should set from
  their own experience, not a fixed guess baked into firmware. Build-
  verified clean (firmware both envs, web typecheck) and visually verified
  via Playwright screenshot.

## Original harness pinout — confirmed by continuity trace (2026-07-03)

The board's own connector pinout is now resolved by direct multimeter trace,
with function labels from the user. This supersedes all earlier
fabacademy/robotshop-sourced guesses below wherever they conflict. What's
actually verified vs. not:

- **Verified** (fetched and quoted directly from
  https://fabacademy.org/2020/labs/agrilab/students/florent-lemaire/projects/final-project-steps/
  — a real reverse-engineering writeup of this board family, by someone who
  built their own replacement control board the same way this project
  does): the hall-sensor harness is a 7-pin connector — black=common GND,
  orange=sensor1(Home) OUT, yellow=sensor2(Dump) OUT, red=sensor1 VCC,
  violet=sensor2 VCC, 2 pins unidentified. The cat-entry and anti-pinch
  switches are wired **in series on one circuit** — that author hit the
  exact same "I assumed each switch was independently grounded" mistake
  before finding the real topology, and calls the series design "totally
  stupid" precisely because it makes "cat in the litter" and "cat stuck
  under the globe" indistinguishable. See the fuller discussion further
  down (under "Pins 6/7 topology") for what this means for the new board —
  it's a bigger problem than originally assumed here, not just an
  acceptable tradeoff.
- **Thin/unverified**: community.robotshop.com repair-guide pages are real
  (confirmed to exist via search) but return 403 to direct fetch and Wayback
  Machine isn't reachable from this environment — only have search-snippet
  paraphrases (motor wire pairing "green↔red, brown↔white"; a hall-sensor
  "white stripe" orientation note). Don't treat these as solid.
- **Discarded as fabricated**: the user pasted two separate "pinouts"
  sourced from Gemini summaries. Both were internally inconsistent (one
  literally said "Pin 1: Positive/Negative" — hedging both ways) and
  contradicted the verified fabacademy data (single sensor vs. two, wrong
  wire colors, invented a separate pinch-switch circuit that doesn't match
  the confirmed series-wiring finding). See `feedback_verify_ai_sourced_claims`
  in Claude's memory for the general lesson here.
- **Confirmed by multimeter continuity trace, with user-supplied function
  labels (2026-07-03)** — the actual source of truth for this board:

  **4-pin connector — motor connector:**
  | Pin | Function |
  |---|---|
  | 1 | 12VDC |
  | 2 | GND |
  | 3 | U5 ("43949SLBT" driver IC) pin 7 — motor drive output |
  | 4 | U5 pin 10 — motor drive output |

  U5 is the original motor driver IC. Pins 3/4 are the two stock wires to
  reuse for the new motor connection.

  **7-pin connector — hall ("globe") sensors + weight/anti-pinch switches:**
  | Pin | Function |
  |---|---|
  | 1 | GND — common return for both hall sensors AND both switches |
  | 2 | Hall sensor "B" OUT — via 330Ω resistor network to PIC16C622A pin 12 |
  | 3 | Hall sensor "A" OUT — via 330Ω resistor network to PIC16C622A pin 10 |
  | 4 | Hall sensor "A" VCC — to PIC pin 14 |
  | 5 | Hall sensor "B" VCC — to PIC pin 14 (**same PIC pin as 4**, confirmed by a visible copper trace bridging the two pads on the PCB) |
  | 6 | Weight switch — direct to PIC pin 6, no resistor network |
  | 7 | Anti-pinch switch — direct to PIC pin 5, no resistor network |

  **Pins 2-5 fully resolved (2026-07-04)**, by physically opening the hall
  sensor package: it contains **two Allegro A1101EUA-T** sensors mounted
  facing each other. Standard A110x pinout is pin1=VCC, pin2=GND, pin3=OUT.
  User traced: both sensors' pin 2 (GND) → connector pin 1; sensor "A"'s
  pin1(VCC)/pin3(OUT) → connector pins 4/3; sensor "B"'s pin3(OUT)/pin1(VCC)
  → connector pins 2/5. This fully explains the pin 4/5 "shared net" that
  was previously just inferred — it's simply both sensors' VCC tied to one
  shared PIC-supplied power line, nothing cleverer. Datasheet-confirmed:
  A1101EUA-T is open-drain output (matches the 330Ω pull-up network found on
  pins 2/3) with VCC range 3.8-24V (5V, already used elsewhere in this
  project, is safely mid-range and confirmed correct — not a guess anymore).
  **Home vs. Dump — closed out (2026-07-05), resolved by convention rather
  than by identifying the original board's A/B sensor labeling.** The new
  build defines Home/Dump by GPIO, not by which stock sensor happens to be
  which: `PIN_HALL_HOME` = GPIO34, `PIN_HALL_DUMP` = GPIO35, and whichever
  sensor gets physically mounted at each position is simply wired to the
  correspondingly-named GPIO. The original dead board's specific A/B
  identity was never actually load-bearing for the new build (new/reused
  sensors are bench-mounted independently of the stock harness either way)
  — this was flagged low-priority for exactly that reason, and it's now
  fully closed rather than just deprioritized. Since it's a known,
  still-stocked part (Digikey), reusing this exact model for the new build
  (rather than a generic A3144) is worth considering if the originals test
  as still functional — guarantees compatibility with the existing magnet
  strength/mounting geometry.

  **Finalized in the chassis (2026-07-06):** both hall sensors are now
  permanently mounted in the actual LR2 and wired through the stock 7-pin
  connector (not bench leads) — confirmed by the user, correcting an
  artifact draft that still described them as bench-only. The physical
  pin 2/3 → GPIO mapping that resulted: pin 3 ("A" OUT) → GPIO34 (Home),
  pin 2 ("B" OUT) → GPIO35 (Dump). All 7 pins on this connector are wired
  and installed as of this date — nothing on this connector is bench-only
  anymore.

  Pins 6 & 7 are the weight-switch and anti-pinch-switch pair — see below,
  this took considerably more work to resolve correctly than 2-5 did.

  **Pins 6/7 topology - RESOLVED by bench test (2026-07-04):** wiring pin 6
  to GPIO32 (`INPUT_PULLUP`) alone read nothing. Tying **pin 7 to ESP32
  GND** made pin 6 work correctly as the weight switch signal. This means
  pins 6/7 are **not** two independently-grounded switch inputs — they're a
  loop, with the original PIC almost certainly using one of its own GPIOs
  (pin 5, i.e. physical pin 7) as a firmware-driven "virtual ground" for the
  switch loop rather than wiring it to true board GND. This **confirms**
  the earlier "thin/unverified" robotshop claim that the cat-entry and
  anti-pinch switches are wired in series on one combined circuit — it was
  right all along, just not independently verified until now.

  **Further confirmed by direct behavioral test:** pulling the anti-pinch
  switch open makes the weight switch stop toggling GPIO32 entirely,
  regardless of its own state. This is exactly what a series loop predicts
  — the anti-pinch switch is normally-closed and opens on trigger, breaking
  the whole loop at that point, so the weight switch downstream becomes
  unreadable while the anti-pinch is held open. **Practical consequence:
  GPIO32 cannot distinguish "cat present" from "pinch condition" — both
  read as the same signal.** This matters more than first assumed: cycling
  only happens after the cat has already left (weight switch open by
  design), so a mid-cycle anti-pinch trip produces **no observable change
  at all** on GPIO32 — the existing `SAFETY_STOP` check doesn't actually
  catch that case, contrary to what was first assumed here.

  **Independently confirmed by a third party, 2026-07-04**: this exact
  series-wiring problem, including someone else hitting the identical "I
  assumed each switch was independently grounded" mistake before finding
  the real topology, is documented first-hand at
  https://fabacademy.org/2020/labs/agrilab/students/florent-lemaire/projects/final-project-steps/
  — the same fabacademy.org source already cited below for the hall-sensor
  color code (black=GND, orange=S1 OUT, yellow=S2 OUT, red=S1 VCC,
  violet=S2 VCC — matches exactly). That author calls the series wiring
  "totally stupid" and confirms it's genuinely impossible to distinguish
  "cat in the litter" from "cat stuck under the globe" with this circuit as
  wired. They mention finding a fix later ("invert some wires & change
  code") but **never elaborate it anywhere on the page** — dead end, not
  worth searching further for specifics.

  **DECISION (2026-07-04): physically split the two switches, implemented
  in firmware.** Considered and rejected two software-only alternatives
  first — see the reasoning trail in this project's chat history if it
  ever needs re-litigating, summarized here so it doesn't get re-derived:
  - *Toggle which pin drives vs. senses, conditionally*: doesn't work.
    Proven with a truth table — a series loop only ever yields "both
    switches closed" vs. "at least one open," from *either* direction,
    since it's a plain resistive network with no diode/asymmetry found.
    The weight switch being open (which is exactly the case during
    `CYCLE_TO_DUMP`/`CYCLE_TO_HOME`, since cycling only starts after the cat
    leaves) already masks the anti-pinch switch's state regardless of which
    pin is doing the driving.
  - *Motor current/stall sensing as a proxy*: technically feasible and
    would sidestep the switch-wiring problem entirely, but a naive
    threshold is genuinely risky with an old motor/gearbox (inrush current,
    temperature/lubricant-dependent friction, position-dependent load,
    PWM ripple) — would need calibration against this specific motor plus
    combining with independent rotation-progress confirmation (current
    elevated *and* hall sensors not advancing) to avoid false positives/
    negatives. Rejected as the *primary* fix for now given the added
    complexity, though it remains a reasonable future defense-in-depth
    addition on top of the hardware split, not instead of it.

  **The chosen fix**: open the case, find the internal jumper wire that
  connects the weight switch and anti-pinch switch together (confirm with a
  multimeter before cutting anything — don't assume the topology), cut it,
  and run a new ground wire to each switch's now-independent terminal. The
  existing harness wires to the connector don't need to change — pin 6
  keeps going to GPIO32, pin 7 (previously the "virtual ground" side) now
  carries the anti-pinch switch's own independent signal. Both use the
  ESP32's internal pull-up, no external resistor needed, same as the weight
  switch always has.

  **Firmware implemented (2026-07-04, `main.cpp`)**: added
  `PIN_ANTI_PINCH = 13` (reusing the diagnostic tool's `PIN_AUX_TEST` GPIO,
  already validated on the bench) and a second `DebouncedInput antiPinch`.
  The `SAFETY_STOP` interlock now triggers on
  `weightSwitch.isActive() || antiPinch.isActive()`, and the resume
  condition requires **both** to clear before resuming — not just whichever
  one triggered it.

  **Hardware split completed and bench-verified (2026-07-05):** the case
  was opened, the internal jumper cut, and both switches confirmed reading
  fully independently via the diagnostic tool (all three test cases —
  weight alone, anti-pinch alone, both together — behave correctly).

  **Real bug caught during that same verification, fixed same day:** the
  anti-pinch switch turned out to be **normally-closed, opening (reading
  HIGH) on trip** — confirmed on the bench (`auxTest`/GPIO13 read "on" at
  rest, "off" when actually tripped). Every other input in this project is
  active-low. `DebouncedInput::isActive()` originally used one global
  `SENSOR_ACTIVE` constant for every instance with no way to override it
  per-pin — meaning `antiPinch.isActive()` was backwards: `true` at rest,
  `false` during an actual pinch. Since that feeds directly into
  `SAFETY_STOP`, this would have triggered constantly during normal
  operation while **missing a real pinch entirely** — about as bad as a
  safety-interlock bug gets. Fixed by adding an `activeHigh` flag to
  `DebouncedInput` (default `false`, matching every other input);
  `antiPinch.begin(PIN_ANTI_PINCH, true, true)` sets it for this one input
  only. Applied the identical fix to the diagnostic tool's `auxTest` input
  (same struct, same bug, same fix) so its bench readings are trustworthy
  too. Both rebuilt and build-verified clean. **Lesson for next time**:
  when a switch is described as a safety/fail-safe input, check its
  normally-open vs. normally-closed polarity explicitly rather than
  assuming it matches whatever convention the rest of the codebase uses.

  **`PIN_ANTI_PINCH` moved from GPIO13 to GPIO14 (2026-07-06).** The
  project switched to an ESP32 mini form-factor board whose header/
  silkscreen doesn't break out GPIO13 at all (confirmed by the user
  checking their board directly, not assumed). GPIO14 was picked as the
  replacement after checking it against every other pin already in use
  (4, 16, 17, 25, 26, 27, 32, 33, 34, 35) and the general ESP32 avoid-list
  (strapping pins 0/2/5/12/15, UART0 on 1/3, flash-internal 6-11,
  input-only 34-39 which can't do `INPUT_PULLUP`) — a plain GPIO with no
  such conflicts. Updated in both `main.cpp` (`PIN_ANTI_PINCH`) and
  `diagnostic_main.cpp` (`PIN_AUX_TEST`, which intentionally mirrors
  whatever pin `PIN_ANTI_PINCH` uses so bench testing stays representative
  of the real wiring) plus the README pinout table and the harness
  worksheet artifact. Everything above this point in this section that
  still says "GPIO13" is accurate *as of the date it was written* — it
  predates this move and is kept as-is for history, not a current pin
  reference.

  This series-wiring problem was independently confirmed as a real, known
  issue by a third party — see below.

  Still open, lower priority (and no longer blocking anything): PIC16C622A
  per-pin function unconfirmed against a datasheet — informational only.

The harness worksheet Artifact (rebuilt 2026-07-03 after the prior one's URL
was lost) is at https://claude.ai/code/artifact/2ee47d2a-c0f0-4be4-9ba9-c5965f2b83b7
— it now has the confirmed pin labels above baked in (4-pin = motor, 7-pin =
hall+weight), plus a fill-in wiring worksheet and open-items checklist. If
this URL 404s or looks stale, check memory first, then ask the user before
building another fresh one.

A companion breadboard wiring worksheet (bench-prototype ESP32 + L298N +
hall sensor wiring, cross-referencing the harness worksheet above for stock
motor/weight-switch pins) was built 2026-07-04:
https://claude.ai/code/artifact/5a0b254f-3836-427a-829a-122c75e76e02

A third, focused artifact — just the 7-pin connector's confirmed pinout
with the *current* ESP32 GPIO assignment for each pin (kept up to date as
pins change, e.g. the GPIO13→14 anti-pinch move) — was built 2026-07-05:
https://claude.ai/code/artifact/5bea343c-a0c4-43d4-8a5f-16ed12475a72

All three are also exported as PDFs at the repo root
(`harness-pinout-worksheet.pdf`, `breadboard-wiring-worksheet.pdf`,
`7pin-connector-map.pdf`) via headless Chrome print-to-pdf — regenerate
with the same command if any Artifact is redeployed, since the PDFs are
static snapshots and won't update on their own.

A fourth artifact, distinct from the three reference worksheets above, is a
**narrative project page** built 2026-07-06 at the user's request — "the
project from start to finish," a readable story (problem → harness
investigation → the pins 6/7 series-wiring discovery → design decisions →
bring-up/harness split → firmware bugs found on real hardware → the
dashboard → the ongoing WiFi ASSOC_LEAVE investigation → current status),
not a technical reference table like the other three:
https://claude.ai/code/artifact/8a8e9092-ef41-4fe4-8adf-625f51e96f2c
— exported as `project-story.pdf` at the repo root, same regenerate-on-
redeploy caveat as the others. **Contains one open item**: the user said
they'd insert a photo of the harness wiring changes themselves — §05
("Bring-up, and the harness split") has a styled placeholder frame with a
caption ("Fig. 1 — internal jumper cut, independent ground returns wired
to each switch") marking exactly where it goes. Once the user provides
that photo, embed it there (as a data URI, per the Artifact CSP) and
redeploy + re-export the PDF — don't build a new page for it.

**Correction, same day**: the first draft of this page (and, it turned out,
the opening paragraph of this file and the top of `README.md` too) opened
with "two hall sensors failed" as the reason for the whole project — wrong.
The user corrected it: **the original control board itself died; the hall
sensors, weight switch, anti-pinch switch, gearmotor, and harness were all
still good** and are reused as-is, not replaced. The A1101EUA-T
identification work (see "Original harness pinout" below) was about
understanding the sensors already there well enough to wire them into the
new board correctly — not sourcing replacement parts for failed ones. Fixed
in this file's opening paragraph, `README.md`'s intro + BOM table, and
throughout the project-page artifact/PDF above. If older context anywhere
still says "the sensors failed," it predates this correction and is wrong.

## Things NOT yet done

- **Custom PCB, sized to match the original controller's physical
  footprint, so the new board mounts exactly where the old one did** —
  schematic/layout in progress as of 2026-07-06, not otherwise documented
  anywhere in this repo before now. The ESP32 mini form factor (see the
  GPIO13/14 anti-pinch note elsewhere in this file) was chosen specifically
  because it needs to physically fit that same footprint/enclosure — a
  full-size DevKit (used for all earlier bench testing) doesn't fit. No
  further detail (tool, stage, target dimensions) captured yet - ask the
  user for specifics if this needs picking back up.
- Cycle count/drawer count persistence, OTA, drawer-full tracking: **done**.
- WiFi/MQTT/wait-timer runtime config + setup portal: **done**.
- On-device dashboard hosting: **done**.
- Harness pinout: **confirmed by continuity trace (2026-07-03) and bench
  testing (2026-07-04)** — see above. Both worksheet artifacts have the
  real pin labels baked in.
- Standalone hardware bring-up/diagnostic tool: **done** (2026-07-04) — see
  "Diagnostic tool" above.
- Build verification: **done** (2026-07-04) — firmware (both envs) and web
  dashboard all build/typecheck clean. See "Status" at the top.
- Motor, weight switch, and hall sensor circuits: **all bench-tested and
  resolved (2026-07-04)** via the diagnostic tool — see "Original harness
  pinout" above for the full trail (motor driver IC identified, hall
  sensors identified as Allegro A1101EUA-T with confirmed pinout, weight
  switch confirmed working).
- Anti-pinch switch: **fully done (2026-07-05)** — hardware split
  completed and bench-verified independently working, firmware polarity
  bug caught and fixed same day (anti-pinch is active-HIGH,
  `DebouncedInput` needed an `activeHigh` flag — see "Pins 6/7 topology"
  above for the full story). The harness/switch story is now completely
  closed out.
- Physical assembly: the weight/anti-pinch switch split was the first piece
  of physical rework done on this project (2026-07-05) — case opened,
  internal jumper cut, both switches independently wired and bench-verified.
  Hall sensors followed (2026-07-06) — both are now permanently mounted in
  the actual LR2 chassis and wired through the stock 7-pin connector, not
  bench leads (see "Home vs. Dump" above for the final pin 2/3 → GPIO
  mapping). The 7-pin connector is therefore fully installed end-to-end;
  motor installation status is the remaining open item on the harness.
- Cycle sequence (dump pause/shake, home overshoot/settle), weight-switch
  2-minute safety cooldown, second wait/issue LED, and usage analytics:
  **all implemented and build-verified (2026-07-05)**, all firmware-side
  changes still **not yet hardware-verified** — real motor testing is the
  next step for all of these, and every new timing constant is a
  placeholder pending that. See "Key decisions" above for the full detail.
- **Real bug found (2026-07-06): dirty resets left WiFi permanently unable
  to associate (repeating `ASSOC_LEAVE`, reason 8) until a full power
  cycle.** Traced back to a sibling project the user maintains,
  `/Users/mgresham/Documents/node32v2` (`src/network/NetworkManager.cpp`),
  whose WiFi "works correctly all of the time under the same
  circumstances" - its `initialize()` explicitly detects "dirty" resets
  (brownout, panic, or any watchdog reset) via `esp_reset_reason()` and
  forces `WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(200);` *before*
  `WiFi.mode(WIFI_STA)`, with a comment explaining why: those reset types
  can leave lwIP's static memory pools in an indeterminate state from the
  previous boot, and a plain `WiFi.mode(WIFI_STA)` doesn't clear that -
  only tearing down and rebuilding the stack via `WIFI_OFF` does. On a
  clean `POWERON_RESET` the BSS is already zeroed by the ROM bootloader, so
  the teardown is skippable there. This exactly matched what had just been
  observed here: an `RTCWDT_RTC_RESET` (from clicking "Save & reboot" on
  the Analytics day/night-hours form - itself likely a benign artifact of
  `ESP.restart()`, see below) immediately preceded a persistent
  `ASSOC_LEAVE` loop that a manual reset alone didn't clear - and every
  attempt to blame the ESP32 mini's power supply (buck converter vs. USB,
  brownout messages, `WiFi.setTxPower()`) had come up empty, since a
  genuine brownout would have printed the ESP32's own brownout-detector
  message and none appeared. The "switching power source fixed it" report
  is also consistent with this theory rather than contradicting it - both
  a cable swap and a buck-converter swap involve fully removing power,
  which is indistinguishable from the "clean `POWERON_RESET`" case this
  fix is built around. Ported the identical pattern (same reset reasons
  checked, same teardown sequence) into `main.cpp`'s `setup()`, right
  before the existing `WiFi.mode(WIFI_STA)` call, with `<esp_system.h>`
  added for `esp_reset_reason()`. Build-verified clean (both envs);
  **not yet hardware-verified** that this actually prevents the
  ASSOC_LEAVE loop from recurring after a watchdog-class reset - next
  step is deliberately reproducing a dirty reset (or just waiting for the
  next one) and confirming WiFi reconnects cleanly afterward.
  **UPDATE: this was not the (or not the only) root cause** - see the
  dated entry further down. The user hit the same ASSOC_LEAVE loop again
  on a genuine `POWERON_RESET`, which has nothing "dirty" for this fix to
  tear down, so it doesn't explain that recurrence. Not reverted (a clean
  teardown after a real dirty reset is still correct behavior on its own
  merits) but don't treat it as *the* fix for this bug going forward.
- **Real bug found the same day: the Usage card's "Today" was a rolling
  24-hour window, not a calendar day - and even fixed server-side, would
  still have been the wrong calendar day for most viewers.**
  `handleAnalytics()`'s `totalToday`/`totalWeek`/`total30Days`/`daily30`
  fields (`main.cpp`) are computed from `dayOffset = (VISIT_LOG_HOURS - 1
  - i) / 24`, which buckets "today" as "the last 24 hours from right
  now," not "since local midnight" - caught from the user noticing a real
  visit from the previous evening still showing up under "Today." Even a
  from-scratch fix on the device side would've had a second problem: the
  device's clock is deliberately UTC-only (see the NTP setup above), so a
  UTC calendar-day boundary would still be wrong for any viewer not in
  UTC. Fixed entirely in `UsageCard.tsx` instead, no firmware change
  needed - it already receives the raw `visitTimes` array (added for the
  Analytics tab), so it now computes today/week/30-day totals and the
  daily bar chart itself, bucketing by each timestamp's local-browser
  calendar day (`date.setHours(0,0,0,0)` per timestamp), mirroring the
  Analytics tab's existing day/night classification approach exactly. The
  device's own precomputed fields are left as-is (still used by any other
  consumer, e.g. MQTT) - this was purely a dashboard-side fix. Verified
  with a Playwright test that mocked the browser clock to 2:00am with one
  visit at 11:30pm the previous night and one at 1:15am - both fall
  inside the device's rolling-24h window (old bug would show "Today: 2"),
  but only the 1:15am one is actually today; the fix correctly shows
  "Today: 1." Build-verified clean (web typecheck + build).
- **ASSOC_LEAVE investigation continued (2026-07-06) - both the "dirty
  reset" and "motor EMI during BOOT_HOMING" theories were ruled out by the
  user's own testing, not confirmed.** The dirty-reset teardown fix above
  doesn't explain a recurrence the user then reported on a genuine
  `POWERON_RESET` (nothing "dirty" to tear down on a truly clean boot).
  Motor EMI was the next theory (`BOOT_HOMING` runs the motor immediately
  on boot, fully independent of WiFi status - `loop()` calls
  `connectWifiIfNeeded()` and `runStateMachine()` unconditionally back to
  back), but the user confirmed no correlation: the motor was already
  stopped when a connection attempt failed, and reported the loop
  "eventually connects after a while" on the *same* power source (USB)
  that had both worked and failed at different times - ruling out a
  simple deterministic power-source-quality explanation too. Root cause
  is still open. Rather than guess a fourth mechanism blind, added
  diagnostic instrumentation instead (all serial + one new dashboard
  field, no behavior changes):
  - `resetReasonName()` + an unconditional boot-reason print in `setup()`
    (previously only printed on a *dirty* reset).
  - `connectWifiIfNeeded()` now logs each attempt's number and
    time-since-boot, plus a one-line summary (attempt count, RSSI,
    channel) the moment it connects.
  - A new serial-only heartbeat (independent of the existing MQTT one),
    same `HEARTBEAT_INTERVAL_MS` cadence, printing uptime/free heap/state/
    WiFi status/RSSI regardless of MQTT or dashboard connection - free
    heap in particular because a leak/fragmentation issue could itself
    cause this same class of intermittent symptom and was otherwise
    completely invisible.
  - **RSSI exposed on the dashboard** (`rssi` in `buildStateJson()`, new
    "WiFi signal" row in the Status card showing dBm plus a qualitative
    label - Excellent/Good/Fair/Weak/Very weak - colored red below -75dBm)
    - previously nowhere visible at all, not even in the serial log.
  Build-verified clean (both firmware envs, web typecheck+build) and the
  new dashboard field visually verified via a Playwright screenshot with
  a mocked WebSocket payload. Next real occurrence should be captured with
  the new logging in place - specifically watch whether RSSI is
  consistently weak at connect time (signal/interference) versus fine
  (pointing somewhere else entirely).
- **Connect-to-strongest-BSSID added (2026-07-06), at the user's request
  and plausibly relevant to the ASSOC_LEAVE investigation itself.** The
  user asked whether the board could be made to connect to the strongest
  AP specifically because their home network may have more than one AP
  sharing the same SSID (mesh/multi-AP) - plain `WiFi.begin(ssid, pass)`
  doesn't scan first, so it can end up associating with a distant/weak AP
  instead of the nearest one, which would itself explain some or all of
  the intermittent connection failures already being chased, independent
  of the dirty-reset and motor-EMI theories that were both ruled out.
  `connectWifiIfNeeded()` now scans first every reconnect cycle (async,
  non-blocking - same `scanNetworks(true)`/`scanComplete()` polling
  pattern as the setup portal's existing WiFi scan in `config_api.cpp`,
  chosen specifically so a multi-second scan can never delay the
  safety-interlock checks that run every `loop()` iteration), picks the
  AP with the best RSSI among all matching the configured SSID, and
  connects to that specific BSSID+channel via `WiFi.begin(ssid, pass,
  channel, bssid)` instead of the plain 2-argument form. Falls back to a
  plain connect if the scan finds no match. Implemented as a small
  `WifiConnectPhase` (`IDLE`/`SCANNING`) state machine so the async scan
  can span multiple `loop()` iterations without blocking. Build-verified
  clean (both envs); **not yet hardware-verified** - next real connection
  should show a new "found N AP(s)... strongest is X dBm on channel Y"
  log line, worth checking whether N is actually > 1 on this network (if
  it's always 1, multi-AP wasn't the issue after all, but the fix is
  harmless either way).
- **Real bug found the same day: mDNS silently stopped working after the
  first WiFi reconnect, never on the first connect.** The user reported
  `http://lr2redux.local/` "still failing sporadically" - direct IP access
  kept working throughout, which was the tell. `setupOtaIfNeeded()`
  gated `MDNS.begin()`/`ArduinoOTA.begin()` behind a one-time latch
  (`otaInitialized`) that, once set on the first successful connect, never
  let that setup code run again for the rest of the boot - but the
  underlying network interface gets torn down and rebuilt on every WiFi
  disconnect/reconnect, and we already know this board reconnects
  periodically (the whole ASSOC_LEAVE investigation above). The mDNS
  responder doesn't survive a reconnect on its own, so it went quiet after
  the first one while the IP-based dashboard kept working fine (plain
  TCP/HTTP over the new connection, nothing to do with mDNS state) -
  exactly the "sporadic" pattern reported. Fixed by detecting the
  disconnected→connected transition on every call (same `wasConnected`
  pattern `connectWifiIfNeeded()` already uses) instead of a one-time
  latch, and calling `MDNS.end()`/`ArduinoOTA.end()` before re-`begin()`ing
  both on every reconnect, not just the first. Renamed the latch
  `otaMdnsArmed` to reflect what it now tracks (has been armed at least
  once, so the next connect should tear down first) rather than "has this
  ever run." Build-verified clean (both envs); **not yet
  hardware-verified** - next step is confirming lr2redux.local keeps
  resolving across a real WiFi disconnect/reconnect cycle, not just right
  after a fresh boot.
- Dashboard header/URL-bar CSS overlap: **fixed and visually verified
  (2026-07-05)** via Playwright screenshots.
