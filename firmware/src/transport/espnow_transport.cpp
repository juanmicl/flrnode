#include "transport/espnow_transport.h"
#include <string.h>
#include "dev_keys.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <espnow.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#endif

#ifndef FLR_CHANNEL
#define FLR_CHANNEL 1
#endif
// Provide as -DFLR_PEER_MAC="AABBCCDDEEFF" at flash time (checklist
// phase 1). Undefined or all-zero: begin() still succeeds but
// peerMacConfigured() is false and the app idles for provisioning.
#ifndef FLR_PEER_MAC
#define FLR_PEER_MAC "000000000000"
#endif

namespace flr {
namespace {
EspNowTransport* g_self = nullptr;  // platform C callbacks carry no ctx
}

static bool parseMac(const char* hex, uint8_t out[6]) {
  if (strlen(hex) != 12) return false;
  for (int i = 0; i < 6; ++i) {
    char byte[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    out[i] = (uint8_t)strtol(byte, nullptr, 16);
  }
  return true;
}

#ifdef ESP8266
// ESP8266 core: no RSSI in the recv callback.
static void onRecvCb(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (!g_self) return;
  if (memcmp(mac, g_self->peerMac(), 6) != 0) return;  // peer filter
  g_self->ringPushIsr(data, len, RSSI_NULL);
}
static void onSendCb(uint8_t* /*mac*/, uint8_t status) {
  if (!g_self) return;
  if (status != 0) g_self->notifySendFailedIsr();
}
#else
#if ESP_ARDUINO_VERSION_MAJOR >= 3
// ESP32 core 3.x: recv_info carries RSSI. (Pinned platform ships 2.x;
// this branch is ready for the upgrade and keeps RSSI out of the core.)
static void onRecvCb(const esp_now_recv_info_t* info, const uint8_t* data,
                     int len) {
  if (!g_self) return;
  if (memcmp(info->src_addr, g_self->peerMac(), 6) != 0) return;
  g_self->ringPushIsr(data, (uint16_t)len, info->rx_ctrl->rssi);
}
#else
// ESP32 core 2.x: legacy signature, no RSSI.
static void onRecvCb(const uint8_t* mac, const uint8_t* data, int len) {
  if (!g_self) return;
  if (memcmp(mac, g_self->peerMac(), 6) != 0) return;
  g_self->ringPushIsr(data, (uint16_t)len, RSSI_NULL);
}
#endif
static void onSendCb(const uint8_t* /*mac*/, esp_now_send_status_t status) {
  if (!g_self) return;
  if (status != ESP_NOW_SEND_SUCCESS) g_self->notifySendFailedIsr();
}
#endif

bool EspNowTransport::begin() {
  g_self = this;
  parseMac(FLR_PEER_MAC, peer_);
  WiFi.persistent(false);  // stored creds must not drag us off-channel
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#ifdef ESP8266
  wifi_set_channel(FLR_CHANNEL);          // BEFORE esp_now_init (quirk)
  if (esp_now_init() != 0) return false;
  // SDK prototypes take non-const u8* though the keys are read-only.
  esp_now_set_kok(const_cast<uint8_t*>(DEV_PMK), 16);
  esp_now_register_recv_cb(onRecvCb);
  esp_now_register_send_cb(onSendCb);
  if (peerMacConfigured())
    esp_now_add_peer(peer_, ESP_NOW_ROLE_COMBO, FLR_CHANNEL,
                     const_cast<uint8_t*>(DEV_LMK), 16);
#else
  esp_wifi_set_channel(FLR_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_set_pmk(DEV_PMK);
  esp_now_register_recv_cb(onRecvCb);
  esp_now_register_send_cb(onSendCb);
  if (peerMacConfigured()) {
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, peer_, 6);
    p.channel = FLR_CHANNEL;
    p.encrypt = true;
    memcpy(p.lmk, DEV_LMK, 16);
    esp_now_add_peer(&p);
  }
#endif
  return true;
}

bool EspNowTransport::send(const uint8_t* data, size_t len) {
  if (len > 250 || !peerMacConfigured()) return false;
  memcpy(pending_, data, len);
  pendingLen_ = (uint16_t)len;
  retried_ = false;
  ++stats_.txOk;
#ifdef ESP8266
  return esp_now_send(peer_, pending_, pendingLen_) == 0;
#else
  return esp_now_send(peer_, pending_, pendingLen_) == ESP_OK;
#endif
}

void EspNowTransport::onFrame(FrameCb cb, void* ctx) { cb_ = cb; ctx_ = ctx; }

void EspNowTransport::pump() {
  // Single immediate retry, decided here (loop context) — never from the
  // WiFi callback (reentrancy risk; spec decision 10).
  if (sendFailed_) {
    sendFailed_ = false;
    if (!retried_) {
      retried_ = true;
      ++stats_.txRetry;
#ifdef ESP8266
      esp_now_send(peer_, pending_, pendingLen_);
#else
      esp_now_send(peer_, pending_, pendingLen_);
#endif
      return;  // result arrives via the callback again
    }
    ++stats_.txFail;  // second failure: real loss for M1 acceptance
  }
  RingFrame f;
  while (ring_.pop(&f)) {
    ++stats_.rx;
    if (cb_) cb_(ctx_, f.data, f.len, f.rssi);
  }
  stats_.ringOverflow = ring_.overflowCount();
}

const uint8_t* EspNowTransport::peerMac() const { return peer_; }

bool EspNowTransport::peerMacConfigured() const {
  for (int i = 0; i < 6; ++i)
    if (peer_[i]) return true;
  return false;
}

void EspNowTransport::ringPushIsr(const uint8_t* d, uint16_t l, int16_t r) {
  ring_.push(d, l, r);
}

void EspNowTransport::notifySendFailedIsr() { sendFailed_ = true; }

}  // namespace flr
