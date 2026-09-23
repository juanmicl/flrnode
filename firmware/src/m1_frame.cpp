#include "m1_frame.h"

namespace flr {

bool m1Encode(uint8_t out[M1_FRAME_LEN], uint8_t type, uint32_t seq,
              uint32_t ts) {
  if (type != M1_TYPE_PING && type != M1_TYPE_PONG) return false;
  out[0] = 'F'; out[1] = 'L'; out[2] = 'R'; out[3] = '1';
  out[4] = type;
  out[5] = (uint8_t)(seq >> 24); out[6] = (uint8_t)(seq >> 16);
  out[7] = (uint8_t)(seq >> 8);  out[8] = (uint8_t)seq;
  out[9] = (uint8_t)(ts >> 24);  out[10] = (uint8_t)(ts >> 16);
  out[11] = (uint8_t)(ts >> 8);  out[12] = (uint8_t)ts;
  return true;
}

bool m1Decode(const uint8_t* in, size_t len, M1Decoded* out) {
  if (len != M1_FRAME_LEN) return false;
  if (in[0] != 'F' || in[1] != 'L' || in[2] != 'R' || in[3] != '1')
    return false;
  if (in[4] != M1_TYPE_PING && in[4] != M1_TYPE_PONG) return false;
  out->type = in[4];
  out->seq = ((uint32_t)in[5] << 24) | ((uint32_t)in[6] << 16) |
             ((uint32_t)in[7] << 8) | (uint32_t)in[8];
  out->ts = ((uint32_t)in[9] << 24) | ((uint32_t)in[10] << 16) |
            ((uint32_t)in[11] << 8) | (uint32_t)in[12];
  return true;
}

}  // namespace flr
