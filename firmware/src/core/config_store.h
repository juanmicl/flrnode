#pragma once
#include <stddef.h>
#include <stdint.h>

namespace flr {

struct Config {
  uint8_t channel = 1;
  uint8_t txPower = 17;      // dBm conducted; valid [0, 17] (EIRP basis)
  uint16_t statsPeriod = 5;  // seconds
  uint8_t pmk[16] = {0};     // zeros => compiled dev key
};

// Pure serialise/parse/clamp for the persistent config image. No
// Arduino includes; the caller (main) performs actual EEPROM I/O.
class ConfigStore {
 public:
  static constexpr uint8_t kMagic = 0xC7;
  static constexpr size_t kImage = 24;
  static constexpr uint8_t kMaxPower = 17;      // dBm conducted
  static constexpr uint8_t kMinChannel = 1, kMaxChannel = 13;

  // Serialise into out[kImage]. Context: any. Blocking: O(1). Failure: none.
  void save(const Config& c, uint8_t out[kImage]);

  // Parse + clamp. Context: any. Blocking: O(1). Failure: false on
  // short input / bad magic / bad checksum; *out = defaults.
  bool load(const uint8_t* in, size_t n, Config* out);
};

}  // namespace flr
