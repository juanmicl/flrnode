#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

constexpr size_t L3_MAX = 512;  // protocol.md section 4: reject above

// Per-fragment delivery: (idx, cnt, slice, sliceLen), idx ascending.
using FragSink = void (*)(void*, uint8_t idx, uint8_t cnt, const uint8_t*,
                          size_t);

// Split one L3 frame into fragments of mtuPayload bytes (all full
// except the last; protocol.md section 6). Context: any. Blocking:
// O(len). Failure: returns 0 on null/oversize args, mtuPayload == 0,
// or a fragment count that would exceed 15.
uint8_t fragSplit(const uint8_t* frame, size_t len, FragSink sink, void* ctx,
                  size_t mtuPayload);

// ---- RX half (added by M2 Task 4) ----

enum class FragRx { STALE, ACCEPTED, OVERFLOW, META_CONFLICT, ACCEPTED_DUP };

// One coherent post-RX readout for ACK bitmap + NAK list. The
// reassembler never transmits; the owning ArqSession interrogates it
// once per received fragment and emits ACK/NAK itself.
struct RxReport {
  uint8_t ackBitmap;   // bit (7-i): seq rcvCum+1+i fully reassembled
  uint16_t nakSeqs[4]; // lowest-seq-first, up to 4 entries
  uint8_t nakMasks[4]; // bit j: fragment j of that seq missing
  size_t nakCount;     // 0..4
};

// Receiver slot state per protocol.md section 5.4: 8 slots for
// rcvCum+1 .. rcvCum+8, per-slot fragment bitmap, completeness-only
// delivery, contiguous rcvCum advance, 5 s slot GC (correctness never
// depends on GC; sender retries + NAK carry it).
class Reassembler {
 public:
  using Deliver = void (*)(void*, const uint8_t*, size_t);
  // Context: setup. Blocking: never. Failure: none; resets all state.
  void begin(Deliver d, void* ctx);
  // Context: loop. Blocking: O(1). Frees slots idle > 5 s; runs the
  // GC-advance (section 6 amendment) and delivery pass.
  void tick(uint32_t nowMs);
  // Context: pump (from ArqSession.onPdu). Blocking: O(1) + one
  // memcpy. Outcome per section 5.4: STALE (absorb silently),
  // ACCEPTED (stored; may still be incomplete), OVERFLOW (outside the
  // POST-baseline window; caller emits empty NAK), META_CONFLICT
  // (arriving fragmentation won; slot reset; caller counts).
  // Pre-baseline (rebooting receiver) nothing overflows: the first
  // completion baselines rcvCum wherever the live sender is.
  FragRx onFragment(uint16_t seq, uint8_t idx, uint8_t cnt,
                    const uint8_t* data, size_t n, uint32_t nowMs);
  // Context: any. Blocking: O(1) over 8 slots.
  RxReport report() const;
  // Section 5.3 NAK candidates under the burst-safe trigger: the slot
  // holding arrSeq contributes ONLY its missing lower-index bits, and
  // only when the ARRIVING fragment has arrIdx > 0 (fragments above the
  // arriving index may still be inbound during a back-to-back burst);
  // any other incomplete slot contributes its full missing mask only if
  // it has stayed partial > kNakAgeMs and was not NAKed since its last
  // touch (one NAK per gap episode; a stored fragment re-arms). Marks
  // aged slots it reports. Context: pump, right after onFragment.
  // Blocking: O(slots). Failure: none (may return zero entries).
  RxReport nakReport(uint16_t arrSeq, uint8_t arrIdx, uint32_t nowMs);
  // Age-only variant for the tick path (no arrival context): every
  // incomplete slot partial > kNakAgeMs not NAKed since its last touch;
  // marks what it reports. Context: loop (tick). Blocking: O(slots).
  // Failure: none (may return zero entries).
  RxReport agedNakReport(uint32_t nowMs);
  // Returns-and-clears the section 5.2 ACK trigger latch: set when ANY
  // in-window slot completes reassembly or rcvCum advances (delivery,
  // GC-advance) — not only when the cumulative itself moves. Context:
  // pump/loop. Blocking: never. Failure: none.
  bool takeCompleted();
  uint16_t rcvCum() const { return rcvCum_; }

 private:
  struct Slot {
    uint16_t seq = 0;
    uint8_t cnt = 0;          // 0 = free
    uint16_t recvBits = 0;    // bit i = fragment i stored
    uint16_t fragSize = 0;    // learned from any non-last fragment
    uint16_t lastLen = 0;
    uint32_t lastTouchMs = 0;
    bool agedNakLatch = false;  // aged NAK fired since lastTouchMs
    uint8_t buf[L3_MAX];
  };
  Slot* findSlot_(uint16_t seq);
  Slot* allocSlot_(uint16_t seq, uint32_t nowMs);
  void tryAdvance_();
  static constexpr uint32_t kSlotGcMs = 5000;
  // NAK age threshold: a partial slot older than this is missing real
  // fragments — a live burst touches its slot every 2-4 ms (protocol.md
  // section 5.3 trigger; prevents NAKing fragments still on the air).
  static constexpr uint32_t kNakAgeMs = 25;
  static constexpr size_t kSlots = 8;
  Slot slots_[kSlots];
  uint16_t rcvCum_ = 0;
  bool haveBase_ = false;   // false until the first completed frame sets
                            // rcvCum_ (a booting receiver baselines on it)
  bool completedSince_ = false;  // 5.2 ACK trigger latch (takeCompleted)
  Deliver deliver_ = nullptr;
  void* dctx_ = nullptr;
};

}  // namespace flr
