#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

// M1 ping-pong wire format. BENCH-ERA ARTIFACT, KEPT: big-endian like
// protocol.md 4. The real L2 PDU (core/l2_pdu.h) replaced this codec on
// the wire at M2, but the M1/M2 build envs and the TEST-CHECKLIST
// hardware gates still flash and drive the ping-pong apps through it,
// so it ships. Do not delete without retiring those envs/gates first.
constexpr size_t M1_FRAME_LEN = 13;
constexpr uint8_t M1_TYPE_PING = 1;
constexpr uint8_t M1_TYPE_PONG = 2;

struct M1Decoded {
  uint8_t type;
  uint32_t seq;
  uint32_t ts;  // pinger millis at TX, echoed untouched by the ponger
};

// Context: any. Blocking: never. Failure: false on bad type.
// `out` must be non-null.
bool m1Encode(uint8_t out[M1_FRAME_LEN], uint8_t type, uint32_t seq,
              uint32_t ts);

// Context: any. Blocking: never. Failure: false on bad magic/length/type.
// `in` and `out` must be non-null.
bool m1Decode(const uint8_t* in, size_t len, M1Decoded* out);

}  // namespace flr
