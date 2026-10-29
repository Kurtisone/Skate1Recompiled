// skate1 - native D3D layer: decoding of the Xenos state the game's D3D keeps
// in its device shadow (see docs/native_d3d.md). Header-only, shared by the
// in-game renderer and tools/native_replay.

#pragma once

#include <cstdint>
#include <cstring>
#include <utility>

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

}  // namespace xenos
