# lr2-redux

Replacement control board for a **Litter Robot 2** (stock board: "LITTER-ROBOT
Rev.2C, Copyright (C) 2003 Intensa Inc.") — two of its original hall-effect
Home/Dump position sensors failed. Rebuild target: ESP32-WROOM-32 + L298N
H-bridge, reusing the stock 12V gearmotor, stock cat-weight switch, and
stock wiring harness wherever possible.

Full technical detail (BOM, wiring, pinout, state machine) lives in
`README.md` and `web/README.md` — this file is oriented instead toward *why*
things are the way they are, and what's still open, so a session on a new
machine doesn't have to re-derive it.

## Status: firmware and web app written, NOT build-verified

Every dev session on this project so far has had **no compiler or Node.js
available** (checked at session start each time — worth re-checking on a new
machine, since if one's just there, USE it to actually build/typecheck
before trusting anything below). Everything was written carefully against
well-established ESP32/Arduino and Vite/React/Grommet idioms, but nothing
has actually been compiled or run. Treat the whole codebase as "should work,
unverified" until it's been through a real `pio run` and `npm run build`.

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
- **Hall sensors get bench-tested on the breadboard, not wired through the
  harness.** The weight switch and motor stay on the original harness
  (can't easily bench-test those), but the two new hall-effect ICs are
  mounted directly on the prototyping breadboard with a hand-held test
  magnet, since that's how you'd actually validate firmware before
  permanent installation.
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

## Open questions — original harness pinout is NOT confirmed

This is the biggest unresolved thread. What's actually verified vs. not:

- **Verified** (fetched and quoted directly from fabacademy.org, a real
  reverse-engineering writeup of this board family): the hall-sensor
  harness is a 7-pin connector — black=common GND, orange=sensor1(Home)
  OUT, yellow=sensor2(Dump) OUT, red=sensor1 VCC, violet=sensor2 VCC, 2
  pins unidentified. The cat-entry and anti-pinch switches are wired **in
  series on one circuit** (can't electrically distinguish the two
  conditions — acceptable for this firmware since both should halt the
  motor anyway, see `SAFETY_STOP`).
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
- **The user was mid-way through a multimeter continuity trace** of the
  actual dead board (tracing each harness wire to the component/IC pad it
  lands on) when this session ended. That result is the actual source of
  truth once it exists — nothing above should be trusted over it. If a new
  session picks this up, ask whether that trace happened and what it found
  before touching the harness worksheet in the design-sheet artifact again.

The breadboard/harness diagram (see the design-sheet Artifact — check
Claude's memory for its URL, it's been redeployed several times across this
project under the same URL) deliberately uses a **generic, unlabeled**
connector for the harness, with a fill-in worksheet, specifically because
none of the above was trustworthy enough to bake into a physical wiring
guide.

## Things NOT yet done

- Cycle count/drawer count persistence, OTA, drawer-full tracking: **done**.
- WiFi/MQTT/wait-timer runtime config + setup portal: **done**.
- On-device dashboard hosting: **done**.
- Harness pinout: **not confirmed** — see above.
- Nothing has been build-tested (no compiler in any session so far).
- No physical assembly/wiring has happened yet as far as this file's author
  knows — this has all been design + firmware + software work.
