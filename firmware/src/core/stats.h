#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

// Print sink: one complete line per call. Arduino-free by construction.
using SinkFn = void (*)(void* ctx, const char* line);

// Emit one line. Context: any. Blocking: the sink call only.
// Failure: none; sink == nullptr is a no-op (headless tests).
void emitLine(void* ctx, SinkFn sink, const char* line);

// Percentile (0..100, clamped) over an ascending sorted array, nearest-rank
// (p=50 with even n → lower median). Context: any.
// Blocking: O(1). Failure: returns 0 when n == 0.
uint16_t percentileOf(const uint16_t* sortedAsc, size_t n, size_t p);

}  // namespace flr
