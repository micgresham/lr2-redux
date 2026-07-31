// LR2-Redux: ESP32 + L298N replacement control board for a Litter Robot 2.
//
// Reuses the stock 12V gearmotor, stock weight-trigger cat switch, and the
// stock hall-effect Home/Dump position sensor mounting points. Adds MQTT +
// Home Assistant discovery so the unit reports state and accepts manual
// cycle / drawer-reset commands over WiFi, plus OTA firmware updates and
// NVS-backed persistence of cycle counts across reboots.
//
// The board serves its own web dashboard (built from web/, copied into
// ./data, flashed to LittleFS via `pio run -t uploadfs`) at
// http://lr2redux.local/ - it talks to the board over a WebSocket
// (ws://lr2redux.local/ws, JSON in both directions) instead of connecting to
// the MQTT broker itself - MQTT here is only for the ESP32<->broker link
// (Home Assistant integration), the browser never needs broker access.
//
// WiFi/MQTT credentials and the cat-leaves wait timer are runtime config
// (see config.h), not build-time secrets - the same firmware image works on
// any unit. If nothing's configured yet, or the cycle button is held 10s,
// the board boots into a SoftAP + captive portal instead (setup_portal.h)
// so you can set them from a phone/laptop.
//
// See README.md for wiring, BOM, and the state machine diagram.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_system.h>
#include "config.h"
#include "config_api.h"
#include "setup_portal.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and set OTA_PASSWORD"
#endif

// ---------------------------------------------------------------------------
// Firmware version
// ---------------------------------------------------------------------------
// Compile-time build timestamp rather than a manually-bumped version number -
// always accurate (can't forget to bump it), and enough to answer "is this
// the build I just flashed" from the dashboard, which is the actual need.
static const char *FIRMWARE_BUILD = __DATE__ " " __TIME__;

// ---------------------------------------------------------------------------
// Pin map
// ---------------------------------------------------------------------------
// DRV8871 (production motor driver as of 2026-07-07) has no separate
// enable/PWM pin - IN1/IN2 are PWM'd directly instead, whichever is the
// active direction. GPIO25 (the old L298N ENA line) is no longer used here;
// it's still driven from the breadboard/diagnostic tool since that keeps
// the L298N ENA line alive there and costs nothing on a DRV8871 board.
static const uint8_t PIN_MOTOR_IN1 = 26;
static const uint8_t PIN_MOTOR_IN2 = 27;
static const uint8_t PIN_HALL_HOME = 34; // input-only pin, needs external pull-up to 3.3V
static const uint8_t PIN_HALL_DUMP = 35; // input-only pin, needs external pull-up to 3.3V
static const uint8_t PIN_WEIGHT_SWITCH = 32; // stock mechanical cat-weight switch
static const uint8_t PIN_ANTI_PINCH = 14; // stock anti-pinch switch, physically split from the weight
                                           // switch's shared loop - see CLAUDE.md "Pins 6/7 topology"
static const uint8_t PIN_MANUAL_BUTTON = 33; // optional stock "cycle now" button
// Three discrete LEDs (not a bi-color/2-channel part) replicating the
// original stock board's status language exactly - see CLAUDE.md for the
// mapping and README's "Status LED" table.
static const uint8_t PIN_LED_GREEN = 4;
static const uint8_t PIN_LED_YELLOW = 16;
static const uint8_t PIN_LED_RED = 17;

static const uint8_t PWM_CHANNEL_IN1 = 0;
static const uint8_t PWM_CHANNEL_IN2 = 1;
static const uint16_t PWM_FREQ_HZ = 5000;
static const uint8_t PWM_RESOLUTION_BITS = 8;
static const uint8_t MOTOR_SPEED = 220; // 0-255, stays below full 255 to soften inrush/noise

// Hall sensors and the weight switch are wired active-low (pulled up, sensor
// pulls the line to GND when triggered).
static const int SENSOR_ACTIVE = LOW;

// ---------------------------------------------------------------------------
// Timing configuration
// ---------------------------------------------------------------------------
static const unsigned long DEBOUNCE_MS = 25;
// how long the cat-leaves wait timer runs is now runtime config (cfg.waitTimerSec) - see config.h
// Placeholder value (2026-07-05) pending a real timed measurement of one
// segment's actual rotation time - the original 15s was never validated
// against real hardware. Raised 15s -> 90s -> 180s, both previous values
// still faulted on real hardware (90s faulted at exactly 90s elapsed).
// 180s is a deliberately generous placeholder to unblock testing; dial it
// back down once an actual segment time is timed with a stopwatch.
// BOOT_HOMING uses 2x this (360s) for its own timeout.
static const unsigned long CYCLE_SEGMENT_TIMEOUT_MS = 180UL * 1000UL; // max time to reach next sensor before fault
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
static const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
static const unsigned long HEARTBEAT_INTERVAL_MS = 30UL * 1000UL;
static const unsigned long SETUP_HOLD_MS = 10UL * 1000UL; // hold the cycle button this long to force setup mode

// Dump dwell: pause 5s at Dump (lets waste finish falling through) before
// the shake/reverse. Placeholder duration, same "unvalidated against real
// hardware" caveat as CYCLE_SEGMENT_TIMEOUT_MS. The shake step/count and
// home-overshoot duration that used to live here are now runtime config
// (cfg.dumpShakeStepMs/dumpShakeCount/homeOvershootMs) - see config.h -
// since how far the motor needs to travel to dislodge clumps or level
// litter depends on litter type/depth, not something one fixed build-time
// value can suit for everyone.
static const unsigned long DUMP_PAUSE_MS = 5UL * 1000UL;

// If the weight switch (not anti-pinch) trips mid-motion, wait this long
// after it clears before actually resuming, rather than continuing the
// instant it clears - gives the cat a real buffer before the globe moves
// again, matching the same reasoning as WAIT_TIMER's minimum. Anti-pinch
// alone doesn't get this extra wait; clearing it means the mechanical
// condition is resolved, not "is the cat truly gone."
static const unsigned long WEIGHT_SAFETY_COOLDOWN_MS = 2UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// MQTT topics
// ---------------------------------------------------------------------------
static const char *TOPIC_STATE = "lr2redux/state";
static const char *TOPIC_CAT_PRESENT = "lr2redux/cat_present";
static const char *TOPIC_CYCLE_COUNT = "lr2redux/cycle_count";
static const char *TOPIC_DRAWER_FULL = "lr2redux/drawer_full";
static const char *TOPIC_DRAWER_CYCLES = "lr2redux/drawer_cycles";
static const char *TOPIC_DRAWER_THRESHOLD = "lr2redux/drawer_threshold";
static const char *TOPIC_DRAWER_THRESHOLD_CMD = "lr2redux/drawer_threshold/set";
static const char *TOPIC_HEARTBEAT = "lr2redux/heartbeat";
static const char *TOPIC_CMD = "lr2redux/cmd";

// Extra sensor data - mirrors fields already in buildStateJson() (the web
// dashboard's WS telemetry) so Home Assistant has the same visibility the
// dashboard does, not just state/cat/cycles/drawer.
static const char *TOPIC_UPTIME = "lr2redux/uptime_seconds";
static const char *TOPIC_RSSI = "lr2redux/rssi";
static const char *TOPIC_IP = "lr2redux/ip_address";
static const char *TOPIC_FIRMWARE = "lr2redux/firmware_build";
static const char *TOPIC_NEEDS_MANUAL_RESET = "lr2redux/needs_manual_reset";
static const char *TOPIC_CAT_PRESENT_WARNING = "lr2redux/cat_present_warning";

// Settings, mirrored from DeviceConfig (config.h) as read/write MQTT
// entities (HA "number"/"switch") - the same fields the web Settings page
// edits via HTTP /save, same validation bounds (config.h), so both control
// surfaces stay in sync and can't disagree on what's a valid value. Unlike
// /save, writing one of these never reboots the board - none of these
// values need a restart to take effect (the state machine already reads
// cfg.* live every loop() iteration) - see onMqttMessage().
static const char *TOPIC_WAIT_TIMER_MIN = "lr2redux/wait_timer_min";
static const char *TOPIC_WAIT_TIMER_MIN_CMD = "lr2redux/wait_timer_min/set";
static const char *TOPIC_CAT_WARNING_MIN = "lr2redux/cat_present_warning_min";
static const char *TOPIC_CAT_WARNING_MIN_CMD = "lr2redux/cat_present_warning_min/set";
static const char *TOPIC_HOME_OVERSHOOT_MS = "lr2redux/home_overshoot_ms";
static const char *TOPIC_HOME_OVERSHOOT_MS_CMD = "lr2redux/home_overshoot_ms/set";
static const char *TOPIC_SHAKE_STEP_MS = "lr2redux/dump_shake_step_ms";
static const char *TOPIC_SHAKE_STEP_MS_CMD = "lr2redux/dump_shake_step_ms/set";
static const char *TOPIC_SHAKE_COUNT = "lr2redux/dump_shake_count";
static const char *TOPIC_SHAKE_COUNT_CMD = "lr2redux/dump_shake_count/set";
static const char *TOPIC_REQUIRE_MANUAL_RESET = "lr2redux/require_manual_reset";
static const char *TOPIC_REQUIRE_MANUAL_RESET_CMD = "lr2redux/require_manual_reset/set";

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum class State {
  BOOT_HOMING,        // power-on: find Home before doing anything else
  IDLE,               // at Home, no cat, nothing to do
  CAT_PRESENT,        // weight switch engaged
  WAIT_TIMER,         // cat left, waiting out the clean-cycle timer
  CYCLE_TO_DUMP,      // motor running forward, looking for the Dump sensor
  CYCLE_DUMP_PAUSE,   // at Dump, motor stopped, dwelling so waste can fall through
  CYCLE_DUMP_SHAKE,   // at Dump, motor oscillating briefly to dislodge stuck waste
  CYCLE_TO_HOME,      // motor running reverse, looking for the Home sensor
  CYCLE_HOME_OVERSHOOT, // past Home, still reversing briefly to help litter settle/level
  CYCLE_HOME_SETTLE,  // running forward briefly back to Home after the overshoot
  SAFETY_STOP,        // weight/anti-pinch triggered mid-motion, motor halted, waiting for it to clear
  FAULT               // a segment timed out; needs an explicit reset command
};

static State state = State::BOOT_HOMING;
static State stateBeforeSafetyStop = State::CYCLE_TO_DUMP;
// BOOT_HOMING doesn't know which way Home is from an arbitrary starting
// position, so it can't safely assume running forward will reach Home
// directly - there may be a mechanical hard stop it would stall against.
// Instead it seeks Dump first (forward, same as a normal cycle), then
// reverses to Home from there - reusing the same direction logic already
// validated for normal cycling instead of a new, unproven strategy. Reset
// to false on every (re-)entry into BOOT_HOMING, see enterState().
static bool bootHomingFoundDump = false;
// 0 = weight switch is currently active (or hasn't cleared yet this
// SAFETY_STOP); non-zero = millis() timestamp of when it first cleared.
// Reset to 0 on every fresh entry into SAFETY_STOP - see the interlock
// check in runStateMachine().
static unsigned long weightClearedAt = 0;
// True if anti-pinch was active at trigger time, or becomes active at any
// point during the current SAFETY_STOP - drives both the flashing-yellow
// LED color and (combined with cfg.requireManualReset) whether this
// episode needs an explicit manual resume rather than auto-resuming.
// Reset on every fresh SAFETY_STOP entry.
static bool safetyStopAntiPinchInvolved = false;
// Set by a manual-button press or an MQTT/WS "resume" command while in
// SAFETY_STOP and manual reset is required. Consumed (reset to false) the
// moment it's actually used to resume.
static bool manualResumeRequested = false;
static unsigned long stateEnteredAt = 0;
static unsigned long cycleCount = 0;
static unsigned long drawerCycles = 0;

// ---------------------------------------------------------------------------
// Usage analytics: a rolling 30-day hourly visit log, persisted to NVS.
// "Visit" = a new occupancy session (IDLE -> CAT_PRESENT), not every
// weight-switch bounce. Indexed by absolute hours-since-epoch modulo the
// buffer size, so it self-ages: a slot only ever holds data from within the
// current 30-day window, since the window size equals the buffer size -
// the one edge case is if the device is powered off for more than 30
// consecutive days, in which case a slot not touched since could still hold
// a stale count from over a month ago until the next visit overwrites it.
// Needs real wall-clock time (NTP) to bucket correctly - recordVisit() is a
// no-op until ntpTimeSynced is true, rather than corrupting the log by
// bucketing into whatever `time()` returns before it's actually synced
// (near-zero, i.e. index 0, on every unsynced boot).
// ---------------------------------------------------------------------------
static const int VISIT_LOG_HOURS = 30 * 24; // 720
static uint8_t visitLog[VISIT_LOG_HOURS];
static bool ntpConfigured = false;
static bool ntpTimeSynced = false;

// Rolling log of individual visit timestamps (raw epoch seconds), for the
// Analytics page's "recent visits" list and its day/night average-interval
// breakdown - the hourly visitLog above only holds counts, not enough
// precision for either of those. Circular buffer: visitTimesHead is the
// next write slot; once full, the oldest entry is overwritten. 300 entries
// is generous for a litter box (usually well under 300 visits/month) while
// staying tiny in NVS (1200 bytes).
static const int VISIT_TIMES_CAPACITY = 300;
static uint32_t visitTimes[VISIT_TIMES_CAPACITY];
static uint16_t visitTimesHead = 0;
static uint16_t visitTimesCount = 0;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static DeviceConfig cfg;
static bool inSetupMode = false;

// ---------------------------------------------------------------------------
// Debounced digital input
// ---------------------------------------------------------------------------
struct DebouncedInput {
  uint8_t pin;
  int stableState;
  int lastReading;
  unsigned long lastChangeAt;
  bool activeHigh; // true for normally-closed switches that open on trip
                    // (e.g. the anti-pinch switch) - everything else in this
                    // project is active-low (SENSOR_ACTIVE), triggered by
                    // pulling the line to GND.

  // useInternalPullup: true for the weight switch / manual button (GPIO32/33
  // support it). Hall sensors on GPIO34/35 don't - they're input-only pins
  // with no pull resistor in hardware, hence the external 10k on the board.
  //
  // activeHigh: false (default) for every active-low input in this project.
  // Set true only for a normally-closed switch like the anti-pinch switch,
  // which reads HIGH (pulled up, circuit open) when actually tripped and
  // LOW (pulled to GND through the closed switch) at rest - the opposite of
  // every other input here, and a real bug if not accounted for, since a
  // fail-safe NC switch reading "active" by default would spend its whole
  // life reporting a trip that isn't there, while silently missing the one
  // that is.
  void begin(uint8_t p, bool useInternalPullup = false, bool activeHighIn = false) {
    pin = p;
    activeHigh = activeHighIn;
    pinMode(pin, useInternalPullup ? INPUT_PULLUP : INPUT);
    stableState = digitalRead(pin);
    lastReading = stableState;
    lastChangeAt = millis();
  }

  void update() {
    int reading = digitalRead(pin);
    if (reading != lastReading) {
      lastChangeAt = millis();
      lastReading = reading;
    }
    if (millis() - lastChangeAt > DEBOUNCE_MS) {
      stableState = lastReading;
    }
  }

  bool isActive() const { return activeHigh ? (stableState == HIGH) : (stableState == SENSOR_ACTIVE); }
};

static DebouncedInput hallHome;
static DebouncedInput hallDump;
static DebouncedInput weightSwitch;
static DebouncedInput antiPinch;
static DebouncedInput manualButton;

// ---------------------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------------------
static void motorStop() {
  ledcWrite(PWM_CHANNEL_IN1, 0);
  ledcWrite(PWM_CHANNEL_IN2, 0);
}

static void motorRunForward() {
  ledcWrite(PWM_CHANNEL_IN2, 0);
  ledcWrite(PWM_CHANNEL_IN1, MOTOR_SPEED);
}

// Home->Dump runs forward; Dump->Home runs reverse - confirmed on real
// hardware (2026-07-05) that the globe does NOT continue forward past Dump
// back around to Home. It needs to stop at Dump and reverse. Bench testing
// without this fix let the motor keep running forward past the Dump
// sensor entirely, overshooting the intended stop.
static void motorRunReverse() {
  ledcWrite(PWM_CHANNEL_IN1, 0);
  ledcWrite(PWM_CHANNEL_IN2, MOTOR_SPEED);
}

// ---------------------------------------------------------------------------
// MQTT publishing
// ---------------------------------------------------------------------------
static const char *stateName(State s) {
  switch (s) {
    case State::BOOT_HOMING: return "homing";
    case State::IDLE: return "idle";
    case State::CAT_PRESENT: return "cat_present";
    case State::WAIT_TIMER: return "waiting";
    case State::CYCLE_TO_DUMP: return "cycling";
    case State::CYCLE_DUMP_PAUSE: return "cycling";
    case State::CYCLE_DUMP_SHAKE: return "cycling";
    case State::CYCLE_TO_HOME: return "cycling";
    case State::CYCLE_HOME_OVERSHOOT: return "cycling";
    case State::CYCLE_HOME_SETTLE: return "cycling";
    case State::SAFETY_STOP: return "safety_stop";
    case State::FAULT: return "fault";
  }
  return "unknown";
}

static void publishState() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_STATE, stateName(state), true);
  mqtt.publish(TOPIC_CAT_PRESENT, weightSwitch.isActive() ? "ON" : "OFF", true);
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", cycleCount);
  mqtt.publish(TOPIC_CYCLE_COUNT, buf, true);
}

static void publishDrawerState() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_DRAWER_FULL, drawerCycles >= cfg.drawerFullCycles ? "ON" : "OFF", true);
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", drawerCycles);
  mqtt.publish(TOPIC_DRAWER_CYCLES, buf, true);
}

// Shared by buildStateJson() (WS telemetry) and publishExtraSensors() (MQTT)
// so the two control surfaces can't drift on what these mean.
static bool needsManualResetNow() {
  // Only meaningful while state == SAFETY_STOP - true if this episode needs
  // an explicit resume (anti-pinch involvement, or cfg.requireManualReset)
  // rather than auto-resuming on its own.
  return state == State::SAFETY_STOP && (safetyStopAntiPinchInvolved || cfg.requireManualReset);
}

static bool catPresentWarningNow() {
  // Only meaningful while state == CAT_PRESENT - the cat's been there
  // longer than cfg.catPresentWarningSec.
  return state == State::CAT_PRESENT &&
         (millis() - stateEnteredAt) > (unsigned long)cfg.catPresentWarningSec * 1000UL;
}

// ---------------------------------------------------------------------------
// Web dashboard: WebSocket state broadcast (JSON), replaces the browser
// talking MQTT directly - only this firmware speaks MQTT now.
// ---------------------------------------------------------------------------
static void buildStateJson(JsonDocument &doc) {
  doc["state"] = stateName(state);
  doc["catPresent"] = weightSwitch.isActive();
  doc["cycleCount"] = cycleCount;
  doc["drawerFull"] = drawerCycles >= cfg.drawerFullCycles;
  doc["drawerCycles"] = drawerCycles;
  doc["drawerThreshold"] = cfg.drawerFullCycles;
  doc["uptimeSeconds"] = millis() / 1000;
  doc["needsManualReset"] = needsManualResetNow();
  doc["catPresentWarning"] = catPresentWarningNow();
  doc["ipAddress"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  // Signal strength - not otherwise visible anywhere (not even the serial
  // log), added specifically to help diagnose the recurring ASSOC_LEAVE
  // WiFi issue (2026-07-06): weak/marginal RSSI at the board's install
  // location would point at signal strength/interference rather than
  // power or firmware. 0 is a real possible RSSI-adjacent sentinel here
  // (not connected), distinguished by the dashboard checking connection
  // status separately rather than treating 0 as "no signal."
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["firmwareBuild"] = FIRMWARE_BUILD;
}

static void broadcastState() {
  if (ws.count() == 0) return;
  JsonDocument doc;
  buildStateJson(doc);
  String payload;
  serializeJson(doc, payload);
  ws.textAll(payload);
}

// Sensor data that only exists on the WS dashboard telemetry until now
// (uptime, WiFi signal, IP, firmware build, the two warning flags) - added
// to MQTT so Home Assistant has the same visibility the dashboard does.
static void publishExtraSensors() {
  if (!mqtt.connected()) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
  mqtt.publish(TOPIC_UPTIME, buf, true);
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  snprintf(buf, sizeof(buf), "%d", wifiUp ? WiFi.RSSI() : 0);
  mqtt.publish(TOPIC_RSSI, buf, true);
  mqtt.publish(TOPIC_IP, wifiUp ? WiFi.localIP().toString().c_str() : "", true);
  mqtt.publish(TOPIC_FIRMWARE, FIRMWARE_BUILD, true);
  mqtt.publish(TOPIC_NEEDS_MANUAL_RESET, needsManualResetNow() ? "ON" : "OFF", true);
  mqtt.publish(TOPIC_CAT_PRESENT_WARNING, catPresentWarningNow() ? "ON" : "OFF", true);
}

// The Settings-page values (config.h) as MQTT state - published on connect
// and whenever one changes via onMqttMessage(), so Home Assistant's number/
// switch entities always show the board's actual current value (including
// snapping back to it if a written value was out of bounds and rejected).
static void publishSettingsState() {
  if (!mqtt.connected()) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", cfg.waitTimerSec / 60);
  mqtt.publish(TOPIC_WAIT_TIMER_MIN, buf, true);
  snprintf(buf, sizeof(buf), "%lu", cfg.catPresentWarningSec / 60);
  mqtt.publish(TOPIC_CAT_WARNING_MIN, buf, true);
  snprintf(buf, sizeof(buf), "%lu", cfg.drawerFullCycles);
  mqtt.publish(TOPIC_DRAWER_THRESHOLD, buf, true);
  snprintf(buf, sizeof(buf), "%lu", cfg.homeOvershootMs);
  mqtt.publish(TOPIC_HOME_OVERSHOOT_MS, buf, true);
  snprintf(buf, sizeof(buf), "%lu", cfg.dumpShakeStepMs);
  mqtt.publish(TOPIC_SHAKE_STEP_MS, buf, true);
  snprintf(buf, sizeof(buf), "%lu", cfg.dumpShakeCount);
  mqtt.publish(TOPIC_SHAKE_COUNT, buf, true);
  mqtt.publish(TOPIC_REQUIRE_MANUAL_RESET, cfg.requireManualReset ? "ON" : "OFF", true);
}

// Minimal Home Assistant MQTT discovery so the unit shows up automatically.
static void publishHomeAssistantDiscovery() {
  const char *deviceBlock =
      "\"device\":{\"identifiers\":[\"lr2redux\"],\"name\":\"Litter Robot 2\",\"manufacturer\":\"lr2-redux\"}";

  char payload[700];

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 State\",\"uniq_id\":\"lr2redux_state\","
           "\"stat_t\":\"%s\",%s}",
           TOPIC_STATE, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/state/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Cat Present\",\"uniq_id\":\"lr2redux_cat\","
           "\"stat_t\":\"%s\",\"dev_cla\":\"occupancy\",%s}",
           TOPIC_CAT_PRESENT, deviceBlock);
  mqtt.publish("homeassistant/binary_sensor/lr2redux/cat/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Cycle Count\",\"uniq_id\":\"lr2redux_cycles\","
           "\"stat_t\":\"%s\",%s}",
           TOPIC_CYCLE_COUNT, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/cycles/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Cycle Now\",\"uniq_id\":\"lr2redux_cycle_btn\","
           "\"cmd_t\":\"%s\",\"payload_press\":\"cycle\",%s}",
           TOPIC_CMD, deviceBlock);
  mqtt.publish("homeassistant/button/lr2redux/cycle/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Drawer Full\",\"uniq_id\":\"lr2redux_drawer_full\","
           "\"stat_t\":\"%s\",\"dev_cla\":\"problem\",%s}",
           TOPIC_DRAWER_FULL, deviceBlock);
  mqtt.publish("homeassistant/binary_sensor/lr2redux/drawer_full/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Drawer Cycles\",\"uniq_id\":\"lr2redux_drawer_cycles\","
           "\"stat_t\":\"%s\",%s}",
           TOPIC_DRAWER_CYCLES, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/drawer_cycles/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Drawer Emptied\",\"uniq_id\":\"lr2redux_drawer_btn\","
           "\"cmd_t\":\"%s\",\"payload_press\":\"drawer_emptied\",%s}",
           TOPIC_CMD, deviceBlock);
  mqtt.publish("homeassistant/button/lr2redux/drawer_emptied/config", payload, true);

  // These two already worked over plain MQTT (handleCommand() accepts them
  // on TOPIC_CMD same as cycle/drawer_emptied) but weren't Home-Assistant-
  // discoverable, so they never showed up as pressable entities.
  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Reset Fault\",\"uniq_id\":\"lr2redux_reset_fault_btn\","
           "\"cmd_t\":\"%s\",\"payload_press\":\"reset_fault\",%s}",
           TOPIC_CMD, deviceBlock);
  mqtt.publish("homeassistant/button/lr2redux/reset_fault/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Resume\",\"uniq_id\":\"lr2redux_resume_btn\","
           "\"cmd_t\":\"%s\",\"payload_press\":\"resume\",%s}",
           TOPIC_CMD, deviceBlock);
  mqtt.publish("homeassistant/button/lr2redux/resume/config", payload, true);

  // Extra sensors - mirrors fields already in the web dashboard's WS
  // telemetry (buildStateJson()), published by publishExtraSensors().
  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Uptime\",\"uniq_id\":\"lr2redux_uptime\","
           "\"stat_t\":\"%s\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",%s}",
           TOPIC_UPTIME, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/uptime/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 WiFi Signal\",\"uniq_id\":\"lr2redux_rssi\","
           "\"stat_t\":\"%s\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",%s}",
           TOPIC_RSSI, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/rssi/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 IP Address\",\"uniq_id\":\"lr2redux_ip\","
           "\"stat_t\":\"%s\",\"ent_cat\":\"diagnostic\",%s}",
           TOPIC_IP, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/ip/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Firmware Build\",\"uniq_id\":\"lr2redux_firmware\","
           "\"stat_t\":\"%s\",\"ent_cat\":\"diagnostic\",%s}",
           TOPIC_FIRMWARE, deviceBlock);
  mqtt.publish("homeassistant/sensor/lr2redux/firmware/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Needs Manual Reset\",\"uniq_id\":\"lr2redux_needs_manual_reset\","
           "\"stat_t\":\"%s\",\"dev_cla\":\"problem\",%s}",
           TOPIC_NEEDS_MANUAL_RESET, deviceBlock);
  mqtt.publish("homeassistant/binary_sensor/lr2redux/needs_manual_reset/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Cat Present Warning\",\"uniq_id\":\"lr2redux_cat_warning\","
           "\"stat_t\":\"%s\",\"dev_cla\":\"problem\",%s}",
           TOPIC_CAT_PRESENT_WARNING, deviceBlock);
  mqtt.publish("homeassistant/binary_sensor/lr2redux/cat_warning/config", payload, true);

  // Settings, as writable "number"/"switch" entities - same fields/bounds
  // as the web Settings page's HTTP /save (config.h holds the shared
  // bounds). Applied immediately on receipt, no reboot needed - see
  // onMqttMessage().
  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Wait Timer (min)\",\"uniq_id\":\"lr2redux_wait_timer\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":60,\"step\":1,"
           "\"unit_of_meas\":\"min\",\"ent_cat\":\"config\",%s}",
           TOPIC_WAIT_TIMER_MIN_CMD, TOPIC_WAIT_TIMER_MIN, CFG_MIN_WAIT_TIMER_MIN, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/wait_timer/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Cat Present Warning (min)\",\"uniq_id\":\"lr2redux_cat_warning_min\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":60,\"step\":1,"
           "\"unit_of_meas\":\"min\",\"ent_cat\":\"config\",%s}",
           TOPIC_CAT_WARNING_MIN_CMD, TOPIC_CAT_WARNING_MIN, CFG_MIN_CAT_WARNING_MIN, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/cat_warning_min/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Drawer Full Threshold\",\"uniq_id\":\"lr2redux_drawer_threshold\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":100,\"step\":1,"
           "\"unit_of_meas\":\"cycles\",\"ent_cat\":\"config\",%s}",
           TOPIC_DRAWER_THRESHOLD_CMD, TOPIC_DRAWER_THRESHOLD, CFG_MIN_DRAWER_FULL_CYCLES, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/drawer_threshold/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Home Overshoot (ms)\",\"uniq_id\":\"lr2redux_home_overshoot\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":100,"
           "\"unit_of_meas\":\"ms\",\"ent_cat\":\"config\",%s}",
           TOPIC_HOME_OVERSHOOT_MS_CMD, TOPIC_HOME_OVERSHOOT_MS, CFG_MIN_HOME_OVERSHOOT_MS,
           CFG_MAX_HOME_OVERSHOOT_MS, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/home_overshoot/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Shake Swing Duration (ms)\",\"uniq_id\":\"lr2redux_shake_step\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":50,"
           "\"unit_of_meas\":\"ms\",\"ent_cat\":\"config\",%s}",
           TOPIC_SHAKE_STEP_MS_CMD, TOPIC_SHAKE_STEP_MS, CFG_MIN_SHAKE_STEP_MS,
           CFG_MAX_SHAKE_STEP_MS, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/shake_step/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Number Of Shakes\",\"uniq_id\":\"lr2redux_shake_count\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"min\":%d,\"max\":%d,\"step\":1,"
           "\"ent_cat\":\"config\",%s}",
           TOPIC_SHAKE_COUNT_CMD, TOPIC_SHAKE_COUNT, CFG_MIN_SHAKE_COUNT,
           CFG_MAX_SHAKE_COUNT, deviceBlock);
  mqtt.publish("homeassistant/number/lr2redux/shake_count/config", payload, true);

  snprintf(payload, sizeof(payload),
           "{\"name\":\"LR2 Require Manual Reset\",\"uniq_id\":\"lr2redux_require_manual_reset\","
           "\"cmd_t\":\"%s\",\"stat_t\":\"%s\",\"ent_cat\":\"config\",%s}",
           TOPIC_REQUIRE_MANUAL_RESET_CMD, TOPIC_REQUIRE_MANUAL_RESET, deviceBlock);
  mqtt.publish("homeassistant/switch/lr2redux/require_manual_reset/config", payload, true);
}

// ---------------------------------------------------------------------------
// Command handling - shared by MQTT (lr2redux/cmd) and the dashboard
// WebSocket ({"cmd":"..."}), so both control surfaces stay in sync.
// ---------------------------------------------------------------------------
static bool manualCycleRequested = false;
static bool faultResetRequested = false;
static bool drawerEmptiedRequested = false;

static void handleCommand(const String &cmd) {
  if (cmd == "cycle") manualCycleRequested = true;
  else if (cmd == "reset_fault") faultResetRequested = true;
  else if (cmd == "drawer_emptied") drawerEmptiedRequested = true;
  // Only meaningful in SAFETY_STOP when manual reset is required (anti-pinch
  // involvement, or cfg.requireManualReset) - a no-op otherwise, mirroring
  // "press any button to reset" from the original stock board.
  else if (cmd == "resume") manualResumeRequested = true;
}

static void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t = String(topic);

  if (t == TOPIC_CMD) {
    handleCommand(msg);
    return;
  }

  // Settings writes - same fields/bounds as config_api.cpp's /save handler
  // (config.h holds the shared bounds). An out-of-bounds value is rejected
  // (not applied), same as /save, but publishSettingsState() always runs
  // below so Home Assistant's entity snaps back to the real current value
  // rather than sticking on a rejected one.
  bool isSettingsTopic = true;
  bool accepted = false;
  if (t == TOPIC_WAIT_TIMER_MIN_CMD) {
    int minutes = msg.toInt();
    accepted = minutes >= CFG_MIN_WAIT_TIMER_MIN;
    if (accepted) cfg.waitTimerSec = (uint32_t)minutes * 60UL;
  } else if (t == TOPIC_CAT_WARNING_MIN_CMD) {
    int minutes = msg.toInt();
    accepted = minutes >= CFG_MIN_CAT_WARNING_MIN;
    if (accepted) cfg.catPresentWarningSec = (uint32_t)minutes * 60UL;
  } else if (t == TOPIC_DRAWER_THRESHOLD_CMD) {
    int cycles = msg.toInt();
    accepted = cycles >= CFG_MIN_DRAWER_FULL_CYCLES;
    if (accepted) cfg.drawerFullCycles = (uint32_t)cycles;
  } else if (t == TOPIC_HOME_OVERSHOOT_MS_CMD) {
    int ms = msg.toInt();
    accepted = ms >= CFG_MIN_HOME_OVERSHOOT_MS && ms <= CFG_MAX_HOME_OVERSHOOT_MS;
    if (accepted) cfg.homeOvershootMs = (uint32_t)ms;
  } else if (t == TOPIC_SHAKE_STEP_MS_CMD) {
    int ms = msg.toInt();
    accepted = ms >= CFG_MIN_SHAKE_STEP_MS && ms <= CFG_MAX_SHAKE_STEP_MS;
    if (accepted) cfg.dumpShakeStepMs = (uint32_t)ms;
  } else if (t == TOPIC_SHAKE_COUNT_CMD) {
    int count = msg.toInt();
    accepted = count >= CFG_MIN_SHAKE_COUNT && count <= CFG_MAX_SHAKE_COUNT;
    if (accepted) cfg.dumpShakeCount = (uint32_t)count;
  } else if (t == TOPIC_REQUIRE_MANUAL_RESET_CMD) {
    cfg.requireManualReset = (msg == "ON" || msg == "true" || msg == "1");
    accepted = true;
  } else {
    isSettingsTopic = false;
  }

  if (isSettingsTopic) {
    if (accepted) saveConfig(cfg);
    publishSettingsState();
  }
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument doc;
    buildStateJson(doc);
    String payload;
    serializeJson(doc, payload);
    client->text(payload);
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      JsonDocument doc;
      if (!deserializeJson(doc, data, len) && doc["cmd"].is<const char *>()) {
        handleCommand(String(doc["cmd"].as<const char *>()));
      }
    }
  }
}

// Attempt count + timing added 2026-07-06 while chasing the recurring
// ASSOC_LEAVE WiFi issue - the ESP32 core's own "_eventCallback(): Reason:
// 8 - ASSOC_LEAVE" log line says a disconnect happened, but not which
// attempt it was or how long the board had already been trying, both of
// which matter for correlating failures against reset reason, RSSI once
// connected, or how long BOOT_HOMING's motor run overlapped with them.
//
// Scan-then-pin-to-BSSID added the same day: plain WiFi.begin(ssid, pass)
// associates with whatever AP the radio happens to see/cache for that
// SSID, not necessarily the strongest one - on a network with more than
// one AP sharing the same SSID (a mesh system, or multiple standalone
// APs), that can mean repeatedly trying a weak/distant AP. Scanning first
// and connecting to the specific BSSID with the best RSSI fixes that.
// Uses the same async scanNetworks(true)/scanComplete() polling pattern
// already used for the setup portal's WiFi scan (see handleScan() in
// config_api.cpp) specifically so this never blocks loop() - a multi-
// second blocking scan would delay the safety interlock checks that run
// every iteration, which isn't acceptable here even briefly.
enum class WifiConnectPhase { IDLE, SCANNING };
static WifiConnectPhase wifiConnectPhase = WifiConnectPhase::IDLE;

// This macro forces the linker to execute this function during the initial boot loader phase
void __attribute__((constructor)) pre_init_disable_brownout() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
}

static void connectWifiIfNeeded() {
  static unsigned long lastAttempt = 0;
  static unsigned long attemptCount = 0;
  static bool wasConnected = false;

  bool isConnected = WiFi.status() == WL_CONNECTED;
  if (isConnected && !wasConnected) {
    Serial.printf("WiFi: connected after %lu attempt(s), t=%lums since boot - RSSI %d dBm, channel %d\n",
                  attemptCount, millis(), WiFi.RSSI(), WiFi.channel());
    attemptCount = 0;
    wifiConnectPhase = WifiConnectPhase::IDLE;
  }
  wasConnected = isConnected;
  if (isConnected) return;

  if (wifiConnectPhase == WifiConnectPhase::SCANNING) {
    int n = WiFi.scanComplete();
    if (n == -1) return; // still scanning - check again next loop() iteration, don't block

    int bestIdx = -1;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == cfg.wifiSsid && (bestIdx < 0 || WiFi.RSSI(i) > WiFi.RSSI(bestIdx))) {
        bestIdx = i;
      }
    }

    attemptCount++;
    if (bestIdx >= 0) {
      Serial.printf("WiFi: found %d AP(s) for \"%s\" - strongest is %d dBm on channel %d (%s) - "
                    "attempt #%lu (t=%lums since boot)\n",
                    n, cfg.wifiSsid.c_str(), WiFi.RSSI(bestIdx), WiFi.channel(bestIdx),
                    WiFi.BSSIDstr(bestIdx).c_str(), attemptCount, millis());
      WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str(), WiFi.channel(bestIdx), WiFi.BSSID(bestIdx));
    } else {
      Serial.printf("WiFi: scan found no AP matching \"%s\" - trying a plain connect anyway "
                    "(attempt #%lu, t=%lums since boot)\n",
                    cfg.wifiSsid.c_str(), attemptCount, millis());
      WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
    }
    WiFi.scanDelete();
    wifiConnectPhase = WifiConnectPhase::IDLE;
    lastAttempt = millis();
    return;
  }

  if (millis() - lastAttempt < WIFI_RECONNECT_INTERVAL_MS) return;
  lastAttempt = millis();
  WiFi.mode(WIFI_STA);

  // Cap the transmission power (Options: 19.5, 17, 15, 13, 11, 8.5, 7, 5, 2)
  // 15dBm or 13dBm drastically cuts current spikes while keeping decent range
  WiFi.setTxPower(WIFI_POWER_13dBm); 
  
  WiFi.scanNetworks(true /* async */);
  wifiConnectPhase = WifiConnectPhase::SCANNING;
}

static void connectMqttIfNeeded() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.mqttHost.length() == 0) return; // MQTT is optional - HA integration only
  if (mqtt.connected()) return;
  if (millis() - lastAttempt < MQTT_RECONNECT_INTERVAL_MS) return;
  lastAttempt = millis();

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setCallback(onMqttMessage);

  bool ok;
  if (cfg.mqttUser.length() > 0) {
    ok = mqtt.connect("lr2redux", cfg.mqttUser.c_str(), cfg.mqttPass.c_str());
  } else {
    ok = mqtt.connect("lr2redux");
  }

  if (ok) {
    mqtt.subscribe(TOPIC_CMD);
    mqtt.subscribe(TOPIC_WAIT_TIMER_MIN_CMD);
    mqtt.subscribe(TOPIC_CAT_WARNING_MIN_CMD);
    mqtt.subscribe(TOPIC_DRAWER_THRESHOLD_CMD);
    mqtt.subscribe(TOPIC_HOME_OVERSHOOT_MS_CMD);
    mqtt.subscribe(TOPIC_SHAKE_STEP_MS_CMD);
    mqtt.subscribe(TOPIC_SHAKE_COUNT_CMD);
    mqtt.subscribe(TOPIC_REQUIRE_MANUAL_RESET_CMD);
    publishHomeAssistantDiscovery();
    publishState();
    publishDrawerState();
    publishSettingsState();
    publishExtraSensors();
  }
}

static bool otaMdnsArmed = false;

// mDNS is what makes ws://lr2redux.local/ws and the OTA upload target
// resolve on the LAN. Re-armed on every fresh connect, not just the first
// one ever (that was a real bug, caught 2026-07-06): mDNS/OTA used to be
// gated behind a one-time latch that never ran again after the first
// successful connect, but the underlying network interface gets torn down
// and rebuilt on every WiFi disconnect/reconnect - which we know happens
// periodically here (see the ASSOC_LEAVE investigation elsewhere in this
// file) - and the mDNS responder doesn't survive that on its own. Result:
// lr2redux.local worked right after boot, then silently stopped resolving
// after the first reconnect, while the IP-based dashboard kept working
// fine (plain TCP/HTTP over the new connection, unaffected by mDNS
// state) - exactly the "sporadic" failure reported. Detecting the
// disconnected->connected transition (like connectWifiIfNeeded() already
// does) and tearing down + re-arming both MDNS and ArduinoOTA each time
// fixes it.
static void setupOtaIfNeeded() {
  static bool wasConnected = false;
  bool isConnected = WiFi.status() == WL_CONNECTED;
  bool justConnected = isConnected && !wasConnected;
  wasConnected = isConnected;
  if (!justConnected) return;

  if (otaMdnsArmed) {
    MDNS.end();
    ArduinoOTA.end();
  }
  otaMdnsArmed = true;

  // ESP32 WiFi modem-sleep (power-save) periodically powers the radio down,
  // which makes it miss incoming mDNS multicast queries - a direct IP
  // connection still works fine (it doesn't depend on catching a one-off
  // multicast packet), but lr2redux.local resolution becomes unreliable or
  // silently fails outright. Disabling sleep is the standard fix.
  WiFi.setSleep(false);
  MDNS.begin("lr2redux");
  MDNS.addService("http", "tcp", 80);
  // mDNS (lr2redux.local) doesn't resolve on every network - some routers/
  // guest or enterprise WiFi block multicast, and Windows needs Bonjour
  // installed. Printing the IP directly is a robust fallback that always
  // works: check the serial monitor once, or your router's DHCP client list.
  Serial.printf("WiFi connected: %s - reachable at http://lr2redux.local/ or http://%s/\n",
                WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str());
  ArduinoOTA.setHostname("lr2redux");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { motorStop(); });
  ArduinoOTA.begin();
}

// ---------------------------------------------------------------------------
// Web-based OTA: upload a .bin straight from the dashboard, instead of
// requiring `pio run -t upload --upload-port`. Reuses OTA_PASSWORD (from
// secrets.h) rather than adding a second credential - sent as a request
// header since a multipart file upload can't easily carry it in the body
// alongside the file. Assumes one upload session at a time, a safe
// assumption for a single-operator local tool.
// ---------------------------------------------------------------------------
static bool otaUploadAuthorized = false;

static void handleOtaUploadData(AsyncWebServerRequest *request, String filename, size_t index,
                                 uint8_t *data, size_t len, bool final) {
  if (index == 0) {
    String suppliedPassword =
        request->hasHeader("X-OTA-Password") ? request->header("X-OTA-Password") : "";
    otaUploadAuthorized = suppliedPassword == OTA_PASSWORD;
    if (!otaUploadAuthorized) {
      Serial.println("[ota-web] rejected: wrong or missing X-OTA-Password header");
      return;
    }
    Serial.printf("[ota-web] update starting: %s\n", filename.c_str());
    motorStop(); // don't flash mid-motion, same precaution as ArduinoOTA.onStart()
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  }
  if (!otaUploadAuthorized) return;

  if (Update.write(data, len) != len) {
    Update.printError(Serial);
  }
  if (final) {
    if (Update.end(true)) {
      Serial.println("[ota-web] update complete - rebooting");
    } else {
      Update.printError(Serial);
    }
  }
}

static void handleOtaUploadRequest(AsyncWebServerRequest *request) {
  if (!otaUploadAuthorized) {
    request->send(401, "text/plain", "Wrong or missing OTA password");
    return;
  }
  bool ok = !Update.hasError();
  request->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Update failed");
  if (ok) {
    delay(500); // let the response flush before the reboot drops the connection
    ESP.restart();
  }
}

// Usage analytics need real wall-clock time to bucket visits by hour - NTP
// over the WiFi link already established, UTC (no DST math attempted).
// configTime() is fire-and-forget; time() keeps returning ~0 until the
// first NTP response actually lands, which is how ntpTimeSynced is detected
// below rather than assuming any fixed delay.
static void updateNtpSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpConfigured) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    ntpConfigured = true;
    return;
  }
  if (ntpTimeSynced) return;
  time_t now = time(nullptr);
  if (now > 100000) { // clearly a real epoch time, not the ~0 default before sync
    ntpTimeSynced = true;
    Serial.printf("NTP time synced: %s", ctime(&now));
  }
}

// Records one occupancy session in the rolling 30-day hourly visit log. A
// no-op until NTP has actually synced - bucketing into time()'s ~0 default
// before sync would corrupt the log by dumping everything into index 0.
static void recordVisit() {
  if (!ntpTimeSynced) return;
  time_t now = time(nullptr);
  long hoursSinceEpoch = (long)(now / 3600);
  int idx = (int)(((hoursSinceEpoch % VISIT_LOG_HOURS) + VISIT_LOG_HOURS) % VISIT_LOG_HOURS);
  if (visitLog[idx] < 255) visitLog[idx]++;
  prefs.putBytes("visitLog", visitLog, sizeof(visitLog));

  visitTimes[visitTimesHead] = (uint32_t)now;
  visitTimesHead = (visitTimesHead + 1) % VISIT_TIMES_CAPACITY;
  if (visitTimesCount < VISIT_TIMES_CAPACITY) visitTimesCount++;
  prefs.putBytes("visitTimes", visitTimes, sizeof(visitTimes));
  prefs.putUShort("vtHead", visitTimesHead);
  prefs.putUShort("vtCount", visitTimesCount);
}

static void handleAnalytics(AsyncWebServerRequest *request) {
  time_t now = time(nullptr);
  long nowHour = (long)(now / 3600);
  long baseHour = nowHour - (VISIT_LOG_HOURS - 1); // oldest hour in the 30-day window

  int hourly24[24] = {0};
  int daily30[30] = {0};
  int totalToday = 0, totalWeek = 0, total30Days = 0;

  for (int i = 0; i < VISIT_LOG_HOURS; i++) {
    long absHour = baseHour + i;
    int idx = (int)(((absHour % VISIT_LOG_HOURS) + VISIT_LOG_HOURS) % VISIT_LOG_HOURS);
    uint8_t count = visitLog[idx];
    int dayOffset = (VISIT_LOG_HOURS - 1 - i) / 24; // 0 = today ... 29 = 29 days ago
    if (dayOffset < 30) daily30[29 - dayOffset] += count; // oldest -> newest
    total30Days += count;
    if (dayOffset == 0) totalToday += count;
    if (dayOffset < 7) totalWeek += count;
    if (i >= VISIT_LOG_HOURS - 24) hourly24[i - (VISIT_LOG_HOURS - 24)] = count; // oldest -> newest
  }

  JsonDocument doc;
  JsonArray h24 = doc["hourly24"].to<JsonArray>();
  for (int i = 0; i < 24; i++) h24.add(hourly24[i]);
  JsonArray d30 = doc["daily30"].to<JsonArray>();
  for (int i = 0; i < 30; i++) d30.add(daily30[i]);
  doc["totalToday"] = totalToday;
  doc["totalWeek"] = totalWeek;
  doc["total30Days"] = total30Days;
  doc["timeSynced"] = ntpTimeSynced;

  // Raw visit timestamps, oldest -> newest, for the Analytics page's visit
  // list and day/night interval math. When the circular buffer hasn't
  // wrapped yet (count < capacity), entries are simply 0..count-1 in
  // insertion order; once full, the oldest entry is at visitTimesHead.
  JsonArray times = doc["visitTimes"].to<JsonArray>();
  int startIdx = (visitTimesCount < VISIT_TIMES_CAPACITY) ? 0 : visitTimesHead;
  for (int i = 0; i < visitTimesCount; i++) {
    int idx = (startIdx + i) % VISIT_TIMES_CAPACITY;
    times.add(visitTimes[idx]);
  }
  doc["dayStartHour"] = cfg.dayStartHour;
  doc["dayEndHour"] = cfg.dayEndHour;

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
static void enterState(State next) {
  state = next;
  stateEnteredAt = millis();
  publishState();
  broadcastState();
}

// Any state where the motor is currently moving, or about to start/resume
// moving imminently (the dump pause/shake are time-bounded pit stops in the
// middle of a cycle, not a truly "at rest" state) - used to gate the
// weight switch's half of the safety interlock below (the anti-pinch half
// applies in every state, not just these, since a pinch is real regardless
// of whether the motor is moving) and to pick the right resume direction
// when leaving SAFETY_STOP.
static bool isMotionState(State s) {
  switch (s) {
    case State::BOOT_HOMING:
    case State::CYCLE_TO_DUMP:
    case State::CYCLE_DUMP_PAUSE:
    case State::CYCLE_DUMP_SHAKE:
    case State::CYCLE_TO_HOME:
    case State::CYCLE_HOME_OVERSHOOT:
    case State::CYCLE_HOME_SETTLE:
      return true;
    default:
      return false;
  }
}

static void runStateMachine() {
  unsigned long elapsed = millis() - stateEnteredAt;

  // Global safety interlock. The weight switch only means "stop a moving
  // cycle" - during IDLE/CAT_PRESENT/WAIT_TIMER its state means something
  // else entirely (cat presence, the normal detection path), not a safety
  // abort, so it stays gated behind isMotionState() as before. Anti-pinch
  // is different: a pinch condition is real and dangerous regardless of
  // whether the motor happens to be moving right now, so unlike the weight
  // switch it triggers SAFETY_STOP from any state (excluding SAFETY_STOP/
  // FAULT themselves, to avoid re-triggering into itself every loop while
  // the pinch stays active). These two switches used to be a single
  // ambiguous signal (the stock harness wires them in series) - now
  // physically split so each is read independently, see CLAUDE.md
  // "Pins 6/7 topology".
  bool weightMotionInterlock = isMotionState(state) && weightSwitch.isActive();
  bool antiPinchInterlock =
      antiPinch.isActive() && state != State::SAFETY_STOP && state != State::FAULT;
  if (weightMotionInterlock || antiPinchInterlock) {
    motorStop();
    stateBeforeSafetyStop = state;
    weightClearedAt = 0; // fresh SAFETY_STOP entry - cooldown clock not started yet
    safetyStopAntiPinchInvolved = antiPinch.isActive();
    manualResumeRequested = false;
    enterState(State::SAFETY_STOP);
    return;
  }

  switch (state) {
    case State::BOOT_HOMING:
      if (hallHome.isActive()) {
        motorStop();
        enterState(State::IDLE);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS * 2) {
        motorStop();
        enterState(State::FAULT);
      } else if (!bootHomingFoundDump && hallDump.isActive()) {
        // found Dump first - now reverse the rest of the way to Home,
        // exactly like a normal cycle's Dump->Home leg
        bootHomingFoundDump = true;
        motorStop();
        motorRunReverse();
      } else if (!bootHomingFoundDump) {
        motorRunForward();
      }
      // else: already found Dump and reversing - nothing to do here each
      // loop, motorRunReverse() above is still driving the motor
      break;

    case State::IDLE:
      if (weightSwitch.isActive()) {
        recordVisit(); // a fresh occupancy session, not a WAIT_TIMER->CAT_PRESENT re-engagement
        enterState(State::CAT_PRESENT);
      } else if (manualCycleRequested || manualButton.isActive()) {
        manualCycleRequested = false;
        motorRunForward();
        enterState(State::CYCLE_TO_DUMP);
      }
      break;

    case State::CAT_PRESENT:
      if (!weightSwitch.isActive()) {
        enterState(State::WAIT_TIMER);
      }
      break;

    case State::WAIT_TIMER:
      if (weightSwitch.isActive()) {
        // cat came back before the timer elapsed - cancel and wait it out again
        enterState(State::CAT_PRESENT);
      } else if (elapsed >= (unsigned long)cfg.waitTimerSec * 1000UL) {
        motorRunForward();
        enterState(State::CYCLE_TO_DUMP);
      }
      break;

    case State::CYCLE_TO_DUMP:
      if (hallDump.isActive()) {
        // Stop and dwell at Dump before reversing - see CYCLE_DUMP_PAUSE/
        // CYCLE_DUMP_SHAKE below, not a direct reverse-and-go.
        motorStop();
        enterState(State::CYCLE_DUMP_PAUSE);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS) {
        motorStop();
        enterState(State::FAULT);
      }
      break;

    case State::CYCLE_DUMP_PAUSE:
      // Motor stays stopped the whole time - dwelling so waste finishes
      // falling through before the shake/reverse. Placeholder duration
      // (DUMP_PAUSE_MS), not yet tuned against real hardware.
      if (elapsed > DUMP_PAUSE_MS) {
        enterState(State::CYCLE_DUMP_SHAKE);
      }
      break;

    case State::CYCLE_DUMP_SHAKE: {
      // Runtime-configurable (cfg.dumpShakeStepMs/dumpShakeCount) - see
      // config.h. dumpShakeCount == 0 means totalMs == 0, so this falls
      // through to CYCLE_TO_HOME on the very next iteration, skipping the
      // shake entirely.
      unsigned long stepMs = cfg.dumpShakeStepMs > 0 ? cfg.dumpShakeStepMs : 1;
      unsigned long totalMs = stepMs * cfg.dumpShakeCount * 2UL;
      if (elapsed > totalMs) {
        motorStop();
        motorRunReverse();
        enterState(State::CYCLE_TO_HOME);
      } else {
        // Oscillate direction every stepMs to dislodge stuck waste - driven
        // purely by elapsed time, no separate step counter needed, and
        // self-corrects correctly on a SAFETY_STOP resume too (elapsed
        // restarts at 0, so the shake just starts over cleanly).
        bool forwardPhase = (elapsed / stepMs) % 2 == 0;
        if (forwardPhase) motorRunForward();
        else motorRunReverse();
      }
      break;
    }

    case State::CYCLE_TO_HOME:
      if (hallHome.isActive()) {
        // Don't stop here - keep reversing past Home briefly (overshoot),
        // then come back forward to settle. Helps litter that piled up to
        // one side during the dump level back out. Motor is already
        // running reverse from CYCLE_DUMP_SHAKE's exit, so no motor
        // command needed on this transition itself.
        enterState(State::CYCLE_HOME_OVERSHOOT);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS) {
        motorStop();
        enterState(State::FAULT);
      }
      break;

    case State::CYCLE_HOME_OVERSHOOT:
      // Still reversing (unchanged from CYCLE_TO_HOME) for a bit past
      // Home. Runtime-configurable (cfg.homeOvershootMs) - see config.h.
      if (elapsed > cfg.homeOvershootMs) {
        motorStop();
        motorRunForward();
        enterState(State::CYCLE_HOME_SETTLE);
      }
      break;

    case State::CYCLE_HOME_SETTLE:
      if (hallHome.isActive()) {
        motorStop();
        cycleCount++;
        drawerCycles++;
        prefs.putULong("cycles", cycleCount);
        prefs.putULong("drawerCycles", drawerCycles);
        publishDrawerState();
        broadcastState();
        enterState(State::IDLE);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS) {
        motorStop();
        enterState(State::FAULT);
      }
      break;

    case State::SAFETY_STOP: {
      // Upgrade to "anti-pinch involved" if it becomes active any time
      // during this episode, even if weight was the original trigger -
      // better to require positive confirmation than risk auto-resuming a
      // pinch condition that only registered after the fact.
      if (antiPinch.isActive()) safetyStopAntiPinchInvolved = true;

      // "Press any button to reset" - the physical manual button works as a
      // resume trigger here too, same as the MQTT/WS "resume" command.
      if (manualButton.isActive()) manualResumeRequested = true;

      // Anti-pinch involvement always requires manual reset (the one hard
      // default, not configurable) - it's the original stock board's exact
      // behavior for this specific case. requireManualReset extends that
      // to weight-only interruptions too, matching the original fully.
      bool needsManualReset = safetyStopAntiPinchInvolved || cfg.requireManualReset;

      if (weightSwitch.isActive()) {
        weightClearedAt = 0;
      } else if (weightClearedAt == 0) {
        weightClearedAt = millis();
      }
      bool weightCooldownDone =
          weightClearedAt != 0 && (millis() - weightClearedAt >= WEIGHT_SAFETY_COOLDOWN_MS);

      bool interlocksClear = !weightSwitch.isActive() && !antiPinch.isActive();
      bool readyToResume = interlocksClear &&
                            (needsManualReset ? manualResumeRequested : weightCooldownDone);

      if (readyToResume) {
        manualResumeRequested = false;
        // Resume in whichever direction matches what was interrupted.
        // Deliberately NOT resetting bootHomingFoundDump here - this is a
        // resume, not a fresh start, see enterState()/the FAULT case for
        // where it's actually reset. CYCLE_DUMP_PAUSE/CYCLE_DUMP_SHAKE need
        // no motor command here - the pause has no motion to resume, and
        // the shake re-derives its own direction from elapsed==0 on re-entry.
        switch (stateBeforeSafetyStop) {
          case State::CYCLE_TO_DUMP:
          case State::CYCLE_HOME_SETTLE:
            motorRunForward();
            break;
          case State::CYCLE_TO_HOME:
          case State::CYCLE_HOME_OVERSHOOT:
            motorRunReverse();
            break;
          case State::BOOT_HOMING:
            if (bootHomingFoundDump) motorRunReverse();
            else motorRunForward();
            break;
          default:
            break;
        }
        enterState(stateBeforeSafetyStop);
      }
      break;
    }

    case State::FAULT:
      motorStop();
      if (faultResetRequested) {
        faultResetRequested = false;
        bootHomingFoundDump = false; // fresh start, not a SAFETY_STOP resume
        enterState(State::BOOT_HOMING);
      }
      break;
  }

  if (drawerEmptiedRequested) {
    drawerEmptiedRequested = false;
    drawerCycles = 0;
    prefs.putULong("drawerCycles", 0);
    publishDrawerState();
    broadcastState();
  }
}

// ---------------------------------------------------------------------------
// Three discrete status LEDs, replicating the original stock board's
// language exactly (per the user, from the original manual):
//   Solid green   - standby, waiting for the cat (IDLE)
//   Slow-blink green - cat left, waiting out the timer before a cycle
//                      starts (WAIT_TIMER) - deliberately still green (not
//                      cycling yet) but blinking so it reads differently
//                      from plain standby; not part of the original stock
//                      board's documented language, added per user request
//                      since WAIT_TIMER and IDLE were otherwise visually
//                      identical.
//   Solid yellow  - actively cycling (any CYCLE_*/BOOT_HOMING phase)
//   Solid red     - a cycle was interrupted (weight-triggered SAFETY_STOP),
//                   or the cat sensor is active (CAT_PRESENT)
//   Flashing red    - cat sensor active beyond cfg.catPresentWarningSec, or FAULT
//   Flashing yellow - anti-pinch-triggered SAFETY_STOP specifically
//                     ("safety pinch detect activated")
// ---------------------------------------------------------------------------
static void updateLeds() {
  bool red = false, yellow = false, green = false;
  unsigned long elapsed = millis() - stateEnteredAt;
  bool blinkFast = (millis() / 150) % 2;
  bool blinkSlow = (millis() / 600) % 2;

  switch (state) {
    case State::IDLE:
      green = true;
      break;

    case State::WAIT_TIMER:
      green = blinkSlow;
      break;

    case State::BOOT_HOMING:
    case State::CYCLE_TO_DUMP:
    case State::CYCLE_DUMP_PAUSE:
    case State::CYCLE_DUMP_SHAKE:
    case State::CYCLE_TO_HOME:
    case State::CYCLE_HOME_OVERSHOOT:
    case State::CYCLE_HOME_SETTLE:
      yellow = true;
      break;

    case State::CAT_PRESENT:
      if (elapsed > (unsigned long)cfg.catPresentWarningSec * 1000UL) {
        red = blinkFast; // flashing red: cat's been there a while
      } else {
        red = true; // solid red: cat sensor active, normal
      }
      break;

    case State::SAFETY_STOP:
      if (safetyStopAntiPinchInvolved) {
        yellow = blinkFast; // flashing yellow: anti-pinch specifically
      } else {
        red = true; // solid red: weight-triggered interruption
      }
      break;

    case State::FAULT:
      red = blinkFast;
      break;
  }

  digitalWrite(PIN_LED_RED, red ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, yellow ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// Holding the manual cycle button 10s forces a reboot into setup mode,
// regardless of current state - useful if WiFi creds are wrong and the unit
// never gets far enough to be reachable any other way.
// ---------------------------------------------------------------------------
static void checkSetupButtonHold() {
  static unsigned long heldSince = 0;
  static bool triggered = false;

  if (!manualButton.isActive()) {
    heldSince = 0;
    triggered = false;
    return;
  }
  if (heldSince == 0) heldSince = millis();
  if (triggered || millis() - heldSince < SETUP_HOLD_MS) return;

  triggered = true;
  // confirmation blink (yellow), then reboot - delay() here is fine, we're
  // not servicing anything else in the ~1s before the restart anyway
  for (int i = 0; i < 6; i++) {
    digitalWrite(PIN_LED_YELLOW, HIGH);
    delay(80);
    digitalWrite(PIN_LED_YELLOW, LOW);
    delay(80);
  }
  requestSetupModeAndRestart();
}

// Human-readable esp_reset_reason_t, for the boot-cause diagnostic print
// in setup() below - the enum's raw int alone isn't self-explanatory in a
// serial log capture months from now.
static const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT_PIN";
    case ESP_RST_SW: return "SW_RESTART";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP_WAKE";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // PubSubClient defaults to a 256-byte MQTT packet buffer - too small for
  // the Home Assistant discovery payloads added alongside the Settings
  // number/switch entities (device block + min/max/step/unit fields push
  // several of them past 256 bytes), which would otherwise fail silently.
  mqtt.setBufferSize(1024);

  // safe regardless of mode: motor pins driven low before anything else runs.
  // Both IN1/IN2 are PWM-capable (DRV8871: PWM whichever pin is the active
  // direction, hold the other at 0) rather than one direction pin + a
  // separate ENA speed pin.
  ledcSetup(PWM_CHANNEL_IN1, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcSetup(PWM_CHANNEL_IN2, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PIN_MOTOR_IN1, PWM_CHANNEL_IN1);
  ledcAttachPin(PIN_MOTOR_IN2, PWM_CHANNEL_IN2);
  motorStop();

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);

  hallHome.begin(PIN_HALL_HOME);
  hallDump.begin(PIN_HALL_DUMP);
  weightSwitch.begin(PIN_WEIGHT_SWITCH, true);
  antiPinch.begin(PIN_ANTI_PINCH, true, true); // activeHigh: normally-closed, opens (reads HIGH) on trip
  manualButton.begin(PIN_MANUAL_BUTTON, true);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  loadConfig(cfg);
  bool forceSetup = consumeForceSetupFlag();
  inSetupMode = forceSetup || !isWifiConfigured(cfg);

  if (inSetupMode) {
    startSetupPortal(cfg);
    return; // no state machine, no MQTT, no dashboard WS this boot
  }

  prefs.begin("lr2redux", false);
  cycleCount = prefs.getULong("cycles", 0);
  drawerCycles = prefs.getULong("drawerCycles", 0);
  memset(visitLog, 0, sizeof(visitLog)); // clean slate if the NVS key doesn't exist yet
  prefs.getBytes("visitLog", visitLog, sizeof(visitLog));
  memset(visitTimes, 0, sizeof(visitTimes));
  prefs.getBytes("visitTimes", visitTimes, sizeof(visitTimes));
  visitTimesHead = prefs.getUShort("vtHead", 0);
  visitTimesCount = prefs.getUShort("vtCount", 0);

  // On a "dirty" reset (brownout, panic, or any watchdog reset) the lwIP
  // static memory pools can be left in an indeterminate state from the
  // previous boot, which manifests as WiFi repeatedly failing to associate
  // (ASSOC_LEAVE) even though the radio/antenna/power are all fine -
  // observed for real on 2026-07-06 after an RTCWDT_RTC_RESET. WIFI_OFF
  // forces esp_wifi_deinit(), tearing down and rebuilding the station
  // netif and lwIP allocators from scratch before we try to connect. On a
  // clean power-on reset the BSS is already zeroed by the ROM bootloader,
  // so this teardown is skipped there to avoid needlessly delaying boot.
  esp_reset_reason_t resetReason = esp_reset_reason();
  bool dirtyReset = resetReason == ESP_RST_BROWNOUT ||
                     resetReason == ESP_RST_PANIC ||
                     resetReason == ESP_RST_INT_WDT ||
                     resetReason == ESP_RST_TASK_WDT ||
                     resetReason == ESP_RST_WDT ||
                     resetReason == ESP_RST_EXT;
  // Printed unconditionally (not just on dirty resets) so every serial
  // capture has boot cause available for free - useful diagnostic context
  // on its own, e.g. for correlating WiFi connection problems against
  // which specific reset preceded them.
  Serial.printf("Boot reason: %s (%d)%s\n", resetReasonName(resetReason), (int)resetReason,
                dirtyReset ? " [dirty]" : "");
  if (dirtyReset) {
    Serial.println("Dirty reset - tearing down WiFi/lwIP stack before reconnecting");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200); // let the lwIP tcpip_thread finish the teardown
  }

  // WiFi mode must be set before the AsyncWebServer/AsyncTCP stack starts
  // (server.begin() below) - starting the server before any network
  // interface exists crashes with "assert failed: tcpip_api_call ...
  // Invalid mbox" on boot. connectWifiIfNeeded() in loop() still owns the
  // actual WiFi.begin() call and reconnect logic; this just brings the
  // interface up first so AsyncTCP has something to attach to.
  WiFi.mode(WIFI_STA);

  // Cap the transmission power (Options: 19.5, 17, 15, 13, 11, 8.5, 7, 5, 2)
  // 15dBm or 13dBm drastically cuts current spikes while keeping decent range
  WiFi.setTxPower(WIFI_POWER_13dBm); 

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  registerConfigApiRoutes(server, cfg);
  server.on("/analytics", HTTP_GET, handleAnalytics);
  server.on("/update", HTTP_POST, handleOtaUploadRequest, handleOtaUploadData);

  // serves the built web dashboard (web/dist, copied to ./data - see
  // web/package.json's build:device script) so the board is reachable at
  // http://lr2redux.local/ with no separate hosting. Registered after the
  // API/WS routes above: AsyncStaticWebHandler only claims a request if a
  // matching file actually exists in LittleFS, so it can't shadow them, but
  // this ordering is the belt-and-suspenders version of that guarantee.
  if (LittleFS.begin(true)) {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  } else {
    Serial.println("LittleFS mount failed - dashboard won't be served; run `pio run -t uploadfs`");
  }

  server.begin();

  stateEnteredAt = millis();
}

void loop() {
  if (inSetupMode) {
    setupPortalLoop();
    digitalWrite(PIN_LED_RED, (millis() / 250) % 2 ? HIGH : LOW); // fast blink red = waiting for setup
    return;
  }

  hallHome.update();
  hallDump.update();
  weightSwitch.update();
  antiPinch.update();
  manualButton.update();
  checkSetupButtonHold();

  connectWifiIfNeeded();
  setupOtaIfNeeded();
  updateNtpSync();
  ArduinoOTA.handle();
  connectMqttIfNeeded();
  mqtt.loop();
  ws.cleanupClients();

  State prevState = state;
  runStateMachine();
  if (state != prevState) {
    publishState();
    publishExtraSensors(); // needsManualReset/catPresentWarning can flip on a state change too
    broadcastState();
  }

  static unsigned long lastHeartbeat = 0;
  if (mqtt.connected() && millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
    mqtt.publish(TOPIC_HEARTBEAT, buf);
    // Periodic refresh so uptime/RSSI/catPresentWarning stay current even
    // when nothing has changed state (e.g. mid WAIT_TIMER or a long
    // CAT_PRESENT triggering the warning without any state transition).
    publishExtraSensors();
  }

  // Serial-only diagnostic heartbeat, independent of MQTT - added
  // 2026-07-06 alongside the WiFi attempt logging above, so a serial
  // capture has ongoing visibility into free heap (a leak/fragmentation
  // issue could itself cause exactly the kind of intermittent instability
  // being chased) and signal strength over time, not just at the moment
  // of connecting.
  static unsigned long lastSerialHeartbeat = 0;
  if (millis() - lastSerialHeartbeat > HEARTBEAT_INTERVAL_MS) {
    lastSerialHeartbeat = millis();
    bool wifiUp = WiFi.status() == WL_CONNECTED;
    Serial.printf("Heartbeat: uptime=%lus, freeHeap=%u, state=%s, wifi=%s, rssi=%s\n", millis() / 1000,
                  ESP.getFreeHeap(), stateName(state), wifiUp ? "up" : "down",
                  wifiUp ? (String(WiFi.RSSI()) + "dBm").c_str() : "n/a");
  }

  // periodic broadcast so uptime/telemetry stays fresh in the dashboard even
  // when nothing has changed state (e.g. mid WAIT_TIMER)
  static unsigned long lastWsBroadcast = 0;
  if (millis() - lastWsBroadcast > 5000) {
    lastWsBroadcast = millis();
    broadcastState();
  }

  updateLeds();
}
