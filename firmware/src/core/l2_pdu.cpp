#include "core/l2_pdu.h"

namespace flr {

static uint8_t makeB0(uint8_t ver, uint8_t ftype, uint8_t flags) {
  return (uint8_t)(((ver & 0x03) << 6) | ((ftype & 0x03) << 4) |
                   (flags & 0x0F));
}

size_t l2Encode(const L2Pdu& h, const uint8_t* payload, size_t n,
                uint8_t* out, size_t cap) {
  if (h.ftype != L2_DATA_FRAG) return 0;
  if (h.ver != L2_VER) return 0;  // encoder symmetric with l2Decode
  if (h.fragCnt == 0 || h.fragCnt > 15 || h.fragIdx >= h.fragCnt) return 0;
  if (h.seq == L2_SEQ_SENTINEL) return 0;
  if (n + 4 > cap) return 0;
  out[0] = makeB0(h.ver, h.ftype, h.flags);
  out[1] = (uint8_t)(h.seq >> 8);
  out[2] = (uint8_t)h.seq;
  out[3] = (uint8_t)((h.fragIdx << 4) | h.fragCnt);
  for (size_t i = 0; i < n; ++i) out[4 + i] = payload[i];
  return n + 4;
}

size_t l2EncodeAck(uint16_t cumSeq, uint8_t bitmap, uint8_t* out, size_t cap) {
  if (cap < 4) return 0;
  out[0] = makeB0(L2_VER, L2_ACK, 0);
  out[1] = (uint8_t)(cumSeq >> 8);
  out[2] = (uint8_t)cumSeq;
  out[3] = bitmap;
  return 4;
}

size_t l2EncodeNak(const uint16_t* seqs, const uint8_t* masks, size_t entries,
                   uint8_t* out, size_t cap) {
  if (entries > 4 || cap < 4 + 3 * entries) return 0;
  out[0] = makeB0(L2_VER, L2_NAK, 0);
  out[1] = 0;  // SEQ unused on NAK
  out[2] = 0;
  out[3] = 0;
  for (size_t i = 0; i < entries; ++i) {
    out[4 + 3 * i] = (uint8_t)(seqs[i] >> 8);
    out[5 + 3 * i] = (uint8_t)seqs[i];
    out[6 + 3 * i] = masks[i];
  }
  return 4 + 3 * entries;
}

bool l2Decode(const uint8_t* in, size_t len, L2Pdu* h,
              const uint8_t** payload, size_t* n) {
  if (len < 4) return false;
  const uint8_t b0 = in[0];
  h->ver = (uint8_t)((b0 >> 6) & 0x03);
  h->ftype = (uint8_t)((b0 >> 4) & 0x03);
  h->flags = (uint8_t)(b0 & 0x0F);
  h->seq = (uint16_t)(((uint16_t)in[1] << 8) | in[2]);
  h->fragIdx = (uint8_t)(in[3] >> 4);
  h->fragCnt = (uint8_t)(in[3] & 0x0F);
  const size_t plen = len - 4;
  if (h->ver != L2_VER) return false;
  *payload = in + 4;
  *n = plen;
  switch (h->ftype) {
    case L2_DATA_FRAG:
      return h->fragCnt != 0 && h->fragIdx < h->fragCnt &&
             h->seq != L2_SEQ_SENTINEL;
    case L2_ACK:
      return plen == 0;
    case L2_NAK:
      return (plen % 3) == 0 && (plen / 3) <= 4;
    case L2_CTRL:
      return true;  // parsed; the ARQ handler drops it (M2 has none)
  }
  return false;
}

}  // namespace flr
