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
};

void loadConfig(DeviceConfig &cfg);
void saveConfig(const DeviceConfig &cfg);
bool isWifiConfigured(const DeviceConfig &cfg);

// The 10s-button-hold path and the "no WiFi configured" boot path both need
// to force a boot into setup mode. Since switching WiFi/server modes cleanly
// at runtime is fiddly, we just flag it in NVS and reboot - setup() reads
// the flag back via consumeForceSetupFlag().
bool consumeForceSetupFlag();
void requestSetupModeAndRestart();
