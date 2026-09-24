#include <unity.h>
#include <vector>
#include "core/fragmenter.h"

struct Cap {
  std::vector<uint8_t> idx, cnt;
  std::vector<std::vector<uint8_t>> slices;
};
static void sinkFn(void* ctx, uint8_t i, uint8_t c, const uint8_t* d, size_t n) {
  Cap* cap = static_cast<Cap*>(ctx);
  cap->idx.push_back(i);
  cap->cnt.push_back(c);
  cap->slices.emplace_back(d, d + n);
}

static void test_single_fragment() {
  uint8_t f[246];
  for (size_t i = 0; i < 246; ++i) f[i] = (uint8_t)i;
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(1, flr::fragSplit(f, 246, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.idx.size());
  TEST_ASSERT_EQUAL_UINT8(0, cap.idx[0]);
  TEST_ASSERT_EQUAL_UINT8(1, cap.cnt[0]);
  TEST_ASSERT_EQUAL_UINT32(246, (uint32_t)cap.slices[0].size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f, cap.slices[0].data(), 246);
}

static void test_ceil_math_and_tail() {
  uint8_t f[247];
  for (size_t i = 0; i < 247; ++i) f[i] = (uint8_t)(i ^ 0x5A);
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(2, flr::fragSplit(f, 247, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT32(246, (uint32_t)cap.slices[0].size());  // full
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)cap.slices[1].size());    // tail
  TEST_ASSERT_EQUAL_UINT8(f[246], cap.slices[1][0]);
}

static void test_max_l3_frame_espnow_and_flrc() {
  static uint8_t f[flr::L3_MAX];
  for (size_t i = 0; i < flr::L3_MAX; ++i) f[i] = (uint8_t)(i * 3);
  Cap cap;
  TEST_ASSERT_EQUAL_UINT8(3, flr::fragSplit(f, 512, sinkFn, &cap, 246));
  TEST_ASSERT_EQUAL_UINT8(246 & 0xFF, (uint8_t)cap.slices[0].size());
  TEST_ASSERT_EQUAL_UINT8(246 & 0xFF, (uint8_t)cap.slices[1].size());
  TEST_ASSERT_EQUAL_UINT8(20, (uint8_t)cap.slices[2].size());
  Cap cap2;
  TEST_ASSERT_EQUAL_UINT8(5, flr::fragSplit(f, 512, sinkFn, &cap2, 123));
  TEST_ASSERT_EQUAL_UINT8(20, (uint8_t)cap2.slices[4].size());
  // ascending idx, constant cnt
  for (uint8_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_UINT8(i, cap2.idx[i]);
    TEST_ASSERT_EQUAL_UINT8(5, cap2.cnt[i]);
  }
}

static void test_invalid_inputs() {
  uint8_t f[4] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(nullptr, 4, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 0, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 513, sinkFn, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 4, nullptr, nullptr, 246));
  TEST_ASSERT_EQUAL_UINT8(0, flr::fragSplit(f, 4, sinkFn, nullptr, 0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_single_fragment);
  RUN_TEST(test_ceil_math_and_tail);
  RUN_TEST(test_max_l3_frame_espnow_and_flrc);
  RUN_TEST(test_invalid_inputs);
  return UNITY_END();
}
