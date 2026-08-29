// LR2-Redux board pin maps.
//
// Extracted out of main.cpp on 2026-08-23 when the ESP32-C6 SuperMini was
// added as a second target. Every dated pin note in CLAUDE.md (the GPIO13->14
// anti-pinch move, the three-discrete-LED redesign, the DRV8871 switch) refers
// to the CLASSIC ESP32 map below - that one is the shipped/installed hardware
// and is unchanged here. The C6 map is a port target, wired but not yet built
// or verified on real hardware.
//
// Selection is by IDF target, so the right map falls out of `board` in
// platformio.ini with nothing to remember at build time.

#pragma once

#include <Arduino.h>

#if defined(CONFIG_IDF_TARGET_ESP32C6)

// ---------------------------------------------------------------------------
// ESP32-C6 SuperMini
// ---------------------------------------------------------------------------
// Reserved on this board, deliberately left out of the map below:
//   GPIO4, 5, 8, 9, 15 - strapping pins (GPIO8/9 select boot mode; GPIO8 also
//                        drives the onboard WS2812, GPIO15 the status LED)
//   GPIO12, 13         - USB D-/D+; this board has NO USB-UART bridge, so the
//                        serial console rides on these. Repurposing them costs
//                        both flashing and every Serial.printf this project
//                        leans on for diagnostics.
//   GPIO16, 17         - UART0 TX/RX (ROM bootloader still logs here)
//   GPIO24-30          - internal SPI flash, not bonded out
// That leaves exactly ten usable pins for ten signals: 0,1,2,3,6,7,14,18,19,20.
//
// Unlike the classic ESP32's GPIO34/35, NONE of these are input-only - every
// pin here supports INPUT_PULLUP. The external 10k hall pull-ups to 3.3V are
// therefore optional on this board, though still worth populating: the
// A1101EUA-T is open-drain and the internal pull is a weak ~45k over a long
// harness run. The 3.3V-not-5V rule is unchanged; the C6 is not 5V tolerant.
static const uint8_t PIN_MOTOR_IN1 = 6;
static const uint8_t PIN_MOTOR_IN2 = 7;
static const uint8_t PIN_HALL_HOME = 2;  // external 10k pull-up to 3.3V optional here
static const uint8_t PIN_HALL_DUMP = 3;  // external 10k pull-up to 3.3V optional here
static const uint8_t PIN_WEIGHT_SWITCH = 14; // stock mechanical cat-weight switch
static const uint8_t PIN_ANTI_PINCH = 20;    // stock anti-pinch switch, physically split from
                                             // the weight switch's shared loop - see CLAUDE.md
                                             // "Pins 6/7 topology". Active-HIGH, unlike the rest.
static const uint8_t PIN_MANUAL_BUTTON = 19; // optional stock "cycle now" button
static const uint8_t PIN_LED_GREEN = 0;
static const uint8_t PIN_LED_YELLOW = 1;
static const uint8_t PIN_LED_RED = 18;

#elif defined(CONFIG_IDF_TARGET_ESP32)

// ---------------------------------------------------------------------------
// Classic ESP32 (ESP32-WROOM-32 / TTGO Mini32) - the installed hardware
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

#else
#error "LR2-Redux: no pin map for this IDF target - add one to include/pins.h"
#endif
