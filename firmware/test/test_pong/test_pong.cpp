#include <unity.h>
#include <string>
#include <vector>
#include "../mock_transport.h"
#include "pong_app.h"
#include "m1_frame.h"

using flr::M1_TYPE_PING;
using flr::M1_TYPE_PONG;

struct SinkCapture { std::vector<std::string> lines; };
// Used by Task 8's ping-app sink assertions; kept here so both suites share
// one capture helper.
[[maybe_unused]] static void capture(void* ctx, const char* line) {
  static_cast<SinkCapture*>(ctx)->lines.push_back(line);
}
static void toPong(void* c, const uint8_t* d, size_t n, int16_t r) {
  static_cast<flr::PongApp*>(c)->onFrame(d, n, r);
}

static void test_pong_echoes_valid_ping() {
  MockTransport pongSide, pingSide;
  pongSide.wire(&pingSide);
  pingSide.wire(&pongSide);
  flr::PongApp pong(pongSide, nullptr, nullptr);
  pong.begin(0);
  pongSide.onFrame(toPong, &pong);

  uint8_t f[13];
  flr::m1Encode(f, M1_TYPE_PING, 42, 7777);
  TEST_ASSERT_TRUE(pingSide.send(f, sizeof(f)));
  pongSide.pump();
  TEST_ASSERT_EQUAL_UINT32(1, pongSide.stats().rx);
  pingSide.pump();
  TEST_ASSERT_EQUAL_UINT32(1, pingSide.stats().rx);  // PONG came back
}

static void test_pong_ignores_garbage() {
  MockTransport pongSide, pingSide;
  pongSide.wire(&pingSide);
  pingSide.wire(&pongSide);
  flr::PongApp pong(pongSide, nullptr, nullptr);
  pong.begin(0);
  pongSide.onFrame(toPong, &pong);

  const uint8_t junk[13] = {0};
  pingSide.send(junk, sizeof(junk));               // bad magic
  uint8_t pongFrame[13];
  flr::m1Encode(pongFrame, M1_TYPE_PONG, 1, 1);    // wrong direction
  pingSide.send(pongFrame, sizeof(pongFrame));
  pongSide.pump();
  pingSide.pump();
  TEST_ASSERT_EQUAL_UINT32(2, pongSide.stats().rx);  // received both
  TEST_ASSERT_EQUAL_UINT32(0, pingSide.stats().rx);  // echoed nothing
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pong_echoes_valid_ping);
  RUN_TEST(test_pong_ignores_garbage);
  return UNITY_END();
}
