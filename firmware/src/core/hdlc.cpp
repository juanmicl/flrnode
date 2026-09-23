#include "core/hdlc.h"
#include <string.h>

namespace flr {

uint16_t crc16Xmodem(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                           : (uint16_t)(crc << 1);
  }
  return crc;
}

size_t hdlcFrame(uint8_t innerType, const uint8_t* payload, size_t len,
                 uint8_t* out, size_t outCap) {
  if (len > 512) return 0;
  uint8_t body[515];
  body[0] = innerType;
  if (len) memcpy(body + 1, payload, len);
  const uint16_t fcs = crc16Xmodem(body, len + 1);
  body[len + 1] = (uint8_t)(fcs & 0xFF);  // FCS_LO
  body[len + 2] = (uint8_t)(fcs >> 8);    // FCS_HI
  size_t o = 0;
  if (outCap < 2) return 0;
  out[o++] = 0x7E;
  for (size_t i = 0; i < len + 3; ++i) {
    const uint8_t b = body[i];
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
    // Frame boundary. Well-formed accumulation: TYPE + payload + 2 FCS.
    if (accLen_ >= 3) {
      const uint16_t fcs =
          (uint16_t)(acc_[accLen_ - 2] | (acc_[accLen_ - 1] << 8));
      if (crc16Xmodem(acc_, accLen_ - 2) == fcs) {
        if (cb_) cb_(ctx_, acc_[0], acc_ + 1, accLen_ - 3);
      } else {
        resync_++;  // CRC failure
      }
    } else if (accLen_ > 0) {
      resync_++;    // stray bytes between flags
    }
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
    // Oversize: count one resync and drop this kMaxAcc-byte chunk; the
    // remainder of the overlong run is re-accumulated as a fresh chunk
    // (a long junk frame costs one resync per full chunk) until the next
    // flag clears the accumulator.
    resync_++;
    accLen_ = 0;
  }
}

uint32_t HdlcDeframer::resyncCount() const { return resync_; }
void HdlcDeframer::reset() { accLen_ = 0; esc_ = false; resync_ = 0; }

}  // namespace flr
