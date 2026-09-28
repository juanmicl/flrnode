#include <unity.h>
#include <string.h>
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
  // Decode emitted bytes as RNS-format HDLC frames (raw payloads).
  // sideB selects the far-end data output; CTRL replies emerge on the
  // REQUESTING side (local handling).
  std::vector<std::vector<uint8_t>> decodeOut(bool sideB = true) {
    ByteCap& cap = sideB ? b.out : a.out;
    std::vector<std::vector<uint8_t>> r;
    struct C { std::vector<std::vector<uint8_t>>* r; } c{&r};
    flr::HdlcDeframer d;
    d.begin([](void* ctx, const uint8_t* p, size_t n) {
      auto* rr = static_cast<C*>(ctx)->r;
      rr->emplace_back(p, p + n);
    }, &c);
    for (uint8_t b : cap.bytes) d.feed(b);
    return r;
  }
  // Node-ref overload so tests can say decodeOut(p.a) / decodeOut(p.b).
  std::vector<std::vector<uint8_t>> decodeOut(const Node& side) {
    return decodeOut(&side == &b);
  }
};

static void feedHdlc(Pair& p, const uint8_t* payload, size_t n) {
  std::vector<uint8_t> wire(1040);
  const size_t w = flr::hdlcFrame(payload, n, wire.data(), wire.size());
  TEST_ASSERT_TRUE(w > 0);
  for (size_t i = 0; i < w; ++i) p.a.app.onSerialByte(wire[i], p.curT);
}

static void test_data_crosses_end_to_end() {
  Pair p; p.init();
  const uint8_t msg[13] = {'h','e','l','l','o',' ','f','l','r','n','o','d','e'};
  feedHdlc(p, msg, sizeof(msg));
  p.run(0, 80);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT32(13, (uint32_t)frames[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, frames[0].data(), 13);
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
  TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)frames[0].size());
  TEST_ASSERT_EQUAL_UINT32(300, (uint32_t)frames[1].size());
  TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)frames[2].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f2, frames[1].data(), 300);
}

static void checkStatsReply(Pair& p) {
  auto frames = p.decodeOut(false);   // CTRL reply: requesting side
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  const char* keys[14] = {"tx_frames","tx_frags","tx_retries","tx_dropped",
    "tx_too_big","tx_window_overflow","meta_conflict","rx_frags",
    "rx_delivered","rx_dups","rx_stale","rx_window_overflow",
    "nak_sent","ack_sent"};
  for (int i = 0; i < 14; ++i)
    TEST_ASSERT_TRUE(s.find(keys[i]) != std::string::npos);
  TEST_ASSERT_TRUE(s.find("{\"stats\":{") == 0);
}

static void test_ctrl_stats_reply_has_14_keys() {
  Pair p; p.init();
  const uint8_t cmd[15] = {'{','"','c','m','d','"',':','"','s','t','a','t','s','"','}'};
  feedHdlc(p, cmd, sizeof(cmd));      // no trailing '\n'
  p.run(0, 40);
  checkStatsReply(p);
}

static void test_ctrl_stats_with_trailing_newline() {
  Pair p; p.init();
  const uint8_t cmd[16] = {'{','"','c','m','d','"',':','"','s','t','a','t','s','"','}','\n'};
  feedHdlc(p, cmd, sizeof(cmd));      // '\n'-terminated request form
  p.run(0, 40);
  checkStatsReply(p);
}

static void test_ctrl_unknown_err() {
  Pair p; p.init();
  const uint8_t cmd[14] = {'{','"','c','m','d','"',':','"','b','o','g','u','s','}'};
  feedHdlc(p, cmd, sizeof(cmd));
  p.run(0, 40);
  auto frames = p.decodeOut(false);   // CTRL reply: requesting side
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("unknown_cmd") != std::string::npos);
}

static void test_binary_jsonlike_data_is_not_ctrl() {
  // A 96-byte binary payload opening with { " is DATA (heuristic upper
  // bound): it must CROSS via ARQ, not be answered locally.
  Pair p; p.init();
  static uint8_t big[96];
  big[0] = '{'; big[1] = '"';
  for (int i = 2; i < 96; ++i) big[i] = (uint8_t)(i * 5);
  feedHdlc(p, big, sizeof(big));
  p.run(0, 200);
  auto frames = p.decodeOut();        // far end: crossed as DATA
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(big, frames[0].data(), 96);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)p.decodeOut(false).size());
}

static void test_oversize_data_rejected() {
  Pair p; p.init();
  std::vector<uint8_t> wire;
  wire.push_back(0x7E);
  for (int i = 0; i < 513; ++i) wire.push_back(0x55);
  wire.push_back(0x7E);               // 513 B payload: L3 too big
  for (uint8_t b : wire) p.a.app.onSerialByte(b, p.curT);
  p.run(0, 60);
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)frames.size());  // rejected, no crossing
}

static void test_output_chunking() {
  Pair p; p.init();
  static uint8_t f[flr::L3_MAX];
  feedHdlc(p, f, flr::L3_MAX);          // up to ~1030 B stuffed wire frame
  p.step(1);                            // crossing + reassembly + delivery
  size_t b0 = p.b.out.bytes.size();
  p.step(2);                            // first drain tick: <=256 B out
  const size_t first = p.b.out.bytes.size() - b0;
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, (uint32_t)first);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(256, (uint32_t)first);
  p.run(3, 40);                         // drains over several ticks
  auto frames = p.decodeOut();
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT32(512, (uint32_t)frames[0].size());
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
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, frames[0].data(), 400);
}

// ---- M3 CTRL get/set (Task 2) ----

static void feedCtrlRaw(Pair& p, const uint8_t* payload, size_t n) {
  std::vector<uint8_t> wire(540);
  const size_t w = flr::hdlcFrame(payload, n, wire.data(), wire.size());
  for (size_t i = 0; i < w; ++i) p.a.app.onSerialByte(wire[i], p.curT);
}

static void test_ctrl_get_channel() {
  Pair p; p.init();
  const uint8_t cmd[] = {'{','"','c','m','d','"',':','"','g','e','t','"',',',
                         '"','k','e','y','"',':','"','c','h','a','n','n','e','l','"','}'};
  feedCtrlRaw(p, cmd, sizeof(cmd));
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"channel\":1") != std::string::npos);
}

static void test_ctrl_set_channel_ok_and_bad_value() {
  Pair p; p.init();
  const uint8_t ok[] = {'{','"','c','m','d','"',':','"','s','e','t','"',',','"','k','e','y','"',':',
                        '"','c','h','a','n','n','e','l','"',',','"','v','a','l','"',':','6','}'};
  feedCtrlRaw(p, ok, sizeof(ok));
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"ok\":true") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT8(6, p.a.app.config().channel);

  const uint8_t bad[] = {'{','"','c','m','d','"',':','"','s','e','t','"',',','"','k','e','y','"',':',
                         '"','c','h','a','n','n','e','l','"',',','"','v','a','l','"',':','1','4','}'};
  feedCtrlRaw(p, bad, sizeof(bad));
  p.run(40, 80);
  frames = p.decodeOut(p.a);
  std::string s2(frames.back().begin(), frames.back().end());
  TEST_ASSERT_TRUE(s2.find("\"err\":\"bad_value\"") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT8(6, p.a.app.config().channel);  // unchanged
}

static void test_ctrl_set_tx_power_clamps() {
  Pair p; p.init();
  const uint8_t cmd[] = {'{','"','c','m','d','"',':','"','s','e','t','"',',','"','k','e','y','"',':',
                         '"','t','x','_','p','o','w','e','r','"',',','"','v','a','l','"',':','3','0','}'};
  feedCtrlRaw(p, cmd, sizeof(cmd));
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"ok\":true") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT8(17, p.a.app.config().txPower);  // clamped
}

static void test_ctrl_pmk_write_only_and_rate() {
  Pair p; p.init();
  const uint8_t g[] = {'{','"','c','m','d','"',':','"','g','e','t','"',',',
                       '"','k','e','y','"',':','"','p','m','k','"','}'};
  feedCtrlRaw(p, g, sizeof(g));
  const uint8_t sr[] = {'{','"','c','m','d','"',':','"','s','e','t','"',',','"','k','e','y','"',':',
                        '"','r','a','t','e','"',',','"','v','a','l','"',':','5','0','0','}'};
  feedCtrlRaw(p, sr, sizeof(sr));
  p.run(0, 60);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)frames.size());
  std::string g1(frames[0].begin(), frames[0].end());
  std::string g2(frames[1].begin(), frames[1].end());
  TEST_ASSERT_TRUE(g1.find("\"err\":\"write_only\"") != std::string::npos);
  TEST_ASSERT_TRUE(g2.find("\"err\":\"unsupported_transport\"") != std::string::npos);
}

static void test_ctrl_unknown_key() {
  Pair p; p.init();
  const uint8_t cmd[] = {'{','"','c','m','d','"',':','"','g','e','t','"',',',
                         '"','k','e','y','"',':','"','n','o','p','e','"','}'};
  feedCtrlRaw(p, cmd, sizeof(cmd));
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"err\":\"unknown_key\"") != std::string::npos);
}

static void test_classifier_rejects_binary_ish_prefix() {
  Pair p; p.init();
  // A "frame" starting with {" but containing non-printable bytes must
  // cross as DATA (be delivered to the radio path), not swallowed as CTRL.
  uint8_t fake[20] = {'{','"', 0x01, 0x02, 0xFF, 0x10, 0x00, 0x7F, 0x01};
  std::vector<uint8_t> wire(540);
  const size_t w = flr::hdlcFrame(fake, 9, wire.data(), wire.size());
  for (size_t i = 0; i < w; ++i) p.a.app.onSerialByte(wire[i], p.curT);
  p.run(0, 40);
  auto frames = p.decodeOut(p.b);   // arrives at B via the radio path
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  TEST_ASSERT_EQUAL_UINT32(9, (uint32_t)frames[0].size());
  TEST_ASSERT_EQUAL_UINT8('{', frames[0][0]);
}

// ---- Review-fix wave 1: full-length pmk, effective values, validation ----

static void feedCtrlStr(Pair& p, const char* cmd) {
  feedCtrlRaw(p, (const uint8_t*)cmd, strlen(cmd));
}

static void test_ctrl_full_length_pmk_set_ok() {
  Pair p; p.init();
  feedCtrlStr(p,
      "{\"cmd\":\"set\",\"key\":\"pmk\",\"val\":"
      "\"00112233445566778899AABBCCDDEEFF\"}");   // 66 B: truncation killer
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"ok\":true") != std::string::npos);
  const uint8_t expect[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
      0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expect, p.a.app.config().pmk, 16);
}

static void test_ctrl_full_length_pmk_malformed() {
  Pair p; p.init();
  feedCtrlStr(p,
      "{\"cmd\":\"set\",\"key\":\"pmk\",\"val\":"
      "\"00112233445566778899AABBCCDDEEFG\"}");   // 32 chars, 'G' not hex
  feedCtrlStr(p,
      "{\"cmd\":\"set\",\"key\":\"pmk\",\"val\":"
      "\"00112233445566778899AABBCCDDEEF\"}");    // 31 hex chars
  p.run(0, 60);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)frames.size());
  for (int i = 0; i < 2; ++i) {
    std::string s(frames[i].begin(), frames[i].end());
    TEST_ASSERT_TRUE(s.find("\"err\":\"bad_value\"") != std::string::npos);
  }
}

static void test_ctrl_tx_power_effective_value() {
  Pair p; p.init();
  feedCtrlStr(p, "{\"cmd\":\"set\",\"key\":\"tx_power\",\"val\":30}");
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("\"ok\":true") != std::string::npos);
  TEST_ASSERT_TRUE(s.find("\"effective\":17") != std::string::npos);
}

static void test_ctrl_val_form_validation() {
  Pair p; p.init();
  feedCtrlStr(p, "{\"cmd\":\"set\",\"key\":\"tx_power\",\"val\":abc}");
  feedCtrlStr(p, "{\"cmd\":\"set\",\"key\":\"channel\",\"val\":6x}");
  p.run(0, 60);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)frames.size());
  for (int i = 0; i < 2; ++i) {
    std::string s(frames[i].begin(), frames[i].end());
    TEST_ASSERT_TRUE(s.find("\"err\":\"bad_value\"") != std::string::npos);
  }
  TEST_ASSERT_EQUAL_UINT8(17, p.a.app.config().txPower);  // unchanged
  TEST_ASSERT_EQUAL_UINT8(1, p.a.app.config().channel);   // unchanged
}

static void test_ctrl_get_node_id() {
  Pair p; p.init();
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  p.a.app.setNodeId(mac);
  feedCtrlStr(p, "{\"cmd\":\"get\",\"key\":\"node_id\"}");
  p.run(0, 40);
  auto frames = p.decodeOut(p.a);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)frames.size());
  std::string s(frames[0].begin(), frames[0].end());
  TEST_ASSERT_TRUE(s.find("{\"node_id\":\"AABBCCDDEEFF\"}") == 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_data_crosses_end_to_end);
  RUN_TEST(test_multi_frame_in_order);
  RUN_TEST(test_ctrl_stats_reply_has_14_keys);
  RUN_TEST(test_ctrl_stats_with_trailing_newline);
  RUN_TEST(test_ctrl_unknown_err);
  RUN_TEST(test_binary_jsonlike_data_is_not_ctrl);
  RUN_TEST(test_oversize_data_rejected);
  RUN_TEST(test_output_chunking);
  RUN_TEST(test_lossy_recovery_through_bridge);
  RUN_TEST(test_ctrl_get_channel);
  RUN_TEST(test_ctrl_set_channel_ok_and_bad_value);
  RUN_TEST(test_ctrl_set_tx_power_clamps);
  RUN_TEST(test_ctrl_pmk_write_only_and_rate);
  RUN_TEST(test_ctrl_unknown_key);
  RUN_TEST(test_classifier_rejects_binary_ish_prefix);
  RUN_TEST(test_ctrl_full_length_pmk_set_ok);
  RUN_TEST(test_ctrl_full_length_pmk_malformed);
  RUN_TEST(test_ctrl_tx_power_effective_value);
  RUN_TEST(test_ctrl_val_form_validation);
  RUN_TEST(test_ctrl_get_node_id);
  return UNITY_END();
}
