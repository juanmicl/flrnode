#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

struct TransportStats {
  uint32_t txOk = 0;        // frames accepted by the radio stack
  uint32_t txFail = 0;      // radio-reported send loss (bridge: counted
                            // where takeSendFailed hands it to ARQ)
  uint32_t txRetry = 0;     // retries issued from pump()
  uint32_t rx = 0;          // frames delivered upward
  uint32_t ringOverflow = 0;  // rejected push attempts incl. len > 250
};

// Radio-agnostic transport. Implementations: EspNowTransport (hardware),
// MockTransport (tests). All methods non-blocking.
class Transport {
 public:
  virtual ~Transport() = default;
  // Context: setup. Blocking: platform init only (< 200 ms).
  // Failure: false when the radio cannot start.
  virtual bool begin() = 0;
  // Queue one frame. Context: any task. Blocking: never (copy only).
  // Failure: false when the frame cannot be queued.
  virtual bool send(const uint8_t* data, size_t len) = 0;
  using FrameCb = void (*)(void* ctx, const uint8_t* data, size_t len,
                           int16_t rssi);
  // rssi is dBm where the platform exposes it and RSSI_NULL otherwise.
  // Context: setup. Blocking: never. Failure: none.
  virtual void onFrame(FrameCb cb, void* ctx) = 0;
  // Loop-side work: drain the RX ring into the callback and run the
  // send-fail retry. Context: main loop / test stepper.
  // Blocking: bounded by ring capacity. Failure: none.
  virtual void pump() = 0;
  // Cumulative counters since construction/reset.
  // Context: any. Blocking: never. Failure: none.
  virtual const TransportStats& stats() const = 0;
  // Bridge-mode hook: returns-and-clears a pending send failure.
  // Default: none (mocks/tests). Context: loop. Blocking: never.
  virtual bool takeSendFailed() { return false; }
};

}  // namespace flr
