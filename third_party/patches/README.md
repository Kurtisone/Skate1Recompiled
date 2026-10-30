Local patches on submodules, applied as commits in the submodule checkouts
(not pushed upstream); re-apply with `git -C third_party/<name> am` or `apply`.

- plume-scaled-vertex-formats.patch: USCALED/SSCALED vertex input formats
  (Xbox 360 vertex fetches read integer components as floats, e.g. skinning
  weights), Vulkan backend only.
- plume-custom-vulkan-loader.patch: keep a loader set with volkInitializeCustom,
  so the native D3D layer uses the same custom driver as the SDK on Android.
