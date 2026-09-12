#include "config.h"
#include <Preferences.h>

static const char *NVS_NAMESPACE = "lr2net";

void loadConfig(DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  cfg.wifiSsid = prefs.getString("wifiSsid", "");
  cfg.wifiPass = prefs.getString("wifiPass", "");
  cfg.mqttHost = prefs.getString("mqttHost", "");
  cfg.mqttPort = prefs.getUShort("mqttPort", 1883);
  cfg.mqttUser = prefs.getString("mqttUser", "");
  cfg.mqttPass = prefs.getString("mqttPass", "");
  cfg.waitTimerSec = prefs.getUInt("waitTimerSec", 420);
  cfg.requireManualReset = prefs.getBool("manualReset", false);
  cfg.catPresentWarningSec = prefs.getUInt("catWarningSec", 120);
  cfg.dayStartHour = prefs.getUChar("dayStartHour", 6);
  cfg.dayEndHour = prefs.getUChar("dayEndHour", 20);
  cfg.drawerFullCycles = prefs.getUInt("drawerFullCyc", 10);
  cfg.homeOvershootMs = prefs.getUInt("overshootMs", 3000);
  cfg.dumpShakeStepMs = prefs.getUInt("shakeStepMs", 400);
  cfg.dumpShakeCount = prefs.getUInt("shakeCount", 3);
  prefs.end();
}

void saveConfig(const DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  // Drop the "these credentials have worked" marker only when the WiFi
  // credentials themselves actually change. A save that touches only MQTT or
  // the advanced settings must not re-arm main.cpp's setup-mode fallback -
  // otherwise an unrelated settings change made while the router happened to
  // be rebooting would send a perfectly good board into AP mode.
  if (prefs.getString("wifiSsid", "") != cfg.wifiSsid ||
      prefs.getString("wifiPass", "") != cfg.wifiPass) {
    prefs.putBool("wifiOk", false);
  }
  prefs.putString("wifiSsid", cfg.wifiSsid);
  prefs.putString("wifiPass", cfg.wifiPass);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPass", cfg.mqttPass);
  prefs.putUInt("waitTimerSec", cfg.waitTimerSec);
  prefs.putBool("manualReset", cfg.requireManualReset);
  prefs.putUInt("catWarningSec", cfg.catPresentWarningSec);
  prefs.putUChar("dayStartHour", cfg.dayStartHour);
  prefs.putUChar("dayEndHour", cfg.dayEndHour);
  prefs.putUInt("drawerFullCyc", cfg.drawerFullCycles);
  prefs.putUInt("overshootMs", cfg.homeOvershootMs);
  prefs.putUInt("shakeStepMs", cfg.dumpShakeStepMs);
  prefs.putUInt("shakeCount", cfg.dumpShakeCount);
  prefs.end();
}

bool isWifiConfigured(const DeviceConfig &cfg) {
  return cfg.wifiSsid.length() > 0;
}

bool isWifiValidated() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  bool ok = prefs.getBool("wifiOk", false);
  prefs.end();
  return ok;
}

void markWifiValidated() {
  if (isWifiValidated()) return; // already set - don't rewrite NVS on every reconnect
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool("wifiOk", true);
  prefs.end();
}

bool consumeForceSetupFlag() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  bool flag = prefs.getBool("forceSetup", false);
  if (flag) prefs.putBool("forceSetup", false);
  prefs.end();
  return flag;
}

void requestSetupModeAndRestart() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBool("forceSetup", true);
  prefs.end();
  delay(200);
  ESP.restart();
}
