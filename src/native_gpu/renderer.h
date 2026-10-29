// skate1 - native D3D layer: renderer (see docs/native_d3d.md)
//
// Turns the game's D3D calls into plume work: draws (XenosRecomp shaders,
// state from the D3D device shadow, vertex/index buffers and textures from
// guest memory), clears and resolves. EDRAM render targets become host
// surfaces keyed by EDRAM base, format and pitch; resolves copy them into
// host textures keyed by destination address, which later texture fetches
// of that address sample instead of guest memory. Fed by the in-game hooks or
// by tools/native_replay from a frame capture.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "xenos_decode.h"

struct ShaderCacheEntry;

namespace native_gpu {

// DrawIndexedVertices / DrawVertices / EndVertices as the device saw them.
struct DrawCall {
  xenos::DeviceState state;
  uint64_t vs_key = 0;
  uint64_t ps_key = 0;
  uint32_t primitive = 0;  // Xenos primitive type
  uint32_t base_vertex = 0;
  uint32_t start = 0;      // first index or vertex
  uint32_t count = 0;
  // The vertex shader's physical part as D3D patched it for this draw.
  const uint8_t* vs_physical = nullptr;
  uint32_t vs_physical_size = 0;
  // Indexed draws: the guest indices from 0 (big-endian), and their size.
  const uint8_t* indices = nullptr;
  uint32_t indices_size = 0;
  bool indices_32bit = false;
  // BeginVertices / EndVertices: the vertices written into the command buffer.
  const uint8_t* inline_vertices = nullptr;
  uint32_t inline_size = 0;
};

// ClearF(flags, rect, color): flags 0xF color targets, 0x10 depth, 0x20 stencil.
struct ClearCall {
  xenos::DeviceState state;
  uint32_t flags = 0;
  bool has_rect = false;
  int32_t rect[4] = {};  // x1, y1, x2, y2
  float color[4] = {};
};

// Resolve(flags, source rect, texture, dest point, ...): flags & 4 = depth,
// 0x100 / 0x200 clear color / depth afterwards. The destination comes from
// the RB_COPY_* registers.
struct ResolveCall {
  xenos::DeviceState state;
  uint32_t flags = 0;
  bool has_rect = false;
  int32_t rect[4] = {};
  bool has_dest_point = false;
  int32_t dest_x = 0;
  int32_t dest_y = 0;
};

class Renderer {
 public:
  // `physical` is the guest physical memory; `container` returns the shader
  // container (big-endian) for a cache key, for the vertex elements.
  bool Initialize(plume::RenderDevice* device, const uint8_t* physical,
                  std::function<const uint8_t*(uint64_t key)> container);

  void BeginFrame();
  void Draw(const DrawCall& call);
  void Clear(const ClearCall& call);
  void Resolve(const ResolveCall& call);
  // Submits the frame and waits for it.
  void EndFrame();

  // Reads back a resolved texture (RGBA8) by its guest address.
  bool ReadResolved(uint32_t address, std::vector<uint8_t>& rgba, uint32_t& width,
                    uint32_t& height);

  // Why draws or textures were skipped, and counts, for reports.
  const std::map<std::string, uint32_t>& stats() const { return stats_; }

 private:
  struct Shader {
    const ShaderCacheEntry* entry = nullptr;
    uint32_t microcode_offset = 0;
    struct Element {
      uint32_t address, usage, usage_index;
    };
    std::vector<Element> elements;
    std::unique_ptr<plume::RenderShader> shader;
  };
  struct Texture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t dimension = 1;  // 1 2D, 2 3D, 3 cube
    uint32_t index = 0;      // in its descriptor heap
  };

  Shader* GetShader(uint64_t key);
  Texture* GetGuestTexture(const xenos::TextureFetch& fetch);
  uint32_t GetSampler(const xenos::TextureFetch& fetch);
  Texture& GetColorSurface(const xenos::DeviceState& state);
  Texture& GetDepthSurface(const xenos::DeviceState& state);
  Texture& CreateSurface(uint64_t key, uint32_t width, uint32_t height, plume::RenderFormat format,
                         bool depth);
  void Transition(plume::RenderTexture* texture, plume::RenderTextureLayout layout);
  void SetTargets(plume::RenderTexture* color, plume::RenderTexture* depth);
  void SetMainLayout();
  uint64_t Allocate(const void* data, uint64_t size, uint64_t alignment = 256);

  plume::RenderDevice* device_ = nullptr;
  const uint8_t* physical_ = nullptr;
  std::function<const uint8_t*(uint64_t)> container_;
  std::vector<uint8_t> spirv_;

  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::unique_ptr<plume::RenderCommandList> commands_;
  std::unique_ptr<plume::RenderCommandFence> fence_;
  std::unique_ptr<plume::RenderDescriptorSet> texture_2d_set_, texture_3d_set_, texture_cube_set_;
  std::unique_ptr<plume::RenderDescriptorSet> sampler_set_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_, resolve_layout_;
  std::unique_ptr<plume::RenderShader> resolve_vs_, resolve_ps_;
  std::unique_ptr<plume::RenderBuffer> upload_;
  uint8_t* upload_memory_ = nullptr;
  uint64_t upload_address_ = 0, upload_offset_ = 0;
  static constexpr uint64_t kUploadSize = 512ull << 20;

  std::unique_ptr<plume::RenderTexture> null_2d_, null_3d_, null_cube_;
  std::unique_ptr<plume::RenderTextureView> null_cube_view_;
  uint32_t next_2d_ = 1, next_3d_ = 1, next_cube_ = 1;

  std::unordered_map<uint64_t, Shader> shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  std::unordered_map<uint64_t, Texture> guest_textures_;
  std::unordered_map<uint64_t, uint32_t> sampler_indices_;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
  std::unordered_map<uint64_t, Texture> surfaces_;   // EDRAM render targets
  std::unordered_map<uint32_t, Texture> resolved_;   // by destination address
  std::map<std::pair<plume::RenderTexture*, plume::RenderTexture*>,
           std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;
  std::unordered_map<plume::RenderTexture*, plume::RenderTextureLayout> layouts_;
  // Textures replaced during the frame, released once it has completed.
  std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures_;
  std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers_;
  std::pair<plume::RenderTexture*, plume::RenderTexture*> current_targets_{};
  bool main_layout_bound_ = false;
  std::map<std::string, uint32_t> stats_;
};

}  // namespace native_gpu
