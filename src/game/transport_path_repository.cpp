// src/game/transport_path_repository.cpp
// Owns and manages transport path data — DBC, taxi, and custom paths.
// Ported from TransportManager (path management subset).
#include "game/transport_path_repository.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include <algorithm>
#include <map>
#include <cmath>

namespace wowee::game {

// ── Simple lookup methods ──────────────────────────────────────

const PathEntry* TransportPathRepository::findPath(uint32_t pathId) const {
    auto it = paths_.find(pathId);
    return it != paths_.end() ? &it->second : nullptr;
}

const PathEntry* TransportPathRepository::findTaxiPath(uint32_t taxiPathId) const {
    auto it = taxiPaths_.find(taxiPathId);
    return it != taxiPaths_.end() ? &it->second : nullptr;
}

bool TransportPathRepository::hasPathForEntry(uint32_t entry) const {
    auto* e = findPath(entry);
    return e != nullptr && e->fromDBC;
}

bool TransportPathRepository::hasTaxiPath(uint32_t taxiPathId) const {
    return taxiPaths_.find(taxiPathId) != taxiPaths_.end();
}

void TransportPathRepository::storePath(uint32_t pathId, PathEntry entry) {
    auto it = paths_.find(pathId);
    if (it != paths_.end()) {
        it->second = std::move(entry);
    } else {
        paths_.emplace(pathId, std::move(entry));
    }
}

// ── Query methods ──────────────────────────────────────────────

bool TransportPathRepository::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    auto* e = findPath(entry);
    if (!e) return false;
    if (!e->fromDBC || e->spline.keyCount() < 2 || e->spline.durationMs() == 0 || e->zOnly) {
        return false;
    }
    return e->spline.hasXYMovement(minXYRange);
}

uint32_t TransportPathRepository::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                                        float maxDistance,
                                                        bool allowZOnly) const {
    float bestD2 = maxDistance * maxDistance;
    uint32_t bestPathId = 0;

    for (const auto& [pathId, entry] : paths_) {
        if (!entry.fromDBC || entry.spline.durationMs() == 0 || entry.spline.keyCount() == 0) {
            continue;
        }
        if (!allowZOnly && entry.zOnly) {
            continue;
        }

        // Find nearest waypoint on this path to spawn
        size_t nearIdx = entry.spline.findNearestKey(spawnWorldPos);
        glm::vec3 diff = entry.spline.keys()[nearIdx].position - spawnWorldPos;
        float d2 = glm::dot(diff, diff);
        if (d2 < bestD2) {
            bestD2 = d2;
            bestPathId = pathId;
        }
    }

    if (bestPathId != 0) {
        LOG_INFO("TransportPathRepository: Inferred DBC path ", bestPathId,
                 " (allowZOnly=", allowZOnly ? "yes" : "no",
                 ") for spawn at (", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z,
                 "), dist=", std::sqrt(bestD2));
    }

    return bestPathId;
}

uint32_t TransportPathRepository::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return inferDbcPathForSpawn(spawnWorldPos, maxDistance, /*allowZOnly=*/false);
}

uint32_t TransportPathRepository::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    auto isUsableMovingPath = [this](uint32_t pathId) -> bool {
        auto* e = findPath(pathId);
        if (!e) return false;
        return e->fromDBC && !e->zOnly && e->spline.durationMs() > 0 && e->spline.keyCount() > 1;
    };

    // Known AzerothCore transport entry remaps (WotLK): server entry -> moving DBC path id.
    // These entries commonly do not match TransportAnimation.dbc ids 1:1.
    static const std::unordered_map<uint32_t, uint32_t> kEntryRemap = {
        {176231u, 176080u}, // The Maiden's Fancy
        {176310u, 176081u}, // The Bravery
        {20808u,  176082u}, // The Black Princess
        {164871u, 193182u}, // The Thundercaller
        {176495u, 193183u}, // The Purple Princess
        {175080u, 193182u}, // The Iron Eagle
        {181689u, 193183u}, // Cloudkisser
        {186238u, 193182u}, // The Mighty Wind
        {181688u, 176083u}, // Northspear (icebreaker)
        {190536u, 176084u}, // Stormwind's Pride (icebreaker)
    };

    auto itMapped = kEntryRemap.find(entry);
    if (itMapped != kEntryRemap.end() && isUsableMovingPath(itMapped->second)) {
        return itMapped->second;
    }

    // Fallback by display model family.
    const bool looksLikeShip =
        (displayId == 3015u || displayId == 2454u || displayId == 7446u);
    const bool looksLikeZeppelin =
        (displayId == 3031u || displayId == 7546u || displayId == 1587u || displayId == 807u || displayId == 808u);

    if (looksLikeShip) {
        static constexpr uint32_t kShipCandidates[] = {176080u, 176081u, 176082u, 176083u, 176084u, 176085u, 194675u};
        for (uint32_t id : kShipCandidates) {
            if (isUsableMovingPath(id)) return id;
        }
    }

    if (looksLikeZeppelin) {
        static constexpr uint32_t kZeppelinCandidates[] = {193182u, 193183u, 188360u, 190587u};
        for (uint32_t id : kZeppelinCandidates) {
            if (isUsableMovingPath(id)) return id;
        }
    }

    // Last-resort: pick any moving DBC path so transport does not remain stationary.
    for (const auto& [pathId, e] : paths_) {
        if (e.fromDBC && !e.zOnly && e.spline.durationMs() > 0 && e.spline.keyCount() > 1) {
            return pathId;
        }
    }

    return 0;
}

// ── Path construction from waypoints ───────────────────────────

void TransportPathRepository::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    if (waypoints.empty()) {
        LOG_ERROR("TransportPathRepository: Cannot load empty path ", pathId);
        return;
    }

    bool isZOnly = false;  // Manually loaded paths are assumed to have XY movement

    // Helper: compute segment duration from distance and speed
    auto segMsFromDist = [&](float dist) -> uint32_t {
        if (speed <= 0.0f) return 1000;
        return static_cast<uint32_t>((dist / speed) * 1000.0f);
    };

    // Single point = stationary (durationMs = 0)
    if (waypoints.size() == 1) {
        std::vector<math::SplineKey> keys;
        keys.push_back({0, waypoints[0]});
        math::CatmullRomSpline spline(std::move(keys), false);
        paths_.emplace(pathId, PathEntry(std::move(spline), pathId, isZOnly, false, false));
        LOG_INFO("TransportPathRepository: Loaded stationary path ", pathId);
        return;
    }

    // Multiple points: calculate cumulative time based on distance and speed
    std::vector<math::SplineKey> keys;
    keys.reserve(waypoints.size() + (looping ? 1 : 0));
    uint32_t cumulativeMs = 0;
    keys.push_back({0, waypoints[0]});

    for (size_t i = 1; i < waypoints.size(); i++) {
        float dist = glm::distance(waypoints[i-1], waypoints[i]);
        cumulativeMs += glm::max(1u, segMsFromDist(dist));
        keys.push_back({cumulativeMs, waypoints[i]});
    }

    // Add explicit wrap segment (last → first) for looping paths.
    // By duplicating the first point at the end with cumulative time, the path
    // becomes time-closed and CatmullRomSpline handles wrap via modular time
    // without requiring special-case index wrapping during evaluation.
    if (looping && waypoints.size() >= 2) {
        float wrapDist = glm::distance(waypoints.back(), waypoints.front());
        cumulativeMs += glm::max(1u, segMsFromDist(wrapDist));
        keys.push_back({cumulativeMs, waypoints[0]});
    }

    math::CatmullRomSpline spline(std::move(keys), false);
    paths_.emplace(pathId, PathEntry(std::move(spline), pathId, isZOnly, false, false));

    auto* stored = findPath(pathId);
    LOG_INFO("TransportPathRepository: Loaded path ", pathId,
             " with ", waypoints.size(), " waypoints",
             (looping ? " + wrap segment" : ""),
             ", duration=", stored ? stored->spline.durationMs() : 0, "ms, speed=", speed);
}

// ── DBC: TransportAnimation ────────────────────────────────────

bool TransportPathRepository::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    LOG_INFO("Loading TransportAnimation.dbc...");

    if (!assetMgr) {
        LOG_ERROR("AssetManager is null");
        return false;
    }

    // Load DBC file
    auto dbcData = assetMgr->readFile("DBFilesClient\\TransportAnimation.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("TransportAnimation.dbc not found - transports will use fallback paths");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData)) {
        LOG_ERROR("Failed to parse TransportAnimation.dbc");
        return false;
    }

    LOG_INFO("TransportAnimation.dbc: ", dbc.getRecordCount(), " records, ",
             dbc.getFieldCount(), " fields per record");

    // Debug: dump first 3 records to see all field values
    for (uint32_t i = 0; i < std::min(3u, dbc.getRecordCount()); i++) {
        LOG_INFO("  DEBUG Record ", i, ": ",
                 " [0]=", dbc.getUInt32(i, 0),
                 " [1]=", dbc.getUInt32(i, 1),
                 " [2]=", dbc.getUInt32(i, 2),
                 " [3]=", dbc.getFloat(i, 3),
                 " [4]=", dbc.getFloat(i, 4),
                 " [5]=", dbc.getFloat(i, 5),
                 " [6]=", dbc.getUInt32(i, 6));
    }

    // Group waypoints by transportEntry
    std::map<uint32_t, std::vector<std::pair<uint32_t, glm::vec3>>> waypointsByTransport;

    for (uint32_t i = 0; i < dbc.getRecordCount(); i++) {
        // uint32_t id = dbc.getUInt32(i, 0);  // Not needed
        uint32_t transportEntry = dbc.getUInt32(i, 1);
        uint32_t timeIndex = dbc.getUInt32(i, 2);
        float posX = dbc.getFloat(i, 3);
        float posY = dbc.getFloat(i, 4);
        float posZ = dbc.getFloat(i, 5);
        // uint32_t sequenceId = dbc.getUInt32(i, 6);  // Not needed for basic paths

        // RAW FLOAT SANITY CHECK: Log first 10 records to see if DBC has real data
        if (i < 10) {
            uint32_t ux = dbc.getUInt32(i, 3);
            uint32_t uy = dbc.getUInt32(i, 4);
            uint32_t uz = dbc.getUInt32(i, 5);
            LOG_INFO("TA raw rec ", i,
                     " entry=", transportEntry,
                     " t=", timeIndex,
                     " raw=(", posX, ",", posY, ",", posZ, ")",
                     " u32=(", ux, ",", uy, ",", uz, ")");
        }

        // DIAGNOSTIC: Log ALL records for problematic ferries (20655, 20657, 149046)
        // AND first few records for known-good transports to verify DBC reading
        if (i < 5 || transportEntry == 2074 ||
            transportEntry == 20655 || transportEntry == 20657 || transportEntry == 149046) {
            LOG_INFO("RAW DBC [", i, "] entry=", transportEntry, " t=", timeIndex,
                     " raw=(", posX, ",", posY, ",", posZ, ")");
        }

        waypointsByTransport[transportEntry].push_back({timeIndex, glm::vec3(posX, posY, posZ)});
    }

    // Create time-indexed paths from waypoints
    int pathsLoaded = 0;
    for (const auto& [transportEntry, waypoints] : waypointsByTransport) {
        if (waypoints.empty()) continue;

        // Sort by timeIndex
        auto sortedWaypoints = waypoints;
        std::sort(sortedWaypoints.begin(), sortedWaypoints.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        // CRITICAL: Normalize timeIndex to start at 0 (DBC records don't start at 0!)
        // This makes evaluatePosition(0) valid and stabilizes basePosition seeding
        uint32_t t0 = sortedWaypoints.front().first;

        // Build SplineKey array with normalized time indices
        std::vector<math::SplineKey> keys;
        keys.reserve(sortedWaypoints.size() + 1);  // +1 for wrap point

        // Log DBC waypoints for tram entries
        if (transportEntry >= 176080 && transportEntry <= 176085) {
            size_t mid = sortedWaypoints.size() / 4;  // ~quarter through
            size_t mid2 = sortedWaypoints.size() / 2; // ~halfway
            LOG_DEBUG("DBC path entry=", transportEntry, " nPts=", sortedWaypoints.size(),
                       " [0] t=", sortedWaypoints[0].first, " raw=(", sortedWaypoints[0].second.x, ",", sortedWaypoints[0].second.y, ",", sortedWaypoints[0].second.z, ")",
                       " [", mid, "] t=", sortedWaypoints[mid].first, " raw=(", sortedWaypoints[mid].second.x, ",", sortedWaypoints[mid].second.y, ",", sortedWaypoints[mid].second.z, ")",
                       " [", mid2, "] t=", sortedWaypoints[mid2].first, " raw=(", sortedWaypoints[mid2].second.x, ",", sortedWaypoints[mid2].second.y, ",", sortedWaypoints[mid2].second.z, ")");
        }

        for (size_t idx = 0; idx < sortedWaypoints.size(); idx++) {
            const auto& [tMs, pos] = sortedWaypoints[idx];

            // TransportAnimation.dbc local offsets use a coordinate system where
            // the travel axis is negated relative to server world coords.
            // Negate X and Y before converting to canonical (Z=height stays the same).
            glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(-pos.x, -pos.y, pos.z));

            // CRITICAL: Detect if serverToCanonical is zeroing nonzero inputs
            if ((pos.x != 0.0f || pos.y != 0.0f || pos.z != 0.0f) &&
                (canonical.x == 0.0f && canonical.y == 0.0f && canonical.z == 0.0f)) {
                LOG_ERROR("serverToCanonical ZEROED! entry=", transportEntry,
                          " server=(", pos.x, ",", pos.y, ",", pos.z, ")",
                          " → canon=(", canonical.x, ",", canonical.y, ",", canonical.z, ")");
            }

            // Debug waypoint conversion for first transport (entry 2074)
            if (transportEntry == 2074 && idx < 5) {
                LOG_INFO("COORD CONVERT: entry=", transportEntry, " t=", tMs,
                         " serverPos=(", pos.x, ", ", pos.y, ", ", pos.z, ")",
                         " → canonical=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ")");
            }

            // DIAGNOSTIC: Log ALL conversions for problematic ferries
            if (transportEntry == 20655 || transportEntry == 20657 || transportEntry == 149046) {
                LOG_INFO("CONVERT ", transportEntry, " t=", tMs,
                         " server=(", pos.x, ",", pos.y, ",", pos.z, ")",
                         " → canon=(", canonical.x, ",", canonical.y, ",", canonical.z, ")");
            }

            keys.push_back({tMs - t0, canonical});  // Normalize: subtract first timeIndex
        }

        // Get base duration from last normalized timeIndex
        uint32_t lastTimeMs = sortedWaypoints.back().first - t0;

        // Calculate wrap duration (last → first segment)
        // Use average segment duration as wrap duration
        uint32_t totalDelta = 0;
        int segmentCount = 0;
        for (size_t i = 1; i < sortedWaypoints.size(); i++) {
            uint32_t delta = sortedWaypoints[i].first - sortedWaypoints[i-1].first;
            if (delta > 0) {
                totalDelta += delta;
                segmentCount++;
            }
        }
        uint32_t wrapMs = (segmentCount > 0) ? (totalDelta / segmentCount) : 1000;

        // Add duplicate first point at end with wrap duration
        // This makes the wrap segment (last → first) have proper duration
        const auto& fp = sortedWaypoints.front().second;
        glm::vec3 firstCanonical = core::coords::serverToCanonical(glm::vec3(-fp.x, -fp.y, fp.z));
        keys.push_back({lastTimeMs + wrapMs, firstCanonical});

        // Build the spline (time-closed=false because we added explicit wrap point)
        math::CatmullRomSpline spline(std::move(keys), false);

        // Detect Z-only paths (elevator/bobbing animation, not real XY travel)
        const auto& sk = spline.keys();
        float minX = sk[0].position.x, maxX = minX;
        float minY = sk[0].position.y, maxY = minY;
        float minZ = sk[0].position.z, maxZ = minZ;
        for (const auto& k : sk) {
            minX = std::min(minX, k.position.x); maxX = std::max(maxX, k.position.x);
            minY = std::min(minY, k.position.y); maxY = std::max(maxY, k.position.y);
            minZ = std::min(minZ, k.position.z); maxZ = std::max(maxZ, k.position.z);
        }
        float rangeX = maxX - minX;
        float rangeY = maxY - minY;
        float rangeZ = maxZ - minZ;
        float rangeXY = std::max(rangeX, rangeY);
        // Some elevator paths have tiny XY jitter. Treat them as z-only when horizontal travel
        // is negligible compared to vertical motion.
        bool isZOnly = (rangeXY < 0.01f) || (rangeXY < 1.0f && rangeZ > 2.0f);

        // Log first, middle, and last points to verify path data
        glm::vec3 firstOffset = sk[0].position;
        size_t midIdx = sk.size() / 2;
        glm::vec3 midOffset = sk[midIdx].position;
        glm::vec3 lastOffset = sk[sk.size() - 2].position;  // -2 to skip wrap duplicate
        uint32_t durationMs = spline.durationMs();
        LOG_INFO("  Transport ", transportEntry, ": ", sk.size() - 1, " waypoints + wrap, ",
                 durationMs, "ms duration (wrap=", wrapMs, "ms, t0_normalized=", sk[0].timeMs, "ms)",
                 " rangeXY=(", rangeX, ",", rangeY, ") rangeZ=", rangeZ, " ",
                 (isZOnly ? "[Z-ONLY]" : "[XY-PATH]"),
                 " firstOffset=(", firstOffset.x, ", ", firstOffset.y, ", ", firstOffset.z, ")",
                 " midOffset=(", midOffset.x, ", ", midOffset.y, ", ", midOffset.z, ")",
                 " lastOffset=(", lastOffset.x, ", ", lastOffset.y, ", ", lastOffset.z, ")");

        // Store path
        paths_.emplace(transportEntry, PathEntry(std::move(spline), transportEntry, isZOnly, true, false));
        pathsLoaded++;
    }

    LOG_INFO("Loaded ", pathsLoaded, " transport paths from TransportAnimation.dbc");
    return pathsLoaded > 0;
}

// ── DBC: TaxiPathNode ──────────────────────────────────────────

bool TransportPathRepository::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    LOG_INFO("Loading TaxiPathNode.dbc...");

    if (!assetMgr) {
        LOG_ERROR("AssetManager is null");
        return false;
    }

    auto dbcData = assetMgr->readFile("DBFilesClient\\TaxiPathNode.dbc");
    if (dbcData.empty()) {
        LOG_WARNING("TaxiPathNode.dbc not found - MO_TRANSPORT will use fallback paths");
        return false;
    }

    pipeline::DBCFile dbc;
    if (!dbc.load(dbcData)) {
        LOG_ERROR("Failed to parse TaxiPathNode.dbc");
        return false;
    }

    LOG_INFO("TaxiPathNode.dbc: ", dbc.getRecordCount(), " records, ",
             dbc.getFieldCount(), " fields per record");

    // Group nodes by PathID, storing (NodeIndex, MapID, X, Y, Z)
    struct TaxiNode {
        uint32_t nodeIndex;
        uint32_t mapId;
        float x, y, z;
    };
    std::map<uint32_t, std::vector<TaxiNode>> nodesByPath;

    for (uint32_t i = 0; i < dbc.getRecordCount(); i++) {
        uint32_t pathId = dbc.getUInt32(i, 1);    // PathID
        uint32_t nodeIdx = dbc.getUInt32(i, 2);   // NodeIndex
        uint32_t mapId = dbc.getUInt32(i, 3);     // MapID
        float posX = dbc.getFloat(i, 4);          // X (server coords)
        float posY = dbc.getFloat(i, 5);          // Y (server coords)
        float posZ = dbc.getFloat(i, 6);          // Z (server coords)

        nodesByPath[pathId].push_back({nodeIdx, mapId, posX, posY, posZ});
    }

    // Build world-coordinate transport paths
    int pathsLoaded = 0;
    for (auto& [pathId, nodes] : nodesByPath) {
        if (nodes.size() < 2) continue;

        // Sort by NodeIndex
        std::sort(nodes.begin(), nodes.end(),
                  [](const TaxiNode& a, const TaxiNode& b) { return a.nodeIndex < b.nodeIndex; });

        // Skip flight-master paths (nodes on different maps are map teleports)
        // Transport paths stay on the same map
        bool sameMap = true;
        uint32_t firstMap = nodes[0].mapId;
        for (const auto& node : nodes) {
            if (node.mapId != firstMap) { sameMap = false; break; }
        }
        if (!sameMap) continue;

        // Build timed points using distance-based timing (28 units/sec default boat speed)
        const float transportSpeed = 28.0f;  // units per second
        std::vector<math::SplineKey> keys;
        keys.reserve(nodes.size() + 1);

        uint32_t cumulativeMs = 0;
        for (size_t i = 0; i < nodes.size(); i++) {
            // Convert server coords to canonical
            glm::vec3 serverPos(nodes[i].x, nodes[i].y, nodes[i].z);
            glm::vec3 canonical = core::coords::serverToCanonical(serverPos);

            keys.push_back({cumulativeMs, canonical});

            if (i + 1 < nodes.size()) {
                float dx = nodes[i+1].x - nodes[i].x;
                float dy = nodes[i+1].y - nodes[i].y;
                float dz = nodes[i+1].z - nodes[i].z;
                float segDist = std::sqrt(dx*dx + dy*dy + dz*dz);
                uint32_t segMs = static_cast<uint32_t>((segDist / transportSpeed) * 1000.0f);
                if (segMs < 100) segMs = 100;  // Minimum 100ms per segment
                cumulativeMs += segMs;
            }
        }

        // Add wrap point (return to start) for looping
        float wrapDx = nodes.front().x - nodes.back().x;
        float wrapDy = nodes.front().y - nodes.back().y;
        float wrapDz = nodes.front().z - nodes.back().z;
        float wrapDist = std::sqrt(wrapDx*wrapDx + wrapDy*wrapDy + wrapDz*wrapDz);
        uint32_t wrapMs = static_cast<uint32_t>((wrapDist / transportSpeed) * 1000.0f);
        if (wrapMs < 100) wrapMs = 100;
        cumulativeMs += wrapMs;
        keys.push_back({cumulativeMs, keys[0].position});

        math::CatmullRomSpline spline(std::move(keys), false);
        taxiPaths_.emplace(pathId, PathEntry(std::move(spline), pathId, false, true, true));
        pathsLoaded++;
    }

    LOG_INFO("Loaded ", pathsLoaded, " TaxiPathNode transport paths (", nodesByPath.size(), " total taxi paths)");
    return pathsLoaded > 0;
}

} // namespace wowee::game
