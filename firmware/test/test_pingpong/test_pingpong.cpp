#include <unity.h>
#include <string>
#include <vector>
#include "../mock_transport.h"
#include "ping_app.h"
#include "pong_app.h"
#include "m1_frame.h"

struct SinkCapture { std::vector<std::string> lines; };
static void capture(void* ctx, const char* line) {
  static_cast<SinkCapture*>(ctx)->lines.push_back(line);
}
static void toPing(void* c, const uint8_t* d, size_t n, int16_t r) {
  static_cast<flr::PingApp*>(c)->onFrame(d, n, r);
}
static void toPong(void* c, const uint8_t* d, size_t n, int16_t r) {
  static_cast<flr::PongApp*>(c)->onFrame(d, n, r);
}

static void wirePair(MockTransport& a, MockTransport& b,
                     flr::PingApp& ping, flr::PongApp& pong) {
  a.wire(&b); b.wire(&a);
  a.onFrame(toPing, &ping);
  b.onFrame(toPong, &pong);
}

// Drives both apps in 10 ms steps like the real loop does: tick both,
// then pump both transports.
static void runExchange(MockTransport& pingSide, MockTransport& pongSide,
                        flr::PingApp& ping, flr::PongApp& pong,
                        uint32_t untilMs) {
  for (uint32_t now = 0; now < untilMs; now += 10) {
    ping.tick(now);
    pong.tick(now);
    pingSide.pump();
    pongSide.pump();
    if (ping.done() && pong.finished()) break;
  }
}

static void test_full_exchange_zero_loss() {
  MockTransport pingSide, pongSide;
  SinkCapture pingSink, pongSink;
  flr::PingApp ping(pingSide, &pingSink, capture);
  flr::PongApp pong(pongSide, &pongSink, capture);
  wirePair(pingSide, pongSide, ping, pong);
  ping.begin(0); pong.begin(0);
  runExchange(pingSide, pongSide, ping, pong, 130000);

  TEST_ASSERT_TRUE(ping.done());
  // boot + 10 progress lines + result = 12 minimum
  TEST_ASSERT_GREATER_OR_EQUAL(12, pingSink.lines.size());
  const std::string result = pingSink.lines.back();
  TEST_ASSERT_TRUE(result.find("\"t\":\"m1_result\"") != std::string::npos);
  TEST_ASSERT_TRUE(result.find("\"loss\":0") != std::string::npos);
  TEST_ASSERT_TRUE(result.find("\"verdict\":\"PASS\"") != std::string::npos);
  // Required keys (spec decision 13: stats assertions live here).
  for (const char* key : {"\"rtt_p50_ms\"", "\"rtt_p95_ms\"", "\"rtt_max_ms\"",
                          "\"send_fail\"", "\"ring_overflow\"", "\"channel\"",
                          "\"dist_m\"", "\"tx\":1000", "\"rx\":1000"}) {
    TEST_ASSERT_TRUE(result.find(key) != std::string::npos);
  }
  // First progress line shape.
  const std::string& prog = pingSink.lines.at(1);
  TEST_ASSERT_TRUE(prog.find("\"t\":\"stats\"") != std::string::npos);
  TEST_ASSERT_TRUE(prog.find("\"seq\":100") != std::string::npos);
  TEST_ASSERT_TRUE(prog.find("\"rtt_last\"") != std::string::npos);
  // Cross-check: the ponger saw and echoed everything.
  const std::string pongFinal = pongSink.lines.back();
  TEST_ASSERT_TRUE(pongFinal.find("\"t\":\"pong_final\"") != std::string::npos);
  TEST_ASSERT_TRUE(pongFinal.find("\"rx\":1000") != std::string::npos);
  TEST_ASSERT_TRUE(pongFinal.find("\"tx\":1000") != std::string::npos);
}

static void test_loss_injection_fails() {
  MockTransport pingSide, pongSide;
  pingSide.setDropEvery(50);  // every 50th PING dies -> 20 lost
  SinkCapture pingSink, pongSink;
  flr::PingApp ping(pingSide, &pingSink, capture);
  flr::PongApp pong(pongSide, &pongSink, capture);
  wirePair(pingSide, pongSide, ping, pong);
  ping.begin(0); pong.begin(0);
  runExchange(pingSide, pongSide, ping, pong, 130000);

  const std::string result = pingSink.lines.back();
  TEST_ASSERT_TRUE(result.find("\"t\":\"m1_result\"") != std::string::npos);
  TEST_ASSERT_TRUE(result.find("\"loss\":20") != std::string::npos);
  TEST_ASSERT_TRUE(result.find("\"verdict\":\"FAIL\"") != std::string::npos);
}

static void test_late_duplicate_pong_ignored() {
  MockTransport pingSide, pongSide;
  SinkCapture pingSink, pongSink;
  flr::PingApp ping(pingSide, &pingSink, capture);
  flr::PongApp pong(pongSide, &pongSink, capture);
  wirePair(pingSide, pongSide, ping, pong);
  ping.begin(0); pong.begin(0);
  runExchange(pingSide, pongSide, ping, pong, 130000);
  TEST_ASSERT_TRUE(ping.done());

  const size_t linesBefore = pingSink.lines.size();
  const std::string resultBefore = pingSink.lines.back();
  // Deliver a stale duplicate PONG (seq 5 was already seen) as if the
  // radio handed it over late; then run loop ticks again.
  uint8_t stale[13];
  flr::m1Encode(stale, flr::M1_TYPE_PONG, 5, 0);
  pingSide.inject(stale, sizeof(stale));
  for (uint32_t now = 120000; now < 150000; now += 10) {
    ping.tick(now);
    pingSide.pump();
  }
  TEST_ASSERT_EQUAL_UINT32(linesBefore, pingSink.lines.size());  // no new line
  TEST_ASSERT_TRUE(pingSink.lines.back() == resultBefore);      // result intact
  TEST_ASSERT_TRUE(resultBefore.find("\"rx\":1000") != std::string::npos);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_exchange_zero_loss);
  RUN_TEST(test_loss_injection_fails);
  RUN_TEST(test_late_duplicate_pong_ignored);
  return UNITY_END();
}
