#pragma once
#include <stdint.h>
#include "core/fragmenter.h"
#include "core/l2_pdu.h"
#include "core/transport.h"

namespace flr {

struct ArqStats {
  uint32_t txFrames = 0, txFrags = 0, txRetries = 0, txDropped = 0,
           txTooBig = 0, txWindowOverflow = 0, metaConflict = 0;
  uint32_t rxFrags = 0, rxDelivered = 0, rxDups = 0, rxStale = 0,
           rxWindowOverflow = 0, nakSent = 0, ackSent = 0;
};

// Single-peer section 5 session: sender window + receiver reassembly.
// All methods loop-context except onPdu (pump context). Timer math is
// uint32 subtraction (wraparound-safe).
class ArqSession {
 public:
  using DeliverCb = void (*)(void*, const uint8_t*, size_t);
  // Context: setup. Blocking: never. seedSeq: test seam (P-11 wrap).
  void begin(Transport* t, uint32_t nowMs);
  void begin(Transport* t, uint32_t nowMs, uint16_t seedSeq);
  // Queue one L3 frame (<= 512 B). Oversize: false + txTooBig++.
  // Window full: parks in the 4-frame backlog (drained in tick).
  // Backlog full: false + txWindowOverflow++ (never silent).
  // Context: loop. Blocking: one fragmentation burst + sends.
  bool tx(const uint8_t* frame, size_t len, uint32_t nowMs);
  // One raw PDU from the transport pump: DATA -> reassembler (+ ACK/NAK
  // emission per section 5.2 triggers), ACK -> retire window frames,
  // NAK -> immediate retransmit of listed fragments. L2_CTRL drops.
  // Context: pump. Blocking: bounded by one frame.
  void onPdu(const uint8_t* raw, size_t len, uint32_t nowMs);
  // Timer ladder (60 ms << retries, cap 500 ms, max 5 -> DROPPED),
  // backlog promotion, receiver slot GC hand-off.
  // Context: loop. Blocking: bounded by window contents.
  void tick(uint32_t nowMs);
  void onDelivered(DeliverCb cb, void* ctx);
  // Consumes the transport's send-failure flag as retry #1 of the
  // oldest unacknowledged fragment (section 7.1). Context: pump.
  void onTransportSendFailed(uint32_t nowMs);
  const ArqStats& stats() const { return stats_; }

 private:
  static constexpr size_t kWindow = 8;
  static constexpr size_t kBacklog = 4;
  static constexpr uint32_t kRetryBaseMs = 60;
  static constexpr uint32_t kRetryCapMs = 500;
  static constexpr uint8_t kMaxRetries = 5;
  static constexpr size_t kMtuPayload = 246;    // ESP-NOW budget (M2)

  struct TxFrame {
    uint16_t seq = 0;        // 0 = free
    uint16_t len = 0;
    uint8_t cnt = 0;
    uint32_t allocOrder = 0; // oldest-unacked tiebreak
    uint16_t covered = 0;    // fragments accounted by last ACK/NAK
    uint8_t retries[15] = {0};
    uint32_t lastSendMs[15] = {0};
    uint8_t frame[L3_MAX];
  };

  TxFrame* freeFrame_();
  TxFrame* findBySeq_(uint16_t seq);
  uint16_t allocSeq_();
  // True when the NEXT seq to allocate would land beyond the receiver's
  // window top (lastCum_ + W, section 5.1). Modular: handles SEQ wrap.
  // Context: loop/pump. Blocking: never. Failure: none.
  bool seqWouldOverflow_() const;
  bool enqueueWindow_(const uint8_t* frame, size_t len, uint32_t nowMs,
                      TxFrame* f);
  void sendFragment_(TxFrame& f, uint8_t idx, bool retry, uint32_t nowMs);
  bool retransmit_(TxFrame& f, uint8_t idx, uint32_t nowMs);
  void dropFrame_(TxFrame& f);
  void retireAcked_(uint16_t cumSeq, uint8_t bitmap);
  void promoteBacklog_(uint32_t nowMs);
  void emitAck_();
  void emitNak_(const RxReport& rep);
  static void reasmDeliver_(void* ctx, const uint8_t* d, size_t n);

  Transport* t_ = nullptr;
  TxFrame win_[kWindow];
  uint8_t backlog_[kBacklog][L3_MAX];
  uint16_t backlogLen_[kBacklog] = {0};
  size_t backlogHead_ = 0, backlogCount_ = 0;
  uint16_t nextSeq_ = 0;
  uint16_t lastCum_ = 0;  // cumulative ACK of the most recent ACK (5.1
                          // window bound for new SEQ allocation)
  uint32_t allocCounter_ = 0;
  Reassembler reasm_;
  DeliverCb dcb_ = nullptr;
  void* dctx_ = nullptr;
  ArqStats stats_;
};

}  // namespace flr
