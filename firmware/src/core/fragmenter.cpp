#include "core/fragmenter.h"
#include <string.h>

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

// ---- Reassembler ----

void Reassembler::begin(Deliver d, void* ctx) {
  deliver_ = d; dctx_ = ctx; rcvCum_ = 0; haveBase_ = false;
  for (auto& s : slots_) s.cnt = 0;
}

void Reassembler::tick(uint32_t nowMs) {
  for (auto& s : slots_)
    if (s.cnt != 0 && nowMs - s.lastTouchMs > kSlotGcMs) s.cnt = 0;
}

Reassembler::Slot* Reassembler::findSlot_(uint16_t seq) {
  for (auto& s : slots_)
    if (s.cnt != 0 && s.seq == seq) return &s;
  return nullptr;
}

Reassembler::Slot* Reassembler::allocSlot_(uint16_t seq, uint32_t nowMs) {
  for (auto& s : slots_)
    if (s.cnt == 0) {
      s.seq = seq; s.cnt = 1;  // cnt set by caller; mark in use
      s.recvBits = 0; s.fragSize = 0; s.lastLen = 0; s.tailSeen = false;
      s.lastTouchMs = nowMs;
      return &s;
    }
  // no free slot: evict the least recently touched
  Slot* victim = &slots_[0];
  for (auto& s : slots_)
    if (s.lastTouchMs < victim->lastTouchMs) victim = &s;
  victim->seq = seq; victim->cnt = 1; victim->recvBits = 0;
  victim->fragSize = 0; victim->lastLen = 0; victim->tailSeen = false;
  victim->lastTouchMs = nowMs;
  return victim;
}

void Reassembler::tryAdvance_() {
  // The FIRST completed frame baselines a booting receiver (rcvCum_ jumps
  // to its seq — there is no earlier history to wait for). Afterwards the
  // advance is strictly contiguous: a complete slot above a gap stays
  // blocked and shows up in the ACK bitmap / NAK instead.
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (auto& s : slots_) {
      if (s.cnt == 0) continue;
      const uint16_t all = (uint16_t)((1u << s.cnt) - 1);
      if (s.recvBits != all) continue;
      if (haveBase_ && s.seq != (uint16_t)(rcvCum_ + 1)) continue;
      const size_t total = (s.cnt == 1)
          ? s.lastLen
          : (size_t)(s.cnt - 1) * s.fragSize + s.lastLen;
      if (deliver_) deliver_(dctx_, s.buf, total);
      rcvCum_ = s.seq;
      haveBase_ = true;
      s.cnt = 0;  // free
      advanced = true;
    }
  }
}

FragRx Reassembler::onFragment(uint16_t seq, uint8_t idx, uint8_t cnt,
                               const uint8_t* data, size_t n,
                               uint32_t nowMs) {
  const int16_t relStale = (int16_t)(seq - rcvCum_);
  if (relStale <= 0) return FragRx::STALE;              // seq <= rcvCum
  const int16_t rel = (int16_t)(seq - (uint16_t)(rcvCum_ + 1));
  if (rel > (int16_t)(kSlots - 1)) return FragRx::OVERFLOW;

  FragRx outcome = FragRx::ACCEPTED;
  Slot* s = findSlot_(seq);
  if (s != nullptr && s->cnt != cnt) {
    // P-13: the ARRIVING fragmentation wins (a re-fragmented resend
    // may use a different, even smaller, cnt). Reset under new cnt;
    // the arriving fragment is stored under the new layout and
    // META_CONFLICT is reported so the caller can count the event.
    s->recvBits = 0; s->fragSize = 0; s->lastLen = 0; s->tailSeen = false;
    s->cnt = cnt; s->lastTouchMs = nowMs;
    outcome = FragRx::META_CONFLICT;
  } else if (s == nullptr) {
    s = allocSlot_(seq, nowMs);
    s->cnt = cnt;
  }
  s->lastTouchMs = nowMs;

  // Placement: non-last fragments are full and define fragSize; the
  // last fragment sits at (cnt-1)*fragSize. A last-fragment arriving
  // before any non-last (fragSize unknown) is accepted but not stored
  // — tailSeen marks it so the NAK mask does not re-request it, while
  // completeness still waits for the re-received tail (converges, P-05).
  if (idx + 1 == cnt && cnt > 1) {
    if (s->fragSize == 0) {
      s->tailSeen = true;                               // seen, unplaceable
      return FragRx::ACCEPTED;
    }
    if ((size_t)(cnt - 1) * s->fragSize + n > L3_MAX) return FragRx::ACCEPTED;
    memcpy(s->buf + (size_t)(cnt - 1) * s->fragSize, data, n);
    s->lastLen = (uint16_t)n;
    s->tailSeen = false;
  } else {
    s->fragSize = (uint16_t)n;                           // full fragment
    if ((size_t)idx * n + n > L3_MAX) return FragRx::ACCEPTED;
    memcpy(s->buf + (size_t)idx * n, data, n);
    if (cnt == 1) s->lastLen = (uint16_t)n;
  }
  s->recvBits |= (uint16_t)(1u << idx);
  tryAdvance_();
  return outcome;
}

RxReport Reassembler::report() const {
  RxReport r{};
  r.ackBitmap = 0;
  r.nakCount = 0;
  // Window includes rcvCum itself (bit 7, complete by definition) once a
  // baseline exists; before it, the window starts at rcvCum+1.
  const uint16_t base = haveBase_ ? rcvCum_ : (uint16_t)(rcvCum_ + 1);
  for (uint8_t i = 0; i < kSlots; ++i) {
    const uint16_t seq = (uint16_t)(base + i);
    if (haveBase_ && i == 0) {
      r.ackBitmap |= (uint8_t)(1u << (7 - i));
      continue;
    }
    const Slot* found = nullptr;
    for (auto& s : slots_)
      if (s.cnt != 0 && s.seq == seq) { found = &s; break; }
    if (found == nullptr) continue;
    const uint16_t all = (uint16_t)((1u << found->cnt) - 1);
    if (found->recvBits == all) {
      r.ackBitmap |= (uint8_t)(1u << (7 - i));
    } else if (r.nakCount < 4) {
      r.nakSeqs[r.nakCount] = seq;
      // A seen-but-unplaceable tail is not re-requested.
      const uint16_t mask = (uint16_t)(~found->recvBits & all);
      r.nakMasks[r.nakCount] =
          (uint8_t)(found->tailSeen ? mask & (uint16_t)~(1u << (found->cnt - 1))
                                    : mask);
      ++r.nakCount;
    }
  }
  return r;
}

}  // namespace flr
