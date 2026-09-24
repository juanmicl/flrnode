#include <unity.h>
#include <vector>
#include "core/fragmenter.h"

struct Cap {
  std::vector<uint8_t> idx, cnt;
  std::vector<std::vector<uint8_t>> slices;
};
static void sinkFn(void* ctx, uint8_t i, uint8_t c, const uint8_t* d, size_t n) {
  Cap* cap = static_cast<Cap*>(ctx);
  cap->idx.push_back(i);
  cap->cnt.push_back(c);
  cap->slices.emplace_back(d, d + n);
}

static void test_single_fragment() {
  uint8_t f[246];
  for (size_t i = 0; i < 246; ++i) f[i] = (uint8_t)i;
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(1, flr::fragSplit(f, 246, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.idx.size());
  TEST_ASSERT_EQUAL_UINT8(0, cap.idx[0]);
  TEST_ASSERT_EQUAL_UINT8(1, cap.cnt[0]);
  TEST_ASSERT_EQUAL_UINT32(246, (uint32_t)cap.slices[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, cap.slices[0].data(), 246);
}

static void test_ceil_math_and_tail() {
  uint8_t f[247];
  for (size_t i = 0; i < 247; ++i) f[i] = (uint8_t)(i ^ 0x5A);
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(2, flr::fragSplit(f, 247, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT32(246, (uint32_t)cap.slices[0].size());  // full
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.slices[1].size());    // tail
  TEST_ASSERT_EQUAL_UINT8(f[246], cap.slices[1][0]);
}

static void test_max_l3_frame_espnow_and_flrc() {
  static uint8_t f[flr::L3_MAX];
  for (size_t i = 0; i < flr::L3_MAX; ++i) f[i] = (uint8_t)(i * 3);
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(3, flr::fragSplit(f, 512, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT8(246 & 0xFF, (uint8_t)cap.slices[0].size());
  TEST_ASSERT_EQUAL_UINT8(246 & 0xFF, (uint8_t)cap.slices[1].size());
  TEST_ASSERT_EQUAL_UINT8(20, (uint8_t)cap.slices[2].size());
  Cap cap2;
  TEST_ASSERT_EQUAL_UINT8(5, flr::fragSplit(f, 512, sinkFn, &cap2, 123));
  TEST_ASSERT_EQUAL_UINT8(20, (uint8_t)cap2.slices[4].size());
  // ascending idx, constant cnt
  for (uint8_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_UINT8(i, cap2.idx[i]);
    TEST_ASSERT_EQUAL_UINT8(5, cap2.cnt[i]);
  }
}

static void test_cnt_over_limit_rejected() {
  uint8_t f[512];
  for (size_t i = 0; i < 512; ++i) f[i] = (uint8_t)(i * 11);
  Cap cap;
  // ceil(512/30) = 18 > 15: the overflow-free ceil keeps this guard
  // reachable by plain arithmetic — reject with zero sink calls
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 512, sinkFn, &cap, 30));
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)cap.idx.size());
}

static void test_invalid_inputs() {
  uint8_t f[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(nullptr, 4, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 0, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 513, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 4, nullptr, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 4, sinkFn, nullptr, 0));
}

// ---- Reassembler (Task 4) ----
#include <string.h>
#include <vector>

struct Delivered {
  std::vector<std::vector<uint8_t>> frames;
};
static void deliverFn(void* ctx, const uint8_t* d, size_t n) {
  static_cast<Delivered*>(ctx)->frames.emplace_back(d, d + n);
}

static void makeFrame(uint8_t* f, size_t len, uint8_t seed) {
  for (size_t i = 0; i < len; ++i) f[i] = (uint8_t)(seed + i);
}

static void test_reassembly_single_fragment() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[10]; makeFrame(f, 10, 0);
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 0, 1, f, 10, 0));
  TEST_ASSERT_EQUAL_UINT16(1, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)d.frames.size());
  TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)d.frames[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, d.frames[0].data(), 10);
}

static void test_reassembly_three_in_order() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[20]; makeFrame(f, 20, 7);
  for (uint8_t i = 0; i < 3; ++i) {
    const size_t n = (i < 2) ? 7 : 6;
    TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
        r.onFragment(2, i, 3, f + (size_t)i * 7, n, 0));
  }
  TEST_ASSERT_EQUAL_UINT16(2, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)d.frames.size());
  TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)d.frames[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, d.frames[0].data(), 20);
}

static void test_reverse_order_converges_p05() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[21]; makeFrame(f, 21, 3);  // cnt=3, fragSize=10, tail=1
  // idx2 first: unplaceable (fragSize unknown) — accepted, bit unset
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 2, 3, f + 20, 1, 0));
  flr::RxReport rep = r.report();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)rep.nakCount);
  TEST_ASSERT_EQUAL_UINT16(1, rep.nakSeqs[0]);
  TEST_ASSERT_EQUAL_UINT8(0x07, rep.nakMasks[0]);  // all three fragments
  // missing: the tail was seen but its bytes are not held — the mask
  // includes it so the NAK fast path re-requests it
  // idx1 teaches fragSize, idx2 repeat becomes placeable, idx0 completes
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 1, 3, f + 10, 10, 0));
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 2, 3, f + 20, 1, 0));
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 0, 3, f, 10, 0));
  TEST_ASSERT_EQUAL_UINT16(1, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(21, (uint32_t)d.frames[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, d.frames[0].data(), 21);
}

static void test_duplicate_fragment_idempotent_p06() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[10]; makeFrame(f, 10, 0);
  r.onFragment(1, 0, 1, f, 10, 0);                 // completes seq 1
  TEST_ASSERT_EQUAL_INT(flr::FragRx::STALE,        // re-delivery of seq 1
      r.onFragment(1, 0, 1, f, 10, 0));
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)d.frames.size());  // delivered once
}

static void test_stale_and_overflow_p07_p10() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[4] = {0, 0, 0, 0};
  r.onFragment(3, 0, 1, f, 4, 0);                  // rcvCum -> 3
  TEST_ASSERT_EQUAL_INT(flr::FragRx::STALE,
      r.onFragment(3, 0, 1, f, 4, 0));
  TEST_ASSERT_EQUAL_INT(flr::FragRx::STALE,
      r.onFragment(2, 0, 1, f, 4, 0));
  TEST_ASSERT_EQUAL_INT(flr::FragRx::OVERFLOW,
      r.onFragment(12, 0, 1, f, 4, 0));            // rcvCum+9 > window
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(11, 0, 1, f, 4, 0));            // rcvCum+8 in window
}

static void test_fragcnt_conflict_p13() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[20]; makeFrame(f, 20, 9);
  r.onFragment(1, 0, 3, f, 7, 0);                  // starts a cnt=3 slot
  TEST_ASSERT_EQUAL_INT(flr::FragRx::META_CONFLICT,
      r.onFragment(1, 0, 2, f, 10, 0));            // re-fragmented: cnt=2
  // slot reset and re-accumulates under cnt=2, fragSize=10
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 1, 2, f + 10, 10, 0));
  TEST_ASSERT_EQUAL_UINT16(1, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(20, (uint32_t)d.frames[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, d.frames[0].data(), 20);
}

static void test_report_bitmap_and_nak_shape() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  r.onFragment(1, 0, 1, f, 8, 0);                  // seq1 complete
  r.onFragment(2, 0, 2, f, 4, 0);                  // seq2 half
  r.onFragment(3, 0, 1, f, 8, 0);                  // seq3 complete (blocked)
  r.onFragment(4, 1, 2, f + 4, 4, 0);              // seq4: frag1 only
  flr::RxReport rep = r.report();
  TEST_ASSERT_EQUAL_UINT8(0b01000000, rep.ackBitmap);
  // rcvCum=1 after seq1 delivered: bit6 = seq3 (rcvCum+2) complete;
  // seq1 itself is <= cum and correctly absent from the bitmap (5.2)
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)rep.nakCount);
  TEST_ASSERT_EQUAL_UINT16(2, rep.nakSeqs[0]);
  TEST_ASSERT_EQUAL_UINT8(0x02, rep.nakMasks[0]);  // frag1 missing
  TEST_ASSERT_EQUAL_UINT16(4, rep.nakSeqs[1]);
  TEST_ASSERT_EQUAL_UINT8(0x03, rep.nakMasks[1]);  // frag0 + unplaced tail
}

static void test_nak_truncates_lowest_first() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[4] = {0, 0, 0, 0};
  for (uint16_t s = 1; s <= 5; ++s) r.onFragment(s, 0, 2, f, 4, 0);  // 5 gaps
  flr::RxReport rep = r.report();
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)rep.nakCount);
  TEST_ASSERT_EQUAL_UINT16(1, rep.nakSeqs[0]);
  TEST_ASSERT_EQUAL_UINT16(4, rep.nakSeqs[3]);
}

static void test_slot_gc_after_five_seconds() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  r.onFragment(1, 0, 2, f, 4, 0);                  // partial slot
  r.tick(5001);                                    // GC boundary
  flr::RxReport rep = r.report();
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)rep.nakCount);  // slot gone
  // a fresh fragment of the same seq starts over
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 0, 2, f, 4, 0));
  rep = r.report();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)rep.nakCount);
}


static void test_gc_advance_skips_absent_head() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  // Pre-baseline guard: absent heads are never skipped before the first
  // delivery (a restarted sender's early seqs must not desync rcvCum)
  r.onFragment(2, 0, 2, f, 4, 0);   // seq2 partial, pre-baseline
  r.tick(5001);                     // ages out; rcvCum must NOT move
  TEST_ASSERT_EQUAL_UINT16(0, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)d.frames.size());
  // Baseline, then the classic skip: partial head ages out, absent head
  // is skipped, complete slot behind it delivers
  r.onFragment(1, 0, 1, f, 8, 0);   // first completion baselines rcvCum=1
  TEST_ASSERT_EQUAL_UINT16(1, r.rcvCum());
  r.onFragment(3, 0, 2, f, 4, 0);   // seq3 partial (head 2 stays absent)
  r.onFragment(4, 0, 1, f, 8, 0);   // seq4 complete but blocked behind 3
  r.tick(5001);                     // seq3 partial ages out; GC-advance
                                    // skips absent 2; seq4 delivers
  TEST_ASSERT_EQUAL_UINT16(4, r.rcvCum());
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)d.frames.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, d.frames[1].data(), 8);
}

static void test_accepted_dup_outcome() {
  flr::Reassembler r; Delivered d;
  r.begin(deliverFn, &d);
  uint8_t f[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED,
      r.onFragment(1, 0, 2, f, 4, 0));
  flr::RxReport before = r.report();
  TEST_ASSERT_EQUAL_INT(flr::FragRx::ACCEPTED_DUP,
      r.onFragment(1, 0, 2, f, 4, 0));           // same fragment again
  flr::RxReport after = r.report();
  TEST_ASSERT_EQUAL_UINT8(before.ackBitmap, after.ackBitmap);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)before.nakCount, (uint32_t)after.nakCount);
  TEST_ASSERT_EQUAL_UINT8(before.nakMasks[0], after.nakMasks[0]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_single_fragment);
  RUN_TEST(test_ceil_math_and_tail);
  RUN_TEST(test_max_l3_frame_espnow_and_flrc);
  RUN_TEST(test_cnt_over_limit_rejected);
  RUN_TEST(test_invalid_inputs);
  RUN_TEST(test_reassembly_single_fragment);
  RUN_TEST(test_reassembly_three_in_order);
  RUN_TEST(test_reverse_order_converges_p05);
  RUN_TEST(test_duplicate_fragment_idempotent_p06);
  RUN_TEST(test_stale_and_overflow_p07_p10);
  RUN_TEST(test_fragcnt_conflict_p13);
  RUN_TEST(test_report_bitmap_and_nak_shape);
  RUN_TEST(test_nak_truncates_lowest_first);
  RUN_TEST(test_slot_gc_after_five_seconds);
  RUN_TEST(test_gc_advance_skips_absent_head);
  RUN_TEST(test_accepted_dup_outcome);
  return UNITY_END();
}
