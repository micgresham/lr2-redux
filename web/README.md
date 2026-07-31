# lr2-redux web dashboard

A [Grommet](https://v2.grommet.io/) + React single-page app that talks
**directly to the ESP32's built-in WebSocket server** — no MQTT broker
involved on the browser side at all. Organized into three tabs: **Dashboard**
(live state, commands, usage summary), **Analytics** (visit history and
day/night interval stats), and **Settings** (WiFi/MQTT/behavior config plus
firmware updates).

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

### Dashboard tab

- **Status** — current state (idle / cat present / waiting / cycling / safety
  stop / fault), cat-present flag (flagged red with "(a while)" once it
  exceeds the configurable cat-present warning threshold), cycle count, a
  freshness indicator (flags red if no update in >15s, 3x the firmware's
  broadcast interval), uptime (formatted as days-hours-mins-secs, e.g. `5d
  10h 54m 56s`), IP address, WiFi signal strength (dBm plus a qualitative
  Excellent/Good/Fair/Weak/Very weak label, colored red below -75dBm), and
  the firmware's build timestamp (so you can confirm which build is
  actually running on the device).
- **Drawer** — cycles since last emptied against the configurable threshold
  reported by the firmware, plus a "Mark drawer emptied" button.
- **Actions** — "Cycle now" (enabled only when idle), "Reset fault" (enabled
  only when faulted), and "Resume" (enabled only during a `SAFETY_STOP` that
  needs an explicit manual confirmation — see the root README's "Manual-reset"
  notes for when that applies).
- **Usage** — today/this-week/last-30-days visit totals and a 30-day daily
  bar chart. Computed **client-side** from the raw visit-timestamp array the
  device reports, bucketed by the viewer's own local calendar day — not
  trusted from the device's own precomputed totals, which use a rolling
  24-hour window in the device's UTC clock rather than a real calendar day
  in your timezone (`UsageCard.tsx`).

### Analytics tab

- **Day / night hours** — editable start/end hours (default 6am–8pm) used to
  split visit intervals below. Saves through the same config API as the
  Settings tab (saving reboots the board).
- **Average time between visits** — the average gap between consecutive
  visits, computed separately for visits classified as "day" vs. "night"
  by each visit's local hour in your browser.
- **Visits per day** — the same 30-day daily bar chart as the Dashboard tab's
  Usage card, reused here via a shared `VisitChart.tsx` component and
  `useDailyVisitBuckets.ts` hook rather than a second copy of the chart code.
- **Recent visits** — a scrollable list of the most recent visit timestamps
  (up to the last 300 the device stores), each tagged Day or Night, plus an
  "Export CSV" button that downloads all stored visits (not just the ones
  shown on-screen) as `epochSeconds,timestampUTC,dayOrNight` rows.

### Settings tab

- WiFi SSID (with a "Scan for networks" button) and password, MQTT
  host/port/user/password, the cat-leaves wait timer, and — under a
  collapsible "Advanced settings" section — the cat-present warning
  threshold, "require manual reset after any interruption," cycles until
  the drawer is considered full, home overshoot duration, and dump shake
  swing duration/count (all also mirrored on MQTT for Home Assistant — see
  the root README's "MQTT topics" section). Talks to the same `/scan`,
  `/config`, `/save` HTTP routes the captive-portal setup page uses (see the
  root README's "Config HTTP API" section) — this is the same config
  surface, just reachable once the board is already on your network instead
  of only during first-time setup. Saving reboots the board; if you changed
  the WiFi network, the dashboard won't be able to reach it again until you
  reconnect to that network too. Otherwise, the button shows "Reconnecting…"
  and the page polls `/config` in the background until the board comes back
  up, then shows "Saved and reconnected" with the fresh values — no manual
  page refresh needed, and a network hiccup right at reboot (expected, since
  the board drops the connection mid-response) isn't reported as a failure.
- **Firmware update** — upload a new `firmware.bin` directly from the
  browser (password-protected with the same `OTA_PASSWORD` as command-line
  OTA), with an upload-progress indicator.

No authentication is implemented beyond the firmware-update password above —
anyone who can reach the board's `/ws` endpoint and knows the command names
can send commands. Fine on a home LAN; put it behind your own auth/reverse
proxy if it's exposed further than that.
