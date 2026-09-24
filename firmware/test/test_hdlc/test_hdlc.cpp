#include <unity.h>
#include <string.h>
#include <vector>
#include "core/hdlc.h"

using flr::HdlcDeframer;
using flr::hdlcFrame;

struct Capture {
  uint8_t type; uint8_t payload[520]; size_t len; int calls = 0;
};
static void onFrame(void* ctx, uint8_t type, const uint8_t* p, size_t n) {
  Capture* c = static_cast<Capture*>(ctx);
  c->type = type; c->len = n; memcpy(c->payload, p, n); c->calls++;
}

static void test_crc_known_answer() {
  // Validation vector from protocol.md section 2.1.
  const uint8_t s[9] = {'1','2','3','4','5','6','7','8','9'};
  TEST_ASSERT_EQUAL_HEX16(0x31C3, flr::crc16Xmodem(s, 9));
}

static void roundtrip(const uint8_t* payload, size_t n, uint8_t type) {
  uint8_t wire[540];
  const size_t wlen = hdlcFrame(type, payload, n, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN_UINT32(4, (uint32_t)wlen);
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[0]);
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[wlen - 1]);
  for (size_t i = 1; i + 1 < wlen; ++i)
    TEST_ASSERT_NOT_EQUAL(0x7E, wire[i]);  // stuffing guarantees this
  Capture cap;
  HdlcDeframer d;
  d.begin(onFrame, &cap);
  for (size_t i = 0; i < wlen; ++i) d.feed(wire[i]);
  TEST_ASSERT_EQUAL_INT32(1, cap.calls);
  TEST_ASSERT_EQUAL_UINT8(type, cap.type);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)cap.len);
  if (n) TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, cap.payload, n);
}

static void test_roundtrip_empty() { const uint8_t none[1] = {0}; roundtrip(none, 0, 0x01); }

static void test_roundtrip_stuffing_bytes() {
  const uint8_t p[4] = {0x7E, 0x7D, 0x11, 0x22};
  roundtrip(p, 4, 0x01);
}

static void test_roundtrip_max_payload() {
  uint8_t p[512];
  for (uint32_t i = 0; i < 512; ++i) p[i] = (uint8_t)(i * 7);
  roundtrip(p, 512, 0x01);
}

static void test_resync_on_garbage_and_crc_error() {
  const uint8_t p[3] = {'a', 'b', 'c'};
  uint8_t wire[540];
  const size_t wlen = hdlcFrame(0x01, p, 3, wire, sizeof(wire));
  wire[3] ^= 0x01;  // corrupt one stuffed payload byte -> CRC failure
  Capture cap;
  HdlcDeframer d;
  d.begin(onFrame, &cap);
  d.feed(0x7E); d.feed(0x55); d.feed(0x66); d.feed(0x7E);  // stray junk run
  for (size_t i = 0; i < wlen; ++i) d.feed(wire[i]);
  TEST_ASSERT_EQUAL_INT32(0, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(2, d.resyncCount());  // junk run + CRC failure
}


// ---- M2 carry-overs (Task 9) ----
struct MultiCap {
  int calls = 0;
  uint8_t types[2];
  std::vector<std::vector<uint8_t>> payloads;
};
static void onFrameMulti(void* ctx, uint8_t type, const uint8_t* p, size_t n) {
  MultiCap* c = static_cast<MultiCap*>(ctx);
  if (c->calls < 2) c->types[c->calls] = type;
  c->payloads.emplace_back(p, p + n);
  ++c->calls;
}

static void test_back_to_back_frames_rule6() {
  const uint8_t p1[3] = {'a', 'b', 'c'};
  const uint8_t p2[2] = {'x', 'y'};
  uint8_t w[64];
  size_t total = hdlcFrame(0x01, p1, 3, w, sizeof(w));
  total += hdlcFrame(0x01, p2, 2, w + total, sizeof(w) - total);
  MultiCap cap;
  HdlcDeframer d;
  d.begin(onFrameMulti, &cap);
  for (size_t i = 0; i < total; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(2, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)cap.payloads[0].size());
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)cap.payloads[1].size());
  TEST_ASSERT_EQUAL_UINT32(0, d.resyncCount());
}

static void test_invalid_escape_then_recovery() {
  MultiCap cap;
  HdlcDeframer d;
  d.begin(onFrameMulti, &cap);
  // 7D followed by 0x31 (invalid): resync at feed, raw byte kept, CRC
  // then fails at the flag -> second resync.
  const uint8_t junk[7] = {0x7E, 0x01, 0x7D, 0x31, 0x00, 0x00, 0x7E};
  for (uint8_t b : junk) d.feed(b);
  TEST_ASSERT_EQUAL_INT32(0, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(2, d.resyncCount());
  const uint8_t p[2] = {'o', 'k'};
  uint8_t w[32];
  const size_t n = hdlcFrame(0x01, p, 2, w, sizeof(w));
  for (size_t i = 0; i < n; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(1, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(2, d.resyncCount());  // no new resyncs
}

static void test_oversize_chunked_drop_recovers() {
  MultiCap cap;
  HdlcDeframer d;
  d.begin(onFrameMulti, &cap);
  d.feed(0x7E);
  for (int i = 0; i < 600; ++i) d.feed(0x55);    // > kMaxAcc (515)
  d.feed(0x7E);
  TEST_ASSERT_EQUAL_INT32(0, cap.calls);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, d.resyncCount());
  const uint8_t p[2] = {'h', 'i'};
  uint8_t w[32];
  const size_t n = hdlcFrame(0x01, p, 2, w, sizeof(w));
  for (size_t i = 0; i < n; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(1, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)cap.payloads[0].size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc_known_answer);
  RUN_TEST(test_roundtrip_empty);
  RUN_TEST(test_roundtrip_stuffing_bytes);
  RUN_TEST(test_roundtrip_max_payload);
  RUN_TEST(test_resync_on_garbage_and_crc_error);
  RUN_TEST(test_back_to_back_frames_rule6);
  RUN_TEST(test_invalid_escape_then_recovery);
  RUN_TEST(test_oversize_chunked_drop_recovers);
  return UNITY_END();
}
