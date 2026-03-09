# AMD FSR2 Integration Notes

WoWee supports two FSR2 backends at runtime:

- `AMD FidelityFX SDK` backend (preferred when available).
- `Internal fallback` backend (used when AMD SDK prerequisites are not met).

## SDK Location

AMD SDK checkout path:

`extern/FidelityFX-FSR2`

FidelityFX SDK checkout path (framegen extern):

`extern/FidelityFX-SDK` (pinned to `v1.1.4` in build scripts and CI)

Detection expects:

- `extern/FidelityFX-FSR2/src/ffx-fsr2-api/ffx_fsr2.h`
- `extern/FidelityFX-FSR2/src/ffx-fsr2-api/vk/shaders/ffx_fsr2_accumulate_pass_permutations.h`
- If permutation headers are missing in the SDK checkout, WoWee CMake copies a vendored snapshot from:
  - `third_party/fsr2_vk_permutations`

## Build Flags

- `WOWEE_ENABLE_AMD_FSR2=ON` (default): attempt AMD backend integration.
- `WOWEE_ENABLE_AMD_FSR3_FRAMEGEN=ON` (default): build AMD FSR3 framegen interface probe when FidelityFX-SDK headers are present.
- `WOWEE_HAS_AMD_FSR2` compile define:
  - `1` when AMD SDK prerequisites are present.
  - `0` when missing, in which case internal fallback remains active.
- `WOWEE_HAS_AMD_FSR3_FRAMEGEN` compile define:
  - `1` when FidelityFX-SDK FI/OF/FSR3+VK headers are detected.
  - `0` when headers are missing (probe target disabled).

Runtime note:

- Renderer/UI now expose a persisted experimental framegen toggle.
- Runtime loader now probes for AMD FSR3 SDK runtime binaries.
- You can point to an explicit runtime binary with:
  - `WOWEE_FFX_SDK_RUNTIME_LIB=/absolute/path/to/libffx_fsr3_vk.so` (or `.dll` / `.dylib`).
- Current runtime status is still `dispatch staged` (FI/OF dispatch activation pending).

## Current Status

- AMD FSR2 Vulkan dispatch path is integrated and used when available.
- UI displays active backend in settings (`AMD FidelityFX SDK` or `Internal fallback`).
- Runtime settings include persisted FSR2 jitter tuning.
- FidelityFX-SDK `v1.1.4` extern is fetched across platforms and validated in Linux CI for Vulkan framegen source presence.
- Startup safety behavior remains enabled:
  - persisted FSR2 is deferred until `IN_WORLD`
  - startup falls back unless `WOWEE_ALLOW_STARTUP_FSR2=1`

## FSR Defaults

- Quality default: `Native (100%)`
- UI quality order: `Native`, `Ultra Quality`, `Quality`, `Balanced`
- Default sharpness: `1.6`
- Default FSR2 jitter sign: `0.38`
- Performance preset is intentionally removed.

## CI Notes

- `build-linux-amd-fsr2` clones AMD's repository and configures with `WOWEE_ENABLE_AMD_FSR2=ON`.
- All build jobs clone:
  - `GPUOpen-Effects/FidelityFX-FSR2` (`master`)
  - `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` (`v1.1.4`)
- Linux CI additionally checks FidelityFX-SDK Vulkan framegen files:
  - `ffx_frameinterpolation_callbacks_glsl.h`
  - `ffx_opticalflow_callbacks_glsl.h`
  - `CMakeShadersFrameinterpolation.txt`
  - `CMakeShadersOpticalflow.txt`
- All CI platform jobs build:
  - `wowee_fsr3_framegen_amd_vk_probe`
- Some upstream SDK checkouts do not include generated Vulkan permutation headers.
- WoWee bootstraps those headers from the vendored snapshot so AMD backend builds remain cross-platform and deterministic.
- If SDK headers are missing entirely, WoWee still falls back to the internal backend.
