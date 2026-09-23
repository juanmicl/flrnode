#include <unity.h>
#include "m1_frame.h"

using flr::M1_FRAME_LEN;
using flr::m1Decode;
using flr::m1Encode;

static void test_encode_big_endian() {
  uint8_t f[13];
  TEST_ASSERT_TRUE(m1Encode(f, flr::M1_TYPE_PING, 0x11223344u, 0xAABBCCDDu));
  TEST_ASSERT_EQUAL_UINT8('F', f[0]); TEST_ASSERT_EQUAL_UINT8('L', f[1]);
  TEST_ASSERT_EQUAL_UINT8('R', f[2]); TEST_ASSERT_EQUAL_UINT8('1', f[3]);
  TEST_ASSERT_EQUAL_UINT8(1, f[4]);
  TEST_ASSERT_EQUAL_UINT8(0x11, f[5]); TEST_ASSERT_EQUAL_UINT8(0x22, f[6]);
  TEST_ASSERT_EQUAL_UINT8(0x33, f[7]); TEST_ASSERT_EQUAL_UINT8(0x44, f[8]);
  TEST_ASSERT_EQUAL_UINT8(0xAA, f[9]); TEST_ASSERT_EQUAL_UINT8(0xBB, f[10]);
  TEST_ASSERT_EQUAL_UINT8(0xCC, f[11]); TEST_ASSERT_EQUAL_UINT8(0xDD, f[12]);
}

static void test_roundtrip() {
  uint8_t f[13];
  m1Encode(f, flr::M1_TYPE_PONG, 999, 123456);
  flr::M1Decoded d;
  TEST_ASSERT_TRUE(m1Decode(f, M1_FRAME_LEN, &d));
  TEST_ASSERT_EQUAL_UINT8(flr::M1_TYPE_PONG, d.type);
  TEST_ASSERT_EQUAL_UINT32(999, d.seq);
  TEST_ASSERT_EQUAL_UINT32(123456, d.ts);
}

static void test_rejects_bad_input() {
  uint8_t f[13];
  m1Encode(f, flr::M1_TYPE_PING, 1, 2);
  flr::M1Decoded d;
  f[0] = 'X'; TEST_ASSERT_FALSE(m1Decode(f, 13, &d));  // bad magic
  f[0] = 'F';
  TEST_ASSERT_FALSE(m1Decode(f, 12, &d));              // short
  TEST_ASSERT_FALSE(m1Decode(f, 14, &d));              // long
  f[4] = 7;    TEST_ASSERT_FALSE(m1Decode(f, 13, &d)); // bad type
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_big_endian);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_rejects_bad_input);
  return UNITY_END();
}
