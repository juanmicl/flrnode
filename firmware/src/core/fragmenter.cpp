#include "core/fragmenter.h"
#include "core/l2_pdu.h"
#include <string.h>

namespace flr {

uint8_t fragSplit(const uint8_t* frame, size_t len, FragSink sink, void* ctx,
                  size_t mtuPayload) {
  if (frame == nullptr || sink == nullptr) return 0;
  if (len == 0 || len > L3_MAX) return 0;
  if (mtuPayload == 0) return 0;
  // Overflow-free ceil: len + mtuPayload - 1 could wrap size_t for a
  // large caller-supplied mtuPayload; divide-then-add cannot.
  const size_t cntSz = len / mtuPayload + (len % mtuPayload ? 1 : 0);
  if (cntSz > 15) return 0;
  const uint8_t cnt = (uint8_t)cntSz;
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
  // GC: aged PARTIAL slots are discarded. Aged COMPLETE slots are kept —
  // delivering a complete frame is always correct, and GC-advance needs
  // them as the aging witness for the skip below.
  for (auto& s : slots_) {
    if (s.cnt == 0 || nowMs - s.lastTouchMs <= kSlotGcMs) continue;
    const uint16_t all = (uint16_t)((1u << s.cnt) - 1);
    if (s.recvBits != all) s.cnt = 0;  // partial → discard
  }
  // GC-advance (section 6.5 amendment): while the head-of-window slot is
  // ABSENT and the oldest existing slot has aged past T_slot_gc, advance
  // rcvCum past it (MODULAR, sentinel-skipping). Partial heads are never
  // skipped; a present head stops the advance; tryAdvance_ below then
  // delivers contiguous completes. Baseline guard: before the first
  // delivery (haveBase_ false) the advance never runs — a restarted
  // sender's early seqs must not desync rcvCum.
  for (;;) {
    if (!haveBase_) break;
    uint16_t head = (uint16_t)(rcvCum_ + 1);
    if (head == L2_SEQ_SENTINEL) head = 1;  // sentinel skip
    if (findSlot_(head) != nullptr) break;
    bool any = false;
    uint32_t oldest = 0;
    for (auto& s : slots_) {
      if (s.cnt == 0) continue;
      if (!any || s.lastTouchMs < oldest) { oldest = s.lastTouchMs; any = true; }
    }
    if (!any || nowMs - oldest <= kSlotGcMs) break;
    rcvCum_ = head;
  }
  tryAdvance_();
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
      s.recvBits = 0; s.fragSize = 0; s.lastLen = 0;
      s.lastTouchMs = nowMs;
      return &s;
    }
  // no free slot: evict the least recently touched
  Slot* victim = &slots_[0];
  for (auto& s : slots_)
    if (s.lastTouchMs < victim->lastTouchMs) victim = &s;
  victim->seq = seq; victim->cnt = 1; victim->recvBits = 0;
  victim->fragSize = 0; victim->lastLen = 0;
  victim->lastTouchMs = nowMs;
  return victim;
}

void Reassembler::tryAdvance_() {
  // The FIRST completed frame baselines a booting receiver (rcvCum_ jumps
  // to its seq — there is no earlier history to wait for); any partial
  // slots at or below the new rcvCum are stale zombies and are freed.
  // Afterwards the advance is strictly contiguous, MODULAR, and skips the
  // 0 sentinel (senders never use it): a complete slot above a gap stays
  // blocked and shows up in the ACK bitmap / NAK instead.
  bool advanced = true;
  while (advanced) {
    advanced = false;
    for (auto& s : slots_) {
      if (s.cnt == 0) continue;
      const uint16_t all = (uint16_t)((1u << s.cnt) - 1);
      if (s.recvBits != all) continue;
      if (haveBase_) {
        uint16_t expected = (uint16_t)(rcvCum_ + 1);
        if (expected == L2_SEQ_SENTINEL) expected = 1;
        if (s.seq != expected) continue;
      }
      const size_t total = (s.cnt == 1)
          ? s.lastLen
          : (size_t)(s.cnt - 1) * s.fragSize + s.lastLen;
      if (deliver_) deliver_(dctx_, s.buf, total);
      rcvCum_ = s.seq;
      if (!haveBase_) {
        haveBase_ = true;
        for (auto& z : slots_)
          if (z.cnt != 0 && (uint16_t)(z.seq - rcvCum_) >= 32768) z.cnt = 0;
      }
      s.cnt = 0;  // free
      advanced = true;
    }
  }
}

FragRx Reassembler::onFragment(uint16_t seq, uint8_t idx, uint8_t cnt,
                               const uint8_t* data, size_t n,
                               uint32_t nowMs) {
  // Stale/window classification is baseline-gated and MODULAR (P-11: a
  // booting receiver accepts the first frame wherever its seq starts,
  // e.g. 65534 — signed-negative distance = wrap-ahead, accepted; a
  //   plainly-forward distance beyond the window overflows). After the
  //   baseline: behind (>= 32768 modular) = stale, ahead > window =
  //   overflow. Pre-baseline: only plainly-forward overflow rejects; the
  //   first completion baselines anywhere.
  const uint16_t relRaw = (uint16_t)(seq - rcvCum_);
  const int16_t srel = (int16_t)relRaw;
  if (haveBase_) {
    if (relRaw == 0 || relRaw >= 32768) return FragRx::STALE;  // seq <= cum
    if (relRaw > (uint16_t)kSlots) return FragRx::OVERFLOW;
  } else if (srel > (int16_t)kSlots) {
    return FragRx::OVERFLOW;
  }

  FragRx outcome = FragRx::ACCEPTED;
  Slot* s = findSlot_(seq);
  if (s != nullptr && s->cnt != cnt) {
    // P-13: the ARRIVING fragmentation wins (a re-fragmented resend
    // may use a different, even smaller, cnt). Reset under new cnt;
    // the arriving fragment is stored under the new layout and
    // META_CONFLICT is reported so the caller can count the event.
    s->recvBits = 0; s->fragSize = 0; s->lastLen = 0;
    s->cnt = cnt; s->lastTouchMs = nowMs;
    outcome = FragRx::META_CONFLICT;
  } else if (s == nullptr) {
    s = allocSlot_(seq, nowMs);
    s->cnt = cnt;
  }
  const uint16_t bit = (uint16_t)(1u << idx);
  if (s->recvBits & bit) return FragRx::ACCEPTED_DUP;  // idempotent merge
  s->lastTouchMs = nowMs;

  // Placement: non-last fragments are full and define fragSize; the
  // last fragment sits at (cnt-1)*fragSize. A last-fragment arriving
  // before any non-last (fragSize unknown) is accepted but not stored
  // — its bytes are not held, so the NAK mask keeps requesting it, and
  // completeness still waits for the re-received tail (converges, P-05).
  if (idx + 1 == cnt && cnt > 1) {
    if (s->fragSize == 0) return FragRx::ACCEPTED;      // unplaceable yet
    if ((size_t)(cnt - 1) * s->fragSize + n > L3_MAX) return FragRx::ACCEPTED;
    memcpy(s->buf + (size_t)(cnt - 1) * s->fragSize, data, n);
    s->lastLen = (uint16_t)n;
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
  // Section 5.2: the bitmap window is ALWAYS rcvCum+1 .. rcvCum+8; the
  // cumulative count itself rides in the ACK's SEQ field, not the bitmap.
  for (uint8_t i = 0; i < kSlots; ++i) {
    const uint16_t seq = (uint16_t)(rcvCum_ + 1 + i);
    const Slot* found = nullptr;
    for (auto& s : slots_)
      if (s.cnt != 0 && s.seq == seq) { found = &s; break; }
    if (found == nullptr) continue;
    const uint16_t all = (uint16_t)((1u << found->cnt) - 1);
    if (found->recvBits == all) {
      r.ackBitmap |= (uint8_t)(1u << (7 - i));
    } else if (r.nakCount < 4) {
      r.nakSeqs[r.nakCount] = seq;
      r.nakMasks[r.nakCount] = (uint8_t)(~found->recvBits & all);
      ++r.nakCount;
    }
  }
  return r;
}

}  // namespace flr
