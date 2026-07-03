// HTTP config routes (WiFi scan, get/save DeviceConfig) - registered on
// whichever AsyncWebServer is live for the current boot (the setup portal's
// in AP mode, or the normal-mode dashboard server in STA mode). Saving
// always reboots: applying new WiFi/MQTT settings cleanly at runtime isn't
// worth the complexity next to just restarting into them.
#pragma once

#include <ESPAsyncWebServer.h>
#include "config.h"

void registerConfigApiRoutes(AsyncWebServer &server, DeviceConfig &cfg);
