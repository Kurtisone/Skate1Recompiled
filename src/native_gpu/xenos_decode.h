// skate1 - native D3D layer: decoding of the Xenos state the game's D3D keeps
// in its device shadow (see docs/native_d3d.md). Header-only, shared by the
// in-game renderer and tools/native_replay.

#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace xenos {

inline uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline float LoadBEFloat(const uint8_t* p) {
  const uint32_t value = LoadBE32(p);
  float f;
  std::memcpy(&f, &value, 4);
  return f;
}

// Device shadow layout (XDK 2.0.5632 D3D device, guest 0x40002200).
namespace device {
constexpr uint32_t kFetchConstants = 0x480;       // 32 x 6 dwords (texture), = 96 x 2 (vertex)
constexpr uint32_t kVertexConstants = 0x780;      // 256 float4
constexpr uint32_t kPixelConstants = 0x1780;      // 256 float4
constexpr uint32_t kVertexBoolConstants = 0x2780; // 4 dwords
constexpr uint32_t kPixelBoolConstants = 0x2790;  // 4 dwords
constexpr uint32_t kIndexBuffer = 12428;
constexpr uint32_t kPixelShader = 12684;
constexpr uint32_t kVertexShader = 12688;
}  // namespace device

// Register groups as the draw flush (sub_82CCD390) lays them out: register
// first + k is at device offset + 4k.
struct RegisterGroup {
  uint32_t first;
  uint32_t offset;
  uint32_t count;
};
constexpr RegisterGroup kRegisterGroups[] = {
    {0x2000, 10368, 19}, {0x2100, 10444, 21}, {0x2180, 10528, 5}, {0x2200, 10548, 12},
    {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 32},
};

// Returns the device offset of a register, or 0 if the shadow does not hold it.
constexpr uint32_t RegisterOffset(uint32_t reg) {
  for (const RegisterGroup& group : kRegisterGroups) {
    if (reg >= group.first && reg < group.first + group.count) {
      return group.offset + 4 * (reg - group.first);
    }
  }
  return 0;
}

namespace reg {
constexpr uint32_t RB_SURFACE_INFO = 0x2000;
constexpr uint32_t RB_COLOR_INFO = 0x2001;
constexpr uint32_t RB_DEPTH_INFO = 0x2002;
constexpr uint32_t RB_COLOR_MASK = 0x2104;
constexpr uint32_t RB_ALPHA_REF = 0x210E;
constexpr uint32_t PA_CL_VPORT_XSCALE = 0x210F;
constexpr uint32_t PA_CL_VPORT_XOFFSET = 0x2110;
constexpr uint32_t PA_CL_VPORT_YSCALE = 0x2111;
constexpr uint32_t PA_CL_VPORT_YOFFSET = 0x2112;
constexpr uint32_t PA_CL_VPORT_ZSCALE = 0x2113;
constexpr uint32_t PA_CL_VPORT_ZOFFSET = 0x2114;
constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
constexpr uint32_t RB_COLORCONTROL = 0x2202;
constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
}  // namespace reg

// A view of a device shadow snapshot (big-endian guest bytes).
struct DeviceState {
  const uint8_t* data;

  uint32_t U32(uint32_t offset) const { return LoadBE32(data + offset); }
  uint32_t Reg(uint32_t r) const { return U32(RegisterOffset(r)); }
  float RegF(uint32_t r) const { return LoadBEFloat(data + RegisterOffset(r)); }
};

// Vertex fetch constant (2 dwords): dword 0 = type | address, dword 1 =
// endian | size in dwords.
struct VertexFetch {
  uint32_t address;  // physical
  uint32_t size;     // bytes
  uint32_t endian;   // 0 none, 1 8in16, 2 8in32, 3 16in32
};

inline VertexFetch DecodeVertexFetch(const DeviceState& state, uint32_t slot) {
  const uint32_t dword0 = state.U32(device::kFetchConstants + 8 * slot);
  const uint32_t dword1 = state.U32(device::kFetchConstants + 8 * slot + 4);
  return {dword0 & 0xFFFFFFFC, (dword1 & 0x03FFFFFC), dword1 & 3};
}

// A vfetch instruction of the microcode (3 big-endian dwords).
struct VertexFetchInstruction {
  bool is_vfetch;
  uint32_t fetch_slot;  // vertex fetch constant 0..95
  uint32_t format;      // VertexFormat
  bool is_signed;
  bool is_integer;      // num_format_all: 0 = fraction (normalized), 1 = integer
  bool is_mini;         // shares the stride and slot of the previous full fetch
  uint32_t stride;      // dwords
  int32_t offset;       // dwords
  int32_t exp_adjust;   // the fetched value is scaled by 2^exp_adjust
};

inline VertexFetchInstruction DecodeVertexFetchInstruction(const uint8_t* instruction) {
  const uint32_t w0 = LoadBE32(instruction);
  const uint32_t w1 = LoadBE32(instruction + 4);
  const uint32_t w2 = LoadBE32(instruction + 8);
  VertexFetchInstruction result{};
  result.is_vfetch = (w0 & 0x1F) == 0;
  result.fetch_slot = ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3);
  result.is_signed = (w1 >> 12) & 1;
  result.is_integer = (w1 >> 13) & 1;
  result.format = (w1 >> 16) & 0x3F;
  result.is_mini = (w1 >> 30) & 1;
  result.exp_adjust = int32_t(w1 << 2) >> 26;  // bits 24..29, signed
  result.stride = w2 & 0xFF;
  result.offset = int32_t(w2 << 1) >> 9;  // bits 8..30, sign-extended
  return result;
}

enum VertexFormat : uint32_t {
  k_8_8_8_8 = 6,
  k_2_10_10_10 = 7,
  k_10_11_11 = 16,
  k_11_11_10 = 17,
  k_16_16 = 25,
  k_16_16_16_16 = 26,
  k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32,
  k_32 = 33,
  k_32_32 = 34,
  k_32_32_32_32 = 35,
  k_32_FLOAT = 36,
  k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38,
  k_32_32_32_FLOAT = 57,
};

// Swaps a buffer of guest data in place to host order for the given endian
// mode; afterwards each 32-bit word holds its components LSB first.
inline void SwapEndian(uint8_t* data, size_t size, uint32_t endian) {
  switch (endian) {
    case 1:  // 8in16
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case 2:  // 8in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case 3:  // 16in32
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

// Texture fetch constant (6 dwords, xe_gpu_texture_fetch_t in the SDK).
struct TextureFetch {
  bool valid;         // type == texture
  uint32_t format;    // TextureFormat
  uint32_t endian;
  bool tiled;
  uint32_t pitch;     // texels
  uint32_t address;   // physical, base level
  uint32_t dimension; // 0 1D, 1 2D, 2 3D, 3 cube
  uint32_t width, height, depth;
  uint32_t swizzle;   // 4 x 3 bits: 0-3 component, 4 zero, 5 one
  uint32_t clamp[3];  // ClampMode
  uint32_t mag_filter, min_filter, mip_filter;
  bool is_signed;     // all components signed
  uint32_t mip_address;  // physical, levels >= 1 (0 = none)
  uint32_t mip_min_level, mip_max_level;
  bool packed_mips;   // levels of 16 texels or less share one tile
};

inline TextureFetch DecodeTextureFetch(const DeviceState& state, uint32_t slot) {
  uint32_t d[6];
  for (uint32_t i = 0; i < 6; ++i) d[i] = state.U32(device::kFetchConstants + 24 * slot + 4 * i);
  TextureFetch t{};
  t.valid = (d[0] & 3) == 2;
  t.is_signed = ((d[0] >> 2) & 0xFF) == 0x55;  // TextureSign::kSigned on xyzw
  t.clamp[0] = (d[0] >> 10) & 7;
  t.clamp[1] = (d[0] >> 13) & 7;
  t.clamp[2] = (d[0] >> 16) & 7;
  t.pitch = ((d[0] >> 22) & 0x1FF) << 5;
  t.tiled = d[0] >> 31;
  t.format = d[1] & 0x3F;
  t.endian = (d[1] >> 6) & 3;
  t.address = d[1] & 0xFFFFF000;
  t.dimension = (d[5] >> 9) & 3;
  if (t.dimension == 2) {
    t.width = (d[2] & 0x7FF) + 1;
    t.height = ((d[2] >> 11) & 0x7FF) + 1;
    t.depth = ((d[2] >> 22) & 0x3FF) + 1;
  } else {
    t.width = (d[2] & 0x1FFF) + 1;
    t.height = ((d[2] >> 13) & 0x1FFF) + 1;
    t.depth = t.dimension == 3 ? 6 : 1;
  }
  t.swizzle = (d[3] >> 1) & 0xFFF;
  t.mag_filter = (d[3] >> 19) & 3;
  t.min_filter = (d[3] >> 21) & 3;
  t.mip_filter = (d[3] >> 23) & 3;
  t.mip_min_level = (d[4] >> 2) & 0xF;
  t.mip_max_level = (d[4] >> 6) & 0xF;
  t.packed_mips = (d[5] >> 11) & 1;
  t.mip_address = d[5] & 0xFFFFF000;
  return t;
}

enum TextureFormat : uint32_t {
  kTex_8 = 2,
  kTex_8_8_8_8 = 6,
  kTex_8_8 = 10,
  kTex_DXT1 = 18,
  kTex_DXT2_3 = 19,
  kTex_DXT4_5 = 20,
  kTex_16_16 = 25,
  kTex_16_16_16_16 = 26,
  kTex_16_16_FLOAT = 31,
  kTex_16_16_16_16_FLOAT = 32,
  kTex_32_FLOAT = 36,
};

// Block size (texels) and bytes per block of a texture format, 0 bytes if
// unsupported.
struct TextureBlock {
  uint32_t size;
  uint32_t bytes;
};

inline TextureBlock GetTextureBlock(uint32_t format) {
  switch (format) {
    case kTex_8: return {1, 1};
    case kTex_8_8: return {1, 2};
    case kTex_8_8_8_8: case kTex_16_16: case kTex_16_16_FLOAT: case kTex_32_FLOAT: return {1, 4};
    case kTex_16_16_16_16: case kTex_16_16_16_16_FLOAT: return {1, 8};
    case kTex_DXT1: return {4, 8};
    case kTex_DXT2_3: case kTex_DXT4_5: return {4, 16};
    default: return {1, 0};
  }
}

// Offset of block (x, y) in a tiled 2D surface (pitch in blocks), from the
// SDK's texture util (Xenia, after UModel).
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Bytes of guest memory a texture's base level spans (tiled slices padded to
// 32x32-block tiles and 4 KB), for write watching.
inline uint32_t TextureByteSize(const TextureFetch& t) {
  const TextureBlock block = GetTextureBlock(t.format);
  const uint32_t bytes = block.bytes ? block.bytes : 4;
  const uint32_t pitch_blocks = (std::max(t.pitch, t.width) + block.size - 1) / block.size;
  const uint32_t blocks_y = (t.height + block.size - 1) / block.size;
  const uint32_t slice = (((pitch_blocks + 31) & ~31u) * ((blocks_y + 31) & ~31u) * bytes + 4095) & ~4095u;
  const uint32_t slices = t.dimension == 2 ? ((t.depth + 3) & ~3u) : t.depth;
  return slice * std::max(slices, 1u);
}

inline uint32_t Log2Ceil(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) ++r;
  return r;
}
inline uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }

// Where one mip level of a 2D or cube texture is in guest memory (the SDK's
// GetGuestTextureLayout and GetPackedMipOffset, from Xenia).
struct MipLevel {
  uint32_t address;       // physical address of the level (or of its packed tail)
  uint32_t pitch_blocks;  // row pitch in blocks
  uint32_t slice_bytes;   // stride between cube faces
  uint32_t x_blocks, y_blocks;  // offset inside a packed tail
  uint32_t width, height;
};

// Offset of packed level `mip` in its tail, in blocks.
inline void PackedMipOffset(uint32_t width, uint32_t height, uint32_t block_size, uint32_t mip,
                            uint32_t& x_blocks, uint32_t& y_blocks) {
  const uint32_t log2_width = Log2Ceil(width), log2_height = Log2Ceil(height);
  const uint32_t log2_size = std::min(log2_width, log2_height);
  const uint32_t packed_base = log2_size > 4 ? log2_size - 4 : 0;
  const uint32_t packed = mip - packed_base;
  x_blocks = y_blocks = 0;
  if (packed < 3) {
    (log2_width > log2_height ? y_blocks : x_blocks) = 16 >> packed;
  } else {
    if (log2_width > log2_height) {
      x_blocks = (1u << (log2_width - packed_base)) >> (packed - 2);
    } else {
      y_blocks = (1u << (log2_height - packed_base)) >> (packed - 2);
    }
  }
  x_blocks /= block_size;
  y_blocks /= block_size;
}

// The levels the host texture gets (base first), empty if unsupported.
inline std::vector<MipLevel> GetMipLevels(const TextureFetch& t) {
  std::vector<MipLevel> levels;
  const TextureBlock block = GetTextureBlock(t.format);
  if (!block.bytes) return levels;
  const uint32_t max_for_size = Log2Ceil(std::max(t.width, t.height) + 1) - 1;
  uint32_t max_level = t.mip_address ? std::min(t.mip_max_level, max_for_size) : 0;
  const uint32_t packed_level =
      t.packed_mips ? (Log2Ceil(std::min(t.width, t.height)) > 4 ? Log2Ceil(std::min(t.width, t.height)) - 4 : 0)
                    : UINT32_MAX;
  auto align = [](uint32_t v, uint32_t a) { return (v + a - 1) / a * a; };
  for (uint32_t level = 0; level <= max_level; ++level) {
    MipLevel m{};
    m.width = std::max(t.width >> level, 1u);
    m.height = std::max(t.height >> level, 1u);
    // Levels from the packed one on live in its tail.
    const uint32_t stored = std::min(level, packed_level);
    uint32_t pitch_texels, rows_texels;
    if (stored == 0) {
      pitch_texels = std::max(t.pitch, t.width);
      rows_texels = t.height;
    } else {
      pitch_texels = std::max(NextPow2(t.width) >> stored, 1u);
      rows_texels = std::max(NextPow2(t.height) >> stored, 1u);
    }
    m.pitch_blocks = align(align(pitch_texels, block.size) / block.size, 32);
    uint32_t row_bytes = m.pitch_blocks * block.bytes;
    if (!t.tiled && stored != 0) row_bytes = align(row_bytes, 256);
    if (!t.tiled) m.pitch_blocks = row_bytes / block.bytes;
    const uint32_t rows = align(align(rows_texels, block.size) / block.size, 32);
    m.slice_bytes = align(row_bytes * rows, 4096);
    if (stored == 0) {
      m.address = t.address;
    } else {
      // Mips follow one another from the mip address, each face by face.
      uint32_t offset = 0;
      for (uint32_t l = 1; l < stored; ++l) {
        const uint32_t p = align(align(std::max(NextPow2(t.width) >> l, 1u), block.size) / block.size, 32);
        uint32_t rb = p * block.bytes;
        if (!t.tiled) rb = align(rb, 256);
        const uint32_t r = align(align(std::max(NextPow2(t.height) >> l, 1u), block.size) / block.size, 32);
        offset += align(rb * r, 4096) * (t.dimension == 3 ? 6 : 1);
      }
      m.address = t.mip_address + offset;
    }
    if (level >= packed_level) {
      PackedMipOffset(t.width, t.height, block.size, level, m.x_blocks, m.y_blocks);
    }
    levels.push_back(m);
  }
  return levels;
}

// Copies one level of slice `slice` into a linear host layout, in host byte
// order. Returns the row pitch in bytes.
inline uint32_t UntileLevel(const uint8_t* physical, const TextureFetch& t, const MipLevel& m,
                            uint32_t slice, uint8_t* out) {
  const TextureBlock block = GetTextureBlock(t.format);
  uint32_t log2 = 0;
  while ((1u << log2) < block.bytes) ++log2;
  const uint32_t blocks_x = (m.width + block.size - 1) / block.size;
  const uint32_t blocks_y = (m.height + block.size - 1) / block.size;
  const uint32_t row_bytes = blocks_x * block.bytes;
  const uint8_t* src = physical + (m.address & 0x1FFFFFFF) + size_t(slice) * m.slice_bytes;
  for (uint32_t y = 0; y < blocks_y; ++y) {
    uint8_t* row = out + size_t(y) * row_bytes;
    for (uint32_t x = 0; x < blocks_x; ++x) {
      const uint32_t sx = x + m.x_blocks, sy = y + m.y_blocks;
      const size_t offset = t.tiled ? size_t(TiledOffset2D(int32_t(sx), int32_t(sy), m.pitch_blocks, log2))
                                    : size_t(sy) * m.pitch_blocks * block.bytes + size_t(sx) * block.bytes;
      std::memcpy(row + x * block.bytes, src + offset, block.bytes);
    }
  }
  SwapEndian(out, size_t(row_bytes) * blocks_y, t.endian);
  return row_bytes;
}

// Offset of block (x, y, z) in a tiled 3D volume (pitch and height in
// blocks), from the SDK's texture util (XGRAPHICS::TileVolume).
inline int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                             uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// Copies the base level of a tiled 3D texture into a linear host layout
// (rows, then slices), in host byte order. Returns the row pitch in bytes, or
// 0 if unsupported (block-compressed or linear volumes).
inline uint32_t UntileTexture3D(const uint8_t* physical, const TextureFetch& t, uint8_t* out) {
  const TextureBlock block = GetTextureBlock(t.format);
  if (!block.bytes || block.size != 1 || !t.tiled) return 0;
  uint32_t log2 = 0;
  while ((1u << log2) < block.bytes) ++log2;
  const uint32_t pitch = std::max(t.pitch, t.width);
  const uint32_t row_bytes = t.width * block.bytes;
  const uint8_t* src = physical + (t.address & 0x1FFFFFFF);
  for (uint32_t z = 0; z < t.depth; ++z) {
    for (uint32_t y = 0; y < t.height; ++y) {
      uint8_t* row = out + (size_t(z) * t.height + y) * row_bytes;
      for (uint32_t x = 0; x < t.width; ++x) {
        std::memcpy(row + x * block.bytes,
                    src + TiledOffset3D(int32_t(x), int32_t(y), int32_t(z), pitch, t.height, log2),
                    block.bytes);
      }
    }
  }
  SwapEndian(out, size_t(row_bytes) * t.height * t.depth, t.endian);
  return row_bytes;
}

// Copies the base level of slice `slice` of a texture out of guest memory
// into a linear host layout (rows of blocks), in host byte order. Returns the
// row pitch in bytes, or 0 if the format is not supported.
inline uint32_t UntileTexture(const uint8_t* physical, const TextureFetch& t, uint32_t slice,
                              uint8_t* out) {
  const TextureBlock block = GetTextureBlock(t.format);
  if (!block.bytes) return 0;
  uint32_t log2 = 0;
  while ((1u << log2) < block.bytes) ++log2;
  const uint32_t blocks_x = (t.width + block.size - 1) / block.size;
  const uint32_t blocks_y = (t.height + block.size - 1) / block.size;
  const uint32_t pitch_blocks = std::max(t.pitch, t.width) / block.size;
  const uint32_t row_bytes = blocks_x * block.bytes;
  // Slices of the base level are padded to 32x32-block tiles and 4 KB.
  const uint32_t slice_bytes =
      (((pitch_blocks + 31) & ~31u) * ((blocks_y + 31) & ~31u) * block.bytes + 4095) & ~4095u;
  const uint8_t* src = physical + (t.address & 0x1FFFFFFF) + size_t(slice) * slice_bytes;
  for (uint32_t y = 0; y < blocks_y; ++y) {
    uint8_t* row = out + size_t(y) * row_bytes;
    if (t.tiled) {
      for (uint32_t x = 0; x < blocks_x; ++x) {
        std::memcpy(row + x * block.bytes, src + TiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, log2),
                    block.bytes);
      }
    } else {
      // Linear rows are 256-byte aligned.
      std::memcpy(row, src + size_t(y) * ((pitch_blocks * block.bytes + 255) & ~255u), row_bytes);
    }
  }
  SwapEndian(out, size_t(row_bytes) * blocks_y, t.endian);
  return row_bytes;
}

}  // namespace xenos
