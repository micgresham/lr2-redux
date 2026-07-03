# lr2-redux web dashboard

A [Grommet](https://v2.grommet.io/) + React single-page app that talks
**directly to the ESP32's built-in WebSocket server** — no MQTT broker
involved on the browser side at all. It shows live state, cat/drawer status,
lets you send `cycle`, `reset_fault`, and `drawer_emptied` commands, and
configure WiFi/MQTT/wait-timer.

**The board serves this app itself** (`npm run build:device` + `pio run -t
uploadfs` from the repo root — see the root README) — visiting
`http://lr2redux.local/` once it's on your network gets you the real
dashboard, no separate hosting required. It also runs fine standalone via
`npm run dev`, e.g. while developing it, or pointed at a different board.

## How it connects

The firmware runs its own WebSocket endpoint at `/ws` (`ESPAsyncWebServer`).
On connect it sends a JSON snapshot of the current state; after that it
pushes an updated snapshot on every state change and at least every 5 seconds
regardless, so the dashboard stays live without polling. Commands go the
other way as `{"cmd":"cycle"}` JSON frames.

When this app is served **by the board**, it derives the WebSocket URL from
`window.location` automatically — no configuration needed, it just points at
wherever it's being loaded from. When run standalone (`npm run dev`, a
different origin), it falls back to `VITE_DEFAULT_DEVICE_URL` or
`ws://lr2redux.local/ws`, and the device field in the UI is always editable
at runtime (persisted to `localStorage`) if you need to point it elsewhere.

MQTT still exists — it's just entirely between the ESP32 and your broker now
(for Home Assistant discovery). The browser never touches it, so there's no
broker websocket listener to configure, no port 9001, nothing beyond the
board itself being reachable on your LAN.

## Standalone development

```
npm install
cp .env.example .env.local   # optional: pins the default device URL during dev
npm run dev
```

Opens on `http://localhost:5173`, talking to whatever board `.env.local` (or
the device field in the UI) points at.

## Building

```
npm run build          # standalone dist/, host it anywhere
npm run build:device   # builds straight into ../data for `pio run -t uploadfs`
```

Both are the same app - `build:device` just changes the output directory to
match what the firmware's LittleFS-based static server expects.

## What it shows

- **Status** — current state (idle / cat present / waiting / cycling / safety
  stop / fault), cat-present flag, cycle count, and a freshness indicator
  (flags red if no update in >15s, 3x the firmware's broadcast interval).
- **Drawer** — cycles since last emptied against the configurable threshold
  reported by the firmware, plus a "Mark drawer emptied" button.
- **Actions** — "Cycle now" (enabled only when idle) and "Reset fault"
  (enabled only when faulted).
- **Settings** — WiFi SSID (with a "Scan for networks" button), WiFi
  password, MQTT host/port/user/password, and the cat-leaves wait timer in
  minutes. Talks to the same `/scan`, `/config`, `/save` HTTP routes the
  captive-portal setup page uses (see the root README's "Config HTTP API"
  section) — this is the same config surface, just reachable once the board
  is already on your network instead of only during first-time setup.
  Saving reboots the board; if you changed the WiFi network, the dashboard
  won't be able to reach it again until you reconnect to that network too.

No authentication is implemented — anyone who can reach the board's `/ws`
endpoint and knows the command names can send commands. Fine on a home LAN;
put it behind your own auth/reverse proxy if it's exposed further than that.
