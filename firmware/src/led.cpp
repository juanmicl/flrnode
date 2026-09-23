#include "led.h"
#include <Arduino.h>

namespace {
// esp32dev variant (core 2.0.x) does not define LED_BUILTIN; both supported
// boards carry the onboard LED on GPIO2.
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif
constexpr int kLedPin = LED_BUILTIN;  // GPIO2 on D1 mini
constexpr bool kActiveLow = true;
}

void ledInit() { pinMode(kLedPin, OUTPUT); ledSet(false); }

void ledSet(bool on) {
  digitalWrite(kLedPin, (on != kActiveLow) ? HIGH : LOW);
}

void ledSosStep() {
  for (int i = 0; i < 3; ++i) { ledSet(true); delay(200); ledSet(false); delay(200); }
  for (int i = 0; i < 3; ++i) { ledSet(true); delay(600); ledSet(false); delay(200); }
  for (int i = 0; i < 3; ++i) { ledSet(true); delay(200); ledSet(false); delay(200); }
  delay(2000);
}
