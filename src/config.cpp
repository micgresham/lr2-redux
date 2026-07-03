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
  prefs.end();
}

void saveConfig(const DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString("wifiSsid", cfg.wifiSsid);
  prefs.putString("wifiPass", cfg.wifiPass);
  prefs.putString("mqttHost", cfg.mqttHost);
  prefs.putUShort("mqttPort", cfg.mqttPort);
  prefs.putString("mqttUser", cfg.mqttUser);
  prefs.putString("mqttPass", cfg.mqttPass);
  prefs.putUInt("waitTimerSec", cfg.waitTimerSec);
  prefs.end();
}

bool isWifiConfigured(const DeviceConfig &cfg) {
  return cfg.wifiSsid.length() > 0;
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
