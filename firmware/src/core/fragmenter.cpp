#include "core/fragmenter.h"

namespace flr {

uint8_t fragSplit(const uint8_t* frame, size_t len, FragSink sink, void* ctx,
                  size_t mtuPayload) {
  if (frame == nullptr || sink == nullptr) return 0;
  if (len == 0 || len > L3_MAX) return 0;
  if (mtuPayload == 0) return 0;
  const uint8_t cnt = (uint8_t)((len + mtuPayload - 1) / mtuPayload);
  if (cnt > 15) return 0;
  for (uint8_t i = 0; i < cnt; ++i) {
    const size_t off = (size_t)i * mtuPayload;
    const size_t n = (off + mtuPayload > len) ? (len - off) : mtuPayload;
    sink(ctx, i, cnt, frame + off, n);
  }
  return cnt;
}

}  // namespace flr
