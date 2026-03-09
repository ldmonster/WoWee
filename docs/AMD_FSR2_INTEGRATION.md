# AMD FSR2 Integration Notes

This project currently has two FSR2 states:

- `AMD FidelityFX SDK` backend (preferred): enabled when SDK sources are present.
- `Internal fallback` backend: used when SDK is missing.

## SDK Location

Drop AMD's official FSR2 repo at:

`extern/FidelityFX-FSR2`

Expected header for detection:

`extern/FidelityFX-FSR2/src/ffx-fsr2-api/ffx_fsr2.h`

## Build Flags

- `WOWEE_ENABLE_AMD_FSR2=ON` (default): try to use AMD SDK if detected.
- `WOWEE_HAS_AMD_FSR2` compile define:
  - `1` when SDK header is found.
  - `0` otherwise (fallback path).

## Current Status

- CMake detects the SDK and defines `WOWEE_HAS_AMD_FSR2`.
- UI shows active FSR2 backend label in settings.
- Runtime logs clearly indicate whether AMD SDK is available.

## Remaining Work (to finish official AMD path)

1. Add backend wrapper around `FfxFsr2Context` and Vulkan backend helpers.
2. Feed required inputs each frame:
   - Color, depth, motion vectors
   - Jitter offsets
   - Delta time and render/display resolution
   - Exposure / reactive mask as needed
3. Replace custom compute accumulation path with `ffxFsr2ContextDispatch`.
4. Keep current fallback path behind a runtime switch for safety.
5. Add a debug overlay:
   - Backend in use
   - Internal resolution
   - Jitter values
   - Motion vector validity stats
6. Validate with fixed camera + movement sweeps:
   - Static shimmer
   - Moving blur/ghosting
   - Fine geometry stability

