# Project Status

**Last updated**: 2026-03-18

## What This Repo Is

Wowee is a native C++ World of Warcraft client experiment focused on connecting to real emulator servers (online/multiplayer) with a custom renderer and asset pipeline.

## Current Code State

Implemented (working in normal use):

- Auth flow: SRP6a auth + realm list + world connect with header encryption
- Rendering: terrain, WMO/M2, water/magma/slime (FBM noise shaders), sky system, particles, shadow mapping, minimap/world map, loading video playback
- Instances: WDT parser, WMO-only dungeon maps, area trigger portals with glow/spin effects, zone transitions
- Character system: creation (including nonbinary gender), selection, 3D preview with equipment, character screen, per-instance NPC hair/skin textures
- Core gameplay: movement (with ACK responses), targeting (hostility-filtered tab-cycle), combat, action bar, inventory/equipment, chat (tabs/channels, emotes, item links)
- Quests: quest markers (! and ?) on NPCs/minimap, quest log with detail queries/retry, objective tracking, accept/complete flow, turn-in, quest item progress
- Trainers: spell trainer UI, buy spells, known/available/unavailable states
- Vendors, loot (including chest/gameobject loot), gossip dialogs (including buyback for most recently sold item)
- Bank: full bank support for all expansions, bag slots, drag-drop, right-click deposit
- Auction house: search with filters, pagination, sell picker, bid/buyout, tooltips
- Mail: item attachment support for sending
- Spellbook with specialty/general/profession/mount/companion tabs, drag-drop to action bar, spell icons, item use
- Talent tree UI with proper visuals and functionality
- Pet tracking (SMSG_PET_SPELLS), dismiss pet button
- Party: group invites, party list, out-of-range member health (SMSG_PARTY_MEMBER_STATS)
- Nameplates: NPC subtitles, guild names, elite/boss/rare borders, quest/raid indicators, cast bars, debuff dots
- Floating combat text: world-space damage/heal numbers above entities with 3D projection
- Target/focus frames: guild name, creature type, rank badges, combo points, cast bars
- Map exploration: subzone-level fog-of-war reveal
- Warden anti-cheat: full module execution via Unicorn Engine x86 emulation; module caching
- Audio: ambient, movement, combat, spell, and UI sound systems
- Bag UI: separate bag windows, open-bag indicator on bag bar, optional collapse-empty mode in aggregate bag view
- Multi-expansion: Classic/Vanilla, TBC, WotLK, and Turtle WoW (1.17) protocol and asset variants
- CI: GitHub Actions for Linux (x86-64, ARM64), Windows (MSYS2), macOS (ARM64); container builds via Podman

In progress / known gaps:

- Transports: M2 transports (trams) working with position-delta riding; WMO transports (ships, zeppelins) working with path following; some edge cases remain
- Visual edge cases: some M2/WMO rendering gaps (some particle effects)
- Lava steam particles: sparse in some areas (tuning opportunity)
- Water refraction: enabled by default; srcAccessMask barrier fix (2026-03-18) resolved prior VK_ERROR_DEVICE_LOST on AMD/Mali GPUs

## Where To Look

- Entry point: `src/main.cpp`, `src/core/application.cpp`
- Networking/auth: `src/auth/`, `src/network/`, `src/game/game_handler.cpp`
- Rendering: `src/rendering/`
- Assets/extraction: `extract_assets.sh`, `tools/asset_extract/`, `src/pipeline/asset_manager.cpp`
