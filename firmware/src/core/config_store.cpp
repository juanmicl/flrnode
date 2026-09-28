#include "core/config_store.h"
#include <string.h>

namespace flr {

void ConfigStore::save(const Config& c, uint8_t out[kImage]) {
  out[0] = kMagic;
  out[1] = c.channel;
  out[2] = c.txPower;
  out[3] = (uint8_t)(c.statsPeriod >> 8);
  out[4] = (uint8_t)c.statsPeriod;
  for (int i = 0; i < 16; ++i) out[5 + i] = c.pmk[i];
  uint16_t sum = 0;
  for (size_t i = 0; i < kImage - 2; ++i) sum = (uint16_t)(sum + out[i]);
  out[kImage - 2] = (uint8_t)(sum >> 8);
  out[kImage - 1] = (uint8_t)sum;
}

bool ConfigStore::load(const uint8_t* in, size_t n, Config* out) {
  const Config dflt{};
  if (n < kImage || in[0] != kMagic) { *out = dflt; return false; }
  uint16_t sum = 0;
  for (size_t i = 0; i < kImage - 2; ++i) sum = (uint16_t)(sum + in[i]);
  const uint16_t stored = (uint16_t)((in[kImage - 2] << 8) | in[kImage - 1]);
  if (sum != stored) { *out = dflt; return false; }
  Config c;
  c.channel = in[1];
  c.txPower = in[2];
  c.statsPeriod = (uint16_t)((in[3] << 8) | in[4]);
  memcpy(c.pmk, in + 5, 16);
  // Clamp every field (belt-and-braces: load path re-clamps even though
  // set clamps too — bit-rot is caught probabilistically, not certainly).
  // Channel snaps to the NEAR bound (14 -> 13, 0 -> 1).
  if (c.channel < kMinChannel) c.channel = kMinChannel;
  else if (c.channel > kMaxChannel) c.channel = kMaxChannel;
  if (c.txPower > kMaxPower) c.txPower = kMaxPower;
  if (c.statsPeriod == 0 || c.statsPeriod > 3600) c.statsPeriod = 5;
  *out = c;
  return true;
}

}  // namespace flr
