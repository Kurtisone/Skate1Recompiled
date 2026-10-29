// Prints the XenosRecomp microcode key (XENOS_RECOMP_MICROCODE_KEY) of each
// shader container file: shader_keys <file>...
// Build: c++ -O2 -std=c++17 -Ithird_party/rexglue-sdk/thirdparty/xxHash tools/d3d/shader_keys.cpp -o shader_keys
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::ifstream in(argv[i], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    if (d.size() < 0x24) continue;
    const uint32_t shader = be32(&d[0x18]);
    const uint32_t microcode = be32(&d[4]) + be32(&d[shader]);
    const uint32_t size = be32(&d[shader + 4]);
    if (microcode + size > d.size()) continue;
    std::printf("%016llX %s\n", (unsigned long long)XXH3_64bits(&d[microcode], size), argv[i]);
  }
}
