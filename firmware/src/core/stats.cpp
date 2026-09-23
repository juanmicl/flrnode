#include "core/stats.h"

namespace flr {

void emitLine(void* ctx, SinkFn sink, const char* line) {
  if (sink) sink(ctx, line);
}

uint16_t percentileOf(const uint16_t* sortedAsc, size_t n, size_t p) {
  if (n == 0) return 0;
  if (p > 100) p = 100;
  const size_t idx = (size_t)((uint64_t)p * (n - 1) / 100);
  return sortedAsc[idx];
}

}  // namespace flr
