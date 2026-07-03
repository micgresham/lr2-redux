// SoftAP + captive-portal config page, used when the unit has no saved
// WiFi credentials yet, or when the cycle button is held 10s to request
// re-configuration. Entered by rebooting into this mode (see config.h);
// startSetupPortal() is a one-shot call from setup(), setupPortalLoop()
// runs from loop() for the lifetime of that boot.
#pragma once

#include "config.h"

void startSetupPortal(DeviceConfig &cfg);
void setupPortalLoop();
