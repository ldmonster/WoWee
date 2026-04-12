// taxi_node_layer.cpp — Flight master diamond icons on the world map.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/taxi_node_layer.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "core/coordinates.hpp"
#include <imgui.h>

namespace wowee {
namespace rendering {
namespace world_map {

void TaxiNodeLayer::render(const LayerContext& ctx) {
    if (!nodes_ || nodes_->empty()) return;
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

    ImVec2 mp = ImGui::GetMousePos();
    for (const auto& node : *nodes_) {
        if (!node.known) continue;
        if (static_cast<int>(node.mapId) != ctx.currentMapId) continue;

        glm::vec3 rPos = core::coords::canonicalToRender(
            glm::vec3(node.wowX, node.wowY, node.wowZ));
        glm::vec2 uv = renderPosToMapUV(rPos, bounds, isContinent);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;

        float px = ctx.imgMin.x + uv.x * ctx.displayW;
        float py = ctx.imgMin.y + uv.y * ctx.displayH;

        constexpr float H = 5.0f;
        ImVec2 top2(px,     py - H);
        ImVec2 right2(px + H, py    );
        ImVec2 bot2(px,     py + H);
        ImVec2 left2(px - H, py    );
        ctx.drawList->AddQuadFilled(top2, right2, bot2, left2,
                                    IM_COL32(255, 215, 0, 230));
        ctx.drawList->AddQuad(top2, right2, bot2, left2,
                              IM_COL32(80, 50, 0, 200), 1.2f);

        if (!node.name.empty()) {
            float mdx = mp.x - px, mdy = mp.y - py;
            if (mdx * mdx + mdy * mdy < 49.0f) {
                ImGui::SetTooltip("%s\n(Flight Master)", node.name.c_str());
            }
        }
    }
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
