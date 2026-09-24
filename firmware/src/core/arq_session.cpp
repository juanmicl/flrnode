#include "core/arq_session.h"
#include <string.h>

namespace flr {

void ArqSession::reasmDeliver_(void* ctx, const uint8_t* d, size_t n) {
  auto* self = static_cast<ArqSession*>(ctx);
  ++self->stats_.rxDelivered;
  if (self->dcb_) self->dcb_(self->dctx_, d, n);
}

void ArqSession::begin(Transport* t, uint32_t nowMs, uint16_t seedSeq) {
  t_ = t;
  nextSeq_ = seedSeq;
  allocCounter_ = 0;
  backlogHead_ = 0;
  backlogCount_ = 0;
  stats_ = ArqStats{};
  for (auto& f : win_) f.seq = 0;
  reasm_.begin(&ArqSession::reasmDeliver_, this);
}
void ArqSession::begin(Transport* t, uint32_t nowMs) { begin(t, nowMs, 0); }

ArqSession::TxFrame* ArqSession::freeFrame_() {
  for (auto& f : win_)
    if (f.seq == 0) return &f;
  return nullptr;
}

ArqSession::TxFrame* ArqSession::findBySeq_(uint16_t seq) {
  for (auto& f : win_)
    if (f.seq != 0 && f.seq == seq) return &f;
  return nullptr;
}

uint16_t ArqSession::allocSeq_() {
  do { ++nextSeq_; } while (nextSeq_ == L2_SEQ_SENTINEL);  // skip 0, wrap
  return nextSeq_;
}

void ArqSession::sendFragment_(TxFrame& f, uint8_t idx, bool retry,
                               uint32_t nowMs) {
  L2Pdu h{};
  h.ftype = L2_DATA_FRAG;
  h.flags = retry ? L2_FLAG_RETRY : 0;
  h.seq = f.seq;
  h.fragIdx = idx;
  h.fragCnt = f.cnt;
  uint8_t wire[4 + kMtuPayload];
  const size_t off = (size_t)idx * kMtuPayload;
  const size_t n = (off + kMtuPayload > f.len) ? (f.len - off) : kMtuPayload;
  const size_t wlen = l2Encode(h, f.frame + off, n, wire, sizeof(wire));
  if (wlen) t_->send(wire, wlen);
  f.lastSendMs[idx] = nowMs;
}

bool ArqSession::retransmit_(TxFrame& f, uint8_t idx, uint32_t nowMs) {
  if (f.retries[idx] >= kMaxRetries) {
    dropFrame_(f);
    return false;
  }
  ++f.retries[idx];
  ++stats_.txRetries;
  sendFragment_(f, idx, true, nowMs);
  return true;
}

void ArqSession::dropFrame_(TxFrame& f) {
  ++stats_.txDropped;   // section 5.1: seq stays used; gap is legal
  f.seq = 0;
}

void ArqSession::retireAcked_(uint16_t cumSeq, uint8_t bitmap) {
  for (auto& f : win_) {
    if (f.seq == 0) continue;
    const int16_t d = (int16_t)(f.seq - cumSeq);
    if (d <= 0) {                       // cum or older: fully delivered
      f.seq = 0;
      continue;
    }
    if (d <= 8 && (bitmap & (1u << (7 - (d - 1))))) {
      f.seq = 0;                        // in-window bitmap bit: complete
    }
  }
}

bool ArqSession::enqueueWindow_(const uint8_t* frame, size_t len,
                                uint32_t nowMs, TxFrame* f) {
  memcpy(f->frame, frame, len);
  f->len = (uint16_t)len;
  f->seq = allocSeq_();
  f->cnt = fragSplit(f->frame, len,
                     [](void*, uint8_t, uint8_t, const uint8_t*, size_t) {},
                     nullptr, kMtuPayload);
  f->covered = 0;
  memset(f->retries, 0, sizeof(f->retries));
  f->allocOrder = ++allocCounter_;
  stats_.txFrags += f->cnt;
  for (uint8_t i = 0; i < f->cnt; ++i) sendFragment_(*f, i, false, nowMs);
  return true;
}

bool ArqSession::tx(const uint8_t* frame, size_t len, uint32_t nowMs) {
  if (len == 0 || len > L3_MAX || frame == nullptr) {
    ++stats_.txTooBig;
    return false;
  }
  TxFrame* f = freeFrame_();
  if (f == nullptr) {
    if (backlogCount_ >= kBacklog) {
      ++stats_.txWindowOverflow;
      return false;
    }
    const size_t slot = (backlogHead_ + backlogCount_) % kBacklog;
    memcpy(backlog_[slot], frame, len);
    backlogLen_[slot] = (uint16_t)len;
    ++backlogCount_;
    ++stats_.txFrames;  // accepted: parked frames count on admission
    return true;
  }
  const bool ok = enqueueWindow_(frame, len, nowMs, f);
  if (ok) ++stats_.txFrames;
  return ok;
}

void ArqSession::promoteBacklog_(uint32_t nowMs) {
  while (backlogCount_ > 0) {
    TxFrame* f = freeFrame_();
    if (f == nullptr) break;
    const size_t slot = backlogHead_;
    const uint16_t len = backlogLen_[slot];
    // Promote into the now-free slot WITHOUT re-counting txFrames (the
    // frame was counted when it was parked).
    enqueueWindow_(backlog_[slot], len, nowMs, f);
    backlogHead_ = (backlogHead_ + 1) % kBacklog;
    --backlogCount_;
  }
}

void ArqSession::onPdu(const uint8_t* raw, size_t len, uint32_t nowMs) {
  L2Pdu h;
  const uint8_t* pl;
  size_t pn;
  if (!l2Decode(raw, len, &h, &pl, &pn)) return;
  if (h.ftype == L2_ACK) {
    retireAcked_(h.seq, (uint8_t)((h.fragIdx << 4) | h.fragCnt));
    return;
  }
  if (h.ftype == L2_NAK) {
    for (size_t e = 0; e + 3 <= pn; e += 3) {
      const uint16_t seq = (uint16_t)((pl[e] << 8) | pl[e + 1]);
      const uint8_t mask = pl[e + 2];
      TxFrame* f = findBySeq_(seq);
      if (f == nullptr) continue;
      for (uint8_t j = 0; j < 15 && j < f->cnt; ++j) {
        if (mask & (1u << j)) {
          // NAK retransmits REGARDLESS of covered (amended 5.1): the
          // listed fragment may have gone missing after being covered.
          if (retransmit_(*f, j, nowMs)) f->covered |= (1u << j);
          else return;  // frame dropped mid-NAK
        }
      }
    }
    return;
  }
  if (h.ftype == L2_DATA_FRAG) {
    ++stats_.rxFrags;
    const uint16_t before = reasm_.rcvCum();
    const FragRx r = reasm_.onFragment(h.seq, h.fragIdx, h.fragCnt, pl,
                                       pn, nowMs);
    switch (r) {
      case FragRx::STALE:
        ++stats_.rxStale;                // silent drop (P-07): no re-ACK
        return;
      case FragRx::ACCEPTED_DUP:
        ++stats_.rxDups;                 // idempotent re-ACK trigger (5.2)
        emitAck_();
        return;
      case FragRx::OVERFLOW:
        ++stats_.rxWindowOverflow;
        emitNak_(RxReport{});            // empty NAK: receiver is ahead
        return;
      case FragRx::META_CONFLICT:
        ++stats_.metaConflict;
        break;                           // fall through to report
      case FragRx::ACCEPTED:
        break;
    }
    if (reasm_.rcvCum() != before) emitAck_();  // completeness (5.2)
    const RxReport rep = reasm_.report();
    if (rep.nakCount > 0) emitNak_(rep);
    return;
  }
  // L2_CTRL: decodes, no M2 handler — drop silently.
}

void ArqSession::tick(uint32_t nowMs) {
  reasm_.tick(nowMs);
  for (auto& f : win_) {
    if (f.seq == 0) continue;
    for (uint8_t i = 0; i < f.cnt; ++i) {
      uint32_t tmo = kRetryBaseMs << f.retries[i];
      if (tmo > kRetryCapMs) tmo = kRetryCapMs;
      if (f.covered & (1u << i)) {
        // Covered = ONE ladder skip (amended 5.1): once the fragment's
        // own ladder boundary passes, the bit clears and it rejoins the
        // normal ladder — every fragment stays reachable until
        // MAX_RETRIES.
        if (nowMs - f.lastSendMs[i] < tmo) continue;
        f.covered &= (uint8_t)~(1u << i);
      }
      if (f.retries[i] >= kMaxRetries) { dropFrame_(f); break; }
      if (nowMs - f.lastSendMs[i] >= tmo) retransmit_(f, i, nowMs);
      if (f.seq == 0) break;                  // dropped inside retransmit_
    }
  }
  promoteBacklog_(nowMs);
}

void ArqSession::onDelivered(DeliverCb cb, void* ctx) {
  dcb_ = cb;
  dctx_ = ctx;
}

void ArqSession::onTransportSendFailed(uint32_t nowMs) {
  TxFrame* oldest = nullptr;
  for (auto& f : win_) {
    if (f.seq == 0) continue;
    if (oldest == nullptr || f.allocOrder < oldest->allocOrder) oldest = &f;
  }
  if (oldest == nullptr) return;
  for (uint8_t i = 0; i < oldest->cnt; ++i) {
    if (!(oldest->covered & (1u << i))) {
      retransmit_(*oldest, i, nowMs);   // retry #1 per section 7.1
      return;
    }
  }
}

void ArqSession::emitAck_() {
  const RxReport rep = reasm_.report();
  uint8_t wire[4];
  const size_t n = l2EncodeAck(reasm_.rcvCum(), rep.ackBitmap, wire,
                               sizeof(wire));
  if (n && t_->send(wire, n)) ++stats_.ackSent;
}

void ArqSession::emitNak_(const RxReport& rep) {
  uint8_t wire[16];
  const size_t n = l2EncodeNak(rep.nakSeqs, rep.nakMasks, rep.nakCount,
                               wire, sizeof(wire));
  if (n && t_->send(wire, n)) ++stats_.nakSent;
}

}  // namespace flr
