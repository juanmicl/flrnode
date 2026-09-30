#pragma once
#include <stdint.h>
#include <atomic>
#include "core/ring_buffer.h"
#include "core/transport.h"

#ifndef FLR_CHANNEL
#define FLR_CHANNEL 1
#endif

namespace flr {

// ESP-NOW unicast transport with CCMP (global PMK + per-peer LMK) and RX
// filtering by peer MAC. Every ESP8266/ESP32 difference lives in the .cpp.
class EspNowTransport : public Transport {
 public:
  // Set the operating channel (default: FLR_CHANNEL build pin). Call
  // BEFORE begin(); begin() applies it to the radio and peer entry.
  // Context: setup. Blocking: never. Failure: none (bounds are the
  // caller's duty — ConfigStore clamps to 1..13).
  void setChannel(uint8_t ch) { channel_ = ch; }
  // Set TX power in dBm conducted (default 17.5 = the M1/M2 compliance
  // pin). Call BEFORE begin(); begin() applies it. Context: setup.
  // Blocking: never. Failure: none (caller clamps; ConfigStore caps 17).
  void setTxPower(float dbm) { txPower_ = dbm; }
  // Configure the CCMP PMK applied by begin(). The pointer is STORED,
  // not copied — it must outlive the transport (main passes the static
  // Config). nullptr or an all-zero key keeps the compiled dev keys
  // (dev_keys.h). A configured key is applied AFTER esp_now_init() and
  // BEFORE the peer is added, and is used BOTH as the PMK and as the
  // single peer's LMK: the bridge is a single-peer point-to-point link
  // today (protocol.md section 3 known limitation), so both ends loading
  // the same key is the working pairing model.
  // Context: setup, BEFORE begin(). Blocking: never. Failure: none (a
  // null/zero key silently selects the dev-key fallback).
  void setKeys(const uint8_t pmk[16]) { pmk_ = pmk; }
  // Parses FLR_PEER_MAC ("AABBCCDDEEFF"), brings the radio up on the
  // configured channel (see setChannel), registers the encrypted peer.
  // Context: setup.
  // Blocking: < 200 ms radio init. Failure: false if esp_now_init fails.
  // peerMacConfigured() false means: caller must idle and print its own
  // MAC for provisioning (see checklist).
  bool begin() override;
  // Context: any. Blocking: never (copies to pending_ buffer + radio call).
  // Failure: false when len > 250 or no peer is configured.
  // Single-in-flight: the caller must pump() between consecutive sends;
  // the pump() retry retries the most recent pending frame.
  bool send(const uint8_t* data, size_t len) override;
  // Registers the RX callback invoked by pump().
  // Context: setup. Blocking: never. Failure: none.
  void onFrame(FrameCb cb, void* ctx) override;
  // Drains the RX ring into the callback and runs the single send-fail
  // retry. Context: main loop. Blocking: bounded by ring capacity.
  // NEVER call from the WiFi callbacks.
  void pump() override;
  // See Transport interface. Context: any. Blocking: never.
  const TransportStats& stats() const override { return stats_; }

  // Last parsed peer MAC (all-zero when unconfigured).
  // Context: any. Blocking: never. Failure: none.
  const uint8_t* peerMac() const;
  // Context: any. Blocking: never. Failure: none.
  // Returns false when FLR_PEER_MAC is unset or all-zero, meaning the
  // caller must idle for provisioning (print its own MAC; see checklist).
  bool peerMacConfigured() const;

  // Callback-context helpers (file-local C callbacks need instance
  // access; platform callbacks carry no user pointer). Minimal work only.
  void ringPushIsr(const uint8_t* d, uint16_t l, int16_t r);
  void notifySendFailedIsr();
  // Returns-and-clears the send-failure flag (counting stats_.txFail).
  // Context: loop (tick). Blocking: never. Used by the bridge app to
  // give the failure to the ARQ layer as retry #1 (protocol.md 7.1)
  // while still counting the radio-level loss. Ping-pong builds never
  // call this; their pump() retry consumes the flag (and counts) itself.
  bool takeSendFailed() override;

 private:
  uint8_t channel_ = FLR_CHANNEL;  // radio/peer channel (setChannel)
  float txPower_ = 17.5f;          // dBm conducted (setTxPower); M1/M2 pin
  const uint8_t* pmk_ = nullptr;   // configured PMK (setKeys); null/zero
                                   // = compiled dev keys (dev_keys.h)
  RingBuffer ring_;
  TransportStats stats_{};
  FrameCb cb_ = nullptr;
  void* ctx_ = nullptr;
  uint8_t peer_[6] = {0, 0, 0, 0, 0, 0};
  uint8_t pending_[250];     // last sent frame, for the single retry
  uint16_t pendingLen_ = 0;
  std::atomic<bool> sendFailed_{false};  // WiFi task -> loop task
  bool retried_ = false;
};

}  // namespace flr
