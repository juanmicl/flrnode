#include <unity.h>
#include <string.h>
#include "core/l2_pdu.h"

using flr::L2Pdu;

static void test_encode_layout_big_endian() {
  L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.flags = flr::L2_FLAG_RETRY;
  h.seq = 0x4142; h.fragIdx = 1; h.fragCnt = 3;
  const uint8_t p[2] = {0xAA, 0xBB};
  uint8_t out[8];
  const size_t n = flr::l2Encode(h, p, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT8(0x41, out[0]);  // ver=1,ft=0,flags=RETRY
  TEST_ASSERT_EQUAL_UINT8(0x41, out[1]);  // seq hi
  TEST_ASSERT_EQUAL_UINT8(0x42, out[2]);  // seq lo
  TEST_ASSERT_EQUAL_UINT8(0x13, out[3]);  // idx1 cnt3
  TEST_ASSERT_EQUAL_UINT8(0xAA, out[4]);
  TEST_ASSERT_EQUAL_UINT8(0xBB, out[5]);
}

static void test_roundtrip_data() {
  L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.seq = 999; h.fragIdx = 0; h.fragCnt = 1;
  const uint8_t p[5] = {1, 2, 3, 4, 5};
  uint8_t out[16];
  const size_t n = flr::l2Encode(h, p, 5, out, sizeof(out));
  L2Pdu d; const uint8_t* pp; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(out, n, &d, &pp, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_DATA_FRAG, d.ftype);
  TEST_ASSERT_EQUAL_UINT16(999, d.seq);
  TEST_ASSERT_EQUAL_UINT8(0, d.fragIdx);
  TEST_ASSERT_EQUAL_UINT8(1, d.fragCnt);
  TEST_ASSERT_EQUAL_UINT32(5, (uint32_t)pn);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(p, pp, 5);
}

static void test_ack_encoding() {
  uint8_t out[8];
  const size_t n = flr::l2EncodeAck(77, 0xA5, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT8(0x50, out[0]);  // ver=1,ft=1,flags=0
  TEST_ASSERT_EQUAL_UINT8(0, out[1]);
  TEST_ASSERT_EQUAL_UINT8(77, out[2]);
  TEST_ASSERT_EQUAL_UINT8(0xA5, out[3]);
  L2Pdu d; const uint8_t* pp; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(out, n, &d, &pp, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_ACK, d.ftype);
  TEST_ASSERT_EQUAL_UINT16(77, d.seq);
  TEST_ASSERT_EQUAL_UINT8(0xA, d.fragIdx);  // bitmap halves via B3
  TEST_ASSERT_EQUAL_UINT8(0x5, d.fragCnt);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)pn);
}

static void test_nak_encoding_zero_to_four_entries() {
  uint8_t out[24];
  TEST_ASSERT_EQUAL_UINT32(4, (uint32_t)flr::l2EncodeNak(nullptr, nullptr, 0, out, sizeof(out)));
  const uint16_t seqs[2] = {0x0102, 0x0304};
  const uint8_t masks[2] = {0x07, 0x01};
  const size_t n = flr::l2EncodeNak(seqs, masks, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(10, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT8(flr::L2_NAK, (out[0] >> 4) & 0x03);
  TEST_ASSERT_EQUAL_UINT8(0x01, out[4]);
  TEST_ASSERT_EQUAL_UINT8(0x02, out[5]);
  TEST_ASSERT_EQUAL_UINT8(0x07, out[6]);
  TEST_ASSERT_EQUAL_UINT8(0x03, out[7]);
  TEST_ASSERT_EQUAL_UINT8(0x04, out[8]);
  TEST_ASSERT_EQUAL_UINT8(0x01, out[9]);
  L2Pdu d; const uint8_t* pp; size_t pn;
  TEST_ASSERT_TRUE(flr::l2Decode(out, n, &d, &pp, &pn));
  TEST_ASSERT_EQUAL_UINT32(6, (uint32_t)pn);
  TEST_ASSERT_EQUAL_UINT16(0x0102, (uint16_t)((pp[0] << 8) | pp[1]));
  TEST_ASSERT_EQUAL_UINT8(0x07, pp[2]);
  TEST_ASSERT_EQUAL_UINT32(0,
      (uint32_t)flr::l2EncodeNak(seqs, masks, 5, out, sizeof(out)));
}

static void test_decode_rejects_invalid() {
  L2Pdu d; const uint8_t* pp; size_t pn;
  const uint8_t ok[6] = {0x40, 0x01, 0x02, 0x13, 0, 0};  // valid DATA
  TEST_ASSERT_TRUE(flr::l2Decode(ok, 6, &d, &pp, &pn));
  const uint8_t shortPdu[3] = {0x40, 0, 0};
  TEST_ASSERT_FALSE(flr::l2Decode(shortPdu, 3, &d, &pp, &pn));
  uint8_t badVer[6]; memcpy(badVer, ok, 6); badVer[0] = 0x80;  // ver=2
  TEST_ASSERT_FALSE(flr::l2Decode(badVer, 6, &d, &pp, &pn));
  uint8_t badCnt[6]; memcpy(badCnt, ok, 6); badCnt[3] = 0x30;  // cnt=0
  TEST_ASSERT_FALSE(flr::l2Decode(badCnt, 6, &d, &pp, &pn));
  uint8_t badIdx[6]; memcpy(badIdx, ok, 6); badIdx[3] = 0x31;  // idx>=cnt
  TEST_ASSERT_FALSE(flr::l2Decode(badIdx, 6, &d, &pp, &pn));
  uint8_t badSeq[6]; memcpy(badSeq, ok, 6); badSeq[1] = 0; badSeq[2] = 0;
  TEST_ASSERT_FALSE(flr::l2Decode(badSeq, 6, &d, &pp, &pn));  // seq 0
  const uint8_t ctrl[5] = {0x70, 0, 1, 0, 9};  // CTRL decodes, undelivered
  TEST_ASSERT_TRUE(flr::l2Decode(ctrl, 5, &d, &pp, &pn));
  TEST_ASSERT_EQUAL_UINT8(flr::L2_CTRL, d.ftype);
  const uint8_t badNak[6] = {0x60, 0, 1, 0, 1, 2};  // payload 2 != 3*k
  TEST_ASSERT_FALSE(flr::l2Decode(badNak, 6, &d, &pp, &pn));
}

static void test_encode_capacity_guard() {
  L2Pdu h{};
  h.ftype = flr::L2_DATA_FRAG; h.seq = 5; h.fragIdx = 0; h.fragCnt = 1;
  const uint8_t p[4] = {0, 0, 0, 0};
  uint8_t out[7];  // needs 8
  TEST_ASSERT_EQUAL_UINT32(0,
      (uint32_t)flr::l2Encode(h, p, 4, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(0,
      (uint32_t)flr::l2EncodeAck(1, 0, out, 3));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_layout_big_endian);
  RUN_TEST(test_roundtrip_data);
  RUN_TEST(test_ack_encoding);
  RUN_TEST(test_nak_encoding_zero_to_four_entries);
  RUN_TEST(test_decode_rejects_invalid);
  RUN_TEST(test_encode_capacity_guard);
  return UNITY_END();
}
