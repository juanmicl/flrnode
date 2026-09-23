#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

// CRC-16/XMODEM: poly 0x1021, init 0x0000, no reflection, xorout 0.
// Validation: crc16Xmodem("123456789") == 0x31C3 (protocol.md 2.1).
// Context: any. Blocking: O(n). Failure: none.
uint16_t crc16Xmodem(const uint8_t* data, size_t len);

// Build one HDLC wire frame: FLAG, stuffed [TYPE, PAYLOAD, FCS_LO, FCS_HI],
// FLAG. FCS is little-endian (FCS_LO first), per protocol.md 2.1 rule 3.
// Context: any. Blocking: O(n). Failure: returns 0 if len > 512 or the
// output does not fit in outCap.
size_t hdlcFrame(uint8_t innerType, const uint8_t* payload, size_t len,
                 uint8_t* out, size_t outCap);

// Incremental deframer. Feed wire bytes one at a time; complete frames
// with valid FCS are delivered via the callback with unstuffed
// TYPE/PAYLOAD. Context: the feeding task. Blocking: never.
class HdlcDeframer {
 public:
  using Cb = void (*)(void* ctx, uint8_t type, const uint8_t* payload, size_t len);

  // Bind the frame callback and start clean.
  // Context: setup. Blocking: never. Failure: none. Implicitly resets all state.
  void begin(Cb cb, void* ctx);

  // Consume one wire byte; complete frames are delivered via the callback.
  // Context: single feeding task. Blocking: never (O(1)). Failure: none;
  // malformed input is counted in resyncCount().
  void feed(uint8_t b);

  // Authoritative resync counter: junk runs + CRC failures + invalid
  // escapes + oversize chunks.
  // Context: any. Blocking: never.
  uint32_t resyncCount() const;

  // Clear accumulator, escape state and resync counter.
  // Context: any. Blocking: never.
  void reset();
 private:
  static constexpr size_t kMaxAcc = 515;  // TYPE + 512 payload + 2 FCS
  uint8_t acc_[kMaxAcc];
  size_t accLen_ = 0;
  bool esc_ = false;
  uint32_t resync_ = 0;
  Cb cb_ = nullptr;
  void* ctx_ = nullptr;
};

}  // namespace flr
