#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#if defined(FLR_APP_BRIDGE)
#include <EEPROM.h>
#include "bridge_app.h"
#include "core/config_store.h"
#include "led.h"
#include "transport/espnow_transport.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#endif

static void serialBytesSink(void* /*ctx*/, const uint8_t* d, size_t n) {
  Serial.write(d, n);  // <= 256 B per tick; ~2.8 ms at 921600, in budget
}

static flr::EspNowTransport transport;
static flr::BridgeApp s_bridge(transport, nullptr, serialBytesSink);

static flr::ConfigStore cfgStore;
static flr::Config cfg;
static constexpr size_t kEepromAddr = 0;

// Persist callback (registered via onConfigPersist): serialise the live
// config and write it to EEPROM. Context: loop (CTRL set path).
// Blocking: EEPROM commit ~ms. Failure: none (best-effort write).
static void persistConfig(const flr::Config& c, void*) {
  uint8_t img[flr::ConfigStore::kImage];
  cfgStore.save(c, img);
  EEPROM.put(kEepromAddr, img);
  EEPROM.commit();
}

static void macHex(const uint8_t mac[6], char* out /*>=13*/) {
  snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

void setup() {
  Serial.begin(921600);  // AGENTS.md SerialInterface rule
  ledInit();
  // Load + clamp the stored config BEFORE esp_now_init so the applied
  // channel/power are the persisted ones from the very first transmit.
  EEPROM.begin(flr::ConfigStore::kImage);
  uint8_t img[flr::ConfigStore::kImage];
  EEPROM.get(kEepromAddr, img);
  const bool stored = cfgStore.load(img, sizeof(img), &cfg);
  s_bridge.setConfig(cfg);
  s_bridge.onConfigPersist(persistConfig, nullptr);
  // Configured PMK (flrn config set pmk): hand it to the transport so
  // begin() applies it to the radio; all-zero means "no configured key"
  // and begin() keeps the compiled dev-key fallback (dev_keys.h).
  bool hasPmk = false;
  for (int i = 0; i < 16; ++i) hasPmk = hasPmk || (cfg.pmk[i] != 0);
  if (hasPmk) transport.setKeys(cfg.pmk);
  transport.setChannel(cfg.channel);        // begin() applies both
  transport.setTxPower((float)cfg.txPower);
  if (!transport.begin()) {
    Serial.println(F("{\"t\":\"error\",\"init\":\"esp_now\"}"));
    while (true) ledSosStep();  // halted error loop; delay() allowed here
  }
  uint8_t ownMac[6] = {0};
  if (sscanf(WiFi.macAddress().c_str(),
             "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &ownMac[0], &ownMac[1],
             &ownMac[2], &ownMac[3], &ownMac[4], &ownMac[5]) != 6) {
    Serial.println(F("{\"t\":\"error\",\"init\":\"mac_parse\"}"));
    while (true) { delay(1000); yield(); }
  }
  char own[13], peer[13];
  macHex(ownMac, own);
  s_bridge.setNodeId(ownMac);  // node identity for get node_id
  macHex(transport.peerMac(), peer);
  // Key status only — a configured key is never echoed. Same JSON keys
  // as before; hosts parse the schema, the "warn" VALUE reports state.
  const char* keyWarn = hasPmk ? "pmk configured" : "dev keys";
  char boot[160];
  snprintf(boot, sizeof(boot),
           "{\"t\":\"boot\",\"fw\":\"m3\",\"mode\":\"bridge\",\"channel\":%d,"
           "\"mac\":\"%s\",\"peer\":\"%s\",\"warn\":\"%s\"%s}",
           cfg.channel, own,
           transport.peerMacConfigured() ? peer : "UNSET-read-mac-above",
           keyWarn,
           stored ? "" : ",\"cfg\":\"default\"");
  Serial.println(boot);
  if (!transport.peerMacConfigured()) {
    Serial.println(F("{\"t\":\"idle\",\"reason\":\"no peer mac\"}"));
    while (true) { delay(1000); yield(); }  // halted provisioning idle
  }
  s_bridge.begin(millis());
}

void loop() {
  const uint32_t now = millis();
  while (Serial.available()) s_bridge.onSerialByte((uint8_t)Serial.read(), now);
  transport.pump();
  s_bridge.tick(now);
  yield();  // ESP8266 watchdog
}

#elif defined(FLR_ROLE_PINGER) || defined(FLR_ROLE_PONGER)
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
  uint8_t ownMac[6] = {0};
  // Garbage boot MAC would mislead provisioning; fail loud and idle.
  if (sscanf(WiFi.macAddress().c_str(),
             "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &ownMac[0], &ownMac[1], &ownMac[2], &ownMac[3], &ownMac[4],
             &ownMac[5]) != 6) {
    Serial.println(F("{\"t\":\"error\",\"init\":\"mac_parse\"}"));
    while (true) { delay(1000); yield(); }  // halted idle; delay() allowed
  }
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
#else
#error "Define FLR_APP_BRIDGE or FLR_ROLE_PINGER/FLR_ROLE_PONGER"
#endif
