#include <unity.h>
#include <thread>
#include "core/ring_buffer.h"

using flr::RingBuffer;
using flr::RingFrame;

static void test_push_pop_fifo_order() {
  RingBuffer rb;
  const uint8_t payload[3] = {1, 2, 3};
  TEST_ASSERT_TRUE(rb.push(payload, 3, -60));
  RingFrame f;
  TEST_ASSERT_TRUE(rb.pop(&f));
  TEST_ASSERT_EQUAL_UINT32(3, f.len);
  TEST_ASSERT_EQUAL_UINT8(1, f.data[0]);
  TEST_ASSERT_EQUAL_INT16(-60, f.rssi);
  TEST_ASSERT_FALSE(rb.pop(&f));
}

static void test_overflow_counted_and_dropped() {
  RingBuffer rb;
  const uint8_t payload[4] = {9, 9, 9, 9};
  for (uint32_t i = 0; i < 8; ++i) TEST_ASSERT_TRUE(rb.push(payload, 4, 0));
  TEST_ASSERT_FALSE(rb.push(payload, 4, 0));
  TEST_ASSERT_EQUAL_UINT32(1, rb.overflowCount());
  RingFrame f;
  uint32_t popped = 0;
  while (rb.pop(&f)) ++popped;
  TEST_ASSERT_EQUAL_UINT32(8, popped);
}

static void test_wraparound_preserves_order() {
  RingBuffer rb;
  uint8_t payload[8];
  for (uint8_t round = 0; round < 10; ++round) {
    for (uint8_t i = 0; i < 8; ++i) {
      payload[0] = round; payload[1] = i;
      TEST_ASSERT_TRUE(rb.push(payload, 2, 0));
      RingFrame f;
      TEST_ASSERT_TRUE(rb.pop(&f));
      TEST_ASSERT_EQUAL_UINT8(round, f.data[0]);
      TEST_ASSERT_EQUAL_UINT8(i, f.data[1]);
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0, rb.overflowCount());
}

static void test_spsc_threads() {
  RingBuffer rb;
  constexpr uint32_t kN = 10000;
  std::thread producer([&] {
    uint8_t buf[16];
    for (uint32_t i = 0; i < kN; ++i) {
      buf[0] = (uint8_t)(i & 0xFF); buf[1] = (uint8_t)(i >> 8);
      while (!rb.push(buf, 2, -42)) {}
    }
  });
  uint32_t got = 0;
  RingFrame f;
  while (got < kN) {
    if (rb.pop(&f)) {
      TEST_ASSERT_EQUAL_UINT8(got & 0xFF, f.data[0]);
      TEST_ASSERT_EQUAL_UINT8((got >> 8) & 0xFF, f.data[1]);
      TEST_ASSERT_EQUAL_INT16(-42, f.rssi);
      ++got;
    }
  }
  producer.join();
  // NOTE: overflowCount() counts every failed push (in production that is a
  // dropped radio frame), and this test's producer retries on a full ring,
  // so it may be > 0 here. Loss-freedom is proven by the ordered delivery
  // above; assert only that the ring drained completely.
  TEST_ASSERT_FALSE(rb.pop(&f));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_push_pop_fifo_order);
  RUN_TEST(test_overflow_counted_and_dropped);
  RUN_TEST(test_wraparound_preserves_order);
  RUN_TEST(test_spsc_threads);
  return UNITY_END();
}
