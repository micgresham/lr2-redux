// LR2-Redux: standalone hardware bring-up / diagnostic firmware.
//
// Separate PlatformIO environment from the production firmware (main.cpp) -
// see `[env:diagnostic]` in platformio.ini. Purpose: verify wiring and
// finalize which physical pin is which *before* trusting the real state
// machine with it. No WiFi/MQTT config, no LittleFS, no OTA - just a SoftAP
// and a single self-contained page, so it's flashable and usable with zero
// setup on a bare breadboard.
//
// Connect to WiFi "LR2-Redux-Diag" (password below), browse to
// http://192.168.4.1/. Shows live sensor pin states (raw + debounced) and
// lets you jog the motor forward/reverse at an adjustable speed. The same
// controls are also available over the serial console (115200 baud) - type
// `help` and press enter - useful when WiFi/browser access is inconvenient
// or you're already watching the serial log for sensor changes.
//
// Flash with: pio run -e diagnostic -t upload

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

// ---------------------------------------------------------------------------
// Pin map - must match the real board's pinout (README.md / main.cpp)
// ---------------------------------------------------------------------------
static const uint8_t PIN_MOTOR_IN1 = 26;
static const uint8_t PIN_MOTOR_IN2 = 27;
static const uint8_t PIN_MOTOR_ENA = 25;
static const uint8_t PIN_HALL_HOME = 34; // input-only, needs external pull-up to 3.3V
static const uint8_t PIN_HALL_DUMP = 35; // input-only, needs external pull-up to 3.3V
static const uint8_t PIN_WEIGHT_SWITCH = 32;
static const uint8_t PIN_MANUAL_BUTTON = 33;
static const uint8_t PIN_STATUS_LED = 4;

// Now confirmed (2026-07-05, after physically splitting the stock weight/
// anti-pinch switch loop) to be the anti-pinch switch's own independent
// signal - matches PIN_ANTI_PINCH in main.cpp. Normally-closed: reads LOW
// (active-low convention, "on") at rest and HIGH ("off") when actually
// tripped - the opposite polarity of every other input here, hence
// activeHigh=true below. See CLAUDE.md, "Pins 6/7 topology".
static const uint8_t PIN_AUX_TEST = 14;

static const uint8_t PWM_CHANNEL = 0;
static const uint16_t PWM_FREQ_HZ = 5000;
static const uint8_t PWM_RESOLUTION_BITS = 8;

static const int SENSOR_ACTIVE = LOW; // active-low: switches/hall ICs pull the line down when triggered
static const unsigned long DEBOUNCE_MS = 25;

// Motor safety: every jog command (browser or serial) carries its own
// auto-stop deadline rather than relying on a fixed global timeout - the
// browser refreshes it every ~200ms while a button is held (so release =
// stop almost immediately), while a one-shot serial `fwd`/`rev` command
// gets a longer fixed pulse since nobody's going to retype it 5x/sec.
static const unsigned long MOTOR_WATCHDOG_MS = 500;   // browser keepalive grace
static const unsigned long SERIAL_JOG_MS = 3000;      // serial one-shot pulse length

static const char *AP_SSID = "LR2-Redux-Diag";
static const char *AP_PASSWORD = "lr2diag123"; // 8+ chars required for WPA2

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ---------------------------------------------------------------------------
struct DebouncedInput {
  uint8_t pin;
  const char *label;
  int rawReading;
  int stableState;
  int loggedState;
  unsigned long lastChangeAt;
  bool activeHigh; // true for normally-closed switches that open (read HIGH)
                    // on trip - e.g. the anti-pinch switch on auxTest, the
                    // opposite polarity of every other active-low input here.

  void begin(uint8_t p, const char *l, bool useInternalPullup, bool activeHighIn = false) {
    pin = p;
    label = l;
    activeHigh = activeHighIn;
    pinMode(pin, useInternalPullup ? INPUT_PULLUP : INPUT);
    rawReading = digitalRead(pin);
    stableState = rawReading;
    loggedState = rawReading;
    lastChangeAt = millis();
    Serial.printf("[init] %-13s GPIO%-2d pullup=%s raw=%d\r\n", label, pin,
                  useInternalPullup ? "yes" : "no (external needed)", rawReading);
  }

  void update() {
    int reading = digitalRead(pin);
    if (reading != rawReading) {
      lastChangeAt = millis();
      rawReading = reading;
    }
    if (millis() - lastChangeAt > DEBOUNCE_MS) {
      stableState = rawReading;
    }
  }

  // Prints only on a debounced state change, not every loop - call once per
  // loop() for each input so bring-up testing has a live serial trace
  // (useful when the web page isn't reachable yet, or as a second source of
  // truth alongside it).
  void logEdge() {
    if (stableState != loggedState) {
      loggedState = stableState;
      Serial.printf("[change] %-13s -> %s (raw=%d)\r\n", label,
                    isActive() ? "ACTIVE" : "idle", rawReading);
    }
  }

  bool isActive() const { return activeHigh ? (stableState == HIGH) : (stableState == SENSOR_ACTIVE); }
};

static DebouncedInput hallHome, hallDump, weightSwitch, manualButton, auxTest;

// ---------------------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------------------
enum class MotorDir { STOPPED, FORWARD, REVERSE };
static MotorDir motorDir = MotorDir::STOPPED;
static uint8_t motorPwm = 0;
static unsigned long motorDeadlineAt = 0;

static const char *motorDirName(MotorDir d) {
  return d == MotorDir::FORWARD ? "FORWARD" : d == MotorDir::REVERSE ? "REVERSE" : "STOPPED";
}

static void motorStop() {
  if (motorDir != MotorDir::STOPPED) {
    Serial.printf("[motor] STOPPED\r\n");
  }
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
  ledcWrite(PWM_CHANNEL, 0);
  motorDir = MotorDir::STOPPED;
}

// timeoutMs: how long this command is good for before auto-stopping if not
// renewed - callers set this per-source (see MOTOR_WATCHDOG_MS/SERIAL_JOG_MS).
static void motorApply(MotorDir dir, uint8_t pwm, unsigned long timeoutMs) {
  if (dir != MotorDir::FORWARD && dir != MotorDir::REVERSE) {
    motorStop();
    return;
  }
  if (dir != motorDir || pwm != motorPwm) {
    Serial.printf("[motor] %s @ pwm=%d\r\n", motorDirName(dir), pwm);
  }
  if (dir == MotorDir::FORWARD) {
    digitalWrite(PIN_MOTOR_IN1, HIGH);
    digitalWrite(PIN_MOTOR_IN2, LOW);
  } else {
    digitalWrite(PIN_MOTOR_IN1, LOW);
    digitalWrite(PIN_MOTOR_IN2, HIGH);
  }
  ledcWrite(PWM_CHANNEL, pwm);
  motorDir = dir;
  motorPwm = pwm;
  motorDeadlineAt = millis() + timeoutMs;
}

// ---------------------------------------------------------------------------
// State broadcast (hand-rolled JSON - keeps this tool dependency-light)
// ---------------------------------------------------------------------------
static void appendSensor(char *buf, size_t bufSize, size_t &pos, const DebouncedInput &in, bool trailingComma) {
  pos += snprintf(buf + pos, bufSize - pos,
                   "\"%s\":{\"raw\":%d,\"active\":%s,\"msSinceChange\":%lu}%s",
                   in.label, in.rawReading, in.isActive() ? "true" : "false",
                   millis() - in.lastChangeAt, trailingComma ? "," : "");
}

static void broadcastState() {
  if (ws.count() == 0) return;
  char buf[768];
  size_t pos = 0;
  pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"sensors\":{");
  appendSensor(buf, sizeof(buf), pos, hallHome, true);
  appendSensor(buf, sizeof(buf), pos, hallDump, true);
  appendSensor(buf, sizeof(buf), pos, weightSwitch, true);
  appendSensor(buf, sizeof(buf), pos, manualButton, true);
  appendSensor(buf, sizeof(buf), pos, auxTest, false);
  pos += snprintf(buf + pos, sizeof(buf) - pos,
                   "},\"motor\":{\"dir\":\"%s\",\"pwm\":%d},"
                   "\"uptimeMs\":%lu,\"apClients\":%d}",
                   motorDir == MotorDir::FORWARD ? "fwd" : motorDir == MotorDir::REVERSE ? "rev" : "stop",
                   motorPwm, millis(), WiFi.softAPgetStationNum());
  ws.textAll(buf);
}

static void onWsMessage(uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  const char *cmd = doc["cmd"] | "";

  if (strcmp(cmd, "motor") == 0) {
    const char *dir = doc["dir"] | "stop";
    int pwm = doc["pwm"] | 0;
    pwm = constrain(pwm, 0, 255);
    if (strcmp(dir, "fwd") == 0) motorApply(MotorDir::FORWARD, (uint8_t)pwm, MOTOR_WATCHDOG_MS);
    else if (strcmp(dir, "rev") == 0) motorApply(MotorDir::REVERSE, (uint8_t)pwm, MOTOR_WATCHDOG_MS);
    else motorStop();
  } else if (strcmp(cmd, "stop") == 0) {
    motorStop();
  }
}

// ---------------------------------------------------------------------------
// Serial console - same controls as the web page, for when WiFi/browser
// access is inconvenient or you're already watching the serial log.
// ---------------------------------------------------------------------------
static uint8_t serialLastPwm = 180;

static void printSerialHelp() {
  Serial.println(
    "[serial] commands: fwd [pwm] | rev [pwm] | stop | status | help\r\n"
    "  fwd 200   - jog forward at pwm 200 (0-255, default last used/180),\r\n"
    "              auto-stops after 3s unless you send another fwd/rev\r\n"
    "  rev       - jog reverse at last-used pwm\r\n"
    "  stop      - stop immediately\r\n"
    "  status    - print current sensor/motor snapshot right now");
}

static void printSerialStatus() {
  Serial.printf("[status] home=%d dump=%d weight=%d button=%d aux=%d motor=%s pwm=%d up=%lus clients=%d\r\n",
                hallHome.isActive(), hallDump.isActive(), weightSwitch.isActive(),
                manualButton.isActive(), auxTest.isActive(), motorDirName(motorDir),
                motorPwm, millis() / 1000, WiFi.softAPgetStationNum());
}

static void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = sp == -1 ? line : line.substring(0, sp);
  String argStr = sp == -1 ? "" : line.substring(sp + 1);
  cmd.toLowerCase();
  argStr.trim();

  if (cmd == "fwd" || cmd == "rev") {
    uint8_t pwm = argStr.length() ? (uint8_t)constrain(argStr.toInt(), 0, 255) : serialLastPwm;
    serialLastPwm = pwm;
    motorApply(cmd == "fwd" ? MotorDir::FORWARD : MotorDir::REVERSE, pwm, SERIAL_JOG_MS);
  } else if (cmd == "stop") {
    motorStop();
  } else if (cmd == "status") {
    printSerialStatus();
  } else if (cmd == "help" || cmd == "?") {
    printSerialHelp();
  } else {
    Serial.printf("[serial] unknown command '%s' - type 'help'\r\n", cmd.c_str());
  }
}

static void pollSerialConsole() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length() > 0) {
        handleSerialLine(buf);
        buf = "";
      }
    } else {
      buf += c;
      if (buf.length() > 48) buf = ""; // guard against a stuck/garbage line
    }
  }
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[ws] client #%u connected from %s\r\n", client->id(),
                  client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      onWsMessage(data, len);
    }
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[ws] client #%u disconnected\r\n", client->id());
    // Don't leave the motor running if the only connected browser drops -
    // the watchdog would also catch this, but no reason to wait for it.
    motorStop();
  }
}

// ---------------------------------------------------------------------------
// Embedded page - no LittleFS/build step needed for a bring-up tool
// ---------------------------------------------------------------------------
static const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LR2-Redux Diagnostic</title>
<style>
  :root { color-scheme: dark; }
  body { background:#121815; color:#e7eee9; font-family:ui-monospace,Consolas,monospace; max-width:520px; margin:0 auto; padding:20px 16px 60px; }
  h1 { font-size:18px; margin:0 0 4px; }
  .sub { color:#8fa398; font-size:12px; margin-bottom:20px; }
  h2 { font-size:13px; text-transform:uppercase; letter-spacing:.08em; color:#8fa398; margin:24px 0 8px; }
  .row { display:flex; align-items:center; gap:10px; padding:8px 10px; background:#182019; border:1px solid #263029; border-radius:8px; margin-bottom:6px; }
  .dot { width:12px; height:12px; border-radius:50%; background:#3a463d; flex:none; }
  .dot.on { background:#3fa487; box-shadow:0 0 8px #3fa48788; }
  .name { flex:1; font-size:13px; }
  .meta { font-size:11px; color:#8fa398; text-align:right; }
  .aux .name::after { content:" (temp/optional)"; color:#d9a441; font-size:10px; }
  .btns { display:flex; gap:10px; }
  button { flex:1; font-family:inherit; font-size:14px; padding:16px 10px; border-radius:8px; border:1px solid #263029; background:#1c2921; color:#e7eee9; }
  button:active { background:#24352a; }
  #stop { background:#4a1f1c; border-color:#7a352f; color:#f2b3ab; font-weight:bold; }
  #stop:active { background:#63281f; }
  .slider-row { display:flex; align-items:center; gap:10px; margin:14px 0; }
  input[type=range] { flex:1; }
  #pwmVal { width:38px; text-align:right; font-size:13px; }
  .status { font-size:11px; color:#8fa398; margin-top:24px; border-top:1px solid #263029; padding-top:10px; }
</style></head>
<body>
  <h1>LR2-Redux — Hardware Diagnostic</h1>
  <div class="sub">Bring-up tool. Not the production dashboard.</div>

  <h2>Sensors</h2>
  <div class="row"><span class="dot" id="d-hallHome"></span><span class="name">Hall — Home (GPIO34)</span><span class="meta" id="m-hallHome">—</span></div>
  <div class="row"><span class="dot" id="d-hallDump"></span><span class="name">Hall — Dump (GPIO35)</span><span class="meta" id="m-hallDump">—</span></div>
  <div class="row"><span class="dot" id="d-weightSwitch"></span><span class="name">Weight switch (GPIO32)</span><span class="meta" id="m-weightSwitch">—</span></div>
  <div class="row"><span class="dot" id="d-manualButton"></span><span class="name">Manual button (GPIO33)</span><span class="meta" id="m-manualButton">—</span></div>
  <div class="row aux"><span class="dot" id="d-auxTest"></span><span class="name">Aux test input (GPIO14)</span><span class="meta" id="m-auxTest">—</span></div>

  <h2>Motor test</h2>
  <div class="slider-row">
    <span>PWM</span>
    <input type="range" id="pwm" min="0" max="255" value="180">
    <span id="pwmVal">180</span>
  </div>
  <div class="btns">
    <button id="fwd">▲ FORWARD (hold)</button>
    <button id="rev">▼ REVERSE (hold)</button>
  </div>
  <div class="btns" style="margin-top:10px;">
    <button id="stop">■ STOP</button>
  </div>

  <div class="status" id="status">connecting…</div>

<script>
  const dot = id => document.getElementById('d-'+id);
  const meta = id => document.getElementById('m-'+id);
  const pwmSlider = document.getElementById('pwm');
  const pwmVal = document.getElementById('pwmVal');
  const statusEl = document.getElementById('status');

  pwmSlider.oninput = () => pwmVal.textContent = pwmSlider.value;

  let ws;
  let keepAlive = null;

  function connect() {
    ws = new WebSocket('ws://' + location.host + '/ws');
    ws.onopen = () => statusEl.textContent = 'connected';
    ws.onclose = () => { statusEl.textContent = 'disconnected — retrying…'; setTimeout(connect, 1000); };
    ws.onerror = () => ws.close();
    ws.onmessage = (ev) => {
      const s = JSON.parse(ev.data);
      for (const key of ['hallHome','hallDump','weightSwitch','manualButton','auxTest']) {
        const sensor = s.sensors[key];
        dot(key).classList.toggle('on', sensor.active);
        meta(key).textContent = 'raw ' + sensor.raw + ' · ' + sensor.msSinceChange + 'ms';
      }
      statusEl.textContent = 'connected · motor ' + s.motor.dir + ' @ ' + s.motor.pwm +
        ' · up ' + Math.floor(s.uptimeMs/1000) + 's · ' + s.apClients + ' client(s)';
    };
  }
  connect();

  function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

  function startJog(dir) {
    send({cmd:'motor', dir, pwm: parseInt(pwmSlider.value, 10)});
    clearInterval(keepAlive);
    keepAlive = setInterval(() => send({cmd:'motor', dir, pwm: parseInt(pwmSlider.value, 10)}), 200);
  }
  function stopJog() {
    clearInterval(keepAlive);
    send({cmd:'stop'});
  }

  for (const [id, dir] of [['fwd','fwd'], ['rev','rev']]) {
    const btn = document.getElementById(id);
    btn.addEventListener('mousedown', () => startJog(dir));
    btn.addEventListener('touchstart', (e) => { e.preventDefault(); startJog(dir); });
    btn.addEventListener('mouseup', stopJog);
    btn.addEventListener('mouseleave', stopJog);
    btn.addEventListener('touchend', stopJog);
    btn.addEventListener('touchcancel', stopJog);
  }
  document.getElementById('stop').addEventListener('click', stopJog);
  document.addEventListener('visibilitychange', () => { if (document.hidden) stopJog(); });
</script>
</body></html>
)HTMLPAGE";

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300); // let the USB-serial side catch up so the banner below isn't cut off
  Serial.println("\n=== LR2-Redux diagnostic firmware ===");

  // Motor pins driven safe before anything else.
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PIN_MOTOR_ENA, PWM_CHANNEL);
  motorStop();

  pinMode(PIN_STATUS_LED, OUTPUT);

  hallHome.begin(PIN_HALL_HOME, "hallHome", false);
  hallDump.begin(PIN_HALL_DUMP, "hallDump", false);
  weightSwitch.begin(PIN_WEIGHT_SWITCH, "weightSwitch", true);
  manualButton.begin(PIN_MANUAL_BUTTON, "manualButton", true);
  auxTest.begin(PIN_AUX_TEST, "auxTest", true, true); // activeHigh: anti-pinch is normally-closed, opens on trip

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[wifi] AP up: %s / %s, http://%s/\r\n", AP_SSID, AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", PAGE_HTML);
  });
  server.begin();
  Serial.println("[http] server started - waiting for sensor changes and motor commands below");
  printSerialHelp();
}

void loop() {
  pollSerialConsole();

  hallHome.update();
  hallDump.update();
  weightSwitch.update();
  manualButton.update();
  auxTest.update();

  hallHome.logEdge();
  hallDump.logEdge();
  weightSwitch.logEdge();
  manualButton.logEdge();
  auxTest.logEdge();

  // Safety interlocks: any live weight-switch or anti-pinch trip, or a stale
  // jog command, stops the motor immediately - mirrors the production
  // firmware's SAFETY_STOP behavior so bring-up testing can't leave it
  // running unattended.
  if (motorDir != MotorDir::STOPPED) {
    if (weightSwitch.isActive() || auxTest.isActive()) {
      Serial.println("[motor] weight switch or anti-pinch tripped - stopping");
      motorStop();
    } else if (millis() > motorDeadlineAt) {
      Serial.println("[motor] auto-stop deadline reached - stopping");
      motorStop();
    }
  }

  ws.cleanupClients();

  static unsigned long lastBroadcast = 0;
  if (millis() - lastBroadcast > 150) {
    lastBroadcast = millis();
    broadcastState();
  }

  // No periodic heartbeat print - serial output is edge-triggered only
  // ([init] at boot, [change] on sensor transitions, [motor]/[ws] on
  // events). Type `status` any time for an on-demand snapshot instead.

  digitalWrite(PIN_STATUS_LED, (millis() / 500) % 2 ? HIGH : LOW);
}
