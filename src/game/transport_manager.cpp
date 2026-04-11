#include "game/transport_manager.hpp"
#include "game/transport_clock_sync.hpp"
#include "game/transport_animator.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <limits>

namespace wowee::game {

TransportManager::TransportManager() = default;
TransportManager::~TransportManager() = default;

void TransportManager::update(float deltaTime) {
    elapsedTime_ += deltaTime;

    for (auto& [guid, transport] : transports_) {
        // Once we have server clock offset, we can predict server time indefinitely
        // No need for watchdog - keep using the offset even if server updates stop
        updateTransportMovement(transport, deltaTime);
    }
}

void TransportManager::registerTransport(uint64_t guid, uint32_t wmoInstanceId, uint32_t pathId, const glm::vec3& spawnWorldPos, uint32_t entry) {
    auto* pathEntry = pathRepo_.findPath(pathId);
    if (!pathEntry) {
        LOG_ERROR("TransportManager: Path ", pathId, " not found for transport ", guid);
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        LOG_ERROR("TransportManager: Path ", pathId, " has no waypoints");
        return;
    }

    ActiveTransport transport;
    transport.guid = guid;
    transport.wmoInstanceId = wmoInstanceId;
    transport.pathId = pathId;
    transport.entry = entry;
    transport.allowBootstrapVelocity = false;

    // CRITICAL: Set basePosition from spawn position and t=0 offset
    // For stationary paths (1 waypoint), just use spawn position directly
    if (spline.durationMs() == 0 || spline.keyCount() <= 1) {
        // Stationary transport - no path animation
        transport.basePosition = spawnWorldPos;
        transport.position = spawnWorldPos;
    } else if (pathEntry->worldCoords) {
        // World-coordinate path (TaxiPathNode) - points are absolute world positions
        transport.basePosition = glm::vec3(0.0f);
        transport.position = spline.evaluatePosition(0);
    } else {
        // Moving transport - infer base from first path offset
        glm::vec3 offset0 = spline.evaluatePosition(0);
        transport.basePosition = spawnWorldPos - offset0;  // Infer base from spawn
        transport.position = spawnWorldPos;  // Start at spawn position (base + offset0)

        // TransportAnimation paths are local offsets; first waypoint is expected near origin.
        // Warn only if the local path itself looks suspicious.
        glm::vec3 firstWaypoint = spline.keys()[0].position;
        if (glm::dot(firstWaypoint, firstWaypoint) > 100.0f) {
            LOG_WARNING("Transport 0x", std::hex, guid, std::dec, " path ", pathId,
                        ": first local waypoint far from origin: (",
                        firstWaypoint.x, ",", firstWaypoint.y, ",", firstWaypoint.z, ")");
        }
    }

    transport.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);  // Identity quaternion
    transport.playerOnBoard = false;
    transport.playerLocalOffset = glm::vec3(0.0f);
    transport.hasDeckBounds = false;
    transport.localClockMs = 0;
    transport.hasServerClock = false;
    transport.serverClockOffsetMs = 0;
    // Start with client-side animation for all DBC paths with real movement.
    // If the server sends actual position updates, updateServerTransport() will switch
    // to server-driven mode. This ensures transports like trams (which the server doesn't
    // stream updates for) still animate, while ships/zeppelins switch to server authority.
    transport.useClientAnimation = (pathEntry->fromDBC && spline.durationMs() > 0);
    transport.clientAnimationReverse = false;
    transport.serverYaw = 0.0f;
    transport.hasServerYaw = false;
    transport.serverYawFlipped180 = false;
    transport.serverYawAlignmentScore = 0;
    transport.lastServerUpdate = 0.0f;
    transport.serverUpdateCount = 0;
    transport.serverLinearVelocity = glm::vec3(0.0f);
    transport.serverAngularVelocity = 0.0f;
    transport.hasServerVelocity = false;

    if (transport.useClientAnimation && spline.durationMs() > 0) {
        // Seed to a stable phase based on our local clock so elevators don't all start at t=0.
        transport.localClockMs = static_cast<uint32_t>(elapsedTime_ * 1000.0) % spline.durationMs();
        LOG_INFO("TransportManager: Enabled client animation for transport 0x",
                 std::hex, guid, std::dec, " path=", pathId,
                 " durationMs=", spline.durationMs(), " seedMs=", transport.localClockMs,
                 (pathEntry->worldCoords ? " [worldCoords]" : (pathEntry->zOnly ? " [z-only]" : "")));
    }

    updateTransformMatrices(transport);

    // CRITICAL: Update WMO renderer with initial transform
    pushTransform(transport);

    transports_[guid] = transport;

    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);
    LOG_INFO("TransportManager: Registered transport 0x", std::hex, guid, std::dec,
             " at path ", pathId, " with ", (pathEntry ? pathEntry->spline.keyCount() : 0u), " waypoints",
             " wmoInstanceId=", wmoInstanceId,
             " spawnPos=(", spawnWorldPos.x, ", ", spawnWorldPos.y, ", ", spawnWorldPos.z, ")",
             " basePos=(", transport.basePosition.x, ", ", transport.basePosition.y, ", ", transport.basePosition.z, ")",
             " initialRenderPos=(", renderPos.x, ", ", renderPos.y, ", ", renderPos.z, ")");
}

void TransportManager::unregisterTransport(uint64_t guid) {
    transports_.erase(guid);
    LOG_INFO("TransportManager: Unregistered transport ", guid);
}

ActiveTransport* TransportManager::getTransport(uint64_t guid) {
    auto it = transports_.find(guid);
    if (it != transports_.end()) {
        return &it->second;
    }
    return nullptr;
}

glm::vec3 TransportManager::getPlayerWorldPosition(uint64_t transportGuid, const glm::vec3& localOffset) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        LOG_WARNING("getPlayerWorldPosition: transport 0x", std::hex, transportGuid, std::dec,
                    " not found — returning localOffset as-is (callers should guard)");
        return localOffset;
    }

    if (transport->isM2) {
        // M2 transports (trams): localOffset is a canonical world-space delta
        // from the transport's canonical position. Just add directly.
        return transport->position + localOffset;
    }

    // WMO transports (ships): localOffset is in transport-local space,
    // use the render-space transform matrix.
    glm::vec4 localPos(localOffset, 1.0f);
    glm::vec4 worldPos = transport->transform * localPos;
    return glm::vec3(worldPos);
}

glm::mat4 TransportManager::getTransportInvTransform(uint64_t transportGuid) {
    auto* transport = getTransport(transportGuid);
    if (!transport) {
        return glm::mat4(1.0f);  // Identity fallback
    }
    return transport->invTransform;
}

void TransportManager::loadPathFromNodes(uint32_t pathId, const std::vector<glm::vec3>& waypoints, bool looping, float speed) {
    pathRepo_.loadPathFromNodes(pathId, waypoints, looping, speed);
}

void TransportManager::setDeckBounds(uint64_t guid, const glm::vec3& min, const glm::vec3& max) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_ERROR("TransportManager: Cannot set deck bounds for unknown transport ", guid);
        return;
    }

    transport->deckMin = min;
    transport->deckMax = max;
    transport->hasDeckBounds = true;
}

void TransportManager::updateTransportMovement(ActiveTransport& transport, float deltaTime) {
    auto* pathEntry = pathRepo_.findPath(transport.pathId);
    if (!pathEntry) {
        return;
    }

    const auto& spline = pathEntry->spline;
    if (spline.keyCount() == 0) {
        return;
    }

    // Stationary transport (durationMs = 0)
    if (spline.durationMs() == 0) {
        // Just update transform (position already set)
        updateTransformMatrices(transport);
        pushTransform(transport);
        return;
    }

    // Compute path time via ClockSync
    uint32_t pathTimeMs = 0;
    if (!clockSync_.computePathTime(transport, spline, elapsedTime_, deltaTime, pathTimeMs)) {
        // Strict server-authoritative mode: do not guess movement between server snapshots.
        updateTransformMatrices(transport);
        pushTransform(transport);
        return;
    }

    // Evaluate position + rotation via Animator
    animator_.evaluateAndApply(transport, *pathEntry, pathTimeMs);

    // Update transform matrices
    updateTransformMatrices(transport);
    pushTransform(transport);

    // Debug logging every 600 frames (~10 seconds at 60fps)
    static int debugFrameCount = 0;
    if (debugFrameCount++ % 600 == 0) {
        LOG_DEBUG("Transport 0x", std::hex, transport.guid, std::dec,
                 " pathTime=", pathTimeMs, "ms / ", spline.durationMs(), "ms",
                 " pos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")",
                 " mode=", (transport.useClientAnimation ? "client" : "server"),
                 " isM2=", transport.isM2);
    }
}

// Push transform to the appropriate renderer (WMO or M2).
void TransportManager::pushTransform(ActiveTransport& transport) {
    if (transport.isM2) {
        if (m2Renderer_) m2Renderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    } else {
        if (wmoRenderer_) wmoRenderer_->setInstanceTransform(transport.wmoInstanceId, transport.transform);
    }
}

void TransportManager::updateTransformMatrices(ActiveTransport& transport) {
    // Convert position from canonical to render coordinates for WMO rendering
    // Canonical: +X=North, +Y=West, +Z=Up
    // Render: renderX=wowY (west), renderY=wowX (north), renderZ=wowZ (up)
    glm::vec3 renderPos = core::coords::canonicalToRender(transport.position);

    // Convert rotation from canonical to render space using proper basis change
    // Canonical → Render is a 90° CCW rotation around Z (swaps X and Y)
    // Proper formula: q_render = q_basis * q_canonical * q_basis^-1
    glm::quat basisRotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    glm::quat basisInverse = glm::conjugate(basisRotation);
    glm::quat renderRot = basisRotation * transport.rotation * basisInverse;

    // Build transform matrix: translate * rotate * scale
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), renderPos);
    glm::mat4 rotation = glm::mat4_cast(renderRot);
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f));  // No scaling for transports

    transport.transform = translation * rotation * scale;
    transport.invTransform = glm::inverse(transport.transform);
}

void TransportManager::updateServerTransport(uint64_t guid, const glm::vec3& position, float orientation) {
    auto* transport = getTransport(guid);
    if (!transport) {
        LOG_WARNING("TransportManager::updateServerTransport: Transport not found: 0x", std::hex, guid, std::dec);
        return;
    }

    auto* pathEntry = pathRepo_.findPath(transport->pathId);

    if (!pathEntry || pathEntry->spline.durationMs() == 0) {
        // No path or stationary — handle directly before delegating to ClockSync.
        // Still track update count so future path assignments work.
        transport->serverUpdateCount++;
        transport->lastServerUpdate = elapsedTime_;
        transport->basePosition = position;
        transport->position = position;
        transport->rotation = glm::angleAxis(orientation, glm::vec3(0.0f, 0.0f, 1.0f));
        updateTransformMatrices(*transport);
        pushTransform(*transport);
        return;
    }

    // Delegate clock sync, yaw correction, and velocity bootstrap to ClockSync.
    clockSync_.processServerUpdate(*transport, pathEntry, position, orientation, elapsedTime_);

    updateTransformMatrices(*transport);
    pushTransform(*transport);
}

bool TransportManager::loadTransportAnimationDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTransportAnimationDBC(assetMgr);
}

bool TransportManager::loadTaxiPathNodeDBC(pipeline::AssetManager* assetMgr) {
    return pathRepo_.loadTaxiPathNodeDBC(assetMgr);
}

bool TransportManager::hasTaxiPath(uint32_t taxiPathId) const {
    return pathRepo_.hasTaxiPath(taxiPathId);
}

bool TransportManager::assignTaxiPathToTransport(uint32_t entry, uint32_t taxiPathId) {
    auto* taxiEntry = pathRepo_.findTaxiPath(taxiPathId);
    if (!taxiEntry) {
        LOG_WARNING("No TaxiPathNode path for taxiPathId=", taxiPathId);
        return false;
    }

    // Find transport(s) with matching entry that are at (0,0,0)
    for (auto& [guid, transport] : transports_) {
        if (transport.entry != entry) continue;
        if (glm::dot(transport.position, transport.position) > 1.0f) continue;  // Already has real position

        // Copy the taxi path into the main paths (indexed by GO entry for this transport)
        PathEntry copied(taxiEntry->spline, entry, taxiEntry->zOnly, taxiEntry->fromDBC, taxiEntry->worldCoords);
        pathRepo_.storePath(entry, std::move(copied));

        auto* storedEntry = pathRepo_.findPath(entry);

        // Update transport to use the new path
        transport.pathId = entry;
        transport.basePosition = glm::vec3(0.0f);  // World-coordinate path, no base offset
        if (storedEntry && storedEntry->spline.keyCount() > 0) {
            transport.position = storedEntry->spline.evaluatePosition(0);
        }
        transport.useClientAnimation = true;  // Server won't send position updates

        // Seed local clock to a deterministic phase
        if (storedEntry && storedEntry->spline.durationMs() > 0) {
            transport.localClockMs = static_cast<uint32_t>(elapsedTime_ * 1000.0) % storedEntry->spline.durationMs();
        }

        updateTransformMatrices(transport);
        pushTransform(transport);

        LOG_INFO("Assigned TaxiPathNode path to transport 0x", std::hex, guid, std::dec,
                 " entry=", entry, " taxiPathId=", taxiPathId,
                 " waypoints=", storedEntry ? storedEntry->spline.keyCount() : 0u,
                 " duration=", storedEntry ? storedEntry->spline.durationMs() : 0u, "ms",
                 " startPos=(", transport.position.x, ", ", transport.position.y, ", ", transport.position.z, ")");
        return true;
    }

    LOG_DEBUG("No transport at (0,0,0) found for entry=", entry, " taxiPathId=", taxiPathId);
    return false;
}

bool TransportManager::hasPathForEntry(uint32_t entry) const {
    return pathRepo_.hasPathForEntry(entry);
}

bool TransportManager::hasUsableMovingPathForEntry(uint32_t entry, float minXYRange) const {
    return pathRepo_.hasUsableMovingPathForEntry(entry, minXYRange);
}

uint32_t TransportManager::inferDbcPathForSpawn(const glm::vec3& spawnWorldPos,
                                               float maxDistance,
                                               bool allowZOnly) const {
    return pathRepo_.inferDbcPathForSpawn(spawnWorldPos, maxDistance, allowZOnly);
}

uint32_t TransportManager::inferMovingPathForSpawn(const glm::vec3& spawnWorldPos, float maxDistance) const {
    return pathRepo_.inferMovingPathForSpawn(spawnWorldPos, maxDistance);
}

uint32_t TransportManager::pickFallbackMovingPath(uint32_t entry, uint32_t displayId) const {
    return pathRepo_.pickFallbackMovingPath(entry, displayId);
}

} // namespace wowee::game
