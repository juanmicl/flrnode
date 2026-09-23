#include <unity.h>
#include <fstream>
#include <stdio.h>
#include <string>
#include <vector>
#include "core/hdlc.h"

using flr::hdlcFrame;

// Resolve the golden dir from this source file location, never from cwd.
static std::string goldenDir() {
  std::string f = __FILE__;
  const size_t slash = f.find_last_of('/');
  return f.substr(0, slash) + "/golden";
}

static bool loadVector(const char* name, std::vector<uint8_t>* out) {
  const std::string path = goldenDir() + "/" + name;
  std::ifstream in(path);
  if (!in) {
    printf("MISSING GOLDEN FILE: %s\n", path.c_str());
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    out->push_back((uint8_t)strtol(line.c_str(), nullptr, 16));
  }
  return true;
}

static void checkAgainstGolden(const char* name, uint8_t type,
                               const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> expected;
  if (!loadVector(name, &expected)) { TEST_FAIL_MESSAGE("golden file missing"); return; }
  uint8_t wire[540];
  const size_t wlen = hdlcFrame(type, payload.data(), payload.size(), wire,
                                sizeof(wire));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)expected.size(), (uint32_t)wlen);
  if (wlen == expected.size())
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), wire, wlen);
}

static void test_p01_empty_data() {
  checkAgainstGolden("p01_empty_data.txt", 0x01, std::vector<uint8_t>());
}
static void test_p02_stuffing() {
  checkAgainstGolden("p02_stuffing.txt", 0x01,
                     {0x7E, 0x7D, 0x11, 0x22});
}
static void test_p04_hello() {
  std::vector<uint8_t> p{'h','e','l','l','o',' ','f','l','r','n','o','d','e'};
  checkAgainstGolden("p04_hello.txt", 0x01, p);
}
static void test_ctrl_stats() {
  // The generator frames the payload WITH the trailing newline.
  std::vector<uint8_t> p{'{','"','c','m','d','"',':','"','s','t','a','t','s','"','}','\n'};
  checkAgainstGolden("ctrl_stats.txt", 0x02, p);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_p01_empty_data);
  RUN_TEST(test_p02_stuffing);
  RUN_TEST(test_p04_hello);
  RUN_TEST(test_ctrl_stats);
  return UNITY_END();
}
