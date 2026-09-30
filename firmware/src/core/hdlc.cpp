#include "core/hdlc.h"

namespace flr {

size_t hdlcFrame(const uint8_t* payload, size_t len,
                 uint8_t* out, size_t outCap) {
  if (len > 512) return 0;
  size_t o = 0;
  if (outCap < 2) return 0;
  out[o++] = 0x7E;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t b = payload[i];
    if (b == 0x7E || b == 0x7D) {
      if (o + 3 > outCap) return 0;
      out[o++] = 0x7D;
      out[o++] = (b == 0x7E) ? 0x5E : 0x5D;
    } else {
      if (o + 2 > outCap) return 0;
      out[o++] = b;
    }
  }
  out[o++] = 0x7E;
  return o;
}

void HdlcDeframer::begin(Cb cb, void* ctx) { cb_ = cb; ctx_ = ctx; reset(); }

void HdlcDeframer::feed(uint8_t b) {
  if (b == 0x7E) {
    // Frame boundary. Deliver ANY non-empty unescaped run (RNS format:
    // no FCS, nothing to validate; empty runs are not frames).
    if (accLen_ > 0 && cb_) cb_(ctx_, acc_, accLen_);
    accLen_ = 0;
    esc_ = false;
    return;
  }
  if (b == 0x7D) { esc_ = true; return; }
  uint8_t v = b;
  if (esc_) {
    if (b == 0x5E) v = 0x7E;
    else if (b == 0x5D) v = 0x7D;
    else resync_++;  // invalid escape; keep raw byte, move on
    esc_ = false;
  }
  if (accLen_ < kMaxAcc) {
    acc_[accLen_++] = v;
  } else {
    // Oversize: count one resync and drop the accumulated kMaxAcc-byte
    // chunk TOGETHER with the triggering byte (nothing here is stored);
    // the remaining bytes of the overlong run re-accumulate as a fresh
    // chunk (a long junk frame costs one resync per full chunk) until
    // the next flag clears the accumulator and the tail delivers like
    // any run.
    resync_++;
    accLen_ = 0;
  }
}

uint32_t HdlcDeframer::resyncCount() const { return resync_; }
void HdlcDeframer::reset() { accLen_ = 0; esc_ = false; resync_ = 0; }

}  // namespace flr
