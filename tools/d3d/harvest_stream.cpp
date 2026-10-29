// Extract the XDK shader containers from Skate's world stream (BAM.xsf: RW4
// chunks, each RefPack-compressed and 128-byte aligned), which the Python
// harvester is too slow for: harvest_stream <file.xsf> <outdir>.
// Build: c++ -O2 -std=c++17 tools/d3d/harvest_stream.cpp -o harvest_stream
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

static uint32_t be32(const uint8_t* p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static std::vector<uint8_t> refpack(const uint8_t* src, size_t n) {
  size_t pos = 2;
  const int size_bytes = (src[0] & 0x80) ? 4 : 3;
  if (src[0] & 1) pos += size_bytes;
  size_t out_size = 0;
  for (int i = 0; i < size_bytes; ++i) out_size = out_size << 8 | src[pos++];
  std::vector<uint8_t> out(out_size);
  size_t o = 0;
  while (pos < n) {
    const uint8_t b0 = src[pos];
    size_t plain, copy = 0, dist = 0;
    if (b0 < 0x80) {
      plain = b0 & 3; copy = ((b0 >> 2) & 7) + 3;
      dist = ((b0 & 0x60) << 3) + src[pos + 1] + 1; pos += 2;
    } else if (b0 < 0xC0) {
      plain = src[pos + 1] >> 6; copy = (b0 & 0x3F) + 4;
      dist = ((src[pos + 1] & 0x3F) << 8) + src[pos + 2] + 1; pos += 3;
    } else if (b0 < 0xE0) {
      plain = b0 & 3; copy = ((b0 & 0x0C) << 6) + src[pos + 3] + 5;
      dist = ((b0 & 0x10) << 12) + (src[pos + 1] << 8) + src[pos + 2] + 1; pos += 4;
    } else if (b0 < 0xFC) {
      plain = ((b0 & 0x1F) << 2) + 4; pos += 1;
    } else {
      plain = b0 & 3; pos += 1;
    }
    if (o + plain + copy > out_size || pos + plain > n || dist > o + plain) {
      out.resize(o);
      return out;  // corrupt or not a chunk
    }
    memcpy(&out[o], src + pos, plain);
    o += plain; pos += plain;
    for (size_t i = 0; i < copy; ++i, ++o) out[o] = out[o - dist];
    if (b0 >= 0xFC) break;
  }
  out.resize(o);
  return out;
}

int main(int argc, char** argv) {
  if (argc != 3) { fprintf(stderr, "usage: harvest_stream <file> <outdir>\n"); return 1; }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
  std::set<size_t> seen;
  size_t chunks = 0, found = 0;
  for (size_t i = 0; i + 10 < file.size(); i += 128) {
    if (file[i] != 0x10 || file[i + 1] != 0xFB || memcmp(&file[i + 6], "\x89RW4", 4)) continue;
    ++chunks;
    const auto blob = refpack(&file[i], file.size() - i);
    for (size_t j = 0; j + 0x24 <= blob.size(); ++j) {
      const uint8_t* c = &blob[j];
      if (c[0] != 0x10 || c[1] != 0x2A || c[2] != 0x11 || (c[3] & 0xFE)) continue;
      const size_t size = size_t(be32(c + 4)) + be32(c + 8);
      if (be32(c + 12) != 0 || be32(c + 16) != 0x24 || !size || size > blob.size() - j) continue;
      const size_t h = std::hash<std::string>()(std::string((const char*)c, size));
      if (seen.insert(h).second) {
        char name[64];
        snprintf(name, sizeof(name), "/%016zx_%s.bin", h, (c[3] & 1) ? "vs" : "ps");
        std::ofstream(std::string(argv[2]) + name, std::ios::binary).write((const char*)c, size);
        ++found;
      }
      j += size - 1;
    }
  }
  printf("%s: %zu chunks, %zu new containers\n", argv[1], chunks, found);
}
