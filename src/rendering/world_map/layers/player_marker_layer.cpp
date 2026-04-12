// player_marker_layer.cpp — Directional player arrow on the world map.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/player_marker_layer.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include <imgui.h>
#include <cmath>

namespace wowee {
namespace rendering {
namespace world_map {

void PlayerMarkerLayer::render(const LayerContext& ctx) {
    if (ctx.currentZoneIdx < 0) return;
    if (ctx.viewLevel != ViewLevel::ZONE && ctx.viewLevel != ViewLevel::CONTINENT) return;
    if (!ctx.zones) return;

    const auto& zone = (*ctx.zones)[ctx.currentZoneIdx];
    ZoneBounds bounds = zone.bounds;
    bool isContinent = zone.areaID == 0;

    // In continent view, only show the player marker if they are actually
    // in a zone belonging to this continent (don't bleed across continents).
    if (isContinent) {
        int playerZone = findZoneForPlayer(*ctx.zones, ctx.playerRenderPos);
        if (playerZone < 0 || !zoneBelongsToContinent(*ctx.zones, playerZone, ctx.currentZoneIdx))
            return;
        float l, r, t, b;
        if (getContinentProjectionBounds(*ctx.zones, ctx.currentZoneIdx, l, r, t, b)) {
            bounds = {l, r, t, b};
        }
    }

    glm::vec2 playerUV = renderPosToMapUV(ctx.playerRenderPos, bounds, isContinent);
    if (playerUV.x < 0.0f || playerUV.x > 1.0f ||
        playerUV.y < 0.0f || playerUV.y > 1.0f) return;

    float px = ctx.imgMin.x + playerUV.x * ctx.displayW;
    float py = ctx.imgMin.y + playerUV.y * ctx.displayH;

    // Directional arrow: render-space (cos,sin) maps to screen (-dx,-dy)
    float yawRad = glm::radians(ctx.playerYawDeg);
    float adx = -std::cos(yawRad);
    float ady = -std::sin(yawRad);
    float apx = -ady, apy = adx;
    constexpr float TIP  = 9.0f;
    constexpr float TAIL = 4.0f;
    constexpr float HALF = 5.0f;
    ImVec2 tip(px + adx * TIP,  py + ady * TIP);
    ImVec2 bl (px - adx * TAIL + apx * HALF,  py - ady * TAIL + apy * HALF);
    ImVec2 br (px - adx * TAIL - apx * HALF,  py - ady * TAIL - apy * HALF);
    ctx.drawList->AddTriangleFilled(tip, bl, br, IM_COL32(255, 40, 40, 255));
    ctx.drawList->AddTriangle(tip, bl, br, IM_COL32(0, 0, 0, 200), 1.5f);
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
