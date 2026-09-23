#pragma once
#include <stdint.h>
#include "core/stats.h"
#include "core/transport.h"

namespace flr {

// M1 initiator: 1000 PINGs at 10 pps, RTT per frame, loss verdict.
// Arduino-free: clock injected via tick(now_ms), output via SinkFn.
// Static memory: sorted RTT array (2 KB) + seen-seq bitmap (125 B);
// no dynamic allocation.
class PingApp {
 public:
  PingApp(Transport& t, void* sinkCtx, SinkFn sink);
  // Store the start time and emit the boot line.
  // Context: setup. Blocking: never. Failure: none.
  void begin(uint32_t nowMs);
  // Send the next PING when due, emit a progress line every 100 frames and
  // the m1_result verdict after the 5 s grace once all 1000 are sent.
  // Context: main loop. Blocking: bounded by one send attempt. Failure: none.
  void tick(uint32_t nowMs);
  // Account one received PONG (dedup via seen-seq bitmap, RTT sample).
  // Context: pump owner. Blocking: never (insertion sort O(n) worst case).
  // Failure: none; non-PONG, out-of-range or duplicate seq is ignored.
  void onFrame(const uint8_t* data, size_t len, int16_t rssi);
  // True once m1_result has been emitted. No-op afterwards.
  // Context: any. Blocking: never. Failure: none.
  bool done() const { return done_; }

 private:
  void emit_(const char* line);
  void insertRtt_(uint16_t ms);
  void emitResult_();
  Transport& t_;
  void* sinkCtx_;
  SinkFn sink_;
  uint32_t nextTxMs_ = 0;
  uint32_t tx_ = 0, rx_ = 0;
  uint16_t rttLast_ = 0;
  uint16_t rtt_[1000];   // insertion-sorted ascending
  uint8_t seen_[125];    // bit (seq-1): late/duplicate PONG guard
  uint32_t lastTxMs_ = 0, nowMs_ = 0;
  bool done_ = false;
};

}  // namespace flr
