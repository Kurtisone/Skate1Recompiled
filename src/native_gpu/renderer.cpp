// skate1 - native D3D layer: renderer (see renderer.h)

#include "renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <smolv.h>
#ifdef NATIVE_GPU_ZSTD
#include <zstd.h>
#endif

#include <plume_vulkan.h>

#include "shader_cache.h"
#include "shaders/native_shaders.h"

using namespace plume;

namespace native_gpu {

namespace {

// Bindless heap sizes.
constexpr uint32_t kTextureHeapSize = 8192;
constexpr uint32_t kSamplerHeapSize = 256;
// Largest EDRAM surface height: 10 MB of EDRAM.
constexpr uint32_t kMaxSurfaceHeight = 2048;

// XenosRecomp's shared constants (SharedConstants in Unleashed's video.cpp).
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

struct ResolvePushConstants {
  uint32_t source_index;
  uint32_t mode;  // 0 color * scale, 1 depth packed as D24S8 in 8_8_8_8
  float scale;
  int32_t offset_x;
  int32_t offset_y;
  uint32_t stencil_index;
};

RenderStencilFaceDesc StencilFace(uint32_t func, uint32_t fail, uint32_t zpass, uint32_t zfail) {
  RenderStencilFaceDesc face;
  face.compareFunction = RenderComparisonFunction(func + 1);
  face.failOp = RenderStencilOp(fail + 1);
  face.passOp = RenderStencilOp(zpass + 1);
  face.depthFailOp = RenderStencilOp(zfail + 1);
  return face;
}

// XenosRecomp's USAGE_LOCATIONS: vertex input location of (usage, index).
int32_t UsageLocation(uint32_t usage, uint32_t index) {
  static constexpr struct {
    uint32_t usage, index, location;
  } kLocations[] = {
      {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},  {5, 1, 5},
      {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},  {1, 0, 10}, {10, 1, 11},
      {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15},
  };
  for (const auto& entry : kLocations) {
    if (entry.usage == usage && entry.index == index) return int32_t(entry.location);
  }
  return -1;
}

// XenosRecomp declares normals, tangents and binormals uint4 and decodes them
// itself: packed 10_11_11 (SPEC_CONSTANT_R11G11B10_NORMAL) or float bits.
bool IsRawUsage(uint32_t usage) { return usage == 3 || usage == 6 || usage == 7; }

RenderFormat RawVertexFormat(uint32_t format) {
  using namespace xenos;
  switch (format) {
    case k_10_11_11: return RenderFormat::R32_UINT;
    case k_32_FLOAT: return RenderFormat::R32_UINT;
    case k_32_32_FLOAT: return RenderFormat::R32G32_UINT;
    case k_32_32_32_FLOAT: return RenderFormat::R32G32B32_UINT;
    case k_32_32_32_32_FLOAT: return RenderFormat::R32G32B32A32_UINT;
    // Half floats, converted by XenosRecomp with SPEC_CONSTANT_HALF_NORMAL.
    case k_16_16_FLOAT: return RenderFormat::R16G16_UINT;
    case k_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_UINT;
    default: return RenderFormat::UNKNOWN;
  }
}

RenderFormat VertexFormat(const xenos::VertexFetchInstruction& fetch, uint32_t usage) {
  using namespace xenos;
  const bool s = fetch.is_signed, i = fetch.is_integer;
  switch (fetch.format) {
    case k_8_8_8_8:
      if (usage == 2) {  // BLENDINDICES: uint4
        return s ? RenderFormat::R8G8B8A8_SINT : RenderFormat::R8G8B8A8_UINT;
      }
      // Integer components feed float4 inputs as their values (scaled).
      return i ? (s ? RenderFormat::R8G8B8A8_SSCALED : RenderFormat::R8G8B8A8_USCALED)
               : (s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM);
    case k_16_16:
      return i ? (s ? RenderFormat::R16G16_SSCALED : RenderFormat::R16G16_USCALED)
               : (s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM);
    case k_16_16_16_16:
      return i ? (s ? RenderFormat::R16G16B16A16_SSCALED : RenderFormat::R16G16B16A16_USCALED)
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

RenderFormat TextureFormat(const xenos::TextureFetch& t) {
  using namespace xenos;
  const bool s = t.is_signed;
  switch (t.format) {
    case kTex_8: return s ? RenderFormat::R8_SNORM : RenderFormat::R8_UNORM;
    case kTex_8_8: return s ? RenderFormat::R8G8_SNORM : RenderFormat::R8G8_UNORM;
    case kTex_8_8_8_8: return s ? RenderFormat::R8G8B8A8_SNORM : RenderFormat::R8G8B8A8_UNORM;
    case kTex_DXT1: return RenderFormat::BC1_UNORM;
    case kTex_DXT2_3: return RenderFormat::BC2_UNORM;
    case kTex_DXT4_5: return RenderFormat::BC3_UNORM;
    case kTex_16_16: return s ? RenderFormat::R16G16_SNORM : RenderFormat::R16G16_UNORM;
    case kTex_16_16_16_16: return s ? RenderFormat::R16G16B16A16_SNORM : RenderFormat::R16G16B16A16_UNORM;
    case kTex_16_16_FLOAT: return RenderFormat::R16G16_FLOAT;
    case kTex_16_16_16_16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
    case kTex_32_FLOAT: return RenderFormat::R32_FLOAT;
    default: return RenderFormat::UNKNOWN;
  }
}

// EDRAM color formats (RB_COLOR_INFO) as host render target formats. 16_16
// and 16_16_16_16 hold -32..32 in EDRAM, kept as floats here.
RenderFormat SurfaceFormat(uint32_t color_format) {
  switch (color_format) {
    case 0: case 1: return RenderFormat::R8G8B8A8_UNORM;  // 8_8_8_8 (_GAMMA)
    case 4: case 6: return RenderFormat::R16G16_FLOAT;
    case 5: case 7: case 2: case 3: case 10: case 12: return RenderFormat::R16G16B16A16_FLOAT;
    case 14: return RenderFormat::R32_FLOAT;
    case 15: return RenderFormat::R32G32_FLOAT;
    default: return RenderFormat::R8G8B8A8_UNORM;
  }
}

// The fetch constant's swizzle (3 bits per destination component: 0-3 source
// component, 4 zero, 5 one) as a view component mapping.
RenderComponentMapping SwizzleMapping(uint32_t swizzle) {
  RenderSwizzle components[4];
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t select = (swizzle >> (3 * c)) & 7;
    components[c] = select < 4 ? RenderSwizzle(uint32_t(RenderSwizzle::R) + select)
                               : select == 4 ? RenderSwizzle::ZERO : RenderSwizzle::ONE;
  }
  return RenderComponentMapping(components[0], components[1], components[2], components[3]);
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

void CopySwapped32(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i < bytes; i += 4) {
    const uint32_t value = xenos::LoadBE32(src + i);
    std::memcpy(dst + i, &value, 4);
  }
}

}  // namespace

bool Renderer::Initialize(RenderDevice* device, const uint8_t* physical,
                          std::function<const uint8_t*(uint64_t key)> container, WriteWatcher* watcher) {
  device_ = device;
  watcher_ = watcher;
  physical_ = physical;
  container_ = std::move(container);
  // The game's cache is stored uncompressed (XENOS_RECOMP_NO_ZSTD, no zstd in
  // the Steam Deck build); zstd only where the replay tool links it.
  if (g_spirvCacheCompressedSize == g_spirvCacheDecompressedSize) {
    spirv_.assign(g_compressedSpirvCache, g_compressedSpirvCache + g_spirvCacheCompressedSize);
  } else {
#ifdef NATIVE_GPU_ZSTD
    spirv_.resize(g_spirvCacheDecompressedSize);
    if (ZSTD_decompress(spirv_.data(), spirv_.size(), g_compressedSpirvCache,
                        g_spirvCacheCompressedSize) != spirv_.size()) {
      return false;
    }
#else
    return false;
#endif
  }
  queue_ = device_->createCommandQueue(RenderCommandListType::DIRECT);
  for (Frame& frame : frames_) {
    frame.commands = queue_->createCommandList();
    frame.fence = device_->createCommandFence();
    frame.upload = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        kUploadSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  }
  commands_ = frames_[0].commands.get();
  UseUploadBuffer(frames_[0].upload.get(), kUploadSize);

  // Bindless heaps and pipeline layout as Unleashed sets them up: 2D, 3D and
  // cube texture heaps, a sampler heap, and 24 bytes of push constants (the
  // vertex, pixel and shared constant buffer addresses).
  RenderDescriptorSetBuilder textures;
  textures.begin();
  textures.addTexture(0, kTextureHeapSize);
  textures.end(true, kTextureHeapSize);
  RenderDescriptorSetBuilder samplers;
  samplers.begin();
  samplers.addSampler(0, kSamplerHeapSize);
  samplers.end(true, kSamplerHeapSize);
  texture_2d_set_ = textures.create(device_);
  texture_3d_set_ = textures.create(device_);
  texture_cube_set_ = textures.create(device_);
  sampler_set_ = samplers.create(device_);
  RenderPipelineLayoutBuilder layout;
  layout.begin(false, true);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(samplers);
  layout.addPushConstant(0, 4, 24, RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layout.end();
  layout_ = layout.create(device_);
  RenderPipelineLayoutBuilder resolveLayout;
  resolveLayout.begin(false, false);
  resolveLayout.addDescriptorSet(textures);
  resolveLayout.addPushConstant(0, 0, sizeof(ResolvePushConstants), RenderShaderStageFlag::PIXEL);
  resolveLayout.end();
  resolve_layout_ = resolveLayout.create(device_);
  resolve_vs_ = device_->createShader(k_resolve_vs, sizeof(k_resolve_vs), "VSMain", RenderShaderFormat::SPIRV);
  resolve_ps_ = device_->createShader(k_resolve_ps, sizeof(k_resolve_ps), "PSMain", RenderShaderFormat::SPIRV);
  pack_ps_ = device_->createShader(k_pack_ps, sizeof(k_pack_ps), "PSPack", RenderShaderFormat::SPIRV);


  // Index 0 of each heap: 1x1 white textures; sampler 0: linear wrap.
  null_2d_ = device_->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_3d_ = device_->createTexture(RenderTextureDesc::Texture3D(1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  null_cube_ = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::CUBE));
  RenderTextureViewDesc cubeView;
  cubeView.format = RenderFormat::R8G8B8A8_UNORM;
  cubeView.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
  cubeView.mipLevels = 1;
  null_cube_view_ = null_cube_->createTextureView(cubeView);
  samplers_.push_back(device_->createSampler(RenderSamplerDesc()));
  sampler_set_->setSampler(0, samplers_.back().get());

  commands_->begin();
  const uint32_t white = 0xFFFFFFFF;
  const uint64_t whiteOffset = Allocate(&white, 4, 512);
  for (RenderTexture* texture : {null_2d_.get(), null_3d_.get(), null_cube_.get()}) {
    Transition(texture, RenderTextureLayout::COPY_DEST);
    for (uint32_t slice = 0; slice < (texture == null_cube_.get() ? 6u : 1u); ++slice) {
      commands_->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(texture, 0, slice),
          RenderTextureCopyLocation::PlacedFootprint(upload_, RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                                                     64, whiteOffset));
    }
    Transition(texture, RenderTextureLayout::SHADER_READ);
  }
  texture_2d_set_->setTexture(0, null_2d_.get(), RenderTextureLayout::SHADER_READ);
  texture_3d_set_->setTexture(0, null_3d_.get(), RenderTextureLayout::SHADER_READ);
  texture_cube_set_->setTexture(0, null_cube_.get(), RenderTextureLayout::SHADER_READ, null_cube_view_.get());
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frames_[0].fence.get());
  queue_->waitForCommandFence(frames_[0].fence.get());
  upload_offset_ = 0;
  return true;
}

void Renderer::UseUploadBuffer(RenderBuffer* buffer, uint64_t size) {
  upload_ = buffer;
  upload_memory_ = static_cast<uint8_t*>(buffer->map());  // persistently mapped
  upload_address_ = buffer->getDeviceAddress();
  upload_size_ = size;
  upload_offset_ = 0;
}

void Renderer::WaitFrame(Frame& frame) {
  if (!frame.pending) return;
  queue_->waitForCommandFence(frame.fence.get());
  frame.pending = false;
  frame.retired_framebuffers.clear();
  frame.retired_buffers.clear();
  frame.retired_views.clear();
  frame.retired_textures.clear();
}

void Renderer::WaitIdle() {
  for (Frame& frame : frames_) WaitFrame(frame);
}

uint64_t Renderer::Allocate(const void* data, uint64_t size, uint64_t alignment) {
  upload_offset_ = (upload_offset_ + alignment - 1) & ~(alignment - 1);
  if (upload_offset_ + size > upload_size_) {
    // Full (texture streaming): continue in an overflow buffer that lives
    // until this frame completes.
    ++stats_["upload overflow buffers"];
    const uint64_t overflowSize = std::max<uint64_t>(kUploadSize, size + alignment);
    auto overflow = device_->createBuffer(RenderBufferDesc::UploadBuffer(
        overflowSize, RenderBufferFlag::CONSTANT | RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    UseUploadBuffer(overflow.get(), overflowSize);
    retired_buffers_.push_back(std::move(overflow));
  }
  const uint64_t at = upload_offset_;
  if (data) std::memcpy(upload_memory_ + at, data, size);
  upload_offset_ += size;
  return at;
}

void Renderer::Transition(RenderTexture* texture, RenderTextureLayout layout) {
  RenderTextureLayout& current = layouts_[texture];
  if (current == layout) return;
  current = layout;
  RenderTextureBarrier barrier(texture, layout);
  commands_->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY, nullptr, 0, &barrier, 1);
}

void Renderer::SetTargets(RenderTexture* color, RenderTexture* depth) {
  Transition(color, RenderTextureLayout::COLOR_WRITE);
  if (depth) Transition(depth, RenderTextureLayout::DEPTH_WRITE);
  if (current_targets_ == std::make_pair(color, depth)) return;
  auto& framebuffer = framebuffers_[{color, depth}];
  if (!framebuffer) {
    const RenderTexture* attachments[] = {color};
    framebuffer = device_->createFramebuffer(RenderFramebufferDesc(attachments, 1, depth));
  }
  commands_->setFramebuffer(framebuffer.get());
  current_targets_ = {color, depth};
}

void Renderer::SetMainLayout() {
  if (main_layout_bound_) return;
  commands_->setGraphicsPipelineLayout(layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  commands_->setGraphicsDescriptorSet(texture_3d_set_.get(), 1);
  commands_->setGraphicsDescriptorSet(texture_cube_set_.get(), 2);
  commands_->setGraphicsDescriptorSet(sampler_set_.get(), 3);
  main_layout_bound_ = true;
}

void Renderer::BeginFrame() {
  Frame& frame = frames_[frame_];
  WaitFrame(frame);
  commands_ = frame.commands.get();
  UseUploadBuffer(frame.upload.get(), kUploadSize);
  commands_->begin();
  current_targets_ = {};
  main_layout_bound_ = false;
}

void Renderer::EndFrame(uint32_t present_address) {
  Frame& frame = frames_[frame_];
  frame.present_ready = false;
  auto front = resolved_.find(present_address & 0xFFFFF000);
  if (present_address && front != resolved_.end() && front->second.texture) {
    // Pack the front buffer as A2B10G10R10 and read it back.
    Texture& source = front->second;
    if (!present_texture_ || present_width_ != source.width || present_height_ != source.height) {
      present_width_ = source.width;
      present_height_ = source.height;
      if (present_texture_) {
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
          if (it->first.first == present_texture_.get()) {
            retired_framebuffers_.push_back(std::move(it->second));
            it = framebuffers_.erase(it);
          } else {
            ++it;
          }
        }
        layouts_.erase(present_texture_.get());
        retired_textures_.push_back(std::move(present_texture_));
      }
      present_texture_ = device_->createTexture(
          RenderTextureDesc::ColorTarget(present_width_, present_height_, RenderFormat::R32_UINT));
    }
    if (frame.present_width != present_width_ || frame.present_height != present_height_) {
      if (frame.present_readback) frame.retired_buffers.push_back(std::move(frame.present_readback));
      frame.present_width = present_width_;
      frame.present_height = present_height_;
      frame.present_readback = device_->createBuffer(
          RenderBufferDesc::ReadbackBuffer(uint64_t(present_width_) * present_height_ * 4));
      frame.present_pixels = static_cast<const uint32_t*>(frame.present_readback->map());
    }
    Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
    SetTargets(present_texture_.get(), nullptr);
    auto& pipeline = pipelines_[uint64_t(RenderFormat::R32_UINT) | 1ull << 61];
    if (!pipeline) {
      RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = resolve_layout_.get();
      desc.vertexShader = resolve_vs_.get();
      desc.pixelShader = pack_ps_.get();
      desc.renderTargetFormat[0] = RenderFormat::R32_UINT;
      desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
      desc.renderTargetCount = 1;
      pipeline = device_->createGraphicsPipeline(desc);
    }
    commands_->setGraphicsPipelineLayout(resolve_layout_.get());
    commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
    main_layout_bound_ = false;
    commands_->setPipeline(pipeline.get());
    ResolvePushConstants push{source.index, 0, 1.0f, 0, 0, 0};
    commands_->setGraphicsPushConstants(0, &push);
    RenderViewport viewport(0.0f, 0.0f, float(present_width_), float(present_height_));
    RenderRect scissor(0, 0, int32_t(present_width_), int32_t(present_height_));
    commands_->setViewports(&viewport, 1);
    commands_->setScissors(&scissor, 1);
    commands_->drawInstanced(3, 1, 0, 0);
    Transition(present_texture_.get(), RenderTextureLayout::COPY_SOURCE);
    // plume has no texture-to-buffer copy.
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {present_width_, present_height_, 1};
    vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                           static_cast<VulkanTexture*>(present_texture_.get())->vk,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           static_cast<VulkanBuffer*>(frame.present_readback.get())->vk, 1, &region);
    frame.present_ready = true;
  }
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  frame.pending = true;
  // What this frame's commands use is released once it has completed.
  for (auto& b : retired_buffers_) frame.retired_buffers.push_back(std::move(b));
  for (auto& t : retired_textures_) frame.retired_textures.push_back(std::move(t));
  for (auto& v : retired_views_) frame.retired_views.push_back(std::move(v));
  for (auto& f : retired_framebuffers_) frame.retired_framebuffers.push_back(std::move(f));
  retired_buffers_.clear();
  retired_textures_.clear();
  retired_views_.clear();
  retired_framebuffers_.clear();
  // Let the GPU run this frame while the game records the next one: only
  // the previous frame has to be finished (its slot records next).
  frame_ = (frame_ + 1) % kFramesInFlight;
  WaitFrame(frames_[frame_]);
  presented_ = &frames_[frame_];
}

Renderer::Shader* Renderer::GetShader(uint64_t key) {
  auto [it, inserted] = shaders_.try_emplace(key);
  Shader& info = it->second;
  if (inserted) {
    const ShaderCacheEntry* begin = g_shaderCacheEntries;
    const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
    const ShaderCacheEntry* entry = std::lower_bound(
        begin, end, key, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
    if (entry == end || entry->hash != key) return nullptr;
    info.entry = entry;
    // The cache's SPIR-V is smol-v encoded.
    const uint8_t* encoded = &spirv_[entry->spirvOffset];
    std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(encoded, entry->spirvSize));
    smolv::Decode(encoded, entry->spirvSize, decoded.data(), decoded.size());
    info.shader = device_->createShader(decoded.data(), decoded.size(), "main", RenderShaderFormat::SPIRV);
    if (const uint8_t* data = container_ ? container_(key) : nullptr) {
      const uint32_t shader = xenos::LoadBE32(data + 0x18);
      info.microcode_offset = xenos::LoadBE32(data + shader);
      if (xenos::LoadBE32(data) & 1) {
        // VertexShader: field18 at +0x18 and the element count at +0x1C of
        // the Shader struct; elements after field18 dwords from +0x24.
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
}

uint32_t Renderer::ResolvedIndex(Texture& texture, uint32_t swizzle) {
  if (swizzle == 0x688) return texture.index;  // xyzw (0, 1, 2, 3)
  auto& [view, index] = texture.swizzled_views[swizzle];
  if (!view) {
    RenderTextureViewDesc desc;
    desc.format = texture.format;
    desc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    desc.mipLevels = 1;
    desc.componentMapping = SwizzleMapping(swizzle);
    view = texture.texture->createTextureView(desc);
    if (!index) index = next_2d_++;
    texture_2d_set_->setTexture(index, texture.texture.get(), RenderTextureLayout::SHADER_READ, view.get());
  }
  return index;
}

const RenderBuffer* Renderer::GetGuestBuffer(uint32_t address, uint32_t size, uint64_t layout,
                                             const std::function<void(uint8_t*)>& convert) {
  const uint64_t key = XXH3_64bits_withSeed(&address, 4, layout ^ uint64_t(size) << 32);
  GuestBuffer& cached = guest_buffers_[key];
  if (cached.buffer && !(watcher_ && watcher_->Dirty(address, size))) {
    return cached.buffer.get();
  }
  if (cached.buffer) {
    // Written to by the game: the frame may still use the old copy.
    retired_buffers_.push_back(std::move(cached.buffer));
    ++stats_["buffers re-uploaded"];
  } else {
    ++stats_["buffers"];
  }
  if (watcher_) watcher_->Watch(address, size);
  cached.buffer = device_->createBuffer(
      RenderBufferDesc::UploadBuffer(std::max(size, 4u), RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
  uint8_t* data = static_cast<uint8_t*>(cached.buffer->map());
  std::memcpy(data, physical_ + address, size);
  convert(data);
  cached.buffer->unmap();
  return cached.buffer.get();
}

uint32_t Renderer::GetSampler(const xenos::TextureFetch& t) {
  RenderSamplerDesc desc;
  auto address = [](uint32_t clamp) {
    switch (clamp) {
      case 0: return RenderTextureAddressMode::WRAP;
      case 1: return RenderTextureAddressMode::MIRROR;
      case 3: return RenderTextureAddressMode::MIRROR_ONCE;
      case 6: case 7: return RenderTextureAddressMode::BORDER;
      default: return RenderTextureAddressMode::CLAMP;
    }
  };
  desc.addressU = address(t.clamp[0]);
  desc.addressV = address(t.clamp[1]);
  desc.addressW = address(t.clamp[2]);
  desc.magFilter = t.mag_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  desc.minFilter = t.min_filter == 1 ? RenderFilter::LINEAR : RenderFilter::NEAREST;
  desc.mipmapMode = RenderMipmapMode::NEAREST;
  desc.maxLOD = 0.0f;
  const uint32_t fields[] = {uint32_t(desc.addressU), uint32_t(desc.addressV), uint32_t(desc.addressW),
                             uint32_t(desc.magFilter), uint32_t(desc.minFilter)};
  auto [it, inserted] = sampler_indices_.try_emplace(XXH3_64bits(fields, sizeof(fields)), 0);
  if (inserted && samplers_.size() < kSamplerHeapSize) {
    it->second = uint32_t(samplers_.size());
    samplers_.push_back(device_->createSampler(desc));
    sampler_set_->setSampler(it->second, samplers_.back().get());
  }
  return it->second;
}

Renderer::Texture* Renderer::GetGuestTexture(const xenos::TextureFetch& t) {
  const uint32_t fields[] = {t.format, t.endian, t.tiled, t.pitch, t.address, t.dimension,
                             t.width, t.height, t.depth, t.swizzle, t.is_signed};
  auto [it, inserted] = guest_textures_.try_emplace(XXH3_64bits(fields, sizeof(fields)));
  Texture& guest = it->second;
  const uint32_t byteSize = xenos::TextureByteSize(t);
  if (!inserted) {
    if (!guest.texture || !(watcher_ && watcher_->Dirty(t.address & 0x1FFFFFFF, byteSize))) {
      return guest.texture ? &guest : nullptr;
    }
    // Written to by the game: upload again into a new texture, same slot.
    layouts_.erase(guest.texture.get());
    retired_textures_.push_back(std::move(guest.texture));
    retired_views_.push_back(std::move(guest.view));
    ++stats_["textures re-uploaded"];
  }
  if (watcher_) watcher_->Watch(t.address & 0x1FFFFFFF, byteSize);
  guest.dimension = t.dimension;
  const RenderFormat format = TextureFormat(t);
  if (t.dimension == 2 && format != RenderFormat::UNKNOWN) {
    // Volume (color grading LUTs).
    const uint64_t size = uint64_t(t.width) * t.height * t.depth * xenos::GetTextureBlock(t.format).bytes;
    const uint64_t at = Allocate(nullptr, size, 512);
    if (!xenos::UntileTexture3D(physical_, t, upload_memory_ + at)) {
      ++stats_["texture skipped: linear or compressed 3D"];
      return nullptr;
    }
    guest.format = format;
    guest.width = t.width;
    guest.height = t.height;
    guest.texture = device_->createTexture(RenderTextureDesc::Texture3D(t.width, t.height, t.depth, 1, format));
    Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
    commands_->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(guest.texture.get()),
        RenderTextureCopyLocation::PlacedFootprint(upload_, format, t.width, t.height, t.depth, t.width, at));
    Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_3D;
    view.mipLevels = 1;
    view.componentMapping = SwizzleMapping(t.swizzle);
    guest.view = guest.texture->createTextureView(view);
    if (!guest.index) guest.index = next_3d_++;
    texture_3d_set_->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
    if (inserted) ++stats_["textures (3D)"];
    return &guest;
  }
  if ((t.dimension != 1 && t.dimension != 3) || format == RenderFormat::UNKNOWN) {
    ++stats_["texture skipped: dimension " + std::to_string(t.dimension) + " format " +
             std::to_string(t.format)];
    return nullptr;
  }
  const bool cube = t.dimension == 3;
  guest.format = format;
  guest.width = t.width;
  guest.height = t.height;
  guest.texture = device_->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, t.width, t.height, 1, 1, cube ? 6 : 1, format,
      cube ? RenderTextureFlag::CUBE : RenderTextureFlag::NONE));
  Transition(guest.texture.get(), RenderTextureLayout::COPY_DEST);
  const xenos::TextureBlock block = xenos::GetTextureBlock(t.format);
  const uint32_t blocksX = (t.width + block.size - 1) / block.size;
  const uint32_t blocksY = (t.height + block.size - 1) / block.size;
  for (uint32_t slice = 0; slice < (cube ? 6u : 1u); ++slice) {
    const uint64_t at = Allocate(nullptr, uint64_t(blocksX) * blocksY * block.bytes, 512);
    xenos::UntileTexture(physical_, t, slice, upload_memory_ + at);
    commands_->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(guest.texture.get(), 0, slice),
        RenderTextureCopyLocation::PlacedFootprint(upload_, format, t.width, t.height, 1,
                                                   blocksX * block.size, at));
  }
  Transition(guest.texture.get(), RenderTextureLayout::SHADER_READ);
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = cube ? RenderTextureViewDimension::TEXTURE_CUBE : RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = 1;
  view.componentMapping = SwizzleMapping(t.swizzle);
  guest.view = guest.texture->createTextureView(view);
  if (!guest.index) guest.index = cube ? next_cube_++ : next_2d_++;
  (cube ? texture_cube_set_ : texture_2d_set_)
      ->setTexture(guest.index, guest.texture.get(), RenderTextureLayout::SHADER_READ, guest.view.get());
  if (inserted) ++stats_[cube ? "textures (cube)" : "textures (2D)"];
  return &guest;
}

Renderer::Texture& Renderer::CreateSurface(uint64_t key, uint32_t width, uint32_t height, RenderFormat format,
                                           bool depth) {
  Texture& surface = surfaces_[key];
  if (surface.texture) return surface;
  surface.format = format;
  surface.width = width;
  surface.height = height;
  surface.texture = device_->createTexture(depth ? RenderTextureDesc::DepthTarget(width, height, format)
                                                 : RenderTextureDesc::ColorTarget(width, height, format));
  RenderTextureViewDesc view;
  view.format = format;
  view.dimension = RenderTextureViewDimension::TEXTURE_2D;
  view.mipLevels = 1;
  surface.view = surface.texture->createTextureView(view);
  surface.index = next_2d_++;
  texture_2d_set_->setTexture(surface.index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                              surface.view.get());
  if (depth) {
    // plume views a depth target's depth aspect only: swap in a stencil
    // aspect view under a plume view object.
    surface.stencil_view = surface.texture->createTextureView(view);
    auto* vulkanView = static_cast<VulkanTextureView*>(surface.stencil_view.get());
    VkDevice vkDevice = static_cast<VulkanDevice*>(device_)->vk;
    vkDestroyImageView(vkDevice, vulkanView->vk, nullptr);
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = static_cast<VulkanTexture*>(surface.texture.get())->vk;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCreateImageView(vkDevice, &info, nullptr, &vulkanView->vk);
    surface.stencil_index = next_2d_++;
    texture_2d_set_->setTexture(surface.stencil_index, surface.texture.get(), RenderTextureLayout::SHADER_READ,
                                surface.stencil_view.get());
  }
  return surface;
}

Renderer::Texture& Renderer::GetColorSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t color = state.Reg(xenos::reg::RB_COLOR_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const uint32_t format = (color >> 16) & 0xF;
  const uint64_t key = uint64_t(color & 0xFFF) | uint64_t(format) << 12 | uint64_t(pitch) << 16;
  return CreateSurface(key, pitch, kMaxSurfaceHeight, SurfaceFormat(format), false);
}

Renderer::Texture& Renderer::GetDepthSurface(const xenos::DeviceState& state) {
  const uint32_t surface = state.Reg(xenos::reg::RB_SURFACE_INFO);
  const uint32_t depth = state.Reg(xenos::reg::RB_DEPTH_INFO);
  const uint32_t pitch = std::max(surface & 0x3FFF, 1u);
  const uint64_t key = uint64_t(depth & 0xFFF) | uint64_t(pitch) << 16 | 1ull << 63;
  return CreateSurface(key, pitch, kMaxSurfaceHeight, RenderFormat::D32_FLOAT_S8_UINT, true);
}

void Renderer::Clear(const ClearCall& call) {
  Texture& color = GetColorSurface(call.state);
  Texture& depth = GetDepthSurface(call.state);
  SetTargets(color.texture.get(), depth.texture.get());
  RenderRect rect(0, 0, int32_t(color.width), int32_t(color.height));
  if (call.has_rect) rect = RenderRect(call.rect[0], call.rect[1], call.rect[2], call.rect[3]);
  if (call.flags & 0xF) {
    commands_->clearColor(0, RenderColor(call.color[0], call.color[1], call.color[2], call.color[3]), &rect, 1);
  }
  if (call.flags & 0x30) {
    // The Z and stencil arguments: RB_DEPTH_CLEAR in the shadow only follows
    // resolve clears.
    commands_->clearDepthStencil(call.flags & 0x10, call.flags & 0x20, call.depth, call.stencil & 0xFF,
                                 &rect, 1);
  }
  ++stats_["clears"];
}

void Renderer::Resolve(const ResolveCall& call) {
  const bool depth = call.flags & 4;
  Texture& source = depth ? GetDepthSurface(call.state) : GetColorSurface(call.state);
  const uint32_t address = call.state.Reg(0x2319) & 0xFFFFF000;  // RB_COPY_DEST_BASE
  const uint32_t destPitch = call.state.Reg(0x231A) & 0x3FFF;      // RB_COPY_DEST_PITCH
  const uint32_t destHeight = (call.state.Reg(0x231A) >> 16) & 0x3FFF;
  int32_t rect[4] = {0, 0, int32_t(destPitch), int32_t(destHeight)};
  if (call.has_rect) std::copy(call.rect, call.rect + 4, rect);
  // The source rect is in surface coordinates; the destination point (or
  // the rect's origin) is where it lands. RB_COPY_DEST_PITCH's height counts
  // from that point (predicated tiling resolves the bands one by one).
  const int32_t x = call.has_dest_point ? call.dest_x : rect[0];
  const int32_t y = call.has_dest_point ? call.dest_y : rect[1];
  // Textures are sampled with normalized coordinates, so the host texture has
  // the size the game fetches it at: the destination pitch, unless that is
  // just the width padded to 32 texels.
  const uint32_t rectWidth = uint32_t(rect[2] - rect[0]);
  const uint32_t width = (rect[0] == 0 && destPitch == ((rectWidth + 31) & ~31u)) ? rectWidth : destPitch;
  const uint32_t height = uint32_t(y) + destHeight;
  RenderFormat format = source.format;
  ResolvePushConstants push{source.index, 0, 1.0f, rect[0] - x, rect[1] - y, source.stencil_index};
  if (depth) {
    format = RenderFormat::R8G8B8A8_UNORM;
    push.mode = 1;
  } else if (format == RenderFormat::R8G8B8A8_UNORM) {
    push.mode = 3;
  } else if (format == RenderFormat::R16G16_FLOAT) {
    // A resolve converts: the EDRAM value (fixed point, -32..32) is written
    // in the destination format, here the unsigned normalized 16_16 the
    // game fetches it as (shadow depth and mask in 0..1).
    format = RenderFormat::R16G16_UNORM;
  }

  Texture& dest = resolved_[address];
  if (!dest.texture || dest.format != format || dest.width != width || dest.height < height) {
    if (dest.texture) {
      // Still referenced by recorded commands: keep it until the frame ends.
      for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->first.first == dest.texture.get()) {
          retired_framebuffers_.push_back(std::move(it->second));
          it = framebuffers_.erase(it);
        } else {
          ++it;
        }
      }
      if (current_targets_.first == dest.texture.get()) current_targets_ = {};
      layouts_.erase(dest.texture.get());
      retired_textures_.push_back(std::move(dest.texture));
      retired_views_.push_back(std::move(dest.view));
      for (auto& [swizzle, entry] : dest.swizzled_views) retired_views_.push_back(std::move(entry.first));
    }
    dest.texture = device_->createTexture(RenderTextureDesc::ColorTarget(width, height, format));
    dest.format = format;
    dest.width = width;
    dest.height = height;
    RenderTextureViewDesc view;
    view.format = format;
    view.dimension = RenderTextureViewDimension::TEXTURE_2D;
    view.mipLevels = 1;
    dest.view = dest.texture->createTextureView(view);
    if (!dest.index) dest.index = next_2d_++;
    texture_2d_set_->setTexture(dest.index, dest.texture.get(), RenderTextureLayout::SHADER_READ,
                                dest.view.get());
  }

  // Draw the source into the destination with the resolve shader.
  Transition(source.texture.get(), RenderTextureLayout::SHADER_READ);
  SetTargets(dest.texture.get(), nullptr);
  const uint64_t key = uint64_t(format) | 1ull << 62;
  auto& pipeline = pipelines_[key];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = resolve_layout_.get();
    desc.vertexShader = resolve_vs_.get();
    desc.pixelShader = resolve_ps_.get();
    desc.renderTargetFormat[0] = format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = device_->createGraphicsPipeline(desc);
  }
  commands_->setGraphicsPipelineLayout(resolve_layout_.get());
  commands_->setGraphicsDescriptorSet(texture_2d_set_.get(), 0);
  main_layout_bound_ = false;
  commands_->setPipeline(pipeline.get());
  commands_->setGraphicsPushConstants(0, &push);
  RenderViewport viewport(float(x), float(y), float(rect[2] - rect[0]), float(rect[3] - rect[1]));
  RenderRect scissor(x, y, x + rect[2] - rect[0], y + rect[3] - rect[1]);
  // The full-screen triangle covers the viewport; the shader reads by pixel.
  viewport = RenderViewport(0.0f, 0.0f, float(dest.width), float(dest.height));
  commands_->setViewports(&viewport, 1);
  commands_->setScissors(&scissor, 1);
  commands_->drawInstanced(3, 1, 0, 0);
  Transition(dest.texture.get(), RenderTextureLayout::SHADER_READ);
  ++stats_[depth ? "resolves (depth)" : "resolves (color)"];

  // Clears that ride on the resolve, limited to the resolved rect (with
  // predicated tiling that is the tile; the other bands are still to resolve).
  if (call.flags & 0x300) {
    ClearCall clear;
    clear.state = call.state;
    clear.has_rect = true;
    std::copy(rect, rect + 4, clear.rect);
    clear.flags = ((call.flags & 0x100) ? 0x1 : 0) | ((call.flags & 0x200) ? 0x10 : 0);
    const uint32_t color = call.state.Reg(0x231E);  // RB_COLOR_CLEAR (8_8_8_8)
    for (int c = 0; c < 4; ++c) clear.color[c] = float((color >> (8 * c)) & 0xFF) / 255.0f;
    clear.depth = call.clear_depth;
    clear.flags |= (call.flags & 0x200) ? 0x20 : 0;
    Clear(clear);
  }
}

void Renderer::Draw(const DrawCall& call) {
  const xenos::DeviceState& state = call.state;
  RenderPrimitiveTopology topology;
  bool quads = false;
  switch (call.primitive) {
    case 1: topology = RenderPrimitiveTopology::POINT_LIST; break;
    case 2: topology = RenderPrimitiveTopology::LINE_LIST; break;
    case 3: topology = RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: topology = RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 6: topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case 13:  // quad list: expanded to triangles below
      topology = RenderPrimitiveTopology::TRIANGLE_LIST;
      quads = true;
      break;
    default:
      ++stats_["draw skipped: primitive " + std::to_string(call.primitive)];
      return;
  }
  if (quads && call.indices_size) {
    ++stats_["draw skipped: indexed quad list"];
    return;
  }
  Shader* vs = GetShader(call.vs_key);
  Shader* ps = GetShader(call.ps_key);
  if (!vs || !ps) {
    ++stats_["draw skipped: shader not in cache"];
    return;
  }
  if (!call.vs_physical) {
    ++stats_["draw skipped: no vertex microcode"];
    return;
  }

  // Vertex input from the patched vfetch instructions.
  std::vector<RenderInputElement> inputElements;
  std::vector<RenderInputSlot> inputSlots;
  std::vector<uint32_t> slotFetch;
  // 16-bit elements per slot (byte offset, dwords): Xenos keeps the first
  // component in the high half of each dword, so after the 32-bit swap their
  // halves are put back in order.
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> halfSwaps;
  uint32_t lastStride = 0, lastSlot = 0;
  bool packedNormals = false, halfNormals = false;
  for (const Shader::Element& element : vs->elements) {
    const size_t at = vs->microcode_offset + element.address * 12;
    if (at + 12 > call.vs_physical_size) {
      ++stats_["draw skipped: vertex fetch outside microcode"];
      return;
    }
    xenos::VertexFetchInstruction fetch = xenos::DecodeVertexFetchInstruction(call.vs_physical + at);
    if (!fetch.is_vfetch) {
      ++stats_["draw skipped: vertex element is not a vfetch"];
      return;
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
    if (raw && (fetch.format == xenos::k_16_16_FLOAT || fetch.format == xenos::k_16_16_16_16_FLOAT)) {
      halfNormals = true;
    }
    const int32_t location = UsageLocation(element.usage, element.usage_index);
    if (format == RenderFormat::UNKNOWN || location < 0) {
      ++stats_[(location < 0 ? "vertex element without a location: usage " : "vertex element format: usage ") +
               std::to_string(element.usage) + "." + std::to_string(element.usage_index) + " format " +
               std::to_string(fetch.format) + (fetch.is_signed ? " signed" : "") +
               (fetch.is_integer ? " integer" : "")];
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
  std::vector<RenderVertexBufferView> vertexViews;
  for (uint32_t slot = 0; slot < slotFetch.size(); ++slot) {
    const xenos::VertexFetch fetch = xenos::DecodeVertexFetch(state, slotFetch[slot]);
    const uint32_t stride = inputSlots[slot].stride;
    auto convert = [&](uint8_t* data, uint32_t size, uint32_t endian) {
      xenos::SwapEndian(data, size, endian);
      for (const auto& [offset, dwords] : halfSwaps[slot]) {
        for (uint32_t vertex = 0; stride && (vertex + 1) * stride <= size; ++vertex) {
          uint8_t* p = data + vertex * stride + offset;
          for (uint32_t d = 0; d < dwords; ++d, p += 4) {
            std::swap(p[0], p[2]);
            std::swap(p[1], p[3]);
          }
        }
      }
    };
    if (call.inline_vertices) {
      // Inline vertices live in the command buffer: converted every time.
      const uint64_t at = Allocate(call.inline_vertices, call.inline_size, 16);
      convert(upload_memory_ + at, call.inline_size, 2);
      vertexViews.push_back({upload_->at(at), call.inline_size});
      continue;
    }
    // Guest vertex buffers are converted once and kept until written to.
    uint64_t layout = XXH3_64bits_withSeed(halfSwaps[slot].data(),
                                  halfSwaps[slot].size() * sizeof(halfSwaps[slot][0]), stride);
    const uint32_t address = fetch.address & 0x1FFFFFFF;
    const RenderBuffer* buffer = GetGuestBuffer(address, fetch.size, layout ^ fetch.endian,
                                                [&](uint8_t* data) { convert(data, fetch.size, fetch.endian); });
    vertexViews.push_back({buffer, fetch.size});
  }

  // Textures (resolved ones by address first) and samplers.
  SharedConstants shared;
  for (uint32_t slot = 0; slot < 16; ++slot) {
    const xenos::TextureFetch t = xenos::DecodeTextureFetch(state, slot);
    if (!t.valid) continue;
    if (auto resolved = resolved_.find(t.address); resolved != resolved_.end()) {
      shared.texture2DIndices[slot] = ResolvedIndex(resolved->second, t.swizzle);
    } else if (Texture* texture = GetGuestTexture(t)) {
      (texture->dimension == 3   ? shared.textureCubeIndices
       : texture->dimension == 2 ? shared.texture3DIndices
                                 : shared.texture2DIndices)[slot] = texture->index;
    }
    shared.samplerIndices[slot] = GetSampler(t);
  }

  // Targets.
  Texture& color = GetColorSurface(state);
  Texture& depth = GetDepthSurface(state);
  SetTargets(color.texture.get(), depth.texture.get());
  SetMainLayout();

  // Pipeline.
  const uint32_t depthControl = state.Reg(xenos::reg::RB_DEPTHCONTROL);
  const uint32_t blendControl = state.Reg(xenos::reg::RB_BLENDCONTROL0);
  const uint32_t colorControl = state.Reg(xenos::reg::RB_COLORCONTROL);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = vs->shader.get();
  desc.pixelShader = ps->shader.get();
  desc.depthEnabled = (depthControl >> 1) & 1;
  desc.depthWriteEnabled = (depthControl >> 2) & 1;
  desc.depthFunction = RenderComparisonFunction(((depthControl >> 4) & 7) + 1);
  // Stencil (RB_DEPTHCONTROL bit 0; front func/fail/zpass/zfail at 8/11/14/17,
  // back at 20/23/26/29 when bit 7; RB_STENCILREFMASK ref/mask/writemask).
  desc.stencilEnabled = depthControl & 1;
  if (desc.stencilEnabled) {
    const uint32_t refMask = state.Reg(0x210D);
    desc.stencilReference = refMask & 0xFF;
    desc.stencilReadMask = (refMask >> 8) & 0xFF;
    desc.stencilWriteMask = (refMask >> 16) & 0xFF;
    desc.stencilFrontFace = StencilFace((depthControl >> 8) & 7, (depthControl >> 11) & 7,
                                        (depthControl >> 14) & 7, (depthControl >> 17) & 7);
    desc.stencilBackFace = (depthControl >> 7) & 1
                               ? StencilFace((depthControl >> 20) & 7, (depthControl >> 23) & 7,
                                             (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                               : desc.stencilFrontFace;
  }
  desc.depthClipEnabled = true;
  desc.primitiveTopology = topology;
  desc.cullMode = RenderCullMode::NONE;
  desc.renderTargetFormat[0] = color.format;
  desc.renderTargetCount = 1;
  desc.depthTargetFormat = depth.format;
  RenderBlendDesc& blend = desc.renderTargetBlend[0];
  blend.srcBlend = BlendFactor(blendControl & 0x1F);
  blend.blendOp = BlendOp((blendControl >> 5) & 7);
  blend.dstBlend = BlendFactor((blendControl >> 8) & 0x1F);
  blend.srcBlendAlpha = BlendFactor((blendControl >> 16) & 0x1F);
  blend.blendOpAlpha = BlendOp((blendControl >> 21) & 7);
  blend.dstBlendAlpha = BlendFactor((blendControl >> 24) & 0x1F);
  blend.blendEnabled = !(blend.srcBlend == RenderBlend::ONE && blend.dstBlend == RenderBlend::ZERO &&
                         blend.blendOp == RenderBlendOperation::ADD &&
                         blend.srcBlendAlpha == RenderBlend::ONE && blend.dstBlendAlpha == RenderBlend::ZERO &&
                         blend.blendOpAlpha == RenderBlendOperation::ADD);
  blend.renderTargetWriteMask = uint8_t(state.Reg(xenos::reg::RB_COLOR_MASK) & 0xF);
  uint32_t specConstants = packedNormals ? 1 : 0;  // SPEC_CONSTANT_R11G11B10_NORMAL
  if (halfNormals) specConstants |= 1 << 5;          // SPEC_CONSTANT_HALF_NORMAL
  if ((colorControl >> 3) & 1) specConstants |= 2;  // SPEC_CONSTANT_ALPHA_TEST
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
  auto& pipeline = pipelines_[key];
  if (!pipeline) pipeline = device_->createGraphicsPipeline(desc);
  if (!pipeline) {
    ++stats_["draw skipped: pipeline creation"];
    return;
  }

  // Constants.
  const uint64_t vsConstants = Allocate(nullptr, 4096);
  CopySwapped32(upload_memory_ + vsConstants, state.data + xenos::device::kVertexConstants, 4096);
  const uint64_t psConstants = Allocate(nullptr, 4096);
  CopySwapped32(upload_memory_ + psConstants, state.data + xenos::device::kPixelConstants, 4096);
  shared.booleans = (state.U32(xenos::device::kVertexBoolConstants) & 0xFF) |
                    ((state.U32(xenos::device::kPixelBoolConstants) & 0xFF) << 16);
  shared.alphaThreshold = state.RegF(xenos::reg::RB_ALPHA_REF);
  const uint64_t sharedConstants = Allocate(&shared, sizeof(shared));
  const uint64_t pushConstants[] = {upload_address_ + vsConstants, upload_address_ + psConstants,
                                    upload_address_ + sharedConstants};

  // Viewport from the D3D viewport transform (the shaders invert Y).
  const float sx = state.RegF(xenos::reg::PA_CL_VPORT_XSCALE);
  const float ox = state.RegF(xenos::reg::PA_CL_VPORT_XOFFSET);
  const float sy = state.RegF(xenos::reg::PA_CL_VPORT_YSCALE);
  const float oy = state.RegF(xenos::reg::PA_CL_VPORT_YOFFSET);
  const float sz = state.RegF(xenos::reg::PA_CL_VPORT_ZSCALE);
  const float oz = state.RegF(xenos::reg::PA_CL_VPORT_ZOFFSET);
  RenderViewport viewport(ox - sx, oy + sy, 2.0f * sx, -2.0f * sy, oz, oz + sz);
  commands_->setViewports(&viewport, 1);
  RenderRect scissor(0, 0, int32_t(color.width), int32_t(color.height));
  commands_->setScissors(&scissor, 1);

  commands_->setPipeline(pipeline.get());
  commands_->setGraphicsPushConstants(0, pushConstants);
  commands_->setVertexBuffers(0, vertexViews.data(), uint32_t(vertexViews.size()), inputSlots.data());
  if (call.indices_size) {
    const RenderBuffer* buffer =
        GetGuestBuffer(call.indices_address, call.indices_size, call.indices_32bit ? 2 : 1,
                       [&](uint8_t* data) { xenos::SwapEndian(data, call.indices_size, call.indices_32bit ? 2 : 1); });
    RenderIndexBufferView view{buffer, call.indices_size,
                               call.indices_32bit ? RenderFormat::R32_UINT : RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(call.count, 1, call.start, int32_t(call.base_vertex), 0);
  } else if (quads) {
    const uint32_t quadCount = call.count / 4;
    std::vector<uint16_t> indices;
    for (uint32_t q = 0; q < quadCount; ++q) {
      const uint16_t v = uint16_t(call.start + 4 * q);
      for (uint16_t i : {0, 1, 2, 0, 2, 3}) indices.push_back(uint16_t(v + i));
    }
    const uint64_t at = Allocate(indices.data(), indices.size() * 2, 16);
    RenderIndexBufferView view{upload_->at(at), uint32_t(indices.size() * 2), RenderFormat::R16_UINT};
    commands_->setIndexBuffer(&view);
    commands_->drawIndexedInstanced(uint32_t(indices.size()), 1, 0, 0, 0);
  } else {
    commands_->drawInstanced(call.count, 1, call.start, 0);
  }
  ++stats_["draws"];
}

bool Renderer::ReadResolved(uint32_t address, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  auto it = resolved_.find(address & 0xFFFFF000);
  if (it == resolved_.end() || (it->second.format != RenderFormat::R8G8B8A8_UNORM &&
                                it->second.format != RenderFormat::R16G16_UNORM)) {
    return false;
  }
  Texture& texture = it->second;
  width = texture.width;
  height = texture.height;
  auto readback = device_->createBuffer(RenderBufferDesc::ReadbackBuffer(uint64_t(width) * height * 4));
  WaitIdle();
  Frame& frame = frames_[frame_];
  commands_ = frame.commands.get();
  commands_->begin();
  layouts_.erase(texture.texture.get());
  layouts_[texture.texture.get()] = RenderTextureLayout::SHADER_READ;
  Transition(texture.texture.get(), RenderTextureLayout::COPY_SOURCE);
  // plume has no texture-to-buffer copy.
  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(static_cast<VulkanCommandList*>(commands_)->vk,
                         static_cast<VulkanTexture*>(texture.texture.get())->vk,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VulkanBuffer*>(readback.get())->vk,
                         1, &region);
  Transition(texture.texture.get(), RenderTextureLayout::SHADER_READ);
  commands_->end();
  const RenderCommandList* lists[] = {commands_};
  queue_->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  queue_->waitForCommandFence(frame.fence.get());
  const uint8_t* data = static_cast<const uint8_t*>(readback->map());
  rgba.assign(data, data + size_t(width) * height * 4);
  readback->unmap();
  return true;
}

}  // namespace native_gpu
