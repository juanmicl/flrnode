#include <unity.h>
#include <vector>
#include "../mock_transport.h"
#include "core/arq_session.h"
#include "core/l2_pdu.h"

struct RadioCapture {
  std::vector<std::vector<uint8_t>> pdus;
};
static void radioCb(void* ctx, const uint8_t* d, size_t n, int16_t) {
  static_cast<RadioCapture*>(ctx)->pdus.emplace_back(d, d + n);
}

static void sessionSetup(MockTransport& a, MockTransport& b,
                         RadioCapture& cap, flr::ArqSession& s,
                         uint16_t seed = 0) {
  a.wire(&b); b.wire(&a);
  b.onFrame(radioCb, &cap);
  if (seed) s.begin(&a, 0, seed); else s.begin(&a, 0);
}

static size_t peerDataCount(const RadioCapture& cap, uint16_t seq,
                            uint8_t idx, bool retryFlag) {
  size_t hits = 0;
  for (auto& p : cap.pdus) {
    flr::L2Pdu h; const uint8_t* pl; size_t pn;
    if (flr::l2Decode(p.data(), p.size(), &h, &pl, &pn) &&
        h.ftype == flr::L2_DATA_FRAG && h.seq == seq &&
        h.fragIdx == idx && ((h.flags & flr::L2_FLAG_RETRY) != 0) == retryFlag)
      ++hits;
  }
  return hits;
}

// Craft one DATA-FRAG wire PDU (test helper for receiver-side feeds).
static size_t mkFrag(uint16_t seq, uint8_t idx, uint8_t cnt,
                     const uint8_t* d, size_t n, uint8_t* wire,
                     size_t cap) {
  flr::L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.seq = seq;
  h.fragIdx = idx; h.fragCnt = cnt;
  return flr::l2Encode(h, d, n, wire, cap);
}

static void test_tx_single_frame_on_air() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[10]; for (int i = 0; i < 10; ++i) f[i] = (uint8_t)i;
  TEST_ASSERT_TRUE(s.tx(f, 10, 0));
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.pdus.size());
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[0].data(), cap.pdus[0].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_DATA_FRAG, h.ftype);
  TEST_ASSERT_EQUAL_UINT16(1, h.seq);          // first seq is 1
  TEST_ASSERT_EQUAL_UINT8(0, h.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, h.fragCnt);
  TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)pn);
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txFrames);
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txFrags);
}

static void test_seq_monotonic_skips_zero() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s, 65534);
  uint8_t f[4] = {0, 0, 0, 0};
  TEST_ASSERT_TRUE(s.tx(f, 4, 0));
  TEST_ASSERT_TRUE(s.tx(f, 4, 0));
  TEST_ASSERT_TRUE(s.tx(f, 4, 0));
  b.pump();
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  flr::l2Decode(cap.pdus[0].data(), cap.pdus[0].size(), &h, &pl, &pn);
  TEST_ASSERT_EQUAL_UINT16(65535, h.seq);
  flr::l2Decode(cap.pdus[1].data(), cap.pdus[1].size(), &h, &pl, &pn);
  TEST_ASSERT_EQUAL_UINT16(1, h.seq);          // 0 skipped
  flr::l2Decode(cap.pdus[2].data(), cap.pdus[2].size(), &h, &pl, &pn);
  TEST_ASSERT_EQUAL_UINT16(2, h.seq);
}

static void test_window_backlog_overflow() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t f[flr::L3_MAX];
  for (size_t i = 0; i < 9; ++i)               // 8 window + 1 backlog
    TEST_ASSERT_TRUE(s.tx(f, flr::L3_MAX, 0));
  for (size_t i = 0; i < 3; ++i)               // backlog fills to 4
    TEST_ASSERT_TRUE(s.tx(f, flr::L3_MAX, 0));
  TEST_ASSERT_FALSE(s.tx(f, flr::L3_MAX, 0));  // 13th: rejected
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txWindowOverflow);
  TEST_ASSERT_EQUAL_UINT32(12, s.stats().txFrames);
}

static void test_retry_ladder_then_dropped() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[4] = {1, 1, 1, 1};
  TEST_ASSERT_TRUE(s.tx(f, 4, 0));
  uint32_t t = 60;                              // ladder timeouts 60,120,240,480,500
  uint32_t step = 60;
  for (int r = 0; r < 5; ++r) {
    t += step;                                  // tick lands on the next
    step = (step * 2 > 500) ? 500 : step * 2;   // timeout boundary
    s.tick(t);
    b.pump();
    TEST_ASSERT_EQUAL_UINT32(r + 1,
        peerDataCount(cap, 1, 0, true));        // one RETRY per ladder step
  }
  s.tick(t + 1000);                             // retries exhausted: DROPPED
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txDropped);
  // gap legal: next frame takes seq 2, not 1
  TEST_ASSERT_TRUE(s.tx(f, 4, t + 1000));
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 2, 0, false));
}

static void test_ack_retires_frames() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[4] = {0, 0, 0, 0};
  s.tx(f, 4, 0);                                 // seq 1
  s.tx(f, 4, 0);                                 // seq 2
  uint8_t ack[8];
  const size_t n = flr::l2EncodeAck(2, 0, ack, sizeof(ack));  // cum=2
  s.onPdu(ack, n, 10);
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().txDropped);
  // window empty now: 8 more accepted
  for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(s.tx(f, 4, 10));
}

static void test_nak_immediate_retransmit_with_retry_flag() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t f[300];                        // 2 fragments of 246
  TEST_ASSERT_TRUE(s.tx(f, 300, 0));            // seq 1, frags 0..1
  b.pump();
  const uint16_t seqs[1] = {1};
  const uint8_t masks[1] = {0x02};              // frag 1 missing
  uint8_t nak[16];
  const size_t n = flr::l2EncodeNak(seqs, masks, 1, nak, sizeof(nak));
  s.onPdu(nak, n, 5);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 1, 1, true));
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txRetries);
}

static void test_transport_failure_oldest_retry1() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[4] = {9, 9, 9, 9};
  s.tx(f, 4, 0);                                 // seq 1 (oldest)
  s.tx(f, 4, 1);                                 // seq 2
  b.pump();
  s.onTransportSendFailed(3);                    // attributed to seq 1
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 1, 0, true));
  TEST_ASSERT_EQUAL_UINT32(0, peerDataCount(cap, 2, 0, true));
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txRetries);
}


static void test_nak_retransmit_lost_recovers() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t f[300];                        // 2 fragments of 246
  TEST_ASSERT_TRUE(s.tx(f, 300, 0));            // seq 1, frags 0..1
  b.pump();
  const uint16_t seqs[1] = {1};
  const uint8_t masks[1] = {0x02};
  uint8_t nak[16];
  const size_t n = flr::l2EncodeNak(seqs, masks, 1, nak, sizeof(nak));
  s.onPdu(nak, n, 5);                            // retransmit #1 (covered)
  b.pump();
  s.onPdu(nak, n, 10);                           // #1 lost on air: the peer
  b.pump();                                      // NAKs again -> fires again
  TEST_ASSERT_EQUAL_UINT32(2, peerDataCount(cap, 1, 1, true));
}

static void test_covered_ladder_expiry() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[4] = {0, 0, 0, 0};
  TEST_ASSERT_TRUE(s.tx(f, 4, 0));               // seq 1
  b.pump();
  const uint16_t seqs[1] = {1};
  const uint8_t masks[1] = {0x01};
  uint8_t nak[16];
  const size_t n = flr::l2EncodeNak(seqs, masks, 1, nak, sizeof(nak));
  s.onPdu(nak, n, 5);                            // retransmit #1, covered
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 1, 0, true));
  s.tick(505);                                   // covered skip expires
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, peerDataCount(cap, 1, 0, true));
  for (int i = 0; i < 3; ++i) {                  // ladder to exhaustion
    s.tick(505 + 500 * (i + 1));
    b.pump();
  }
  TEST_ASSERT_EQUAL_UINT32(5, peerDataCount(cap, 1, 0, true));
  s.tick(505 + 500 * 4 + 1000);                  // retries exhausted
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().txDropped);
}

static void test_stale_is_silent() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t f[4] = {0, 0, 0, 0};
  flr::L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.seq = 1; h.fragIdx = 0; h.fragCnt = 1;
  uint8_t pdu[8];
  const size_t pn = flr::l2Encode(h, f, 4, pdu, sizeof(pdu));
  s.onPdu(pdu, pn, 10);                          // completes: rcvCum = 1
  const uint32_t acksBefore = s.stats().ackSent;
  const uint32_t dupsBefore = s.stats().rxDups;
  const uint32_t staleBefore = s.stats().rxStale;
  s.onPdu(pdu, pn, 20);                          // same seq again: STALE
  TEST_ASSERT_EQUAL_UINT32(acksBefore, s.stats().ackSent);   // silent
  TEST_ASSERT_EQUAL_UINT32(dupsBefore, s.stats().rxDups);
  TEST_ASSERT_EQUAL_UINT32(staleBefore + 1, s.stats().rxStale);
}

// ---- Integration over a wired mock pair (Task 6) ----
#include <string.h>

struct TwoNode {
  MockTransport ta, tb;
  flr::ArqSession sa, sb;
  std::vector<std::vector<uint8_t>> delivered;
  std::vector<std::vector<uint8_t>> saRx;  // PDUs sa received (ACKs/NAKs)
  uint32_t curT = 0;

  void init(uint16_t seedA = 0) {
    ta.wire(&tb); tb.wire(&ta);
    if (seedA) sa.begin(&ta, 0, seedA); else sa.begin(&ta, 0);
    sb.begin(&tb, 0);
    sb.onDelivered([](void* c, const uint8_t* d, size_t n) {
      static_cast<TwoNode*>(c)->delivered.emplace_back(d, d + n);
    }, this);
    ta.onFrame([](void* c, const uint8_t* d, size_t n, int16_t) {
      auto* N = static_cast<TwoNode*>(c);
      N->saRx.emplace_back(d, d + n);
      N->sa.onPdu(d, n, N->curT);
    }, this);
    tb.onFrame([](void* c, const uint8_t* d, size_t n, int16_t) {
      auto* N = static_cast<TwoNode*>(c);
      N->sb.onPdu(d, n, N->curT);
    }, this);
  }
  void step(uint32_t now) {
    curT = now;
    sa.tick(now);
    sb.tick(now);
    ta.pump();
    tb.pump();
  }
  void run(uint32_t from, uint32_t to, uint32_t dt = 5) {
    for (uint32_t t = from; t < to; t += dt) step(t);
  }
};

// First NAK (lowest-seq entry) sa received for `seq`; false if none.
static bool firstNakEntry(const TwoNode& n, uint16_t seq, uint8_t* maskOut) {
  for (auto& p : n.saRx) {
    flr::L2Pdu h; const uint8_t* pl; size_t pn;
    if (!flr::l2Decode(p.data(), p.size(), &h, &pl, &pn)) continue;
    if (h.ftype != flr::L2_NAK || pn < 3) continue;
    if ((uint16_t)((pl[0] << 8) | pl[1]) == seq) {
      *maskOut = pl[2];
      return true;
    }
  }
  return false;
}

static void test_p04_happy_path_end_to_end() {
  TwoNode n;
  n.init();
  uint8_t f[20];
  for (int i = 0; i < 20; ++i) f[i] = (uint8_t)(i * 5);
  TEST_ASSERT_TRUE(n.sa.tx(f, 20, 0));
  n.run(0, 60);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, n.delivered[0].data(), 20);
  TEST_ASSERT_EQUAL_UINT32(1, n.sb.stats().ackSent);
  TEST_ASSERT_EQUAL_UINT32(0, n.sa.stats().txRetries);
  // ACK retired the sender window: 8 more frames accepted
  for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(n.sa.tx(f, 20, 60));
}

static void test_p08_gap_nak_exact_recovery() {
  TwoNode n;
  n.init();
  n.ta.setDropEvery(2);      // drops the 2nd send: fragment 1 of seq 1
  static uint8_t f[500];
  for (size_t i = 0; i < 500; ++i) f[i] = (uint8_t)(i ^ 0xA7);
  TEST_ASSERT_TRUE(n.sa.tx(f, 500, 0));       // 3 fragments
  n.run(0, 60);
  // Section 5.3 trigger: the first NAK must be the lower-gap one fired
  // by fragment 2's arrival (mask 0x02 = fragment 1 only). The old
  // burst trigger NAKed from fragment 0's arrival with mask 0x06
  // (requesting the still-in-air fragments 1 AND 2) — never again.
  uint8_t mask = 0;
  TEST_ASSERT_TRUE(firstNakEntry(n, 1, &mask));
  TEST_ASSERT_EQUAL_UINT8(0x02, mask);
  n.run(60, 400);   // recovery: NAK retransmits slip the dropEvery(2)
                    // parity and the covered-skip ladder
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
  TEST_ASSERT_EQUAL_UINT32(500, (uint32_t)n.delivered[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, n.delivered[0].data(), 500);
  TEST_ASSERT_EQUAL_UINT32(0, n.sa.stats().txDropped);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, n.sb.stats().nakSent);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, n.sa.stats().txRetries);
}

static void test_p10_overflow_empty_nak() {
  TwoNode n;
  n.init();
  // Baseline first: window policing is a POST-baseline behaviour (a
  // rebooting receiver baselines on first contact instead — see the
  // reboot-resync test); P-10 tests the enforcement after it.
  const uint8_t p[4] = {1, 2, 3, 4};
  uint8_t wire[16];
  size_t w = mkFrag(1, 0, 1, p, 4, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n.ta.send(wire, w));       // baselines sb at rcvCum=1
  n.step(0);
  w = mkFrag(50, 0, 1, p, 4, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n.ta.send(wire, w));       // 50 >> rcvCum+8
  n.step(5);
  TEST_ASSERT_EQUAL_UINT32(1, n.sb.stats().rxWindowOverflow);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
}

static void test_p11_seq_wrap() {
  TwoNode n;
  n.init(65533);               // seqs 65534, 65535, 1 (0 skipped)
  uint8_t f[4] = {0, 0, 0, 0};
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(n.sa.tx(f, 4, 0));
  n.run(0, 80);
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n.delivered.size());
  TEST_ASSERT_EQUAL_UINT32(0, n.sa.stats().txDropped);
}

static void test_lossy_soak_full_recovery() {
  TwoNode n;
  n.init();
  n.ta.setDropEvery(5);
  n.tb.setDropEvery(7);
  static uint8_t f[flr::L3_MAX];
  for (size_t i = 0; i < flr::L3_MAX; ++i) f[i] = (uint8_t)(i * 7 + 3);
  const size_t sizes[6] = {10, 100, 246, 247, 400, 512};
  uint32_t t = 0;
  for (int i = 0; i < 18; ++i) {
    f[0] = (uint8_t)i;  // per-frame seed: identity is pinned, not just size
    TEST_ASSERT_TRUE(n.sa.tx(f, sizes[i % 6], t));
    // 120 ms of stepping per frame: ACKs flow, the window drains, and
    // the backlog never exceeds its 4-frame bound (capacity is 12).
    n.run(t, t + 120, 5);
    t += 120;
  }
  n.run(t, t + 8000, 5);   // settle: retries, NAKs and GC-advance
  // GC-advance (protocol amendment): a retry-exhausted DROPPED frame
  // leaves a gap the receiver skips after T_slot_gc — delivered is
  // every NON-dropped frame, in order (a subsequence of the feed).
  const uint32_t dropped = n.sa.stats().txDropped;
  TEST_ASSERT_EQUAL_UINT32(18 - dropped, (uint32_t)n.delivered.size());
  size_t fi = 0;
  for (size_t di = 0; di < n.delivered.size(); ++di) {
    while (fi < 18 && n.delivered[di].size() != sizes[fi % 6]) ++fi;
    TEST_ASSERT_TRUE(fi < 18);
    f[0] = (uint8_t)fi;  // expected seed of feed frame fi
    TEST_ASSERT_EQUAL_UINT8_ARRAY(f, n.delivered[di].data(), sizes[fi % 6]);
    ++fi;
  }
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, n.sa.stats().txRetries);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, n.sb.stats().nakSent);
}

static void test_ack_discipline_dup_vs_stale() {
  TwoNode n;
  n.init();
  static uint8_t f[500];
  TEST_ASSERT_TRUE(n.sa.tx(f, 500, 0));       // seq 1: 3 fragments
  n.run(0, 100);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
  TEST_ASSERT_EQUAL_UINT32(1, n.sb.stats().ackSent);  // NOT per-fragment
  // STALE (seq <= rcvCum): SILENT per P-07 — no re-ACK, no dup count
  flr::L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.seq = 1; h.fragIdx = 0; h.fragCnt = 1;
  const uint8_t p[4] = {9, 9, 9, 9};
  uint8_t wire[16];
  size_t w = flr::l2Encode(h, p, 4, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n.ta.send(wire, w));
  n.run(100, 140);
  TEST_ASSERT_EQUAL_UINT32(1, n.sb.stats().ackSent);   // still one
  TEST_ASSERT_EQUAL_UINT32(0, n.sb.stats().rxDups);
  // rxStale >= 1: the crafted stale fragment AND any in-flight NAK
  // retransmissions crossing the completion all count as stale.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, n.sb.stats().rxStale);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
  // within-slot duplicate (P-06): re-ACK + rxDups, delivered once
  TEST_ASSERT_TRUE(n.sa.tx(f, 300, 140));      // seq 2: 2 fragments
  n.run(140, 200);                             // delivered; ACK #2
  h.seq = 3; h.fragIdx = 0; h.fragCnt = 2;     // in-window partial slot
  w = flr::l2Encode(h, p, 4, wire, sizeof(wire));
  TEST_ASSERT_TRUE(n.ta.send(wire, w));
  TEST_ASSERT_TRUE(n.ta.send(wire, w));        // exact duplicate
  n.run(200, 260);
  TEST_ASSERT_EQUAL_UINT32(3, n.sb.stats().ackSent);   // re-ACK fired
  TEST_ASSERT_EQUAL_UINT32(1, n.sb.stats().rxDups);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n.delivered.size());
}

// ---- Fix 2: section 5.3 NAK trigger (burst-safe) ----

// (a) an in-order 3-fragment burst with nothing lost produces ZERO NAKs
static void test_burst_in_order_zero_naks() {
  TwoNode n;
  n.init();
  static uint8_t f[500];                    // 3 fragments, in order
  for (size_t i = 0; i < 500; ++i) f[i] = (uint8_t)(i * 13);
  TEST_ASSERT_TRUE(n.sa.tx(f, 500, 0));
  n.run(0, 120);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n.delivered.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, n.delivered[0].data(), 500);
  TEST_ASSERT_EQUAL_UINT32(0, n.sb.stats().nakSent);
  TEST_ASSERT_EQUAL_UINT32(0, n.sa.stats().txRetries);
}

// (b) fragment 1 of 3 arriving after fragment 0 was lost produces
// exactly one NAK listing fragment 0 (never the higher fragments)
static void test_nak_gap_lists_lower_missing_only() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t f[500];
  for (size_t i = 0; i < 500; ++i) f[i] = (uint8_t)(i ^ 0x3C);
  uint8_t wire[300]; size_t w;
  w = mkFrag(1, 1, 3, f + 246, 246, wire, sizeof(wire));  // frag 0 LOST
  s.onPdu(wire, w, 0);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().nakSent);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.pdus.size());
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[0].data(), cap.pdus[0].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_NAK, h.ftype);
  TEST_ASSERT_EQUAL_UINT16(1, (uint16_t)((pl[0] << 8) | pl[1]));  // entry
  TEST_ASSERT_EQUAL_UINT8(0x01, pl[2]);    // fragment 0 ONLY (not 0x05)
  // burst tail arrives: the lower gap re-NAKs fragment 0, still nothing
  // above the arriving index is requested
  w = mkFrag(1, 2, 3, f + 492, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 5);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().nakSent);
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[1].data(), cap.pdus[1].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(0x01, pl[2]);
  // fragment 0 arrives: completes, delivers, ACKs — no further NAK
  w = mkFrag(1, 0, 3, f, 246, wire, sizeof(wire));
  s.onPdu(wire, w, 10);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().nakSent);
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().ackSent);
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().rxDelivered);
}

// (c) a slot partial beyond the ~25 ms age threshold produces a NAK
// from tick(), latched to one per gap episode
static void test_nak_from_tick_after_age_threshold() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t f[500];
  uint8_t wire[300]; size_t w;
  w = mkFrag(1, 0, 3, f, 246, wire, sizeof(wire));  // frags 1..2 missing
  s.onPdu(wire, w, 0);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().nakSent);   // idx 0: never NAKs
  s.tick(20);                                       // below threshold
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().nakSent);
  s.tick(30);                                       // partial > 25 ms
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().nakSent);
  s.tick(100);                                      // latched: no storm
  s.tick(200);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().nakSent);
  // a stored fragment re-arms the latch
  w = mkFrag(1, 2, 3, f + 492, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 210);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().nakSent);   // lower-gap NAK (frag 1)
  s.tick(300);                                      // aged again, re-armed
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().nakSent);
}

// ---- Fix 3: rebooted receiver resyncs on first contact ----

static void test_receiver_reboot_baselines_mid_stream() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  std::vector<std::vector<uint8_t>> got;
  s.onDelivered([](void* c, const uint8_t* d, size_t n) {
    static_cast<std::vector<std::vector<uint8_t>>*>(c)->emplace_back(d, d + n);
  }, &got);
  static uint8_t f[500];
  for (size_t i = 0; i < 500; ++i) f[i] = (uint8_t)(i ^ 0x5C);
  // Sender mid-stream at seq 40000, receiver just booted (rcvCum 0, no
  // baseline): the burst must deliver and baseline there, NOT overflow.
  uint8_t wire[300]; size_t w;
  w = mkFrag(40000, 0, 3, f, 246, wire, sizeof(wire));
  s.onPdu(wire, w, 0);
  w = mkFrag(40000, 1, 3, f + 246, 246, wire, sizeof(wire));
  s.onPdu(wire, w, 3);
  w = mkFrag(40000, 2, 3, f + 492, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 6);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().rxWindowOverflow);  // no overflow
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)got.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, got[0].data(), 500);
  // ACK path works: the completion ACK carries cum = 40000
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().ackSent);
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[0].data(), cap.pdus[0].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_ACK, h.ftype);
  TEST_ASSERT_EQUAL_UINT16(40000, h.seq);
  // post-baseline: the window is now 40001..40008 and enforcement is on
  uint8_t p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  w = mkFrag(40001, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 10);
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().rxDelivered);
}

// ---- Fix 4: SEQ allocation stays inside the ACKed window ----

static void test_tx_parks_beyond_ack_window() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  static uint8_t big[500];                  // 3 fragments each
  static uint8_t small[10];
  TEST_ASSERT_TRUE(s.tx(small, 10, 0));     // seq 1
  for (int i = 0; i < 7; ++i)
    TEST_ASSERT_TRUE(s.tx(big, 500, 0));    // seqs 2..8 (oracle setup)
  b.pump();
  // Frame 2 lost a fragment; frame 3 completed out of order: receiver
  // ACKs cum=1 with seq3's bitmap bit -> seq1 + seq3 retire (2 slots).
  uint8_t ack[8];
  const size_t n = flr::l2EncodeAck(1, 0x40, ack, sizeof(ack));
  s.onPdu(ack, n, 10);
  TEST_ASSERT_TRUE(s.tx(small, 10, 10));    // seq 9 <= cum(1)+8: on air
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 9, 0, false));
  // Next allocation would be 10 > cum+8: PARKED, never on air (pre-fix
  // it was sent and overflow-dropped by the receiver after 5 retries).
  TEST_ASSERT_TRUE(s.tx(small, 10, 10));
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(0, peerDataCount(cap, 10, 0, false));
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().txTooBig);
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().txWindowOverflow);
  TEST_ASSERT_EQUAL_UINT32(10, s.stats().txFrames);  // parked counts
  // Window advances (cum=8): the parked frame promotes on tick.
  const size_t n2 = flr::l2EncodeAck(8, 0, ack, sizeof(ack));
  s.onPdu(ack, n2, 20);
  s.tick(25);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, peerDataCount(cap, 10, 0, false));
}

// ---- Fix 5: ACK triggers ----

// (a) ACK whenever ANY in-window slot completes, not only on cum advance
static void test_ack_on_out_of_order_completion() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t wire[16]; size_t w;
  w = mkFrag(1, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 0);                      // cum 0 -> 1: ACK 1
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().ackSent);
  w = mkFrag(3, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 5);                      // complete but blocked by 2
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().ackSent);   // 5.2: completion ACK
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[1].data(), cap.pdus[1].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_ACK, h.ftype);
  TEST_ASSERT_EQUAL_UINT16(1, h.seq);               // cum unchanged
  TEST_ASSERT_EQUAL_UINT8(0x40, (uint8_t)((h.fragIdx << 4) | h.fragCnt));
  TEST_ASSERT_EQUAL_UINT32(0, s.stats().nakSent);   // no NAK rides along
  w = mkFrag(2, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 10);                     // cum 1 -> 3 (2 and 3 deliver)
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().ackSent);
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[2].data(), cap.pdus[2].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT16(3, h.seq);
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().rxDelivered);
}

// (b) tick-side deliveries / GC-advance also produce the ACK
static void test_ack_on_tick_gc_advance() {
  MockTransport a, b; RadioCapture cap; flr::ArqSession s;
  sessionSetup(a, b, cap, s);
  uint8_t p[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t wire[16]; size_t w;
  w = mkFrag(1, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 0);                      // cum 0 -> 1: ACK 1
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(1, s.stats().ackSent);
  w = mkFrag(3, 0, 1, p, 8, wire, sizeof(wire));
  s.onPdu(wire, w, 10);  // complete witness behind the ABSENT seq 2
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().ackSent);
  // tick past T_slot_gc: seq 2 is skipped (GC-advance), the kept
  // complete seq 3 delivers, and ArqSession emits the ACK for the
  // tick-side advance (cum 1 -> 3).
  s.tick(5000 + 50);
  b.pump();
  TEST_ASSERT_EQUAL_UINT32(3, s.stats().ackSent);
  flr::L2Pdu h; const uint8_t* pl; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(cap.pdus[2].data(), cap.pdus[2].size(),
                                 &h, &pl, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_ACK, h.ftype);
  TEST_ASSERT_EQUAL_UINT16(3, h.seq);
  TEST_ASSERT_EQUAL_UINT32(2, s.stats().rxDelivered);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tx_single_frame_on_air);
  RUN_TEST(test_seq_monotonic_skips_zero);
  RUN_TEST(test_window_backlog_overflow);
  RUN_TEST(test_retry_ladder_then_dropped);
  RUN_TEST(test_ack_retires_frames);
  RUN_TEST(test_nak_immediate_retransmit_with_retry_flag);
  RUN_TEST(test_transport_failure_oldest_retry1);
  RUN_TEST(test_nak_retransmit_lost_recovers);
  RUN_TEST(test_covered_ladder_expiry);
  RUN_TEST(test_stale_is_silent);
  RUN_TEST(test_p04_happy_path_end_to_end);
  RUN_TEST(test_p08_gap_nak_exact_recovery);
  RUN_TEST(test_p10_overflow_empty_nak);
  RUN_TEST(test_p11_seq_wrap);
  RUN_TEST(test_lossy_soak_full_recovery);
  RUN_TEST(test_ack_discipline_dup_vs_stale);
  RUN_TEST(test_burst_in_order_zero_naks);
  RUN_TEST(test_nak_gap_lists_lower_missing_only);
  RUN_TEST(test_nak_from_tick_after_age_threshold);
  RUN_TEST(test_receiver_reboot_baselines_mid_stream);
  RUN_TEST(test_tx_parks_beyond_ack_window);
  RUN_TEST(test_ack_on_out_of_order_completion);
  RUN_TEST(test_ack_on_tick_gc_advance);
  return UNITY_END();
}
