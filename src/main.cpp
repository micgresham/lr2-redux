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
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "config.h"
#include "config_api.h"
#include "setup_portal.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.h.example to include/secrets.h and set OTA_PASSWORD"
#endif

// ---------------------------------------------------------------------------
// Pin map
// ---------------------------------------------------------------------------
static const uint8_t PIN_MOTOR_IN1 = 26;
static const uint8_t PIN_MOTOR_IN2 = 27;
static const uint8_t PIN_MOTOR_ENA = 25; // PWM speed
static const uint8_t PIN_HALL_HOME = 34; // input-only pin, needs external pull-up to 3.3V
static const uint8_t PIN_HALL_DUMP = 35; // input-only pin, needs external pull-up to 3.3V
static const uint8_t PIN_WEIGHT_SWITCH = 32; // stock mechanical cat-weight switch
static const uint8_t PIN_MANUAL_BUTTON = 33; // optional stock "cycle now" button
static const uint8_t PIN_STATUS_LED = 4;

static const uint8_t PWM_CHANNEL = 0;
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
static const unsigned long CYCLE_SEGMENT_TIMEOUT_MS = 15UL * 1000UL; // max time to reach next sensor before fault
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
static const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
static const unsigned long HEARTBEAT_INTERVAL_MS = 30UL * 1000UL;
static const unsigned long SETUP_HOLD_MS = 10UL * 1000UL; // hold the cycle button this long to force setup mode

// Stock LR2 warns the drawer is full after a set number of cycles since it
// was last emptied. Tune to taste.
static const unsigned long DRAWER_FULL_CYCLES = 10;

// ---------------------------------------------------------------------------
// MQTT topics
// ---------------------------------------------------------------------------
static const char *TOPIC_STATE = "lr2redux/state";
static const char *TOPIC_CAT_PRESENT = "lr2redux/cat_present";
static const char *TOPIC_CYCLE_COUNT = "lr2redux/cycle_count";
static const char *TOPIC_DRAWER_FULL = "lr2redux/drawer_full";
static const char *TOPIC_DRAWER_CYCLES = "lr2redux/drawer_cycles";
static const char *TOPIC_DRAWER_THRESHOLD = "lr2redux/drawer_threshold";
static const char *TOPIC_HEARTBEAT = "lr2redux/heartbeat";
static const char *TOPIC_CMD = "lr2redux/cmd";

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum class State {
  BOOT_HOMING,   // power-on: find Home before doing anything else
  IDLE,          // at Home, no cat, nothing to do
  CAT_PRESENT,   // weight switch engaged
  WAIT_TIMER,    // cat left, waiting out the clean-cycle timer
  CYCLE_TO_DUMP, // motor running, looking for the Dump sensor
  CYCLE_TO_HOME, // motor running, looking for the Home sensor
  SAFETY_STOP,   // weight switch triggered mid-cycle, motor halted, waiting for it to clear
  FAULT          // a segment timed out; needs an explicit reset command
};

static State state = State::BOOT_HOMING;
static State stateBeforeSafetyStop = State::CYCLE_TO_DUMP;
static unsigned long stateEnteredAt = 0;
static unsigned long cycleCount = 0;
static unsigned long drawerCycles = 0;

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

  // useInternalPullup: true for the weight switch / manual button (GPIO32/33
  // support it). Hall sensors on GPIO34/35 don't - they're input-only pins
  // with no pull resistor in hardware, hence the external 10k on the board.
  void begin(uint8_t p, bool useInternalPullup = false) {
    pin = p;
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

  bool isActive() const { return stableState == SENSOR_ACTIVE; }
};

static DebouncedInput hallHome;
static DebouncedInput hallDump;
static DebouncedInput weightSwitch;
static DebouncedInput manualButton;

// ---------------------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------------------
static void motorStop() {
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
  ledcWrite(PWM_CHANNEL, 0);
}

static void motorRunForward() {
  digitalWrite(PIN_MOTOR_IN1, HIGH);
  digitalWrite(PIN_MOTOR_IN2, LOW);
  ledcWrite(PWM_CHANNEL, MOTOR_SPEED);
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
    case State::CYCLE_TO_HOME: return "cycling";
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
  mqtt.publish(TOPIC_DRAWER_FULL, drawerCycles >= DRAWER_FULL_CYCLES ? "ON" : "OFF", true);
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", drawerCycles);
  mqtt.publish(TOPIC_DRAWER_CYCLES, buf, true);
}

// ---------------------------------------------------------------------------
// Web dashboard: WebSocket state broadcast (JSON), replaces the browser
// talking MQTT directly - only this firmware speaks MQTT now.
// ---------------------------------------------------------------------------
static void buildStateJson(JsonDocument &doc) {
  doc["state"] = stateName(state);
  doc["catPresent"] = weightSwitch.isActive();
  doc["cycleCount"] = cycleCount;
  doc["drawerFull"] = drawerCycles >= DRAWER_FULL_CYCLES;
  doc["drawerCycles"] = drawerCycles;
  doc["drawerThreshold"] = DRAWER_FULL_CYCLES;
  doc["uptimeSeconds"] = millis() / 1000;
}

static void broadcastState() {
  if (ws.count() == 0) return;
  JsonDocument doc;
  buildStateJson(doc);
  String payload;
  serializeJson(doc, payload);
  ws.textAll(payload);
}

// Minimal Home Assistant MQTT discovery so the unit shows up automatically.
static void publishHomeAssistantDiscovery() {
  const char *deviceBlock =
      "\"device\":{\"identifiers\":[\"lr2redux\"],\"name\":\"Litter Robot 2\",\"manufacturer\":\"lr2-redux\"}";

  char payload[512];

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
}

static void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (String(topic) == TOPIC_CMD) {
    handleCommand(msg);
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

static void connectWifiIfNeeded() {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastAttempt < WIFI_RECONNECT_INTERVAL_MS) return;
  lastAttempt = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
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
    publishHomeAssistantDiscovery();
    publishState();
    publishDrawerState();
    char buf[8];
    snprintf(buf, sizeof(buf), "%lu", DRAWER_FULL_CYCLES);
    mqtt.publish(TOPIC_DRAWER_THRESHOLD, buf, true);
  }
}

static bool otaInitialized = false;

// mDNS is what makes ws://lr2redux.local/ws and the OTA upload target
// resolve on the LAN - started once, here, the first time WiFi comes up.
static void setupOtaIfNeeded() {
  if (otaInitialized || WiFi.status() != WL_CONNECTED) return;
  otaInitialized = true;
  MDNS.begin("lr2redux");
  ArduinoOTA.setHostname("lr2redux");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { motorStop(); });
  ArduinoOTA.begin();
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

static void runStateMachine() {
  unsigned long elapsed = millis() - stateEnteredAt;

  // Global safety interlock: weight switch engaging mid-cycle always halts
  // the motor immediately, regardless of which cycling sub-state we're in.
  if ((state == State::CYCLE_TO_DUMP || state == State::CYCLE_TO_HOME) &&
      weightSwitch.isActive()) {
    motorStop();
    stateBeforeSafetyStop = state;
    enterState(State::SAFETY_STOP);
    return;
  }

  switch (state) {
    case State::BOOT_HOMING:
      motorRunForward();
      if (hallHome.isActive()) {
        motorStop();
        enterState(State::IDLE);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS * 2) {
        motorStop();
        enterState(State::FAULT);
      }
      break;

    case State::IDLE:
      if (weightSwitch.isActive()) {
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
        enterState(State::CYCLE_TO_HOME);
      } else if (elapsed > CYCLE_SEGMENT_TIMEOUT_MS) {
        motorStop();
        enterState(State::FAULT);
      }
      break;

    case State::CYCLE_TO_HOME:
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

    case State::SAFETY_STOP:
      if (!weightSwitch.isActive()) {
        // resume rotating in the same direction from wherever it stopped
        motorRunForward();
        enterState(stateBeforeSafetyStop);
      }
      break;

    case State::FAULT:
      motorStop();
      if (faultResetRequested) {
        faultResetRequested = false;
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
// Status LED: solid in idle-ish states, blinking while cycling, fast blink on fault
// ---------------------------------------------------------------------------
static void updateStatusLed() {
  bool on;
  switch (state) {
    case State::FAULT:
      on = (millis() / 150) % 2;
      break;
    case State::CYCLE_TO_DUMP:
    case State::CYCLE_TO_HOME:
    case State::BOOT_HOMING:
      on = (millis() / 400) % 2;
      break;
    default:
      on = true;
      break;
  }
  digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW);
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
  // confirmation blink, then reboot - delay() here is fine, we're not
  // servicing anything else in the ~1s before the restart anyway
  for (int i = 0; i < 6; i++) {
    digitalWrite(PIN_STATUS_LED, HIGH);
    delay(80);
    digitalWrite(PIN_STATUS_LED, LOW);
    delay(80);
  }
  requestSetupModeAndRestart();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // safe regardless of mode: motor pins driven low before anything else runs
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PIN_MOTOR_ENA, PWM_CHANNEL);
  motorStop();

  pinMode(PIN_STATUS_LED, OUTPUT);

  hallHome.begin(PIN_HALL_HOME);
  hallDump.begin(PIN_HALL_DUMP);
  weightSwitch.begin(PIN_WEIGHT_SWITCH, true);
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

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  registerConfigApiRoutes(server, cfg);

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
    digitalWrite(PIN_STATUS_LED, (millis() / 250) % 2 ? HIGH : LOW); // fast blink = waiting for setup
    return;
  }

  hallHome.update();
  hallDump.update();
  weightSwitch.update();
  manualButton.update();
  checkSetupButtonHold();

  connectWifiIfNeeded();
  setupOtaIfNeeded();
  ArduinoOTA.handle();
  connectMqttIfNeeded();
  mqtt.loop();
  ws.cleanupClients();

  State prevState = state;
  runStateMachine();
  if (state != prevState) {
    publishState();
    broadcastState();
  }

  static unsigned long lastHeartbeat = 0;
  if (mqtt.connected() && millis() - lastHeartbeat > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
    mqtt.publish(TOPIC_HEARTBEAT, buf);
  }

  // periodic broadcast so uptime/telemetry stays fresh in the dashboard even
  // when nothing has changed state (e.g. mid WAIT_TIMER)
  static unsigned long lastWsBroadcast = 0;
  if (millis() - lastWsBroadcast > 5000) {
    lastWsBroadcast = millis();
    broadcastState();
  }

  updateStatusLed();
}
