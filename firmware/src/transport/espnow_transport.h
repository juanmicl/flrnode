#pragma once
#include <stdint.h>
#include <atomic>
#include "core/ring_buffer.h"
#include "core/transport.h"

namespace flr {

// ESP-NOW unicast transport with CCMP (global PMK + per-peer LMK) and RX
// filtering by peer MAC. Every ESP8266/ESP32 difference lives in the .cpp.
class EspNowTransport : public Transport {
 public:
  // Parses FLR_PEER_MAC ("AABBCCDDEEFF"), brings the radio up on
  // FLR_CHANNEL, registers the encrypted peer. Context: setup.
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
  // Returns-and-clears the send-failure flag. Context: loop (tick).
  // Blocking: never. Used by the bridge app to give the failure to
  // the ARQ layer as retry #1 (protocol.md 7.1). Ping-pong builds
  // never call this; their pump() retry consumes the flag itself.
  bool takeSendFailed();

 private:
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
