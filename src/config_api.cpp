#include "config_api.h"
#include <WiFi.h>
#include <ArduinoJson.h>

// WiFi.scanComplete() return values (named constants aren't reliably in
// scope across core versions, so spell out what they mean here):
//   -2 = no scan has been started (or previous results already consumed)
//   -1 = scan in progress
//  >=0 = number of networks found
static const int SCAN_NOT_STARTED = -2;
static const int SCAN_RUNNING = -1;

static void handleScan(AsyncWebServerRequest *request) {
  int n = WiFi.scanComplete();

  if (n == SCAN_NOT_STARTED) {
    WiFi.scanNetworks(true /* async */);
    request->send(202, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  if (n == SCAN_RUNNING) {
    request->send(202, "application/json", "{\"status\":\"scanning\"}");
    return;
  }

  JsonDocument doc;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject net = networks.add<JsonObject>();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void registerConfigApiRoutes(AsyncWebServer &server, DeviceConfig &cfg) {
  server.on("/scan", HTTP_GET, handleScan);

  server.on("/config", HTTP_GET, [&cfg](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["wifiSsid"] = cfg.wifiSsid;
    doc["mqttHost"] = cfg.mqttHost;
    doc["mqttPort"] = cfg.mqttPort;
    doc["mqttUser"] = cfg.mqttUser;
    doc["waitTimerMin"] = cfg.waitTimerSec / 60;
    doc["requireManualReset"] = cfg.requireManualReset;
    doc["catPresentWarningMin"] = cfg.catPresentWarningSec / 60;
    doc["dayStartHour"] = cfg.dayStartHour;
    doc["dayEndHour"] = cfg.dayEndHour;
    doc["drawerFullCycles"] = cfg.drawerFullCycles;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  AsyncCallbackJsonWebHandler *saveHandler = new AsyncCallbackJsonWebHandler(
      "/save", [&cfg](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject obj = json.as<JsonObject>();

        if (obj["wifiSsid"].is<const char *>()) cfg.wifiSsid = obj["wifiSsid"].as<const char *>();
        if (obj["mqttHost"].is<const char *>()) cfg.mqttHost = obj["mqttHost"].as<const char *>();
        if (obj["mqttUser"].is<const char *>()) cfg.mqttUser = obj["mqttUser"].as<const char *>();
        if (obj["mqttPort"].is<int>()) cfg.mqttPort = obj["mqttPort"].as<int>();
        if (obj["waitTimerMin"].is<int>()) {
          int minutes = obj["waitTimerMin"].as<int>();
          // Enforced minimum: give the cat enough time to actually leave the
          // globe before it cycles, not just clear the weight switch briefly
          // mid-shuffle. The web UI also enforces this, but the firmware is
          // the authoritative check since /save can be hit directly.
          if (minutes >= 2) cfg.waitTimerSec = (uint32_t)minutes * 60UL;
        }
        if (obj["requireManualReset"].is<bool>()) {
          cfg.requireManualReset = obj["requireManualReset"].as<bool>();
        }
        if (obj["catPresentWarningMin"].is<int>()) {
          int minutes = obj["catPresentWarningMin"].as<int>();
          // Enforced minimum matches the original board's documented
          // threshold - can be set higher (a more patient/tolerant
          // threshold), not lower.
          if (minutes >= 2) cfg.catPresentWarningSec = (uint32_t)minutes * 60UL;
        }
        if (obj["dayStartHour"].is<int>()) {
          int h = obj["dayStartHour"].as<int>();
          if (h >= 0 && h <= 23) cfg.dayStartHour = (uint8_t)h;
        }
        if (obj["dayEndHour"].is<int>()) {
          int h = obj["dayEndHour"].as<int>();
          if (h >= 0 && h <= 23) cfg.dayEndHour = (uint8_t)h;
        }
        if (obj["drawerFullCycles"].is<int>()) {
          int cycles = obj["drawerFullCycles"].as<int>();
          // Enforced minimum of 1 - a threshold of 0 would mean "always
          // full," which isn't a meaningful setting.
          if (cycles >= 1) cfg.drawerFullCycles = (uint32_t)cycles;
        }
        // blank password fields mean "leave unchanged", not "clear it"
        if (obj["wifiPass"].is<const char *>() && strlen(obj["wifiPass"]) > 0) {
          cfg.wifiPass = obj["wifiPass"].as<const char *>();
        }
        if (obj["mqttPass"].is<const char *>() && strlen(obj["mqttPass"]) > 0) {
          cfg.mqttPass = obj["mqttPass"].as<const char *>();
        }

        saveConfig(cfg);
        request->send(200, "application/json", "{\"status\":\"saved, rebooting\"}");

        // let the response flush before the AP/STA link drops from under it
        delay(500);
        ESP.restart();
      });
  server.addHandler(saveHandler);
}
