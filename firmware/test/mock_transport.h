#pragma once
#include "core/ring_buffer.h"
#include "core/transport.h"

// Test-only Transport. Models the real split: send() fills the PEER's RX
// ring (producer side, like the radio callback); pump() drains OWN ring
// into the registered callback (consumer side, like the main loop).
class MockTransport : public flr::Transport {
 public:
  void wire(MockTransport* peer) { peer_ = peer; }
  void setDropEvery(size_t n) { dropEvery_ = n; }  // 0 = deliver all

  bool begin() override { return peer_ != nullptr; }

  bool send(const uint8_t* data, size_t len) override {
    if (!peer_) return false;
    ++sentCount_;
    if (dropEvery_ != 0 && sentCount_ % dropEvery_ == 0) {
      ++stats_.txFail;  // deterministic bench drop; counts as loss
      return true;
    }
    ++stats_.txOk;
    return peer_->ring_.push(data, (uint16_t)len, -60);
  }

  void onFrame(FrameCb cb, void* ctx) override { cb_ = cb; ctx_ = ctx; }

  void pump() override {
    flr::RingFrame f;
    while (ring_.pop(&f)) {
      ++stats_.rx;
      if (cb_) cb_(ctx_, f.data, f.len, f.rssi);
    }
    stats_.ringOverflow = ring_.overflowCount();
  }

  const flr::TransportStats& stats() const override { return stats_; }

  // Test helper: deliver a crafted frame to this transport as if the radio
  // received it (producer side of OWN ring).
  void inject(const uint8_t* data, size_t len) {
    ring_.push(data, (uint16_t)len, -60);
  }

 private:
  MockTransport* peer_ = nullptr;
  flr::RingBuffer ring_;
  flr::TransportStats stats_{};
  FrameCb cb_ = nullptr;
  void* ctx_ = nullptr;
  size_t dropEvery_ = 0;
  size_t sentCount_ = 0;
};
