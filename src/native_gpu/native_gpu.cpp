// skate1 - native D3D layer (port of Unleashed Recompiled's renderer on
// plume, see docs/native_d3d.md). For now: create the plume device and report
// what the renderer needs from it.

#include "native_gpu.h"

#include <memory>

#include <plume_render_interface.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "shader_registry.h"

REXCVAR_DEFINE_BOOL(native_gpu, false, "Skate1",
                    "Render with the native D3D layer instead of the emulated Xenos GPU "
                    "(work in progress)");

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace {
std::unique_ptr<plume::RenderInterface> g_interface;
std::unique_ptr<plume::RenderDevice> g_device;
}  // namespace

void StartNativeGpu(rex::Runtime* runtime) {
  if (!REXCVAR_GET(native_gpu)) {
    return;
  }
  StartShaderRegistry(runtime->memory());
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
}
