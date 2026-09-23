#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

// M1 ping-pong wire format. TEST-ONLY: big-endian like protocol.md 4,
// but this codec is deleted at M2 when the real L2 PDU arrives.
constexpr size_t M1_FRAME_LEN = 13;
constexpr uint8_t M1_TYPE_PING = 1;
constexpr uint8_t M1_TYPE_PONG = 2;

struct M1Decoded {
  uint8_t type;
  uint32_t seq;
  uint32_t ts;  // pinger millis at TX, echoed untouched by the ponger
};

// Context: any. Blocking: never. Failure: false on bad type.
bool m1Encode(uint8_t out[M1_FRAME_LEN], uint8_t type, uint32_t seq,
              uint32_t ts);

// Context: any. Blocking: never. Failure: false on bad magic/length/type.
bool m1Decode(const uint8_t* in, size_t len, M1Decoded* out);

}  // namespace flr
