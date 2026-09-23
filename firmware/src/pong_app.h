#pragma once
#include <stdint.h>
#include "core/stats.h"
#include "core/transport.h"

namespace flr {

// M1 responder. Validates PING frames and echoes them as PONG (seq and
// timestamp untouched). Heartbeat line every 100 received frames; final
// pong_final line when traffic stops (5 s idle after rx, or 120 s total).
// Arduino-free: clock injected via tick(now_ms), output via SinkFn.
class PongApp {
 public:
  PongApp(Transport& t, void* sinkCtx, SinkFn sink);
  // Store the start time and emit the boot line.
  // Context: setup. Blocking: never. Failure: none.
  void begin(uint32_t nowMs);
  // Update the clock, emit the 100-rx heartbeat and the pong_final line
  // when due (5 s idle after any rx, or 120 s total, only if rx > 0).
  // Context: main loop. Blocking: bounded by one send attempt. Failure: none.
  void tick(uint32_t nowMs);
  // Validate one received frame and echo PING as PONG.
  // Context: pump owner. Blocking: never (one send attempt).
  // Failure: none; non-PING or undecodable input is ignored.
  void onFrame(const uint8_t* data, size_t len, int16_t rssi);
  // True once pong_final has been emitted. No-op afterwards.
  // Context: any. Blocking: never. Failure: none.
  bool finished() const { return finished_; }

 private:
  void emit_(const char* line);
  Transport& t_;
  void* sinkCtx_;
  SinkFn sink_;
  uint32_t rx_ = 0, tx_ = 0;
  uint32_t highestSeq_ = 0;
  uint32_t lastRxMs_ = 0, startMs_ = 0;
  uint32_t tickNowMs_ = 0;        // last tick time, used by onFrame
  uint32_t lastHeartbeatAt_ = 0;  // rx count of the last heartbeat line
  bool finished_ = false;
};

}  // namespace flr
