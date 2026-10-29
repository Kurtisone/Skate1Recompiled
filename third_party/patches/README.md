Local patches on submodules, applied as commits in the submodule checkouts
(not pushed upstream); re-apply with `git -C third_party/<name> am` or `apply`.

- plume-scaled-vertex-formats.patch: USCALED/SSCALED vertex input formats
  (Xbox 360 vertex fetches read integer components as floats, e.g. skinning
  weights), Vulkan backend only.
