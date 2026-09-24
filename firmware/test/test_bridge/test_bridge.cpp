#include <unity.h>
#include <string>
#include <vector>
#include "../mock_transport.h"
#include "bridge_app.h"
#include "core/hdlc.h"

struct ByteCap { std::vector<uint8_t> bytes; };
static void byteSink(void* ctx, const uint8_t* d, size_t n) {
  auto* c = static_cast<ByteCap*>(ctx);
  c->bytes.insert(c->bytes.end(), d, d + n);
}

struct Node {
  MockTransport t;
  ByteCap out;
  flr::BridgeApp app;
  Node() : app(t, &out, byteSink) {}  // member-init: t and out first
};

struct Pair {
  Node a, b;
  uint32_t curT = 0;
  void init() {
    a.t.wire(&b.t);
    b.t.wire(&a.t);
    a.app.begin(0);
    b.app.begin(0);
  }
  void step(uint32_t now) {
    curT = now;
    a.app.tick(now);
    b.app.tick(now);
    a.t.pump();
    b.t.pump();
  }
  void run(uint32_t from, uint32_t to, uint32_t dt = 5) {
    for (uint32_t t = from; t < to; t += dt) step(t);
  }
  // Decode emitted bytes as HDLC frames. sideB selects the far-end data
  // output; CTRL replies emerge on the REQUESTING side (local handling).
  std::vector<std::pair<uint8_t, std::vector<uint8_t>>> decodeOut(
      bool sideB = true) {
    ByteCap& cap = sideB ? b.out : a.out;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> r;
    struct C { std::vector<std::pair<uint8_t, std::vector<uint8_t>>>* r; } c{&r};
    flr::HdlcDeframer d;
    d.begin([](void* ctx, uint8_t ty, const uint8_t* p, size_t n) {
      auto* rr = static_cast<C*>(ctx)->r;
      rr->emplace_back(ty, std::vector<uint8_t>(p, p + n));
    }, &c);
    for (uint8_t b : cap.bytes) d.feed(b);
    return r;
  }
};

static void feedHdlc(Pair& p, const uint8_t* payload, size_t n,
                     uint8_t innerType = 0x01) {
  std::vector<uint8_t> wire(540);
  const size_t w = flr::hdlcFrame(innerType, payload, n, wire.data(),
                                  wire.size());
  for (size_t i = 0; i < w; ++i) p.a.app.onSerialByte(wire[i], p.curT);
}

static void test_data_crosses_end_to_end() {
  Pair p; p.init();
  const uint8_t msg[13] = {'h','e','l','l','o',' ','f','l','r','n','o','d','e'};
  feedHdlc(p, msg, sizeof(msg));
  p.run(0, 80);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[0].first);
  TEST_ASSERT_EQUAL_UINT32(13, (uint32_t)frames[0].second.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, frames[0].second.data(), 13);
}

static void test_multi_frame_in_order() {
  Pair p; p.init();
  uint8_t f1[5] = {1, 1, 1, 1, 1};
  uint8_t f2[300];
  for (int i = 0; i < 300; ++i) f2[i] = (uint8_t)i;
  feedHdlc(p, f1, sizeof(f1));
  feedHdlc(p, f2, sizeof(f2));
  feedHdlc(p, f1, sizeof(f1));
  p.run(0, 300);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)frames[0].second.size());
  TEST_ASSERT_EQUAL_UINT32(300, (uint32_t)frames[1].second.size());
  TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)frames[2].second.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f2, frames[1].second.data(), 300);
}

static void test_ctrl_stats_reply_has_14_keys() {
  Pair p; p.init();
  const uint8_t cmd[16] = {'{','"','c','m','d','"',':','"','s','t','a','t','s','"','}','\n'};
  feedHdlc(p, cmd, sizeof(cmd), 0x02);
  p.run(0, 40);
  auto frames = p.decodeOut(false);   // CTRL reply: requesting side
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, frames[0].first);
  std::string s(frames[0].second.begin(), frames[0].second.end());
  const char* keys[14] = {"tx_frames","tx_frags","tx_retries","tx_dropped",
    "tx_too_big","tx_window_overflow","meta_conflict","rx_frags",
    "rx_delivered","rx_dups","rx_stale","rx_window_overflow",
    "nak_sent","ack_sent"};
  for (int i = 0; i < 14; ++i)
    TEST_ASSERT_TRUE(s.find(keys[i]) != std::string::npos);
  TEST_ASSERT_TRUE(s.find("{\"stats\":{") == 0);
}

static void test_ctrl_unknown_err() {
  Pair p; p.init();
  const uint8_t cmd[14] = {'{','"','c','m','d','"',':','"','b','o','g','u','s','}'};
  feedHdlc(p, cmd, sizeof(cmd), 0x02);
  p.run(0, 40);
  auto frames = p.decodeOut(false);   // CTRL reply: requesting side
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, frames[0].first);
  std::string s(frames[0].second.begin(), frames[0].second.end());
  TEST_ASSERT_TRUE(s.find("unknown_cmd") != std::string::npos);
}

static void test_oversize_data_dropped_by_deframer() {
  Pair p; p.init();
  static uint8_t big[513];
  feedHdlc(p, big, sizeof(big));        // deframer cap: resync, no frame
  p.run(0, 60);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)frames.size());
}

static void test_output_chunking() {
  Pair p; p.init();
  static uint8_t f[flr::L3_MAX];
  feedHdlc(p, f, flr::L3_MAX);          // ~1030 B stuffed wire frame
  p.step(1);                            // crossing + reassembly + delivery
  size_t b0 = p.b.out.bytes.size();
  p.step(2);                            // first drain tick: <=256 B out
  const size_t first = p.b.out.bytes.size() - b0;
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, (uint32_t)first);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(256, (uint32_t)first);
  p.run(3, 40);                         // drains over several ticks
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)frames[0].second.size());
}

static void test_lossy_recovery_through_bridge() {
  Pair p; p.init();
  p.a.t.setDropEvery(4);
  static uint8_t f[400];
  for (int i = 0; i < 400; ++i) f[i] = (uint8_t)(i * 3);
  feedHdlc(p, f, 400);
  p.run(0, 2000);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, frames[0].second.data(), 400);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_data_crosses_end_to_end);
  RUN_TEST(test_multi_frame_in_order);
  RUN_TEST(test_ctrl_stats_reply_has_14_keys);
  RUN_TEST(test_ctrl_unknown_err);
  RUN_TEST(test_oversize_data_dropped_by_deframer);
  RUN_TEST(test_output_chunking);
  RUN_TEST(test_lossy_recovery_through_bridge);
  return UNITY_END();
}
