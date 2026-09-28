#include <unity.h>
#include <string.h>
#include "core/config_store.h"

using flr::Config;
using flr::ConfigStore;

static void test_defaults_match_spec() {
  Config c;
  TEST_ASSERT_EQUAL_UINT8(1, c.channel);
  TEST_ASSERT_EQUAL_UINT8(17, c.txPower);
  TEST_ASSERT_EQUAL_UINT16(5, c.statsPeriod);
}

static void test_roundtrip() {
  ConfigStore s;
  Config in;
  in.channel = 6; in.txPower = 14; in.statsPeriod = 2;
  memset(in.pmk, 0xAB, 16);
  uint8_t img[24];
  s.save(in, img);
  Config out;
  TEST_ASSERT_TRUE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT8(6, out.channel);
  TEST_ASSERT_EQUAL_UINT8(14, out.txPower);
  TEST_ASSERT_EQUAL_UINT16(2, out.statsPeriod);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(in.pmk, out.pmk, 16);
}

static void test_clamp_on_load() {
  ConfigStore s;
  Config in;
  in.txPower = 30;   // over cap -> clamped at load
  in.channel = 14;   // out of EU domain -> clamped to 13
  uint8_t img[24];
  s.save(in, img);
  Config out;
  TEST_ASSERT_TRUE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT8(17, out.txPower);
  TEST_ASSERT_EQUAL_UINT8(13, out.channel);
}

static void test_corrupt_magic_defaults() {
  ConfigStore s;
  uint8_t img[24] = {0};
  img[0] = 0x00;  // bad magic
  Config out;
  TEST_ASSERT_FALSE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT8(1, out.channel);       // defaults
  TEST_ASSERT_EQUAL_UINT8(17, out.txPower);
}

static void test_corrupt_checksum_defaults() {
  ConfigStore s;
  Config in;
  in.channel = 6;
  uint8_t img[24];
  s.save(in, img);
  img[23] ^= 0xFF;  // flip last checksum byte
  Config out;
  TEST_ASSERT_FALSE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT8(1, out.channel);
}

static void test_short_input_defaults() {
  ConfigStore s;
  uint8_t img[10] = {0};
  Config out;
  TEST_ASSERT_FALSE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT8(17, out.txPower);
}

static void test_save_is_deterministic() {
  ConfigStore s;
  Config in;
  in.channel = 6;
  uint8_t a[24], b[24];
  // poison both buffers differently so uninitialised bytes would show
  memset(a, 0xAA, sizeof(a)); memset(b, 0x55, sizeof(b));
  s.save(in, a); s.save(in, b);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, 24);
}

static void test_stats_period_clamp_to_default() {
  ConfigStore s;
  Config in;
  in.statsPeriod = 7200;   // > 3600 -> default 5
  uint8_t img[24];
  s.save(in, img);
  Config out;
  TEST_ASSERT_TRUE(s.load(img, sizeof(img), &out));
  TEST_ASSERT_EQUAL_UINT16(5, out.statsPeriod);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_match_spec);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_clamp_on_load);
  RUN_TEST(test_corrupt_magic_defaults);
  RUN_TEST(test_corrupt_checksum_defaults);
  RUN_TEST(test_short_input_defaults);
  RUN_TEST(test_save_is_deterministic);
  RUN_TEST(test_stats_period_clamp_to_default);
  return UNITY_END();
}
