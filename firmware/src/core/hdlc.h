#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

// Build one HDLC wire frame: FLAG, escaped payload, FLAG — RNS
// SerialInterface format (NO FCS, NO inner type byte). Escaping:
// 0x7E -> 7D 5E, 0x7D -> 7D 5D (ESC_MASK 0x20), per protocol.md 2.
// Context: any. Blocking: O(n). Failure: returns 0 if len > 512 or the
// output does not fit in outCap.
size_t hdlcFrame(const uint8_t* payload, size_t len,
                 uint8_t* out, size_t outCap);

// Incremental deframer. Feed wire bytes one at a time; ANY non-empty
// unescaped run between flags is delivered via the callback (RNS's
// parser: no FCS, so corruption is undetectable at this layer — the
// callback receives whatever the run contains). Context: the feeding
// task. Blocking: never.
class HdlcDeframer {
 public:
  using Cb = void (*)(void* ctx, const uint8_t* payload, size_t len);

  // Bind the frame callback and start clean.
  // Context: setup. Blocking: never. Failure: none. Implicitly resets all state.
  void begin(Cb cb, void* ctx);

  // Consume one wire byte; complete frames are delivered via the callback.
  // Context: single feeding task. Blocking: never — O(1) per byte.
  // Failure: none; invalid escapes and oversize runs are counted in
  // resyncCount().
  void feed(uint8_t b);

  // Authoritative resync counter: invalid escapes + oversize chunks.
  // (A junk RUN between flags is indistinguishable from a frame in this
  // FCS-less format and is delivered like any other run.)
  // Context: any. Blocking: never.
  uint32_t resyncCount() const;

  // Clear accumulator, escape state and resync counter.
  // Context: any. Blocking: never.
  void reset();
 private:
  static constexpr size_t kMaxAcc = 515;  // 512 payload + headroom
  uint8_t acc_[kMaxAcc];
  size_t accLen_ = 0;
  bool esc_ = false;
  uint32_t resync_ = 0;
  Cb cb_ = nullptr;
  void* ctx_ = nullptr;
};

}  // namespace flr
