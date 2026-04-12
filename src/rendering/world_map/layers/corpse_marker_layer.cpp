// corpse_marker_layer.cpp — Death corpse X marker on the world map.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/corpse_marker_layer.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include <imgui.h>

namespace wowee {
namespace rendering {
namespace world_map {

void CorpseMarkerLayer::render(const LayerContext& ctx) {
    if (!hasCorpse_) return;
    if (ctx.currentZoneIdx < 0) return;
    if (ctx.viewLevel != ViewLevel::ZONE && ctx.viewLevel != ViewLevel::CONTINENT) return;
    if (!ctx.zones) return;

    const auto& zone = (*ctx.zones)[ctx.currentZoneIdx];
    ZoneBounds bounds = zone.bounds;
    bool isContinent = zone.areaID == 0;
    if (isContinent) {
        float l, r, t, b;
        if (getContinentProjectionBounds(*ctx.zones, ctx.currentZoneIdx, l, r, t, b)) {
            bounds = {l, r, t, b};
        }
    }

    glm::vec2 uv = renderPosToMapUV(corpseRenderPos_, bounds, isContinent);
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) return;

    float cx = ctx.imgMin.x + uv.x * ctx.displayW;
    float cy = ctx.imgMin.y + uv.y * ctx.displayH;
    constexpr float R = 5.0f;
    constexpr float T = 1.8f;
    // Dark outline
    ctx.drawList->AddLine(ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R),
                          IM_COL32(0, 0, 0, 220), T + 1.5f);
    ctx.drawList->AddLine(ImVec2(cx + R, cy - R), ImVec2(cx - R, cy + R),
                          IM_COL32(0, 0, 0, 220), T + 1.5f);
    // Bone-white X
    ctx.drawList->AddLine(ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R),
                          IM_COL32(230, 220, 200, 240), T);
    ctx.drawList->AddLine(ImVec2(cx + R, cy - R), ImVec2(cx - R, cy + R),
                          IM_COL32(230, 220, 200, 240), T);
    // Tooltip on hover
    ImVec2 mp = ImGui::GetMousePos();
    float dx = mp.x - cx, dy = mp.y - cy;
    if (dx * dx + dy * dy < 64.0f) {
        ImGui::SetTooltip("Your corpse");
    }
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
