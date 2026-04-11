// include/game/transport_path_repository.hpp
// Owns and manages transport path data — DBC, taxi, and custom paths.
// Uses CatmullRomSpline for spline evaluation (replaces duplicated evalTimedCatmullRom).
// Separated from TransportManager for SOLID-S (single responsibility).
#pragma once

#include "math/spline.hpp"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

namespace wowee::pipeline {
    class AssetManager;
}

namespace wowee::game {

/// Metadata + CatmullRomSpline for a transport path.
struct PathEntry {
    math::CatmullRomSpline spline;
    uint32_t pathId = 0;
    bool zOnly = false;       // Elevator/bobbing — no meaningful XY travel
    bool fromDBC = false;     // Loaded from TransportAnimation.dbc
    bool worldCoords = false; // TaxiPathNode absolute world positions (not local offsets)

    PathEntry(math::CatmullRomSpline s, uint32_t id, bool zo, bool dbc, bool wc)
        : spline(std::move(s)), pathId(id), zOnly(zo), fromDBC(dbc), worldCoords(wc) {}
};

/// Owns and manages transport path data.
class TransportPathRepository {
public:
    TransportPathRepository() = default;

    // ── DBC loading ─────────────────────────────────────────
    bool loadTransportAnimationDBC(pipeline::AssetManager* assetMgr);
    bool loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr);

    // ── Path construction ───────────────────────────────────
    void loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints,
                           bool looping = true, float speed = 18.0f);

    // ── Lookup ──────────────────────────────────────────────
    const PathEntry* findPath(uint32_t pathId) const;
    const PathEntry* findTaxiPath(uint32_t taxiPathId) const;
    bool hasPathForEntry(uint32_t entry) const;
    bool hasTaxiPath(uint32_t taxiPathId) const;

    // ── Query ───────────────────────────────────────────────
    bool hasUsableMovingPathForEntry(uint32_t entry, float minXYRange = 1.0f) const;
    uint32_t inferDbcPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance,
                                  bool allowZOnly) const;
    uint32_t inferMovingPathForSpawn(const glm::vec3& spawnWorldPos,
                                     float maxDistance = 1200.0f) const;
    uint32_t pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const;

    // ── Mutation ─────────────────────────────────────────────
    /// Store or overwrite a path entry (used by assignTaxiPathToTransport).
    void storePath(uint32_t pathId, PathEntry entry);

private:
    std::unordered_map<uint32_t, PathEntry> paths_;
    std::unordered_map<uint32_t, PathEntry> taxiPaths_;
};

} // namespace wowee::game
