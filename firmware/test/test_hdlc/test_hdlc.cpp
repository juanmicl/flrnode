#include <unity.h>
#include <string.h>
#include <vector>
#include "core/hdlc.h"

using flr::HdlcDeframer;
using flr::hdlcFrame;

struct Capture {
  uint8_t payload[520]; size_t len; int calls = 0;
};
static void onFrame(void* ctx, const uint8_t* p, size_t n) {
  Capture* c = static_cast<Capture*>(ctx);
  c->len = n; memcpy(c->payload, p, n); c->calls++;
}

struct VecCap {
  std::vector<std::vector<uint8_t>> frames;
};
static void captureVec(void* ctx, const uint8_t* p, size_t n) {
  static_cast<VecCap*>(ctx)->frames.emplace_back(p, p + n);
}

// Roundtrip helper: encode, verify stuffed-only wire shape, decode.
static void roundtrip(const uint8_t* payload, size_t n) {
  uint8_t wire[1040];
  const size_t wlen = hdlcFrame(payload, n, wire, sizeof(wire));
  TEST_ASSERT_GREATER_THAN_UINT32(1, (uint32_t)wlen);
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[0]);
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[wlen - 1]);
  for (size_t i = 1; i + 1 < wlen; ++i)
    TEST_ASSERT_NOT_EQUAL(0x7E, wire[i]);  // no-interior-flag invariant
  Capture cap;
  HdlcDeframer d;
  d.begin(onFrame, &cap);
  for (size_t i = 0; i < wlen; ++i) d.feed(wire[i]);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)cap.len);
  if (n) TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, cap.payload, n);
  return;  // caller checks delivery count when it matters
}

static void test_roundtrip_empty() {
  // RNS format: an empty payload frame is 7E 7E and decodes to NOTHING
  // (an empty run is not a frame — matches RNS's parser).
  const uint8_t wire[2] = {0x7E, 0x7E};
  Capture cap;
  HdlcDeframer d;
  d.begin(onFrame, &cap);
  for (uint8_t b : wire) d.feed(b);
  TEST_ASSERT_EQUAL_INT32(0, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(0, d.resyncCount());
  // Encoder side: hdlcFrame of an empty payload emits exactly 7E 7E.
  uint8_t w[4];
  TEST_ASSERT_EQUAL_UINT32(2, hdlcFrame(nullptr, 0, w, sizeof(w)));
  TEST_ASSERT_EQUAL_UINT8(0x7E, w[0]);
  TEST_ASSERT_EQUAL_UINT8(0x7E, w[1]);
}

static void test_roundtrip_stuffing_bytes() {
  const uint8_t p[4] = {0x7E, 0x7D, 0x11, 0x22};
  uint8_t wire[16];
  const size_t wlen = hdlcFrame(p, 4, wire, sizeof(wire));
  const uint8_t expect[8] = {0x7E, 0x7D, 0x5E, 0x7D, 0x5D, 0x11, 0x22, 0x7E};
  TEST_ASSERT_EQUAL_UINT32(8, wlen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, wire, 8);
  roundtrip(p, 4);
  // and the decoded delivery is byte-exact:
  Capture cap;
  HdlcDeframer d;
  d.begin(onFrame, &cap);
  for (size_t i = 0; i < wlen; ++i) d.feed(wire[i]);
  TEST_ASSERT_EQUAL_INT32(1, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)cap.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(p, cap.payload, 4);
}

static void test_roundtrip_max_payload() {
  uint8_t p[512];
  for (uint32_t i = 0; i < 512; ++i) p[i] = (uint8_t)(i * 7);
  roundtrip(p, 512);
  Capture cap;
  HdlcDeframer d;
  uint8_t wire[1040];
  const size_t wlen = hdlcFrame(p, 512, wire, sizeof(wire));
  d.begin(onFrame, &cap);
  for (size_t i = 0; i < wlen; ++i) d.feed(wire[i]);
  TEST_ASSERT_EQUAL_INT32(1, cap.calls);
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)cap.len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(p, cap.payload, 512);
}

static void test_back_to_back_frames_rule6() {
  const uint8_t p1[3] = {'a', 'b', 'c'};
  const uint8_t p2[2] = {'x', 'y'};
  uint8_t w[64];
  size_t total = hdlcFrame(p1, 3, w, sizeof(w));
  total += hdlcFrame(p2, 2, w + total, sizeof(w) - total);
  VecCap cap;
  HdlcDeframer d;
  d.begin(captureVec, &cap);
  for (size_t i = 0; i < total; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(2, cap.frames.size());
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)cap.frames[0].size());
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)cap.frames[1].size());
  TEST_ASSERT_EQUAL_UINT32(0, d.resyncCount());
}

static void test_invalid_escape_then_recovery() {
  VecCap cap;
  HdlcDeframer d;
  d.begin(captureVec, &cap);
  // 7D followed by 0x31 (invalid): one resync, raw byte kept, and the
  // run STILL delivers at the flag (RNS parser behavior — no CRC to
  // fail; the frame is handed upstream as-is).
  const uint8_t junk[7] = {0x7E, 0x01, 0x7D, 0x31, 0x00, 0x00, 0x7E};
  for (uint8_t b : junk) d.feed(b);
  TEST_ASSERT_EQUAL_INT32(1, cap.frames.size());
  // run = 0x01 + kept-raw 0x31 + 0x00 + 0x00
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)cap.frames[0].size());
  TEST_ASSERT_EQUAL_UINT32(1, d.resyncCount());
  const uint8_t p[2] = {'o', 'k'};
  uint8_t w[32];
  const size_t n = hdlcFrame(p, 2, w, sizeof(w));
  for (size_t i = 0; i < n; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(2, cap.frames.size());
  TEST_ASSERT_EQUAL_UINT32(1, d.resyncCount());  // no new resyncs
}

static void test_oversize_chunked_drop_recovers() {
  VecCap cap;
  HdlcDeframer d;
  d.begin(captureVec, &cap);
  d.feed(0x7E);
  for (int i = 0; i < 600; ++i) d.feed(0x55);    // > kMaxAcc (515)
  d.feed(0x7E);
  // Chunked drop: one resync per full 515-byte chunk (the boundary byte
  // is discarded with the chunk), and the tail run delivers like any
  // run (undetectable as "junk").
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, d.resyncCount());
  TEST_ASSERT_EQUAL_INT32(1, cap.frames.size());
  TEST_ASSERT_EQUAL_UINT32(600 - 515 - 1, (uint32_t)cap.frames[0].size());
  const uint8_t p[2] = {'h', 'i'};
  uint8_t w[32];
  const size_t n = hdlcFrame(p, 2, w, sizeof(w));
  for (size_t i = 0; i < n; ++i) d.feed(w[i]);
  TEST_ASSERT_EQUAL_INT32(2, cap.frames.size());
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)cap.frames[1].size());
}

static void test_no_interior_flag_invariant() {
  // Payloads dense in FLAG/ESC bytes must never produce an interior
  // 0x7E on the wire (RX would resync mid-frame).
  const uint8_t p[8] = {0x7E, 0x7E, 0x7D, 0x7D, 0x7E, 0x7D, 0x11, 0x22};
  uint8_t wire[32];
  const size_t wlen = hdlcFrame(p, 8, wire, sizeof(wire));
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[0]);
  TEST_ASSERT_EQUAL_UINT8(0x7E, wire[wlen - 1]);
  for (size_t i = 1; i + 1 < wlen; ++i)
    TEST_ASSERT_NOT_EQUAL(0x7E, wire[i]);
  TEST_ASSERT_EQUAL_UINT32(2 * 6 + 2 + 2, wlen);  // 6 escaped + 2 plain + flags
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_empty);
  RUN_TEST(test_roundtrip_stuffing_bytes);
  RUN_TEST(test_roundtrip_max_payload);
  RUN_TEST(test_back_to_back_frames_rule6);
  RUN_TEST(test_invalid_escape_then_recovery);
  RUN_TEST(test_oversize_chunked_drop_recovers);
  RUN_TEST(test_no_interior_flag_invariant);
  return UNITY_END();
}
