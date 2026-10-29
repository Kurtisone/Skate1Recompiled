// skate1 - offline replay of a frame capture with the native renderer
// (src/native_gpu/renderer.cpp, milestone B in docs/native_d3d.md).
//
//   native_replay <capture.sk1c> <shader cache inputs dir> <out.ppm> [first last]
//
// Feeds the capture's draws, clears and resolves (events [first, last],
// default all) to the renderer and writes the front buffer (the last color
// resolve) as a PPM.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <plume_render_interface.h>

#include "native_gpu/renderer.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace {

constexpr uint32_t kDrawIndexed = 0x82CB38E8;
constexpr uint32_t kDraw = 0x82CB3500;
constexpr uint32_t kBeginVertices = 0x82CB29D8;
constexpr uint32_t kEndVertices = 0x82CB2E78;
constexpr uint32_t kClear = 0x82CC7510;
constexpr uint32_t kResolve = 0x82CC7FF0;
constexpr uint32_t kBeginTiling = 0x82CC4FF0;

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
    if (!address) return nullptr;
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

const uint8_t* MapPhysical(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY);
  struct stat st {};
  if (fd < 0 || fstat(fd, &st)) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(1);
  }
  return static_cast<const uint8_t*>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
}

// Shader containers by microcode key (XENOS_RECOMP_MICROCODE_KEY).
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

// D3DRECT {x1, y1, x2, y2} (big-endian LONGs).
bool ReadRect(const Event& e, uint32_t address, int32_t rect[4]) {
  const Blob* blob = e.FindBlob(address);
  if (!blob) return false;
  for (int i = 0; i < 4; ++i) rect[i] = int32_t(xenos::LoadBE32(&blob->data[4 * i]));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4 && argc != 6) {
    std::fprintf(stderr, "usage: %s <capture.sk1c> <cache inputs dir> <out.ppm> [first last]\n", argv[0]);
    return 1;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  const std::vector<Event> events = ReadCapture(argv[1]);
  const uint8_t* physical = MapPhysical(std::string(argv[1]) + ".phys");
  const auto containers = LoadContainers(argv[2]);
  size_t first = 0, last = events.size() - 1;
  if (argc == 6) {
    first = std::stoul(argv[4]);
    last = std::min<size_t>(std::stoul(argv[5]), last);
  }

  auto interface = plume::CreateVulkanInterface();
  auto device = interface->createDevice();
  native_gpu::Renderer renderer;
  if (!renderer.Initialize(device.get(), physical, [&](uint64_t key) -> const uint8_t* {
        auto it = containers.find(key);
        return it == containers.end() ? nullptr : it->second.data();
      })) {
    std::fprintf(stderr, "renderer initialization failed\n");
    return 1;
  }
  std::printf("%zu events, %zu containers, replaying %zu..%zu on %s\n", events.size(), containers.size(),
              first, last, device->getDescription().name.c_str());

  renderer.BeginFrame();
  uint32_t frontBuffer = 0;
  std::vector<uint32_t> resolveTargets;
  const Event* begin = nullptr;  // last BeginVertices
  for (size_t i = first; i <= last; ++i) {
    const Event& e = events[i];
    const xenos::DeviceState state{e.device.data()};
    switch (e.function) {
      case kBeginVertices:
        begin = &e;
        break;
      case kDrawIndexed:
      case kDraw:
      case kEndVertices: {
        native_gpu::DrawCall call;
        call.state = state;
        call.vs_key = e.vs_key;
        call.ps_key = e.ps_key;
        size_t blob = 0;
        if (e.function == kEndVertices) {
          // BeginVertices(device, prim, count, stride); the inline vertices
          // are the first blob, the patched microcode the second.
          if (!begin || e.blobs.size() < 2) break;
          call.primitive = begin->regs[1];
          call.count = begin->regs[2];
          call.inline_vertices = e.blobs[0].data.data();
          call.inline_size = uint32_t(e.blobs[0].data.size());
          blob = 1;
        } else {
          call.primitive = e.regs[1];
          if (e.function == kDrawIndexed) {
            // DrawIndexedVertices(device, prim, base vertex, start index, count):
            // the index buffer object (+0 bit 31 = 32-bit, +24 data) and the indices.
            call.base_vertex = e.regs[2];
            call.start = e.regs[3];
            call.count = e.regs[4];
            const Blob* object = e.FindBlob(state.U32(xenos::device::kIndexBuffer));
            const Blob* indices = object ? e.FindBlob(xenos::LoadBE32(object->data.data() + 24)) : nullptr;
            if (!indices) break;
            call.indices = indices->data.data();
            call.indices_size = uint32_t(indices->data.size());
            call.indices_32bit = xenos::LoadBE32(object->data.data()) >> 31;
          } else {
            // DrawVertices(device, prim, start vertex, count).
            call.start = e.regs[2];
            call.count = e.regs[3];
          }
        }
        if (e.blobs.size() > blob) {
          call.vs_physical = e.blobs[blob].data.data();
          call.vs_physical_size = uint32_t(e.blobs[blob].data.size());
        }
        renderer.Draw(call);
        break;
      }
      case kClear: {
        // ClearF(device, flags, rect, color).
        native_gpu::ClearCall call;
        call.state = state;
        call.flags = e.regs[1];
        call.has_rect = ReadRect(e, e.regs[2], call.rect);
        if (const Blob* color = e.FindBlob(e.regs[3])) {
          for (int c = 0; c < 4; ++c) call.color[c] = xenos::LoadBEFloat(&color->data[4 * c]);
        }
        renderer.Clear(call);
        break;
      }
      case kBeginTiling: {
        // BeginTiling(device, flags, count, tile rects, clear color, ...):
        // each tile is cleared when its pass starts; rendered untiled, that
        // is one clear of all the tiles.
        native_gpu::ClearCall call;
        call.state = state;
        const Blob* rects = e.FindBlob(e.regs[3]);
        if (!rects || !e.regs[2]) break;
        call.flags = 0x30;
        call.has_rect = true;
        call.rect[0] = call.rect[1] = INT32_MAX;
        for (uint32_t t = 0; t < e.regs[2] && 16 * t + 16 <= rects->data.size(); ++t) {
          for (int c = 0; c < 4; ++c) {
            const int32_t v = int32_t(xenos::LoadBE32(&rects->data[16 * t + 4 * c]));
            call.rect[c] = c < 2 ? std::min(call.rect[c], v) : std::max(call.rect[c], v);
          }
        }
        if (const Blob* color = e.FindBlob(e.regs[4])) {
          call.flags |= 0xF;
          for (int c = 0; c < 4; ++c) call.color[c] = xenos::LoadBEFloat(&color->data[4 * c]);
        }
        renderer.Clear(call);
        break;
      }
      case kResolve: {
        // Resolve(device, flags, source rect, texture, dest point, ...).
        native_gpu::ResolveCall call;
        call.state = state;
        call.flags = e.regs[1];
        call.has_rect = ReadRect(e, e.regs[2], call.rect);
        if (const Blob* point = e.FindBlob(e.regs[4])) {
          call.has_dest_point = true;
          call.dest_x = int32_t(xenos::LoadBE32(&point->data[0]));
          call.dest_y = int32_t(xenos::LoadBE32(&point->data[4]));
        }
        renderer.Resolve(call);
        resolveTargets.push_back(state.Reg(0x2319) & 0xFFFFF000);
        if (!(call.flags & 4)) frontBuffer = state.Reg(0x2319);
        break;
      }
      default:
        break;
    }
  }
  renderer.EndFrame();

  for (const auto& [what, count] : renderer.stats()) std::printf("  %5u %s\n", count, what.c_str());
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  // REPLAY_DUMP=<dir>: every 8_8_8_8 resolve target as it ends the frame.
  if (const char* dir = getenv("REPLAY_DUMP")) {
    for (uint32_t address : resolveTargets) {
      if (!renderer.ReadResolved(address, rgba, width, height)) continue;
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%08X.ppm", dir, address);
      FILE* f = std::fopen(path, "wb");
      std::fprintf(f, "P6\n%u %u\n255\n", width, height);
      for (size_t p = 0; p < size_t(width) * height; ++p) std::fwrite(&rgba[p * 4], 1, 3, f);
      std::fclose(f);
    }
  }
  if (!renderer.ReadResolved(frontBuffer, rgba, width, height)) {
    std::fprintf(stderr, "no 8_8_8_8 resolve to read back at %08X\n", frontBuffer);
    return 1;
  }
  FILE* f = std::fopen(argv[3], "wb");
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  for (size_t p = 0; p < size_t(width) * height; ++p) std::fwrite(&rgba[p * 4], 1, 3, f);
  std::fclose(f);
  std::printf("front buffer %08X (%ux%u) -> %s\n", frontBuffer, width, height, argv[3]);
  return 0;
}
