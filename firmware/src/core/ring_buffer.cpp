#include "core/ring_buffer.h"
#include <string.h>

namespace flr {

bool RingBuffer::push(const uint8_t* data, uint16_t len, int16_t rssi) {
  // overflow_ is producer-only (SPSC contract), so the increment is done
  // as load+store: the lx106 toolchain has no libatomic for the RMW form.
  const auto bumpOverflow_ = [this]() {
    overflow_.store(overflow_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
  };
  if (len > sizeof(RingFrame::data)) { bumpOverflow_(); return false; }
  const size_t head = head_.load(std::memory_order_relaxed);
  const size_t next = (head + 1) % kSlots;
  if (next == tail_.load(std::memory_order_acquire)) { bumpOverflow_(); return false; }
  RingFrame& f = buf_[head];
  memcpy(f.data, data, len);
  f.len = len;
  f.rssi = rssi;
  head_.store(next, std::memory_order_release);
  return true;
}

bool RingBuffer::pop(RingFrame* out) {
  const size_t tail = tail_.load(std::memory_order_relaxed);
  if (tail == head_.load(std::memory_order_acquire)) return false;
  memcpy(out, &buf_[tail], sizeof(RingFrame));
  tail_.store((tail + 1) % kSlots, std::memory_order_release);
  return true;
}

uint32_t RingBuffer::overflowCount() const {
  return overflow_.load(std::memory_order_relaxed);
}

void RingBuffer::reset() {
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  overflow_.store(0, std::memory_order_relaxed);
}

}  // namespace flr
