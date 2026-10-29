// skate1 - native D3D layer (see docs/native_d3d.md)

#pragma once

namespace rex {
class Runtime;
}

// Called once at startup; does nothing unless native_gpu is set.
void StartNativeGpu(rex::Runtime* runtime);
