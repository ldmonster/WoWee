# AMD FSR2 Integration Notes

WoWee supports two FSR2 backends at runtime:

- `AMD FidelityFX SDK` backend (preferred when available).
- `Internal fallback` backend (used when AMD SDK prerequisites are not met).

## SDK Location

AMD SDK checkout path:

`extern/FidelityFX-FSR2`

FidelityFX SDK checkout path (framegen extern):

`extern/FidelityFX-SDK` (default branch `main` from WoWee's fork in build scripts and CI)

Override knobs for local build scripts:

- `WOWEE_FFX_SDK_REPO` (default: `https://github.com/Kelsidavis/FidelityFX-SDK.git`)
- `WOWEE_FFX_SDK_REF` (default: `main`)

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
- Runtime loader now supports:
  - Path A: AMD SDK runtime binaries (`ffx_fsr3_vk`).
  - Path B: wrapper runtime libraries implementing WoWee's wrapper ABI.
- You can point to an explicit runtime binary with:
  - `WOWEE_FFX_SDK_RUNTIME_LIB=/absolute/path/to/libffx_fsr3_vk.so` (or `.dll` / `.dylib`).
- You can point to an explicit wrapper binary with:
  - `WOWEE_FFX_SDK_RUNTIME_WRAPPER_LIB=/absolute/path/to/libffx_fsr3_vk_wrapper.so` (or `.dll` / `.dylib`).
- WoWee now ships an in-tree wrapper target:
  - `wowee_fsr3_vk_wrapper` (output in `build/bin`).
- Wrapper backend runtime override:
  - `WOWEE_FSR3_WRAPPER_BACKEND_LIB=/absolute/path/to/libffx_fsr3_vk.so` (or `.dll` / `.dylib`).
- Wrapper backend mode selection:
  - `WOWEE_FSR3_WRAPPER_BACKEND=vulkan_runtime`
  - `WOWEE_FSR3_WRAPPER_BACKEND=dx12_bridge`
- Default is `vulkan_runtime` on all platforms.
- `dx12_bridge` is opt-in.
- On Windows: `dx12_bridge` performs DX12/Vulkan preflight, then loads the first runtime library exposing the required FSR3 dispatch exports.
- On Linux: `dx12_bridge` is enabled for wrapper runtime compatibility mode and uses Vulkan dispatch symbols in this build.
- Linux bridge preflight validates Vulkan FD interop support:
  - required device functions: `vkGetMemoryFdKHR`, `vkGetSemaphoreFdKHR`
  - required device extensions: `VK_KHR_external_memory`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore`, `VK_KHR_external_semaphore_fd`
- DX12 bridge runtime override:
  - `WOWEE_FSR3_DX12_RUNTIME_LIB=<path-to-amd_fidelityfx_framegeneration_dx12.dll>`
- DX12 bridge device preflight toggle:
  - `WOWEE_FSR3_WRAPPER_DX12_VALIDATE_DEVICE=1` (default)
  - `WOWEE_FSR3_WRAPPER_DX12_VALIDATE_DEVICE=0` to skip DXGI/D3D12 device creation probe
- DX12 bridge preflight also validates Vulkan Win32 interop support:
  - required device functions: `vkGetMemoryWin32HandleKHR`, `vkImportSemaphoreWin32HandleKHR`, `vkGetSemaphoreWin32HandleKHR`
  - required device extensions: `VK_KHR_external_memory`, `VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore`, `VK_KHR_external_semaphore_win32`
- Path B wrapper ABI contract is declared in:
  - `include/rendering/amd_fsr3_wrapper_abi.h`
- Current wrapper ABI version: `3` (dispatch payload carries external memory/semaphore handles and acquire/release fence values for bridge synchronization).
- Required wrapper exports:
  - `wowee_fsr3_wrapper_get_abi_version`
  - `wowee_fsr3_wrapper_get_backend`
  - `wowee_fsr3_wrapper_initialize`
  - `wowee_fsr3_wrapper_dispatch_upscale`
  - `wowee_fsr3_wrapper_shutdown`
- Optional wrapper export:
  - `wowee_fsr3_wrapper_dispatch_framegen`
  - `wowee_fsr3_wrapper_get_last_error`
  - `wowee_fsr3_wrapper_get_capabilities`

## Current Status

- AMD FSR2 Vulkan dispatch path is integrated and used when available.
- UI displays active backend in settings (`AMD FidelityFX SDK` or `Internal fallback`).
- Runtime settings include persisted FSR2 jitter tuning.
- FidelityFX-SDK extern is fetched across platforms (default: `Kelsidavis/FidelityFX-SDK` on `main`).
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
  - `Kelsidavis/FidelityFX-SDK` (`main`) by default
- Linux CI additionally checks FidelityFX-SDK framegen files (legacy `sdk/...` and Kits layouts):
  - `ffx_frameinterpolation_callbacks_glsl.h`
  - `ffx_opticalflow_callbacks_glsl.h`
  - `CMakeShadersFrameinterpolation.txt`
  - `CMakeShadersOpticalflow.txt`
- CI builds `wowee_fsr3_framegen_amd_vk_probe` when that target is generated by CMake for the detected SDK layout.
- Some upstream SDK checkouts do not include generated Vulkan permutation headers.
- WoWee bootstraps those headers from the vendored snapshot so AMD backend builds remain cross-platform and deterministic.
- If SDK headers are missing entirely, WoWee still falls back to the internal backend.
