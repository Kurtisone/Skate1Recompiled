// skate1 - offline replay of a frame capture with the native renderer's
// building blocks (milestone B in docs/native_d3d.md).
//
//   native_replay <capture.sk1c> <shader cache inputs dir> <out.ppm> [first last]
//
// Replays the draws of events [first, last] (default: the scene pass, between
// BeginTiling and EndTiling) into one 1280x720 color target with plume:
// XenosRecomp shaders from the shader cache, the captured constants, vertex
// and index buffers, and for now a white texture for every texture fetch.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <smolv.h>
#include <zstd.h>

#include <plume_render_interface.h>
#include <plume_vulkan.h>

#include "native_gpu/shader_cache.h"
#include "native_gpu/xenos_decode.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace {

constexpr uint32_t kDrawIndexed = 0x82CB38E8;
constexpr uint32_t kDraw = 0x82CB3500;
constexpr uint32_t kBeginTiling = 0x82CC4FF0;
constexpr uint32_t kEndTiling = 0x82CC5578;
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

// ---- capture ---------------------------------------------------------------

struct Blob {
  uint32_t address;
  std::vector<uint8_t> data;
};

struct Event {
  uint32_t function;
  uint32_t regs[8];  // r3..r10
  uint32_t result;
  uint64_t vs_key;
  uint64_t ps_key;
  std::vector<uint8_t> device;
  std::vector<Blob> blobs;

  const Blob* FindBlob(uint32_t address) const {
    for (const Blob& blob : blobs) {
      if (blob.address == address) return &blob;
    }
    return nullptr;
  }
};

std::vector<Event> ReadCapture(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
  auto u32 = [&](size_t pos) {
    uint32_t value;
    std::memcpy(&value, &data[pos], 4);
    return value;
  };
  if (data.size() < 16 || std::memcmp(data.data(), "SK1C", 4) || u32(4) != 1) {
    std::fprintf(stderr, "%s: not a frame capture\n", path.c_str());
    std::exit(1);
  }
  const uint32_t snapshot = u32(12);
  std::vector<Event> events;
  size_t pos = 16;
  while (pos + 4 <= data.size() && !std::memcmp(&data[pos], "EVNT", 4)) {
    Event e;
    e.function = u32(pos + 4);
    std::memcpy(e.regs, &data[pos + 8], 32);
    e.result = u32(pos + 40);
    std::memcpy(&e.vs_key, &data[pos + 48], 8);
    std::memcpy(&e.ps_key, &data[pos + 56], 8);
    const uint32_t blob_count = u32(pos + 64);
    pos += 68;
    e.device.assign(data.begin() + pos, data.begin() + pos + snapshot);
    pos += snapshot;
    for (uint32_t i = 0; i < blob_count; ++i) {
      const uint32_t address = u32(pos), size = u32(pos + 4);
      e.blobs.push_back({address, {data.begin() + pos + 8, data.begin() + pos + 8 + size}});
      pos += 8 + size;
    }
    events.push_back(std::move(e));
  }
  return events;
}

// The physical memory snapshot (<capture>.phys).
const uint8_t* MapPhysical(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY);
  struct stat st {};
  if (fd < 0 || fstat(fd, &st)) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(1);
  }
  return static_cast<const uint8_t*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
}

// ---- shaders ---------------------------------------------------------------

struct VertexElement {
  uint32_t address;  // microcode instruction index of its vfetch
  uint32_t usage;
  uint32_t usage_index;
};

struct ShaderInfo {
  const ShaderCacheEntry* entry = nullptr;
  uint32_t microcode_offset = 0;  // in the physical part
  std::vector<VertexElement> elements;
  std::unique_ptr<RenderShader> shader;
};

// Shader containers by microcode key, from the cache inputs (the vertex
// elements are in the container's virtual part).
std::unordered_map<uint64_t, std::vector<uint8_t>> LoadContainers(const std::string& dir) {
  std::unordered_map<uint64_t, std::vector<uint8_t>> containers;
  for (const auto& file : std::filesystem::directory_iterator(dir)) {
    std::ifstream in(file.path(), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 0x24) continue;
    const uint32_t virtual_size = xenos::LoadBE32(&data[4]);
    const uint32_t shader = xenos::LoadBE32(&data[0x18]);
    const uint32_t microcode = virtual_size + xenos::LoadBE32(&data[shader]);
    const uint32_t size = xenos::LoadBE32(&data[shader + 4]);
    if (microcode + size > data.size()) continue;
    containers[XXH3_64bits(&data[microcode], size)] = std::move(data);
  }
  return containers;
}

constexpr struct {
  uint32_t usage, index, location;
} kUsageLocations[] = {
    // XenosRecomp's USAGE_LOCATIONS.
    {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},  {5, 1, 5},
    {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},  {1, 0, 10}, {10, 1, 11},
    {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
};

int32_t UsageLocation(uint32_t usage, uint32_t index) {
  for (const auto& entry : kUsageLocations) {
    if (entry.usage == usage && entry.index == index) return int32_t(entry.location);
  }
  return -1;
}

// XenosRecomp declares normals, tangents and binormals uint4 and decodes
// them itself: packed 10_11_11 (with SPEC_CONSTANT_R11G11B10_NORMAL) or the
// raw bits of floats.
bool IsRawUsage(uint32_t usage) { return usage == 3 || usage == 6 || usage == 7; }

RenderFormat RawVertexFormat(uint32_t format) {
  using namespace xenos;
  switch (format) {
    case k_10_11_11: return RenderFormat::R32_UINT;
    case k_32_FLOAT: return RenderFormat::R32_UINT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_UINT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_UINT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat VertexFormat(const xenos::VertexFetchInstruction& fetch, uint32_t usage) {
  using namespace xenos;
  const bool s = fetch.is_signed, i = fetch.is_integer;
  switch (fetch.format) {
    case k_8_8_8_8:
      if (usage == 2) {  // BLENDINDICES: uint4 in XenosRecomp
        return s ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      }
      // TODO: integer bytes read as floats (blend weights) need USCALED,
      // which plume does not have; normalized for now.
      (void)i;
      return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case k_16_16:
      return i ? (s ? RenderFormat::R16G16_SINT : RenderFormat::R16G16_UINT)
               : (s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case k_16_16_16_16:
      return i ? (s ? RenderFormat::R16G16B16A16_SINT : RenderFormat::R16G16B16A16_UINT)
               : (s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM);
    case k_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case k_32: return s ? RenderFormat::R32_SINT : RenderFormat::R32_UINT;
    case k_32_32: return s ? RenderFormat::R32G32_SINT : RenderFormat::R32G32_UINT;
    case k_32_32_32_32: return s ? RenderFormat::R32G32B32A32_SINT : RenderFormat::R32G32B32A32_UINT;
    case k_32_FLOAT: return RenderFormat::R32_FLOAT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_FLOAT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_FLOAT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderBlend BlendFactor(uint32_t factor) {
  switch (factor) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    case 12: case 14: return RenderBlend::BLEND_FACTOR;
    case 13: case 15: return RenderBlend::INV_BLEND_FACTOR;
    case 16: return RenderBlend::SRC_ALPHA_SAT;
    default: return RenderBlend::ONE;
  }
}

RenderBlendOperation BlendOp(uint32_t op) {
  switch (op) {
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}

// ---- upload ----------------------------------------------------------------

struct Upload {
  std::unique_ptr<RenderBuffer> buffer;
  uint8_t* memory = nullptr;
  uint64_t address = 0;
  uint64_t offset = 0;
  uint64_t size = 0;

  uint64_t Allocate(const void* data, uint64_t bytes, uint64_t alignment = 256) {
    offset = (offset + alignment - 1) & ~(alignment - 1);
    if (offset + bytes > size) {
      std::fprintf(stderr, "upload buffer full\n");
      std::exit(1);
    }
    const uint64_t at = offset;
    if (data) std::memcpy(memory + at, data, bytes);
    offset += bytes;
    return at;
  }
};

// Guest constants: 32-bit big-endian words to host order.
void CopySwapped32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t value = xenos::LoadBE32(src + i);
    std::memcpy(dst + i, &value, 4);
  }
}

struct SharedConstants {
  uint32_t texture2DIndices[16]{};
  uint32_t texture3DIndices[16]{};
  uint32_t textureCubeIndices[16]{};
  uint32_t samplerIndices[16]{};
  uint32_t booleans{};
  uint32_t swappedTexcoords{};
  float halfPixelOffsetX{};
  float halfPixelOffsetY{};
  float alphaThreshold{};
};

void WritePpm(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height,
              uint32_t row_pitch) {
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      std::fwrite(rgba + y * row_pitch + x * 4, 1, 3, f);
    }
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 && argc != 6) {
    std::fprintf(stderr, "usage: %s <capture.sk1c> <cache inputs dir> <out.ppm> [first last]\n",
                 argv[0]);
    return 1;
  }
  const std::vector<Event> events = ReadCapture(argv[1]);
  const uint8_t* physical = MapPhysical(std::string(argv[1]) + ".phys");
  auto containers = LoadContainers(argv[2]);
  size_t first = 0, last = events.size() - 1;
  if (argc == 6) {
    first = std::stoul(argv[4]);
    last = std::stoul(argv[5]);
  } else {
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i].function == kBeginTiling) first = i;
      if (events[i].function == kEndTiling) last = i;
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("%zu events, %zu containers, replaying %zu..%zu\n", events.size(), containers.size(),
              first, last);

  // SPIR-V cache.
  std::vector<uint8_t> spirv(g_spirvCacheDecompressedSize);
  ZSTD_decompress(spirv.data(), spirv.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize);

  auto interface = CreateVulkanInterface();
  auto device = interface->createDevice();
  auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
  auto commandList = queue->createCommandList();
  auto fence = device->createCommandFence();
  std::printf("device: %s\n", device->getDescription().name.c_str());

  // Bindless heaps and pipeline layout as Unleashed sets them up: three
  // texture heaps (2D, 3D, cube), a sampler heap, 24 bytes of push constants
  // (vertex, pixel and shared constant buffer addresses).
  constexpr uint32_t kTextureHeapSize = 4096;
  RenderDescriptorSetBuilder textureSetBuilder;
  textureSetBuilder.begin();
  textureSetBuilder.addTexture(0, kTextureHeapSize);
  textureSetBuilder.end(true, kTextureHeapSize);
  RenderDescriptorSetBuilder samplerSetBuilder;
  samplerSetBuilder.begin();
  samplerSetBuilder.addSampler(0, 256);
  samplerSetBuilder.end(true, 256);
  auto texture2DSet = textureSetBuilder.create(device.get());
  auto texture3DSet = textureSetBuilder.create(device.get());
  auto textureCubeSet = textureSetBuilder.create(device.get());
  auto samplerSet = samplerSetBuilder.create(device.get());
  RenderPipelineLayoutBuilder layoutBuilder;
  layoutBuilder.begin(false, true);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(samplerSetBuilder);
  layoutBuilder.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layoutBuilder.end();
  auto pipelineLayout = layoutBuilder.create(device.get());

  // White placeholder textures in slot 0 of each heap.
  auto white2D = device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  auto white3D = device->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  RenderTextureDesc cubeDesc = RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
                                                          RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE);
  auto whiteCube = device->createTexture(cubeDesc);
  RenderTextureViewDesc cubeView;
  cubeView.format = RenderFormat::R8G8B8A8_UNORM;
  cubeView.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  cubeView.mipLevels = 1;
  auto whiteCubeView = whiteCube->createTextureView(cubeView);
  auto sampler = device->createSampler(RenderSamplerDesc());
  samplerSet->setSampler(0, sampler.get());

  // Targets and readback.
  auto color = device->createTexture(RenderTextureDesc::ColorTarget(kWidth, kHeight, RenderFormat::R8G8B8A8_UNORM));
  auto depth = device->createTexture(RenderTextureDesc::DepthTarget(kWidth, kHeight, RenderFormat::D32_FLOAT));
  const RenderTexture* colorAttachments[] = {color.get()};
  auto framebuffer = device->createFramebuffer(RenderFramebufferDesc(colorAttachments, 1, depth.get()));
  const uint32_t rowPitch = kWidth * 4;
  auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(rowPitch) * kHeight));

  Upload upload;
  upload.size = 512ull << 20;
  upload.buffer = device->createBuffer(RenderBufferDesc::UploadBuffer(
      upload.size, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  upload.memory = static_cast<uint8_t*>(upload.buffer->map());
  upload.address = upload.buffer->getDeviceAddress();

  // White pixels for the placeholder textures.
  const uint32_t whitePixel = 0xFFFFFFFF;
  const uint64_t whiteOffset = upload.Allocate(&whitePixel, 4, 512);

  commandList->begin();
  RenderTextureBarrier initial[] = {
      {white2D.get(), RenderTextureLayout::COPY_DEST},
      {white3D.get(), RenderTextureLayout::COPY_DEST},
      {whiteCube.get(), RenderTextureLayout::COPY_DEST},
  };
  commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, initial, 3);
  auto copyWhite = [&](RenderTexture* texture, uint32_t arrayIndex) {
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(texture, 0, arrayIndex),
        RenderTextureCopyLocation::PlacedFootprint(upload.buffer.get(), RenderFormat::R8G8B8A8_UNORM,
                                                   1, 1, 1, 64, whiteOffset));
  };
  copyWhite(white2D.get(), 0);
  copyWhite(white3D.get(), 0);
  for (uint32_t face = 0; face < 6; ++face) copyWhite(whiteCube.get(), face);
  RenderTextureBarrier toRead[] = {
      {white2D.get(), RenderTextureLayout::SHADER_READ},
      {white3D.get(), RenderTextureLayout::SHADER_READ},
      {whiteCube.get(), RenderTextureLayout::SHADER_READ},
      {color.get(), RenderTextureLayout::COLOR_WRITE},
      {depth.get(), RenderTextureLayout::DEPTH_WRITE},
  };
  commandList->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, nullptr, 0, toRead, 5);
  texture2DSet->setTexture(0, white2D.get(), RenderTextureLayout::SHADER_READ);
  texture3DSet->setTexture(0, white3D.get(), RenderTextureLayout::SHADER_READ);
  textureCubeSet->setTexture(0, whiteCube.get(), RenderTextureLayout::SHADER_READ, whiteCubeView.get());

  commandList->setFramebuffer(framebuffer.get());
  commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  commandList->clearDepthStencil(true, false, 1.0f, 0);
  commandList->setGraphicsPipelineLayout(pipelineLayout.get());
  commandList->setGraphicsDescriptorSet(texture2DSet.get(), 0);
  commandList->setGraphicsDescriptorSet(texture3DSet.get(), 1);
  commandList->setGraphicsDescriptorSet(textureCubeSet.get(), 2);
  commandList->setGraphicsDescriptorSet(samplerSet.get(), 3);

  std::unordered_map<uint64_t, ShaderInfo> shaders;
  auto getShader = [&](uint64_t key) -> ShaderInfo* {
    auto [it, inserted] = shaders.try_emplace(key);
    ShaderInfo& info = it->second;
    if (inserted) {
      const ShaderCacheEntry* begin = g_shaderCacheEntries;
      const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
      const ShaderCacheEntry* entry = std::lower_bound(
          begin, end, key, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
      if (entry == end || entry->hash != key) return nullptr;
      info.entry = entry;
      // The cache's SPIR-V is smol-v encoded.
      const uint8_t* encoded = &spirv[entry->spirvOffset];
      std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(encoded, entry->spirvSize));
      smolv::Decode(encoded, entry->spirvSize, decoded.data(), decoded.size());
      info.shader = device->createShader(decoded.data(), decoded.size(), "main",
                                         RenderShaderFormat::SPIRV);
      auto container = containers.find(key);
      if (container != containers.end()) {
        const uint8_t* data = container->second.data();
        const uint32_t shader = xenos::LoadBE32(data + 0x18);
        info.microcode_offset = xenos::LoadBE32(data + shader);
        if (xenos::LoadBE32(data) & 1) {
          // VertexShader: field18 at +0x18, element count at +0x1C, then
          // field18 dwords before the elements.
          const uint32_t field18 = xenos::LoadBE32(data + shader + 0x18);
          const uint32_t count = xenos::LoadBE32(data + shader + 0x1C);
          for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = xenos::LoadBE32(data + shader + 0x24 + 4 * (field18 + i));
            info.elements.push_back({value & 0xFFF, (value >> 12) & 0xF, (value >> 16) & 0xF});
          }
        }
      }
    }
    return info.entry ? &info : nullptr;
  };

  std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines;
  std::map<std::string, uint32_t> skipped;
  std::map<std::string, uint32_t> formats;
  uint32_t drawn = 0;

  for (size_t index = first; index <= last && index < events.size(); ++index) {
    const Event& e = events[index];
    if (e.function != kDrawIndexed && e.function != kDraw) continue;
    const xenos::DeviceState state{e.device.data()};
    const uint32_t primitive = e.regs[1];
    RenderPrimitiveTopology topology;
    switch (primitive) {
      case 1: topology = RenderPrimitiveTopology::POINT_LIST; break;
      case 2: topology = RenderPrimitiveTopology::LINE_LIST; break;
      case 3: topology = RenderPrimitiveTopology::LINE_STRIP; break;
      case 4: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
      case 6: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
      default: ++skipped["primitive " + std::to_string(primitive)]; continue;
    }
    ShaderInfo* vs = getShader(e.vs_key);
    ShaderInfo* ps = getShader(e.ps_key);
    if (!vs || !ps) {
      ++skipped["shader not in cache"];
      continue;
    }
    if (vs->elements.empty()) {
      ++skipped["no vertex elements"];
      continue;
    }

    // Vertex input from the patched vfetch instructions.
    // The capture records the vertex shader's physical part first.
    const Blob* microcodeBlob = e.blobs.empty() ? nullptr : &e.blobs[0];
    if (!microcodeBlob) {
      ++skipped["no vertex microcode"];
      continue;
    }
    std::vector<RenderInputElement> inputElements;
    std::vector<RenderInputSlot> inputSlots;
    std::vector<RenderVertexBufferView> vertexViews;
    std::vector<uint32_t> slotFetch;  // fetch constant per input slot
    // 16-bit elements per slot (byte offset, dwords): Xenos keeps the first
    // component in the high half of each dword, so after the 32-bit swap
    // their halves are put back in order.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> halfSwaps;
    bool ok = true;
    uint32_t lastStride = 0, lastSlot = 0;
    bool packedNormals = false;
    for (const VertexElement& element : vs->elements) {
      const size_t at = vs->microcode_offset + element.address * 12;
      if (at + 12 > microcodeBlob->data.size()) {
        ok = false;
        break;
      }
      xenos::VertexFetchInstruction fetch = xenos::DecodeVertexFetchInstruction(&microcodeBlob->data[at]);
      if (!fetch.is_vfetch) {
        static int logged = 0;
        if (logged++ < 3) {
          std::printf("DEBUG vs %016llx microcode +%u blob %zu elements:", (unsigned long long)e.vs_key,
                      vs->microcode_offset, microcodeBlob->data.size());
          for (const VertexElement& el : vs->elements)
            std::printf(" [%u u%u.%u]", el.address, el.usage, el.usage_index);
          std::printf("\n  dwords:");
          for (size_t k = vs->microcode_offset; k < std::min<size_t>(microcodeBlob->data.size(), vs->microcode_offset + 96); k += 4)
            std::printf(" %08x", xenos::LoadBE32(&microcodeBlob->data[k]));
          std::printf("\n");
        }
        ok = false;
        break;
      }
      if (fetch.is_mini) {
        fetch.stride = lastStride;
        fetch.fetch_slot = lastSlot;
      } else {
        lastStride = fetch.stride;
        lastSlot = fetch.fetch_slot;
      }
      const bool raw = IsRawUsage(element.usage);
      const RenderFormat format = raw ? RawVertexFormat(fetch.format) : VertexFormat(fetch, element.usage);
      if (raw && fetch.format == xenos::k_10_11_11) packedNormals = true;
      ++formats["usage " + std::to_string(element.usage) + "." + std::to_string(element.usage_index) +
                " format " + std::to_string(fetch.format) + (fetch.is_signed ? " s" : " u") +
                (fetch.is_integer ? "int" : "norm")];
      const int32_t location = UsageLocation(element.usage, element.usage_index);
      if (format == RenderFormat::UNKNOWN || location < 0) {
        ++skipped[location < 0 ? "element without a location" : "vertex format"];
        continue;
      }
      uint32_t slot = 0;
      while (slot < slotFetch.size() && slotFetch[slot] != fetch.fetch_slot) ++slot;
      if (slot == slotFetch.size()) {
        slotFetch.push_back(fetch.fetch_slot);
        inputSlots.emplace_back(slot, fetch.stride * 4);
        halfSwaps.emplace_back();
      }
      if (fetch.format == xenos::k_16_16 || fetch.format == xenos::k_16_16_FLOAT) {
        halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 1);
      } else if (fetch.format == xenos::k_16_16_16_16 || fetch.format == xenos::k_16_16_16_16_FLOAT) {
        halfSwaps[slot].emplace_back(uint32_t(fetch.offset * 4), 2);
      }
      inputElements.emplace_back("", 0, uint32_t(location), format, slot, uint32_t(fetch.offset * 4));
    }
    if (!ok) {
      ++skipped["vertex fetch decode"];
      continue;
    }
    for (uint32_t slot = 0; slot < slotFetch.size(); ++slot) {
      const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(state, slotFetch[slot]);
      const uint64_t at = upload.Allocate(physical + (fetch.address & 0x1FFFFFFF), fetch.size, 16);
      xenos::SwapEndian(upload.memory + at, fetch.size, fetch.endian);
      const uint32_t stride = inputSlots[slot].stride;
      for (const auto& [offset, dwords] : halfSwaps[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= fetch.size; ++vertex) {
          uint8_t* p = upload.memory + at + vertex * stride + offset;
          for (uint32_t d = 0; d < dwords; ++d, p += 4) {
            std::swap(p[0], p[2]);
            std::swap(p[1], p[3]);
          }
        }
      }
      if (getenv("REPLAY_DEBUG") && drawn < 6) {
        float v[6];
        std::memcpy(v, upload.memory + at, sizeof(v));
        std::printf("DEBUG draw %u slot %u fetch %u addr %08x size %u endian %u stride %u: %g %g %g %g %g %g\n",
                    drawn, slot, slotFetch[slot], fetch.address, fetch.size, fetch.endian,
                    inputSlots[slot].stride, v[0], v[1], v[2], v[3], v[4], v[5]);
        for (const RenderInputElement& el : inputElements)
          if (el.slotIndex == slot)
            std::printf("   loc %u format %d offset %u\n", el.location, int(el.format), el.alignedByteOffset);
      }
      vertexViews.push_back({upload.buffer->at(at), fetch.size});
    }

    // Pipeline state.
    const uint32_t depthControl = state.Reg(xenos::reg::RB_DEPTHCONTROL);
    const uint32_t blendControl = state.Reg(xenos::reg::RB_BLENDCONTROL0);
    const uint32_t colorControl = state.Reg(xenos::reg::RB_COLORCONTROL);
    const uint32_t colorMask = state.Reg(xenos::reg::RB_COLOR_MASK) & 0xF;
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = pipelineLayout.get();
    desc.vertexShader = vs->shader.get();
    desc.pixelShader = ps->shader.get();
    desc.depthEnabled = (depthControl >> 1) & 1;
    desc.depthWriteEnabled = (depthControl >> 2) & 1;
    desc.depthFunction = RenderComparisonFunction(((depthControl >> 4) & 7) + 1);
    desc.depthClipEnabled = true;
    desc.primitiveTopology = topology;
    desc.cullMode = RenderCullMode::NONE;
    desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
    desc.renderTargetCount = 1;
    desc.depthTargetFormat = RenderFormat::D32_FLOAT;
    RenderBlendDesc& blend = desc.renderTargetBlend[0];
    blend.srcBlend = BlendFactor(blendControl & 0x1F);
    blend.blendOp = BlendOp((blendControl >> 5) & 7);
    blend.dstBlend = BlendFactor((blendControl >> 8) & 0x1F);
    blend.srcBlendAlpha = BlendFactor((blendControl >> 16) & 0x1F);
    blend.blendOpAlpha = BlendOp((blendControl >> 21) & 7);
    blend.dstBlendAlpha = BlendFactor((blendControl >> 24) & 0x1F);
    blend.blendEnabled = !(blend.srcBlend == RenderBlend::ONE && blend.dstBlend == RenderBlend::ZERO &&
                           blend.blendOp == RenderBlendOperation::ADD &&
                           blend.srcBlendAlpha == RenderBlend::ONE &&
                           blend.dstBlendAlpha == RenderBlend::ZERO &&
                           blend.blendOpAlpha == RenderBlendOperation::ADD);
    blend.renderTargetWriteMask = uint8_t(colorMask);
    uint32_t specConstants = packedNormals ? 1 : 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
    const bool alphaTest = (colorControl >> 3) & 1;
    if (alphaTest) specConstants |= 2;  // SPEC_CONSTANT_ALPHA_TEST
    RenderSpecConstant spec(0, specConstants);
    desc.specConstants = &spec;
    desc.specConstantsCount = 1;
    desc.inputSlots = inputSlots.data();
    desc.inputSlotsCount = uint32_t(inputSlots.size());
    desc.inputElements = inputElements.data();
    desc.inputElementsCount = uint32_t(inputElements.size());

    uint64_t key = XXH3_64bits(&desc, offsetof(RenderGraphicsPipelineDesc, inputSlots));
    key = XXH3_64bits_withSeed(inputSlots.data(), inputSlots.size() * sizeof(RenderInputSlot), key);
    for (const RenderInputElement& element : inputElements) {
      const uint32_t fields[] = {element.location, uint32_t(element.format), element.slotIndex,
                                 element.alignedByteOffset};
      key = XXH3_64bits_withSeed(fields, sizeof(fields), key);
    }
    key = XXH3_64bits_withSeed(&specConstants, 4, key);
    auto& pipeline = pipelines[key];
    if (!pipeline) {
      pipeline = device->createGraphicsPipeline(desc);
    }
    if (!pipeline) {
      ++skipped["pipeline creation"];
      continue;
    }

    // Constants.
    const uint64_t vsConstants = upload.Allocate(nullptr, 4096);
    CopySwapped32(upload.memory + vsConstants, &e.device[xenos::device::kVertexConstants], 4096);
    const uint64_t psConstants = upload.Allocate(nullptr, 4096);
    CopySwapped32(upload.memory + psConstants, &e.device[xenos::device::kPixelConstants], 4096);
    SharedConstants shared;
    shared.booleans = (state.U32(xenos::device::kVertexBoolConstants) & 0xFF) |
                      ((state.U32(xenos::device::kPixelBoolConstants) & 0xFF) << 16);
    shared.alphaThreshold = state.RegF(xenos::reg::RB_ALPHA_REF);
    const uint64_t sharedConstants = upload.Allocate(&shared, sizeof(shared));
    const uint64_t pushConstants[] = {upload.address + vsConstants, upload.address + psConstants,
                                      upload.address + sharedConstants};

    // Viewport from the D3D viewport transform (y flipped for Vulkan).
    const float sx = state.RegF(xenos::reg::PA_CL_VPORT_XSCALE);
    const float ox = state.RegF(xenos::reg::PA_CL_VPORT_XOFFSET);
    const float sy = state.RegF(xenos::reg::PA_CL_VPORT_YSCALE);
    const float oy = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET);
    const float sz = state.RegF(xenos::reg::PA_CL_VPORT_ZSCALE);
    const float oz = state.RegF(xenos::reg::PA_CL_VPORT_ZOFFSET);
    RenderViewport viewport(ox - sx, oy + sy, 2.0f * sx, -2.0f * sy, oz, oz + sz);
    commandList->setViewports(&viewport, 1);
    RenderRect scissor(0, 0, kWidth, kHeight);
    commandList->setScissors(&scissor, 1);

    commandList->setPipeline(pipeline.get());
    commandList->setGraphicsPushConstants(0, pushConstants);
    commandList->setVertexBuffers(0, vertexViews.data(), uint32_t(vertexViews.size()), inputSlots.data());
    if (e.function == kDrawIndexed) {
      // DrawIndexedVertices(device, prim, base vertex, start index, count).
      const uint32_t indexObject = state.U32(xenos::device::kIndexBuffer);
      const Blob* object = e.FindBlob(indexObject);
      if (!object) {
        ++skipped["no index buffer"];
        continue;
      }
      const bool is32 = xenos::LoadBE32(object->data.data()) >> 31;
      const Blob* indices = e.FindBlob(xenos::LoadBE32(object->data.data() + 24));
      if (!indices) {
        ++skipped["no indices"];
        continue;
      }
      const uint64_t at = upload.Allocate(indices->data.data(), indices->data.size(), 16);
      xenos::SwapEndian(upload.memory + at, indices->data.size(), is32 ? 2 : 1);
      RenderIndexBufferView indexView{upload.buffer->at(at), uint32_t(indices->data.size()),
                                      is32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT};
      commandList->setIndexBuffer(&indexView);
      commandList->drawIndexedInstanced(e.regs[4], 1, e.regs[3], int32_t(e.regs[2]), 0);
    } else {
      // DrawVertices(device, prim, start vertex, count).
      commandList->drawInstanced(e.regs[3], 1, e.regs[2], 0);
    }
    ++drawn;
  }

  // plume has no texture-to-buffer copy: record it with Vulkan directly.
  RenderTextureBarrier toCopy(color.get(), RenderTextureLayout::COPY_SOURCE);
  commandList->barriers(RenderBarrierStage::COPY, nullptr, 0, &toCopy, 1);
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {kWidth, kHeight, 1};
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commandList.get())->vk,
                         static_cast<VulkanTexture*>(color.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         static_cast<VulkanBuffer*>(readback.get())->vk, 1, &region);
  commandList->end();
  const RenderCommandList* lists[] = {commandList.get()};
  queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());

  WritePpm(argv[3], static_cast<const uint8_t*>(readback->map()), kWidth, kHeight, rowPitch);
  std::printf("%u draws replayed, %zu pipelines\n", drawn, pipelines.size());
  for (const auto& [reason, count] : skipped) std::printf("  skipped %u: %s\n", count, reason.c_str());
  for (const auto& [format, count] : formats) std::printf("  %5u x %s\n", count, format.c_str());
  return 0;
}
