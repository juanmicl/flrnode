#include <Arduino.h>
#include <stdio.h>
#include "core/stats.h"
#include "led.h"
#include "ping_app.h"
#include "pong_app.h"
#include "transport/espnow_transport.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#endif

#ifndef FLR_CHANNEL
#define FLR_CHANNEL 1
#endif

static void serialSink(void* /*ctx*/, const char* line) { Serial.println(line); }

static flr::EspNowTransport transport;
static flr::PingApp s_ping(transport, nullptr, serialSink);
static flr::PongApp s_pong(transport, nullptr, serialSink);

static void pingFrameCb(void* ctx, const uint8_t* d, size_t n, int16_t r) {
  static_cast<flr::PingApp*>(ctx)->onFrame(d, n, r);
}
static void pongFrameCb(void* ctx, const uint8_t* d, size_t n, int16_t r) {
  static_cast<flr::PongApp*>(ctx)->onFrame(d, n, r);
}
static void macHex(const uint8_t mac[6], char* out /*>=13*/) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

void setup() {
  Serial.begin(115200);
  ledInit();
  if (!transport.begin()) {
    Serial.println(F("{\"t\":\"error\",\"init\":\"esp_now\"}"));
    while (true) ledSosStep();  // halted error loop; delay() allowed here
  }
  char own[13], peer[13];
  // WiFi.macAddress() returns "AA:BB:CC:DD:EE:FF"; parse it to bytes.
  uint8_t ownMac[6];
  sscanf(WiFi.macAddress().c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
         &ownMac[0], &ownMac[1], &ownMac[2], &ownMac[3], &ownMac[4],
         &ownMac[5]);
  macHex(ownMac, own);
  macHex(transport.peerMac(), peer);
  char boot[160];
  snprintf(boot, sizeof(boot),
           "{\"t\":\"boot\",\"fw\":\"m1\",\"channel\":%d,\"mac\":\"%s\","
           "\"peer\":\"%s\",\"warn\":\"dev keys\"}",
           FLR_CHANNEL, own,
           transport.peerMacConfigured() ? peer : "UNSET-read-mac-above");
  Serial.println(boot);

  if (!transport.peerMacConfigured()) {
    // Provisioning step: flash, read MAC, rebuild with -DFLR_PEER_MAC.
    Serial.println(F("{\"t\":\"idle\",\"reason\":\"no peer mac\"}"));
    while (true) { delay(1000); yield(); }  // halted provisioning idle
  }

#if defined(FLR_ROLE_PINGER)
  transport.onFrame(pingFrameCb, &s_ping);
  s_ping.begin(millis());
#elif defined(FLR_ROLE_PONGER)
  transport.onFrame(pongFrameCb, &s_pong);
  s_pong.begin(millis());
#else
#error "Define FLR_ROLE_PINGER or FLR_ROLE_PONGER"
#endif
}

void loop() {
  const uint32_t now = millis();
  transport.pump();
#if defined(FLR_ROLE_PINGER)
  s_ping.tick(now);
#else
  s_pong.tick(now);
#endif
  yield();  // ESP8266 watchdog
}
