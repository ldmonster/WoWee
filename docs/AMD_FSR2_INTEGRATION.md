# AMD FSR2 Integration Notes

WoWee supports two FSR2 backends at runtime:

- `AMD FidelityFX SDK` backend (preferred when available).
- `Internal fallback` backend (used when AMD SDK prerequisites are not met).

## SDK Location

AMD SDK checkout path:

`extern/FidelityFX-FSR2`

Detection expects:

- `extern/FidelityFX-FSR2/src/ffx-fsr2-api/ffx_fsr2.h`
- `extern/FidelityFX-FSR2/src/ffx-fsr2-api/vk/shaders/ffx_fsr2_accumulate_pass_permutations.h`
- If permutation headers are missing in the SDK checkout, WoWee CMake copies a vendored snapshot from:
  - `third_party/fsr2_vk_permutations`

## Build Flags

- `WOWEE_ENABLE_AMD_FSR2=ON` (default): attempt AMD backend integration.
- `WOWEE_HAS_AMD_FSR2` compile define:
  - `1` when AMD SDK prerequisites are present.
  - `0` when missing, in which case internal fallback remains active.

## Current Status

- AMD FSR2 Vulkan dispatch path is integrated and used when available.
- UI displays active backend in settings (`AMD FidelityFX SDK` or `Internal fallback`).
- Runtime settings include persisted FSR2 jitter tuning.
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
- Some upstream SDK checkouts do not include generated Vulkan permutation headers.
- WoWee bootstraps those headers from the vendored snapshot so AMD backend builds remain cross-platform and deterministic.
- If SDK headers are missing entirely, WoWee still falls back to the internal backend.
