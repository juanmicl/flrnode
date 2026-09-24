#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

constexpr uint8_t L2_VER = 1;
enum L2Ftype : uint8_t {
  L2_DATA_FRAG = 0,
  L2_ACK = 1,
  L2_NAK = 2,
  L2_CTRL = 3  // decodes (section 4-complete); no M2 handler delivers it
};
constexpr uint16_t L2_SEQ_SENTINEL = 0;
constexpr uint8_t L2_FLAG_RETRY = 0x01;  // DATA-FRAG retransmission mark

struct L2Pdu {
  uint8_t ver = L2_VER;
  uint8_t ftype;
  uint8_t flags;   // DATA-FRAG: bit0 RETRY
  uint16_t seq;    // never 0 on air (sentinel, section 5.4)
  uint8_t fragIdx; // DATA-FRAG: fragment index; ACK: bitmap hi nibble
  uint8_t fragCnt; // DATA-FRAG: fragment count; ACK: bitmap lo nibble
};

// Build one DATA-FRAG wire PDU: 4-byte big-endian header + payload.
// Context: any. Blocking: O(n). Failure: 0 on invalid header fields,
// seq == sentinel, or insufficient cap.
size_t l2Encode(const L2Pdu& h, const uint8_t* payload, size_t n,
                uint8_t* out, size_t cap);

// ACK PDU (section 5.2): SEQ = cumulative N, B3 = 8-bit bitmap.
// Context: any. Blocking: O(1). Failure: 0 if cap < 4.
size_t l2EncodeAck(uint16_t cumSeq, uint8_t bitmap, uint8_t* out, size_t cap);

// NAK PDU (section 5.3): payload entries [seq u16 BE][missing_mask u8].
// entries 0..4; zero entries = empty NAK (receiver is ahead, 5.4).
// Context: any. Blocking: O(1). Failure: 0 if entries > 4 or cap short.
size_t l2EncodeNak(const uint16_t* seqs, const uint8_t* masks, size_t entries,
                   uint8_t* out, size_t cap);

// Decode one wire PDU; payload points INTO in on success.
// Context: any. Blocking: O(1). Failure: false when len < 4, ver != 1
// (ftype > 3 is unreachable by construction — 2-bit field), DATA-FRAG
// with cnt == 0 / idx >= cnt / seq == 0, ACK with trailing bytes, or
// NAK payload not 3*k with k <= 4.
bool l2Decode(const uint8_t* in, size_t len, L2Pdu* h,
              const uint8_t** payload, size_t* n);

}  // namespace flr
