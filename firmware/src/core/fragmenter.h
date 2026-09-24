#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

constexpr size_t L3_MAX = 512;  // protocol.md section 4: reject above

// Per-fragment delivery: (idx, cnt, slice, sliceLen), idx ascending.
using FragSink = void (*)(void*, uint8_t idx, uint8_t cnt, const uint8_t*,
                          size_t);

// Split one L3 frame into fragments of mtuPayload bytes (all full
// except the last; protocol.md section 6). Context: any. Blocking:
// O(len). Failure: returns 0 on null/oversize args, mtuPayload == 0,
// or a fragment count that would exceed 15.
uint8_t fragSplit(const uint8_t* frame, size_t len, FragSink sink, void* ctx,
                  size_t mtuPayload);

}  // namespace flr
