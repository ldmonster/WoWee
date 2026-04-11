// src/game/transport_animator.cpp
// Path evaluation, Z clamping, and orientation for transports.
// Extracted from TransportManager::updateTransportMovement (Phase 3b of spline refactoring).
#include "game/transport_animator.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "math/spline.hpp"
#include <glm/gtc/constants.hpp>
#include <algorithm>

namespace wowee::game {

void TransportAnimator::evaluateAndApply(
    ActiveTransport& transport,
    const PathEntry& pathEntry,
    uint32_t pathTimeMs) const
{
    const auto& spline = pathEntry.spline;

    // Evaluate position from time via CatmullRomSpline (path is local offsets, add base position)
    glm::vec3 pathOffset = spline.evaluatePosition(pathTimeMs);

    pathOffset.z = clampZOffset(
        pathOffset.z,
        pathEntry.worldCoords,
        transport.useClientAnimation,
        transport.serverUpdateCount,
        transport.hasServerClock);

    transport.position = transport.basePosition + pathOffset;

    // Use server yaw if available (authoritative), otherwise compute from spline tangent
    if (transport.hasServerYaw) {
        float effectiveYaw = transport.serverYaw +
            (transport.serverYawFlipped180 ? glm::pi<float>() : 0.0f);
        transport.rotation = glm::angleAxis(effectiveYaw, glm::vec3(0.0f, 0.0f, 1.0f));
    } else {
        auto result = spline.evaluate(pathTimeMs);
        transport.rotation = math::CatmullRomSpline::orientationFromTangent(result.tangent);
    }
}

float TransportAnimator::clampZOffset(float z, bool worldCoords, bool clientAnim,
                                       int serverUpdateCount, bool hasServerClock)
{
    // Skip Z clamping for world-coordinate paths (TaxiPathNode) where values are absolute positions.
    if (worldCoords) return z;

    constexpr float kMinFallbackZOffset = -2.0f;
    constexpr float kMaxFallbackZOffset =  8.0f;

    // Clamp fallback Z offsets for non-world-coordinate paths to prevent transport
    // models from sinking below sea level on paths derived only from spawn-time data
    // (notably icebreaker routes where the DBC path has steep vertical curves).
    if (clientAnim && serverUpdateCount <= 1) {
        z = std::max(z, kMinFallbackZOffset);
    }
    if (!clientAnim && !hasServerClock) {
        z = std::clamp(z, kMinFallbackZOffset, kMaxFallbackZOffset);
    }
    return z;
}

} // namespace wowee::game
