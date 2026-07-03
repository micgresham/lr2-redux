# lr2-redux

Replacement control board for a Litter Robot 2 (LR2), built around an ESP32-WROOM
dev board and an L298N dual H-bridge motor driver. Reuses the stock 12V gearmotor
and stock weight-trigger cat switch; replaces the failed Home/Dump hall-effect
position sensors with new ones on the same stock magnet mounting points.

Adds MQTT + Home Assistant auto-discovery so the unit reports state and accepts a
manual "cycle now" command over WiFi.

## Bill of materials

| Part | Notes |
|---|---|
| ESP32-WROOM dev board | already have |
| L298N dual H-bridge module | already have; drives the single stock 12V gearmotor |
| 2x digital hall-effect switch (e.g. A3144, US5881) | Home + Dump position sensing, reusing stock magnets on the globe |
| 2x 10kΩ resistor | pull-ups for the hall sensor outputs (pulled to 3.3V, see wiring notes) |
| 12V → 5V buck converter module (LM2596 or similar, 1A+) | clean logic supply for ESP32 + hall sensors, isolated from motor switching noise |
| Inline fuse, 2A, on the 12V feed | not strictly required, but cheap insurance on a device that runs unattended |
| Stock 12V wall adapter | reused as-is |
| Stock weight-trigger cat switch | reused as-is, wired straight to a GPIO |
| Stock manual cycle button (optional) | reused if present/working |

## Pinout

| ESP32 pin | Function | Notes |
|---|---|---|
| GPIO26 | Motor IN1 | L298N direction |
| GPIO27 | Motor IN2 | L298N direction |
| GPIO25 | Motor ENA | PWM speed (ledc channel 0, 5kHz) |
| GPIO34 | Hall - Home | input-only pin, needs the external 10k pull-up (no internal pull-up available) |
| GPIO35 | Hall - Dump | input-only pin, needs the external 10k pull-up (no internal pull-up available) |
| GPIO32 | Weight/cat switch | active-low, internal pull-up used |
| GPIO33 | Manual cycle button (optional) | active-low, internal pull-up used |
| GPIO4 | Status LED | solid = idle/waiting, slow blink = cycling, fast blink = fault |
| 5V (from buck) | ESP32 VIN, hall sensor VCC, L298N logic supply | do **not** power ESP32 from the L298N's onboard 5V regulator directly off the 12V rail — motor switching noise rides straight into it |
| GND | common ground | tie ESP32 GND, L298N GND, buck converter GND, and 12V supply GND all together |

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
- Only one motor direction is used (`motorRunForward()` in firmware) — matches
  the stock LR2, which always rotates the globe the same way through a full
  revolution per cycle.

## Firmware

PlatformIO project targeting `esp32dev` / Arduino framework.

1. `cp include/secrets.h.example include/secrets.h` and set `OTA_PASSWORD`. That's
   the only build-time secret left — WiFi and MQTT are configured at runtime, see below.
2. Build the dashboard into the filesystem image: `cd web && npm install && npm run build:device && cd ..`
   (outputs to `./data`, which `uploadfs` below flashes to LittleFS).
3. `pio run -t uploadfs` (flashes the dashboard), then `pio run -t upload` (flashes the firmware).
4. Power it on. With no WiFi configured yet, it boots straight into setup mode.

The board serves the dashboard itself from then on — `http://lr2redux.local/`
once it's on your network, no separate hosting needed. Re-run step 2-3
whenever you change anything under `web/`.

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
BOOT_HOMING → IDLE ⇄ CAT_PRESENT → WAIT_TIMER → CYCLE_TO_DUMP → CYCLE_TO_HOME → IDLE
                                        │                │             │
                                        └── cat returns ─┘             │
                                                                        │
                       (weight switch trips during either CYCLE_* state)
                                        ▼
                                  SAFETY_STOP ──(switch clears)──► resumes cycling
                                        
              (any segment exceeds its timeout) ──► FAULT ──(MQTT reset_fault)──► BOOT_HOMING
```

- `WAIT_TIMER` defaults to 7 minutes after the cat leaves, matching stock LR2 behavior.
- Each cycle segment (Home→Dump, Dump→Home) has a 15s timeout; exceeding it stops
  the motor and enters `FAULT` rather than stalling the motor indefinitely.
- If the weight switch trips mid-cycle (cat jumps back in), the motor stops
  immediately and resumes from the same spot once the switch clears — it does not
  restart the segment.

### MQTT topics

MQTT is now strictly between the ESP32 and your broker (for Home Assistant) —
the web dashboard doesn't touch it at all, see
[WebSocket dashboard interface](#websocket-dashboard-interface) below.

| Topic | Direction | Payload |
|---|---|---|
| `lr2redux/state` | publish, retained | `homing`, `idle`, `cat_present`, `waiting`, `cycling`, `safety_stop`, `fault` |
| `lr2redux/cat_present` | publish, retained | `ON` / `OFF` |
| `lr2redux/cycle_count` | publish, retained | integer, persisted across reboots (NVS) |
| `lr2redux/drawer_full` | publish, retained | `ON` once `drawer_cycles` reaches `DRAWER_FULL_CYCLES` (default 10) |
| `lr2redux/drawer_cycles` | publish, retained | integer, cycles since the drawer was last emptied, persisted across reboots |
| `lr2redux/drawer_threshold` | publish, retained | `DRAWER_FULL_CYCLES` value, published once on connect so clients don't need to hardcode it |
| `lr2redux/heartbeat` | publish | uptime in seconds, every 30s |
| `lr2redux/cmd` | subscribe | `cycle` (manual trigger from IDLE), `reset_fault`, `drawer_emptied` (resets `drawer_cycles` to 0) |

Home Assistant MQTT discovery configs are published automatically on connect
under `homeassistant/.../lr2redux/...` — the device, its cycle/drawer sensors,
and the cycle-now/drawer-emptied buttons should all appear without any manual
`configuration.yaml` entries.

### WebSocket dashboard interface

The firmware also runs its own async web server (`ESPAsyncWebServer`) with a
WebSocket endpoint at `ws://lr2redux.local/ws`, independent of MQTT. This is
what the web dashboard talks to — no broker, no websocket-listener config, no
extra hop. mDNS (`lr2redux.local`) comes up automatically once the board
joins WiFi.

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
    "uptimeSeconds": 12345
  }
  ```
- Commands go the other way as JSON text frames: `{"cmd":"cycle"}`,
  `{"cmd":"reset_fault"}`, `{"cmd":"drawer_emptied"}` — same command set as
  the MQTT `lr2redux/cmd` topic, just a different transport.
- No auth on either interface currently; see `web/README.md` for the caveat.

### Config HTTP API

Three plain HTTP routes, available both in setup mode (on the SoftAP) and in
normal operation (on the STA connection) — same handlers either way:

| Route | Method | Body / response |
|---|---|---|
| `/scan` | GET | `{"status":"scanning"}` (202, poll again) or `{"networks":[{"ssid","rssi","secure"}]}` (200) |
| `/config` | GET | current `{wifiSsid, mqttHost, mqttPort, mqttUser, waitTimerMin}` (passwords never read back) |
| `/save` | POST | `{wifiSsid, wifiPass, mqttHost, mqttPort, mqttUser, mqttPass, waitTimerMin}` — blank password fields keep the existing one; saving always reboots the board |

This is what both the captive-portal setup page and the Grommet dashboard's
Settings card use — the dashboard just points at the same host as its
WebSocket connection. `Access-Control-Allow-Origin: *` is set globally so the
dashboard can call it cross-origin during `npm run dev`.

### Persistence, OTA, and the drawer counter

- `cycle_count` and `drawer_cycles` are stored in NVS (`Preferences` library,
  namespace `lr2redux`) and reloaded on boot, so a power cycle or firmware
  update doesn't lose them.
- `drawer_cycles` increments alongside `cycle_count` but resets independently
  via the `drawer_emptied` MQTT command (wired to the "LR2 Drawer Emptied"
  button in Home Assistant) — publish it after you empty the drawer.
- `DRAWER_FULL_CYCLES` in `main.cpp` sets the threshold (default 10); adjust to
  taste based on how many cats/cycles your drawer actually holds.
- OTA updates are enabled via `ArduinoOTA` (hostname `lr2redux`, password from
  `OTA_PASSWORD` in `secrets.h`). Once the board is on your network:
  `pio run -t upload --upload-port lr2redux.local`
  (or its IP). OTA starts a motor stop first so an update can't land mid-rotation.

## Web dashboard

See [`web/README.md`](web/README.md) — a Grommet + React single-page app that
talks straight to the ESP32's own WebSocket server (no MQTT broker, no
backend) to show live state, send `cycle` / `reset_fault` / `drawer_emptied`
commands, and (via its Settings card) reconfigure WiFi/MQTT/wait-timer over
the same config HTTP API the setup portal uses, including a network scan.

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
