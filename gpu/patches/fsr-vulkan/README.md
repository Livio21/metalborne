# Local changes to gpu/third_party/fsr-vulkan

The submodule points at upstream FireBurn/FSR-Vulkan (`c64f093`). `build.sh` applies the
patches here to its working tree when they are not applied yet:

- `0001-...`: `BB_FSR4_PROFILE` (GPU time per FSR 4 pass) and `BB_FSR4_STATS` (driver
  statistics of each pass) in the FSR 4 v07 provider.
- `0002-...`: allow a host-visible/device-local memory type when no invisible
  device-local heap matches. This retains the AMD device-coherent-memory guard
  and the preference for invisible device-local memory on discrete GPUs.
  Reused from [bmy/bbport-mac](https://github.com/bmy/bbport-mac/blob/0a25a43693fb03b645eada115f16365148dafc4a/gpu/patches/fsr-vulkan/0004-Unified-memory-fallback-in-findMemoryTypeIndex.patch).
