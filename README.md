# lr2-redux

Replacement control board for a Litter Robot 2 (LR2) whose original control
board died — built around an ESP32-WROOM mini-form-factor dev board and an
L298N dual H-bridge motor driver. The stock 12V gearmotor, weight-trigger cat
switch, anti-pinch switch, and Home/Dump hall-effect position sensors were
all still good; this project reuses every one of them as-is, wired into the
new board instead of the dead one.

Adds MQTT + Home Assistant auto-discovery so the unit reports state and accepts a
manual "cycle now" command over WiFi.

## Bill of materials

| Part | Notes |
|---|---|
| ESP32-WROOM mini-form-factor dev board | already have — GPIO breakout varies by specific mini board (this project's board doesn't expose GPIO13; anti-pinch uses GPIO14 instead — double-check your own board's silkscreen before wiring, don't assume the same pins are broken out) |
| L298N dual H-bridge module | already have; drives the single stock 12V gearmotor |
| Stock hall-effect sensors (identified as Allegro A1101EUA-T by opening the package — A3144/US5881 are generic substitutes if one ever needs replacing) | **Reused as-is** — these were never the failed part; the original control board was. Home + Dump position sensing, same stock magnets on the globe |
| 2x 10kΩ resistor | pull-ups for the hall sensor outputs (pulled to 3.3V, see wiring notes) |
| 12V → 5V buck converter module (LM2596 or similar, 1A+) | clean logic supply for ESP32 + hall sensors, isolated from motor switching noise |
| Inline fuse, 2A, on the 12V feed | not strictly required, but cheap insurance on a device that runs unattended |
| Stock 12V wall adapter | reused as-is |
| Stock weight-trigger cat switch | reused as-is, wired straight to a GPIO |
| Stock anti-pinch switch | reused as-is, on its **own** GPIO — physically split from the weight switch's shared harness loop; see "Weight switch and anti-pinch switch" below |
| Stock manual cycle button (optional) | reused if present/working |
| 3x LED (green/yellow/red) + 3x current-limit resistor (~330Ω) | replicates the original stock board's status language exactly — see "Status LED" below |

## Pinout

| ESP32 pin | Function | Notes |
|---|---|---|
| GPIO26 | Motor IN1 | L298N direction |
| GPIO27 | Motor IN2 | L298N direction |
| GPIO25 | Motor ENA | PWM speed (ledc channel 0, 5kHz) |
| GPIO34 | Hall - Home | input-only pin, needs the external 10k pull-up (no internal pull-up available) |
| GPIO35 | Hall - Dump | input-only pin, needs the external 10k pull-up (no internal pull-up available) |
| GPIO32 | Weight/cat switch | active-low, internal pull-up used |
| GPIO14 | Anti-pinch switch | **active-HIGH** (opposite of every other input here), internal pull-up used — see "Weight switch and anti-pinch switch" below before wiring this one |
| GPIO33 | Manual cycle button (optional) | active-low, internal pull-up used |
| GPIO4 | Status LED — green | see "Status LED" below |
| GPIO16 | Status LED — yellow | see "Status LED" below |
| GPIO17 | Status LED — red | see "Status LED" below |
| 5V (from buck) | ESP32 VIN, hall sensor VCC, L298N logic supply | do **not** power ESP32 from the L298N's onboard 5V regulator directly off the 12V rail — motor switching noise rides straight into it |
| GND | common ground | tie ESP32 GND, L298N GND, buck converter GND, and 12V supply GND all together |

### Status LED

Three discrete LEDs (green/yellow/red — not a bi-color part) replicate the
original stock board's status language exactly, straight from its manual:

| Signal | Meaning |
|---|---|
| Solid green | Standby, waiting for the cat (`IDLE` / `WAIT_TIMER`) |
| Solid yellow | Actively cycling (any `CYCLE_*`/`BOOT_HOMING` phase) |
| Solid red | Cat sensor active (`CAT_PRESENT`), or a weight-triggered `SAFETY_STOP` |
| Flashing red | Cat sensor active beyond the configurable warning threshold, or `FAULT` |
| Flashing yellow | Anti-pinch-triggered `SAFETY_STOP` specifically |

### Weight switch and anti-pinch switch

These are two separate physical switches, but the **stock harness wires them
in series on one shared 2-wire loop** (confirmed both by direct continuity
trace on this unit and independently by another Litter-Robot teardown/rebuild
— see `CLAUDE.md`, "Pins 6/7 topology"). Read as a single combined signal,
they're indistinguishable: "cat present" and "pinch detected" produce the
same reading, and — worse — a pinch that happens while the cat has already
left (i.e., during an actual cycle, exactly when it matters) produces **no
signal change at all**, since the weight switch being open already breaks
the loop regardless of the anti-pinch switch's state.

**This board requires physically splitting the two switches** so each gets
its own dedicated ground return, rather than reading the shared loop as-is:

1. Open the case and trace the internal wiring from each switch to find the
   jumper/splice connecting them together (confirm with a multimeter before
   cutting anything — don't assume the topology).
2. Cut that internal jumper.
3. Run a new ground wire to each switch's now-independent terminal (a
   shared ground point is fine, they don't need separate grounds from each
   other, just each needs *a* ground).
4. The existing harness wires to the connector don't need to change — one
   already lands on GPIO32 (weight switch), the other on GPIO14 (anti-pinch).

Both use the ESP32's internal pull-up (`INPUT_PULLUP`), same as the weight
switch always has — no external resistor needed for either.

**Polarity is different between the two.** The weight switch is
normally-open (active-low: reads HIGH at rest, LOW when the cat's weight
closes it). The anti-pinch switch is **normally-closed and opens on
trip** — confirmed on the bench — so it reads the *opposite* way: LOW at
rest, HIGH when actually tripped. This is a sensible fail-safe design (a
broken wire or disconnected switch reads as "tripped" rather than silently
"fine"), but it means `DebouncedInput` needs an `activeHigh` flag for this
one input — see `antiPinch.begin(PIN_ANTI_PINCH, true, true)` in
`main.cpp`. Getting this backwards is a real, already-caught bug: it would
make `SAFETY_STOP` trigger constantly during normal operation while
missing an actual pinch entirely.

### Hall sensor wiring detail

Digital hall switches like the A3144 need ≥4.5V to operate reliably, but ESP32
GPIOs are **not 5V tolerant**. Power the sensors from the 5V rail, but pull the
open-collector output up to **3.3V** (not 5V) with the 10kΩ resistor:

```
5V ──► Hall VCC
Hall OUT ──┬──► ESP32 GPIO (34 or 35)
           └──[10k]──► 3.3V
Hall GND ──► GND
```

This keeps the sensor's internal circuit happy at 5V while the logic swing on
the GPIO stays within the ESP32's 3.3V-max input range.

## Motor driver notes

- L298N has ~2V forward drop per channel; it'll run warm under sustained load.
  The stock LR2 gearmotor draws well under its 2A/channel rating, so this is a
  non-issue for the short duty cycles here, but give the module some airflow.
- Most L298N breakout boards already include the four flyback diodes needed for
  a DC motor — verify yours does before assuming you're covered.
- **Both motor directions are used** — `motorRunForward()` drives Home→Dump,
  `motorRunReverse()` drives Dump→Home. This was corrected 2026-07-05 after
  real hardware testing showed the globe does **not** continue forward past
  Dump all the way back around to Home (an earlier assumption, never
  validated before build-testing existed) — it needs to stop at Dump and
  reverse. See `CLAUDE.md`, "Key decisions" for the full story.

## Firmware

PlatformIO project targeting `esp32dev` / Arduino framework.

1. `cp include/secrets.h.example include/secrets.h` and set `OTA_PASSWORD`. That's
   the only build-time secret left — WiFi and MQTT are configured at runtime, see below.
2. `pio run -e esp32dev -t upload` — builds the firmware **and** the web
   dashboard, flashes the firmware, then automatically flashes the
   dashboard to LittleFS too. One command, nothing else to remember.
3. Power it on. With no WiFi configured yet, it boots straight into setup mode.

The board serves the dashboard itself from then on — `http://lr2redux.local/`
once it's on your network, no separate hosting needed.

**How the one-command build works** (`platformio.ini` + `scripts/build_web.py`):
every `pio run -e esp32dev ...` runs `npm run build:device` first
(`web/` → `./data`), so the dashboard is never stale, and `upload` automatically
chains an `uploadfs` run afterward. If Node/npm aren't installed on the
machine, this step warns and skips rather than failing the firmware build —
tool availability has varied session-to-session on this project. This only
applies to `env:esp32dev`; `env:diagnostic` doesn't touch the web dashboard
at all.

### Serial diagnostics

At 115200 baud, every boot prints its reset cause up front (`Boot reason:
POWERON (1)`, `BROWNOUT (9) [dirty]`, etc. — see `resetReasonName()` in
`main.cpp`), useful for correlating any instability against what actually
caused the last reboot. While WiFi isn't connected, each reconnect cycle
scans first, logs how many APs it found for the configured SSID and which
one's strongest (dBm, channel, BSSID), then connects to that specific AP
rather than an arbitrary one — see "Connecting to the strongest AP" below.
Once connected, a one-line summary prints how many attempts it took plus
RSSI and channel. Independent of that, a serial-only heartbeat (matching
`HEARTBEAT_INTERVAL_MS`, same cadence as the MQTT heartbeat) prints uptime,
free heap, current state, and WiFi status/RSSI every cycle — regardless of
whether MQTT or the dashboard are connected, so a plain serial monitor
alone is enough to watch for memory or connectivity drift over time.

### Connecting to the strongest AP

If more than one access point broadcasts the same SSID (a mesh system, or
several standalone APs covering a house), plain `WiFi.begin(ssid, pass)`
associates with whatever AP the radio happens to see/cache first — not
necessarily the closest or strongest one. `connectWifiIfNeeded()` instead
scans first (non-blocking — async `WiFi.scanNetworks(true)` polled via
`WiFi.scanComplete()` across `loop()` iterations, same pattern the setup
portal's WiFi scan already uses, specifically so a multi-second scan never
delays the safety-interlock checks that run every loop iteration), finds
every AP matching the configured SSID, and connects to the one with the
strongest RSSI by passing its specific channel and BSSID to `WiFi.begin()`.
Falls back to a plain `WiFi.begin(ssid, pass)` if the scan doesn't turn up
a match (e.g. an AP that's momentarily quiet). Re-evaluated on every
reconnect cycle, not just once at boot, so it re-picks the best AP if
conditions change (device moved, an AP goes down, etc.).

### First-time setup / re-configuring WiFi

The board has no WiFi credentials baked in, so the same firmware image works on
any unit. On boot, if nothing's configured yet, it starts a SoftAP + captive
portal instead of trying to connect:

1. Connect to the **`LR2-Redux-Setup`** WiFi network (password `lr2setup`,
   both set in `setup_portal.cpp` if you want to change them).
2. A captive-portal prompt should pop up automatically; if not, browse to
   `http://192.168.4.1`.
3. Scan for your network (or type the SSID manually), enter the WiFi
   password, optionally MQTT host/port/user/pass, and the cat-leaves wait
   timer in minutes.
4. Save. The board reboots and connects to your network.

**To re-enter setup mode later** (change WiFi, move to a new network, fix a
typo): hold the manual cycle button for **10 seconds**. The status LED blinks
fast to confirm, then the board reboots into the same SoftAP portal. This
works regardless of the robot's current state — useful if bad credentials
are the reason it's unreachable in the first place.

Setup mode does nothing else while active: no cycling, no MQTT, no dashboard
WebSocket. The motor is held stopped the whole time.

### State machine

```
BOOT_HOMING → IDLE ⇄ CAT_PRESENT → WAIT_TIMER → CYCLE_TO_DUMP → CYCLE_DUMP_PAUSE → CYCLE_DUMP_SHAKE
                                        │                │                                    │
                                        └── cat returns ─┘                                     ▼
                                                                          CYCLE_TO_HOME → CYCLE_HOME_OVERSHOOT
                                                                                │                    │
                                                                                ▼                    ▼
                                                                          CYCLE_HOME_SETTLE ◄─────────┘
                                                                                │
                                                                                ▼
                                                                              IDLE

   (weight switch trips during BOOT_HOMING/any CYCLE_* phase, OR anti-pinch trips in ANY state)
                                        ▼
                                  SAFETY_STOP ──► resumes motion once both clear:
                                                  anti-pinch clears immediately;
                                                  weight switch needs a 2-minute
                                                  cooldown after it clears, too

              (any sensor-wait phase exceeds its timeout) ──► FAULT ──(MQTT reset_fault)──► BOOT_HOMING
```

- `BOOT_HOMING` doesn't assume it knows which direction Home is from an
  arbitrary starting position — it seeks **Dump first** (forward, same
  direction as a normal cycle's first leg), then reverses to Home from
  there. This avoids assuming a direct forward path to Home is always
  mechanically safe (it could stall against a hard stop if one exists on
  that side) and reuses direction logic already validated for normal
  cycling. Also covered by the `SAFETY_STOP` interlock now, since it's a
  real multi-phase motor sequence, not a single simple spin.
- `WAIT_TIMER` defaults to 7 minutes after the cat leaves, matching stock LR2
  behavior. Configurable at runtime under the dashboard's "Advanced settings"
  (`SettingsCard.tsx`), with a **hard-enforced 2-minute minimum** — both
  client-side and, authoritatively, in `config_api.cpp` — so it can't be set
  low enough to risk cycling while the cat is still in or near the globe.
- At Dump, the motor stops and **dwells 5s** (`CYCLE_DUMP_PAUSE`) so waste
  finishes falling through, then **shakes** briefly — oscillating direction
  every 400ms for ~2.4s (`CYCLE_DUMP_SHAKE`) — to dislodge anything stuck,
  before reversing toward Home. Both durations are placeholders pending
  real-hardware tuning.
- At Home, the motor doesn't stop immediately — it **overshoots** a few
  seconds past Home in reverse (`CYCLE_HOME_OVERSHOOT`), then runs forward
  again to **settle** back at Home (`CYCLE_HOME_SETTLE`), which is what
  actually ends the cycle. This helps litter that piled up to one side
  during the dump level back out. Placeholder duration, same caveat.
- Each sensor-wait phase (`CYCLE_TO_DUMP`, `CYCLE_TO_HOME`, `CYCLE_HOME_SETTLE`)
  has a 180s timeout (placeholder, pending a real timed measurement — see
  `CLAUDE.md`); exceeding it stops the motor and enters `FAULT`. The
  time-bounded phases (`CYCLE_DUMP_PAUSE`, `CYCLE_DUMP_SHAKE`,
  `CYCLE_HOME_OVERSHOOT`) don't need this — they always advance after their
  own fixed duration.
- If the weight switch trips mid-motion (`BOOT_HOMING`/any `CYCLE_*` phase),
  the motor stops immediately (`SAFETY_STOP`) and resumes from the same
  spot once conditions clear — it does not restart the segment. The
  anti-pinch switch is checked more broadly: it triggers `SAFETY_STOP` from
  **any** state, including `IDLE`/`CAT_PRESENT`/`WAIT_TIMER`, not just
  mid-motion — a pinch condition matters even with the motor stopped (e.g.
  it should block a cycle from ever starting, not just interrupt one
  already running). These are read as two independent inputs (GPIO32 /
  GPIO14); see "Weight switch and anti-pinch switch" above for why that
  requires physically splitting the stock harness rather than wiring it
  as-is.
  - **Weight-only interruption** (default): auto-resumes on its own, but
    only after a **2-minute cooldown** once the weight switch clears — the
    globe doesn't start moving again the instant the cat steps off.
  - **Anti-pinch involvement** (always, not configurable): requires an
    explicit manual reset — press the manual button, or send `resume` over
    MQTT/the dashboard — rather than auto-resuming. This matches the
    original stock board's behavior for a detected pinch specifically.
  - **"Require manual reset after any interruption"** (dashboard Advanced
    Settings, off by default): extends the manual-reset requirement to
    weight-only interruptions too, matching the original stock board
    exactly — with this on, nothing auto-resumes, ever.
- The weight switch being continuously active (cat present) for longer
  than a configurable threshold (`catPresentWarningMin`, default 2 minutes,
  matching the original board, can be set higher but not lower) flags a
  warning — flashing red on the status LED, `catPresentWarning: true` over
  the WebSocket — without interrupting anything. Just a heads-up that the
  cat's been in there a while.

### MQTT topics

MQTT is now strictly between the ESP32 and your broker (for Home Assistant) —
the web dashboard doesn't touch it at all, see
[WebSocket dashboard interface](#websocket-dashboard-interface) below.

| Topic | Direction | Payload |
|---|---|---|
| `lr2redux/state` | publish, retained | `homing`, `idle`, `cat_present`, `waiting`, `cycling`, `safety_stop`, `fault` |
| `lr2redux/cat_present` | publish, retained | `ON` / `OFF` |
| `lr2redux/cycle_count` | publish, retained | integer, persisted across reboots (NVS) |
| `lr2redux/drawer_full` | publish, retained | `ON` once `drawer_cycles` reaches the configured threshold (`drawerFullCycles`, default 10) |
| `lr2redux/drawer_cycles` | publish, retained | integer, cycles since the drawer was last emptied, persisted across reboots |
| `lr2redux/drawer_threshold` | publish, retained | current `drawerFullCycles` value, published once on connect so clients don't need to hardcode it |
| `lr2redux/heartbeat` | publish | uptime in seconds, every 30s |
| `lr2redux/cmd` | subscribe | `cycle` (manual trigger from IDLE), `reset_fault`, `drawer_emptied` (resets `drawer_cycles` to 0), `resume` (confirms a `SAFETY_STOP` that needs manual reset — a no-op otherwise) |

Home Assistant MQTT discovery configs are published automatically on connect
under `homeassistant/.../lr2redux/...` — the device, its cycle/drawer sensors,
and the cycle-now/drawer-emptied buttons should all appear without any manual
`configuration.yaml` entries.

### WebSocket dashboard interface

The firmware also runs its own async web server (`ESPAsyncWebServer`) with a
WebSocket endpoint at `ws://lr2redux.local/ws`, independent of MQTT. This is
what the web dashboard talks to — no broker, no websocket-listener config, no
extra hop. mDNS (`lr2redux.local`) comes up automatically once the board
joins WiFi, and re-arms itself (along with OTA) on every reconnect, not
just the first connect after boot — it used to silently stop responding
after any WiFi disconnect/reconnect cycle while direct-IP access kept
working fine, since the two don't share any state.

- On connect, and on every state change (plus at least every 5s regardless),
  the board pushes a JSON snapshot:
  ```json
  {
    "state": "idle",
    "catPresent": false,
    "cycleCount": 42,
    "drawerFull": false,
    "drawerCycles": 3,
    "drawerThreshold": 10,
    "uptimeSeconds": 12345,
    "needsManualReset": false,
    "catPresentWarning": false,
    "ipAddress": "192.168.1.42",
    "rssi": -58
  }
  ```
  `needsManualReset` is only meaningful while `state == "safety_stop"`
  (see the state machine notes above); `catPresentWarning` only while
  `state == "cat_present"`. `rssi` is `0` when not connected — the
  dashboard's Status card checks connection state separately rather than
  treating `0` as "no signal."
- Commands go the other way as JSON text frames: `{"cmd":"cycle"}`,
  `{"cmd":"reset_fault"}`, `{"cmd":"drawer_emptied"}`, `{"cmd":"resume"}`
  (confirms a `SAFETY_STOP` that needs manual reset — a no-op otherwise) —
  same command set as the MQTT `lr2redux/cmd` topic, just a different
  transport.
- No auth on either interface currently; see `web/README.md` for the caveat.
- The JSON snapshot above also includes `ipAddress` and `rssi` (WiFi signal
  strength in dBm, `0` when not connected — the dashboard's Status card
  shows both, with a qualitative label like "Fair"/"Weak" next to the raw
  dBm value). Added specifically to help diagnose intermittent WiFi
  connection issues without needing serial access every time.

### Config HTTP API

Three plain HTTP routes, available both in setup mode (on the SoftAP) and in
normal operation (on the STA connection) — same handlers either way:

| Route | Method | Body / response |
|---|---|---|
| `/scan` | GET | `{"status":"scanning"}` (202, poll again) or `{"networks":[{"ssid","rssi","secure"}]}` (200) |
| `/config` | GET | current `{wifiSsid, mqttHost, mqttPort, mqttUser, waitTimerMin, requireManualReset, catPresentWarningMin, dayStartHour, dayEndHour, drawerFullCycles}` (passwords never read back) |
| `/save` | POST | any subset of the same fields plus `wifiPass`/`mqttPass` — only fields present in the body are changed, everything else keeps its current value; blank password fields keep the existing one; saving always reboots the board |

This is what both the captive-portal setup page and the Grommet dashboard's
Settings card use — the dashboard just points at the same host as its
WebSocket connection. `Access-Control-Allow-Origin: *` is set globally so the
dashboard can call it cross-origin during `npm run dev`.

### Usage analytics

`/analytics` (GET) returns visit counts — a "visit" is a fresh occupancy
session (`IDLE` → `CAT_PRESENT`), not every weight-switch bounce, and not
a cat that briefly leaves and returns before `WAIT_TIMER` elapses:

```json
{
  "hourly24": [ /* 24 ints, oldest to newest */ ],
  "daily30":  [ /* 30 ints, oldest to newest */ ],
  "totalToday": 0,
  "totalWeek": 0,
  "total30Days": 0,
  "timeSynced": true,
  "visitTimes": [ /* raw epoch seconds (UTC), oldest to newest, up to 300 */ ],
  "dayStartHour": 6,
  "dayEndHour": 20
}
```

- Needs real wall-clock time to bucket visits correctly, so the board syncs
  time over NTP (`pool.ntp.org`, UTC, no DST) once WiFi connects. Visit
  counting is a no-op until that sync completes (`timeSynced: false`) —
  the dashboard's Usage card shows a note while it waits, rather than
  silently miscounting into the wrong hour.
- Stored as a 720-byte (30 days × 24 hours) array in NVS, indexed by
  absolute hours-since-epoch modulo 720 — it self-ages, so data older than
  30 days is naturally overwritten rather than needing an explicit prune
  step. One known edge case: if the board is powered off for **more than**
  30 consecutive days, a hint of data from over a month ago could
  transiently reappear in a slot nothing's overwritten yet — accepted as a
  rare, low-stakes cosmetic inaccuracy rather than adding per-slot
  timestamp tracking to engineer around it.
- The dashboard's Usage card (`UsageCard.tsx`) polls `/analytics` every 60s
  (visit counts change rarely — no need to ride the WebSocket telemetry
  stream), but **computes today/this-week/30-day totals and the daily bar
  chart itself from the raw `visitTimes` array**, not from the `hourly24`/
  `daily30`/`total*` fields the device precomputes. Those device-side
  fields use a rolling 24-hour window in the device's UTC clock, not a
  real calendar-day boundary in the viewer's own timezone — a visit from
  late last night could otherwise still count as "today" depending on
  what time it is right now. Same reasoning as the Analytics tab's day/
  night split below: classify by calendar day using each timestamp's
  local browser time, not the device's.
- A separate circular buffer (`visitTimes`, 300 raw epoch-second entries,
  ~1.2KB in NVS) records each individual visit's timestamp — enough
  precision for the dashboard's **Analytics tab**, which the summary above
  can't support on its own (hourly counts alone can't reconstruct exact
  visit times or gaps between them). The Analytics tab shows a scrollable
  list of recent visit times and the average time between visits, split
  into **day** and **night** buckets using a configurable hour boundary
  (`dayStartHour`/`dayEndHour`, default 6am–8pm, persisted like every other
  setting and editable right on the Analytics tab). Classification happens
  entirely client-side against each timestamp's *local browser* hour — the
  board's own clock stays UTC/DST-free internally (see NTP setup above), so
  this is the one place that should ever apply the viewer's timezone.

### Persistence, OTA, and the drawer counter

- `cycle_count`, `drawer_cycles`, and the 30-day visit log are stored in NVS
  (`Preferences` library, namespace `lr2redux`) and reloaded on boot, so a
  power cycle or firmware update doesn't lose them.
- `drawer_cycles` increments alongside `cycle_count` but resets independently
  via the `drawer_emptied` MQTT command (wired to the "LR2 Drawer Emptied"
  button in Home Assistant) — publish it after you empty the drawer.
- The drawer-full threshold (`drawerFullCycles`, default 10) is a runtime
  setting, not a compile-time constant — set it under the dashboard's
  Settings tab → Advanced settings ("Cycles until drawer full") based on
  how many cats/cycles your drawer actually holds before it needs emptying.
- OTA updates work two ways, both password-protected with the same
  `OTA_PASSWORD` (from `secrets.h`), both stopping the motor first so an
  update can't land mid-rotation:
  - Command line, via `ArduinoOTA` (hostname `lr2redux`):
    `pio run -t upload --upload-port lr2redux.local` (or its IP).
  - Browser, via the dashboard's Settings tab ("Firmware update" card,
    `FirmwareUpdateCard.tsx`): pick a `.bin` built with `pio run -e
    esp32dev`, enter the OTA password, upload. Backed by a `POST /update`
    route that streams the upload into the ESP32 `Update` library — same
    underlying mechanism as the CLI path, just reachable without a
    terminal. The password is sent as an `X-OTA-Password` request header
    rather than in the multipart body.

## Web dashboard

See [`web/README.md`](web/README.md) — a Grommet + React single-page app that
talks straight to the ESP32's own WebSocket server (no MQTT broker, no
backend), organized into three tabs:

- **Dashboard** — live state, cycle/drawer counts, `cycle` / `reset_fault` /
  `drawer_emptied` / `resume` commands, IP address and WiFi signal strength,
  and a 30-day usage summary (`UsageCard.tsx`, today/week/30-day totals plus
  a daily bar chart, all computed client-side from raw visit timestamps in
  the viewer's own local timezone — see "Usage analytics" below).
- **Analytics** — a deeper dive into visit history: a scrollable list of
  recent visit times and the average time between visits, split into
  configurable day/night hours.
- **Settings** — WiFi/MQTT/wait-timer plus a collapsible "Advanced settings"
  section (cat-present warning, manual-reset behavior, cycles-to-drawer-full)
  over the same config HTTP API the setup portal uses, plus a firmware
  update uploader directly from the browser.

**The board serves this itself** — `npm run build:device` in `web/` builds it
straight into `./data`, which `pio run -t uploadfs` flashes to LittleFS. Once
the board's on your network, `http://lr2redux.local/` just works, no separate
hosting. It auto-detects it's being served by the device and points its
WebSocket/API calls at `window.location` accordingly — no config needed. You
can still run it standalone with `npm run dev` for development, or point a
separately-hosted build at a different board by typing its address into the
dashboard's device field.

## Future work / not included

- No local fallback if the MQTT broker is unreachable — the state machine
  itself doesn't depend on MQTT, but drawer/cycle telemetry silently stops
  publishing to Home Assistant until it reconnects. The WebSocket dashboard
  is unaffected either way, since it never touches the broker.
