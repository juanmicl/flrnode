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
  return UNITY_END();
}
