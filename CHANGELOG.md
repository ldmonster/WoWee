# Changelog

## [Unreleased] — changes since v1.8.9-preview

### Architecture
- Break Application::getInstance() singleton from GameHandler via GameServices struct
- EntityController refactoring (SOLID decomposition)
- Extract 8 domain handler classes from GameHandler
- Replace 3,300-line switch with dispatch table
- Multi-platform Docker build system (Linux, macOS arm64/x86_64, Windows cross-compilation)
- Decompose ChatPanel monolith into 15+ modules under `src/ui/chat/` with IChatCommand interface, ChatCommandRegistry, MacroEvaluator, ChatMarkupParser/Renderer, ChatBubbleManager, ChatTabManager, GameStateAdapter, and 11 command modules (PR #62)
- Decompose WorldMap (1,360 LOC) into 16 modules under `src/rendering/world_map/` with WorldMapFacade (PIMPL), CompositeRenderer, DataRepository, CoordinateProjection, ViewStateMachine, 9 overlay layers (PR #61)
- Extract reusable CatmullRomSpline module to `src/math/` with O(log n) binary search and fused position+tangent evaluation (PR #60)
- Decompose TransportManager (1,200→500 LOC): extract TransportPathRepository, TransportClockSync, TransportAnimator; consolidate 7 duplicated spline parsers into `spline_packet.cpp` (PR #60)

### Features
- Spell visual effects system with bone-tracked ribbons and particles (PR #58)
- GM command support: 190-command data table with dot-prefix interception, tab-completion, `/gmhelp` with category filter (PR #62)
- ZMP pixel-accurate zone hover detection on world map (PR #63)
- Textured player arrow (MinimapArrow.blp) on world map (PR #63)
- Multi-segment path interpolation for entity movement (PR #59)
- Character screen keyboard navigation (Up/Down/Enter) (PR #59)

### Bug Fixes (v1.8.10+)
- Fix walk/run animation persisting after entity arrival (PR #59)
- Fix entity teleport during dead-reckoning overrun phase (PR #59)
- Fix Vulkan crash on window resize when minimized (0×0 extent) (PR #59)
- Fix quest log not populating on quest accept (PR #59)
- Fix hit-reaction animation being overridden on next frame (PR #59)
- Fix ChatType enum values to match WoW wire protocol (SAY=0x01 not 0x00) (PR #62)
- Fix BG_SYSTEM_* values from 82–84 (UB in bitmask shifts) to 0x24–0x26 (PR #62)
- Fix infinite Enter key loop after teleport (PR #62)
- Remove stale kVOffset (-0.15) from zone hover detection causing ~15% vertical offset
- Add null guard for cachedGameHandler_ in ChatPanel input callback
- Fix cosmic highlight aspect ratio with resolution-independent square rendering
- Skip transport waypoints with broken coordinate conversion instead of silent use
- Fix spline endpoint validation bypass for entities near world origin
- Fix off-by-one in chat link insertion buffer capacity check
- Zero window border in world map to eliminate content/window gap

### Tests
- Add 19 new test files (27 total, up from 8):
  - Chat: chat_markup_parser, chat_tab_completer, gm_commands, macro_evaluator
  - World map: world_map, coordinate_projection, exploration_state, map_resolver, view_state_machine, zone_metadata
  - Transport/spline: spline, transport_components, transport_path_repo
  - Animation: animation_ids, locomotion_fsm, combat_fsm, activity_fsm, anim_capability, indoor_shadows

### Bug Fixes (v1.8.2–v1.8.9)
- Fix VkTexture ownsSampler_ flag after move/destroy (prevented double-free)
- Fix unsigned underflow in Warden PE section loading (buffer overflow on malformed modules)
- Add bounds checks to Warden readLE32/readLE16 (out-of-bounds on untrusted PE data)
- Fix undefined behavior: SDL_BUTTON(0) computed 1 << -1 (negative shift)
- Fix BigNum::toHex/toDecimal null dereference on OpenSSL allocation failure
- Remove duplicate zone weather entry silently overwriting Dustwallow Marsh
- Fix LLVM apt repo codename (jammy→noble) in macOS Docker build
- Add missing mkdir in Linux Docker build script
- Clamp player percentage stats (block/dodge/parry/crit) to prevent NaN from corrupted packets
- Guard fsPath underflow in tryLoadPngOverride

### Code Quality (v1.8.2–v1.8.9)
- 30+ named constants replacing magic numbers across game, rendering, and pipeline code
- 55+ why-comments documenting WoW protocol quirks, format specifics, and design rationale
- 8 DRY extractions (findOnUseSpellId, createFallbackTextures, finalizeSampler,
  renderClassRestriction/renderRaceRestriction, and more)
- Scope macOS -undefined dynamic_lookup linker flag to wowee target only
- Replace goto patterns with structured control flow (do/while(false), lambdas)
- Zero out GameServices in Application::shutdown to prevent dangling pointers

---

## [v1.8.1-preview] — 2026-03-23

### Performance
- Eliminate ~70 unnecessary sqrt ops per frame; constexpr reciprocals and cache optimizations
- Skip bone animation for LOD3 models; frustum-cull water surfaces
- Eliminate per-frame heap allocations in M2 renderer
- Convert entity/skill/DBC/warden maps to unordered_map; fix 3x contacts scan
- Eliminate double map lookups and dynamic_cast in render loops
- Use second GPU queue for parallel texture/buffer uploads
- Time-budget tile finalization to prevent 1+ second main-loop stalls
- Add Vulkan pipeline cache persistence for faster startup

### Bug Fixes
- Fix spline parsing with expansion context; preload DBC caches at world entry
- Fix NPC/player attack animation to use weapon-appropriate anim ID
- Fix equipment visibility and follow-target run speed
- Fix inspect (packed GUID) and client-side auto-walk for follow
- Fix mail money uint64, other-player cape textures, zone toast dedup, TCP_NODELAY
- Guard spline point loop against unsigned underflow; guard hexDecode/stoi/stof
- Fix infinite recursion in toLowerInPlace and operator precedence bugs
- Fix 3D audio coords for PLAY_OBJECT_SOUND; correct melee swing sound paths
- Prevent Vulkan sampler exhaustion crash; skip pipeline cache on NVIDIA
- Skip FSR3 frame gen on non-AMD GPUs to prevent driver crash
- Fix chest GO interaction (send GAMEOBJ_USE+LOOT together)
- Restore WMO wall collision threshold; fix off-screen bag positions
- Guard texture log dedup sets with mutex for thread safety
- Fix lua_pcall return check in ACTIONBAR_PAGE_CHANGED

### Features
- Render equipment on other players (helmets, weapons, belts, wrists, shoulders)
- Target frame right-click context menu
- Crafting sounds and Create All button
- Server-synced bag sort
- Log GPU vendor/name at init

### Security
- Add path traversal rejection and packet length validation

### Code Quality
- Packet API: add readPackedGuid, writePackedGuid, writeFloat, getRemainingSize,
  hasRemaining, hasData, skipAll (replacing 1300+ verbose expressions)
- GameHandler helpers: isInWorld, isPreWotlk, guidToUnitId, lookupName,
  getUnitByGuid, fireAddonEvent, withSoundManager
- Dispatch table: registerHandler, registerSkipHandler, registerWorldHandler,
  registerErrorHandler (replacing 120+ lambda wrappers)
- Shared ui_colors.hpp with named constants replacing 200+ inline color literals
- Promote 50+ static const arrays to constexpr across audio/core/rendering/UI
- Deduplicate class name/color functions, enchantment cache, item-set DBC keys
- Extract settings tabs, GameHandler::update() phases, loadWeaponM2 into methods
- Remove 12 duplicate dispatch registrations and C-style casts
- Extract toHexString, toLowerInPlace, duration formatting, Lua return helpers
