// Runtime device configuration (WiFi, MQTT, wait-timer), stored in NVS.
// Replaces the old compile-time WIFI_SSID/MQTT_HOST-in-secrets.h approach -
// these are now set through the setup portal (see setup_portal.h) instead of
// being flashed in, so the same firmware image works on any unit.
#pragma once

#include <Arduino.h>

struct DeviceConfig {
  String wifiSsid;
  String wifiPass;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  uint32_t waitTimerSec = 420; // 7 min default, matches the original hardcoded behavior
  // Default: only anti-pinch-triggered SAFETY_STOP requires manual reset;
  // weight-triggered ones auto-resume after WEIGHT_SAFETY_COOLDOWN_MS. When
  // true, matches the original stock board's behavior exactly - ANY
  // interruption (weight or anti-pinch) requires pressing a button / an
  // explicit "resume" command, nothing auto-resumes.
  bool requireManualReset = false;
  // How long the weight switch can stay continuously active (cat present)
  // before it's flagged as a warning (flashing red on the original board) -
  // default matches the original's documented 2-minute threshold. Enforced
  // minimum of 2 min in config_api.cpp; can be set higher, not lower.
  uint32_t catPresentWarningSec = 120;
  // Hour-of-day (0-23) boundary used only by the dashboard's Analytics page
  // to split "average time between visits" into day vs. night buckets. The
  // firmware never interprets these itself - classification happens
  // client-side against each visit's local browser time, so DST/timezone is
  // never a firmware concern here. Default 6am-8pm.
  uint8_t dayStartHour = 6;
  uint8_t dayEndHour = 20;
  // Stock LR2 warns the drawer is full after a set number of cycles since
  // it was last emptied - varies by household (litter type, number of
  // cats), so it's user-tunable rather than a fixed guess. Default matches
  // the original hardcoded value; enforced minimum of 1 in config_api.cpp.
  uint32_t drawerFullCycles = 10;
  // How long (ms) the motor keeps reversing past Home at the end of a
  // cycle before coming back forward to settle - see CYCLE_HOME_OVERSHOOT
  // in main.cpp. There's no rotary encoder, so "how far past Home" is only
  // controllable as time at the motor's fixed PWM speed, not a true angle -
  // more time = more rotation. Tune this per litter type/depth to level the
  // litter without dragging the globe too far. Default matches the
  // original hardcoded placeholder (3s). Enforced minimum in config_api.cpp.
  uint32_t homeOvershootMs = 3000;
  // Dump-position shake, to dislodge stuck clumps before reversing home:
  // the motor oscillates forward/reverse, each swing lasting
  // dumpShakeStepMs, for dumpShakeCount total back-and-forth swings (total
  // shake duration = dumpShakeStepMs * dumpShakeCount * 2). Same "time
  // instead of angle" caveat as homeOvershootMs applies to
  // dumpShakeStepMs - a longer swing rotates further at the same motor
  // speed. dumpShakeCount = 0 skips the shake phase entirely. Defaults
  // match the original hardcoded placeholders (400ms step, 3 shakes).
  // Enforced minimums in config_api.cpp.
  uint32_t dumpShakeStepMs = 400;
  uint32_t dumpShakeCount = 3;
};

// Validation bounds for the fields above - shared between config_api.cpp
// (HTTP /save, used by the web Settings page) and main.cpp (MQTT settings
// topics, used by Home Assistant) so the two control surfaces can't
// silently disagree on what's a valid value.
static const int CFG_MIN_WAIT_TIMER_MIN = 2;
static const int CFG_MIN_CAT_WARNING_MIN = 2;
static const int CFG_MIN_DRAWER_FULL_CYCLES = 1;
static const int CFG_MIN_HOME_OVERSHOOT_MS = 0;
static const int CFG_MAX_HOME_OVERSHOOT_MS = 15000;
static const int CFG_MIN_SHAKE_STEP_MS = 50;
static const int CFG_MAX_SHAKE_STEP_MS = 5000;
static const int CFG_MIN_SHAKE_COUNT = 0;
static const int CFG_MAX_SHAKE_COUNT = 20;

void loadConfig(DeviceConfig &cfg);
void saveConfig(const DeviceConfig &cfg);
bool isWifiConfigured(const DeviceConfig &cfg);

// The 10s-button-hold path and the "no WiFi configured" boot path both need
// to force a boot into setup mode. Since switching WiFi/server modes cleanly
// at runtime is fiddly, we just flag it in NVS and reboot - setup() reads
// the flag back via consumeForceSetupFlag().
bool consumeForceSetupFlag();
void requestSetupModeAndRestart();
