# Contributing to Wowee

## Build Setup

See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) for full platform-specific details.
The short version: CMake + Make on Linux/macOS, MSYS2 on Windows.

```
cmake -B build -DCMAKE_BUILD_TYPE=Debug
make -C build -j$(nproc)
```

## Code Style

- **C++20**. Use `#pragma once` for include guards.
- Namespaces: `wowee::game`, `wowee::rendering`, `wowee::ui`, `wowee::core`, `wowee::network`.
- Conventional commit messages in imperative mood:
  - `feat:` new feature
  - `fix:` bug fix
  - `refactor:` code restructuring with no behavior change
  - `perf:` performance improvement
- Prefer `constexpr` over `static const` for compile-time data.
- Mark functions whose return value should not be ignored with `[[nodiscard]]`.

## Pull Request Process

1. Branch from `master`.
2. Keep commits focused -- one logical change per commit.
3. Describe *what* changed and *why* in the PR description.
4. Ensure the project compiles cleanly before submitting.
5. Manual testing against a WoW 3.3.5a server (e.g. AzerothCore/ChromieCraft) is expected
   for gameplay-affecting changes.

## Architecture Overview

See [docs/architecture.md](docs/architecture.md) for the full picture. Key namespaces:

| Namespace | Responsibility |
|---|---|
| `wowee::game` | Game state, packet handling (`GameHandler`), opcode dispatch |
| `wowee::rendering` | Vulkan renderer, M2/WMO/terrain, sky system |
| `wowee::ui` | ImGui windows and HUD (`GameScreen`) |
| `wowee::core` | Coordinates, math, utilities |
| `wowee::network` | Connection, `Packet` read/write API |

## Packet Handlers

The standard pattern for adding a new server packet handler:

1. Define a `struct FooData` holding the parsed fields.
2. Write `void GameHandler::handleFoo(network::Packet& packet)` to parse into `FooData`.
3. Register it in the dispatch table: `registerHandler(LogicalOpcode::SMSG_FOO, &GameHandler::handleFoo)`.

Helper variants: `registerWorldHandler` (requires `isInWorld()`), `registerSkipHandler` (discard),
`registerErrorHandler` (log warning).

## Testing

There is no automated test suite. Changes are verified by manual testing against
WoW 3.3.5a private servers (primarily ChromieCraft/AzerothCore). Classic and TBC
expansion paths are tested against their respective server builds.

## Key Files for New Contributors

| File | What it does |
|---|---|
| `include/game/game_handler.hpp` | Central game state and all packet handler declarations |
| `src/game/game_handler.cpp` | Packet dispatch registration and handler implementations |
| `include/network/packet.hpp` | `Packet` class -- the read/write API every handler uses |
| `include/ui/game_screen.hpp` | Main gameplay UI screen (ImGui) |
| `src/rendering/m2_renderer.cpp` | M2 model loading and rendering |
| `docs/architecture.md` | High-level system architecture reference |
