// skate1 - native D3D layer (port of Unleashed Recompiled's renderer on
// plume, see docs/native_d3d.md). For now: create the plume device and report
// what the renderer needs from it.

#include "native_gpu.h"

#include <memory>

#include <plume_render_interface.h>
#ifdef __ANDROID__
#include <dlfcn.h>

#include <volk.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "frame_capture.h"
#include "live_renderer.h"
#include "shader_registry.h"

REXCVAR_DEFINE_BOOL(native_gpu, false, "Skate1",
                    "Render with the native D3D layer instead of the emulated Xenos GPU "
                    "(work in progress)");

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

#ifdef __ANDROID__
namespace rex::ui::vulkan {
void* GetLoaderLibraryOverride();
}
#endif

namespace {
std::unique_ptr<plume::RenderInterface> g_interface;
std::unique_ptr<plume::RenderDevice> g_device;
}  // namespace

void StartNativeGpu(rex::Runtime* runtime) {
  if (!REXCVAR_GET(native_gpu)) {
    return;
  }
  StartShaderRegistry(runtime->memory());
  StartFrameCapture(runtime->memory());
#ifdef __ANDROID__
  // A custom driver (adrenotools) is only reachable through the SDK's handle.
  if (void* driver = rex::ui::vulkan::GetLoaderLibraryOverride()) {
    if (auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver, "vkGetInstanceProcAddr"))) {
      volkInitializeCustom(proc);
    }
  }
#endif
  g_interface = plume::CreateVulkanInterface();
  if (!g_interface) {
    REXLOG_ERROR("native_gpu: no Vulkan interface");
    return;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXLOG_ERROR("native_gpu: no Vulkan device");
    return;
  }
  const plume::RenderDeviceDescription& description = g_device->getDescription();
  const plume::RenderDeviceCapabilities& capabilities = g_device->getCapabilities();
  REXLOG_INFO(
      "native_gpu: plume device '{}', descriptor indexing {}, buffer device address {}, "
      "scalar block layout {}, unified memory {}",
      description.name, capabilities.descriptorIndexing, capabilities.bufferDeviceAddress,
      capabilities.scalarBlockLayout, capabilities.uma);
  StartLiveRenderer(g_device.get(), runtime->memory());
}
