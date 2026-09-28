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

// FLR_CHANNEL default lives in the header (member initializer); the
// runtime channel comes from setChannel().
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
  for (int i = 0; i < 12; ++i) {
    const char c = hex[i];
    const bool hexDigit = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                          (c >= 'a' && c <= 'f');
    if (!hexDigit) return false;  // leave out untouched (stays zero)
  }
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
  // A malformed FLR_PEER_MAC leaves peer_ zero (parse ignored on purpose):
  // fail loud at boot beats a silent wrong MAC at the bench — the app
  // lands in the idle-provisioning path via peerMacConfigured() == false.
  parseMac(FLR_PEER_MAC, peer_);
  WiFi.persistent(false);  // stored creds must not drag us off-channel
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#ifdef ESP8266
  // EIRP compliance: txPower_ (default 17.5, M3: config-clamped <= 17)
  // + 2 dBi antenna <= 19.5 dBm EIRP <= 20 dBm (docs/compliance.md)
  WiFi.setOutputPower(txPower_);
  wifi_set_channel(channel_);          // BEFORE esp_now_init (quirk)
  if (esp_now_init() != 0) return false;
  // The ESP8266 espnow API requires the node's own role before any TX;
  // without it esp_now_send returns -3 on every call (bench-verified).
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  // SDK prototypes take non-const u8* though the keys are read-only.
  esp_now_set_kok(const_cast<uint8_t*>(DEV_PMK), 16);
  esp_now_register_recv_cb(onRecvCb);
  esp_now_register_send_cb(onSendCb);
  if (peerMacConfigured())
    esp_now_add_peer(peer_, ESP_NOW_ROLE_COMBO, channel_,
                     const_cast<uint8_t*>(DEV_LMK), 16);
#else
  // EIRP compliance: txPower_ (default 17.5, M3: config-clamped <= 17)
  // + 2 dBi antenna <= 19.5 dBm EIRP <= 20 dBm (docs/compliance.md).
  // Core 2.0.x wifi_power_t has no 17.5 dBm step, so use the 0.25 dBm-
  // unit IDF API: 70 = 17.5 dBm, i.e. units of 0.25 dBm.
  esp_wifi_set_max_tx_power((int8_t)(txPower_ * 4));
  esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_set_pmk(DEV_PMK);
  esp_now_register_recv_cb(onRecvCb);
  esp_now_register_send_cb(onSendCb);
  if (peerMacConfigured()) {
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, peer_, 6);
    p.channel = channel_;
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
#if !defined(FLR_APP_BRIDGE)
  // Ping-pong builds: autonomous single retry from loop context (M1
  // behavior). Bridge builds skip this entirely: the flag is consumed
  // by ArqSession::onTransportSendFailed as retry #1 per 7.1, and the
  // ARQ counters — not txFail — carry the story.
  // Single immediate retry, decided here (loop context) — never from the
  // WiFi callback (reentrancy risk; loop-context retry by design).
  if (sendFailed_.load(std::memory_order_relaxed)) {
    sendFailed_.store(false, std::memory_order_relaxed);
    if (!retried_) {
      retried_ = true;
      ++stats_.txRetry;
#ifdef ESP8266
      esp_now_send(peer_, pending_, pendingLen_);
#else
      esp_now_send(peer_, pending_, pendingLen_);
#endif
      // Result arrives via the callback again; the RX drain below is
      // skipped for this one pump() call (harmless one-iteration delay).
      return;
    }
    ++stats_.txFail;  // second failure: real loss for M1 acceptance
  }
#else
  // FLR_APP_BRIDGE: ARQ owns retransmission (section 7.1).
#endif
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

void EspNowTransport::notifySendFailedIsr() {
  sendFailed_.store(true, std::memory_order_relaxed);
}

bool EspNowTransport::takeSendFailed() {
  // lx106 gcc emits __atomic_exchange_1 for exchange() and the ESP8266
  // toolchain ships no libatomic to satisfy it; use the same relaxed
  // load/store pair pump() already uses. Single consumer (loop task),
  // single producer (WiFi task), one-core ESP8266. A signal landing in
  // the clear window is bounded: the ARQ ladder recovers the fragment.
  if (sendFailed_.load(std::memory_order_relaxed)) {
    sendFailed_.store(false, std::memory_order_relaxed);
    return true;
  }
  return false;
}

}  // namespace flr
