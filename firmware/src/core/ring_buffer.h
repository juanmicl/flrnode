#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace flr {

// RSSI value used when the platform does not expose it (ESP8266 core;
// ESP32 core 2.x). See plan notes.
constexpr int16_t RSSI_NULL = INT16_MIN;

// One received frame as stored in the ring. 250 B = ESP-NOW payload budget.
struct RingFrame {
  uint8_t data[250];
  uint16_t len;
  int16_t rssi;
};

// Single-producer (radio callback) / single-consumer (loop) frame queue.
// All storage is static; never allocates. Exactly one producer and one
// consumer; not safe for multiple producers.
class RingBuffer {
 public:
  // Producer side. Copies len bytes into the next free slot.
  // Context: radio/WiFi callback or test producer thread.
  // Blocking: never. Failure: returns false when full or len > 250;
  // overflowCount()++.
  bool push(const uint8_t* data, uint16_t len, int16_t rssi);

  // Consumer side. `out` must be non-null.
  // Context: main loop / test consumer.
  // Blocking: never. Failure: returns false when empty.
  bool pop(RingFrame* out);

  // Rejected push attempts (ring full or oversized). Context: any.
  // Blocking: never.
  uint32_t overflowCount() const;

  // Drop everything and clear counters. Context: setup/tests only.
  // Blocking: never.
  void reset();

 private:
  // 9 slots yield 8 usable frames: one slot is kept empty to distinguish
  // full from empty in this lock-free SPSC scheme.
  static constexpr size_t kSlots = 9;
  RingFrame buf_[kSlots];
  std::atomic<size_t> head_{0};   // producer index
  std::atomic<size_t> tail_{0};   // consumer index
  // Incremented via load/store; producer-only — no libatomic on lx106.
  std::atomic<uint32_t> overflow_{0};
};

}  // namespace flr
