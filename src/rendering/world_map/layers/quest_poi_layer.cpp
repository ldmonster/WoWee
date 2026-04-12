// quest_poi_layer.cpp — Quest objective markers on the world map.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/quest_poi_layer.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "core/coordinates.hpp"
#include <imgui.h>

namespace wowee {
namespace rendering {
namespace world_map {

void QuestPOILayer::render(const LayerContext& ctx) {
    if (!pois_ || pois_->empty()) return;
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
    ImFont* qFont = ImGui::GetFont();
    for (const auto& qp : *pois_) {
        glm::vec3 rPos = core::coords::canonicalToRender(
            glm::vec3(qp.wowX, qp.wowY, 0.0f));
        glm::vec2 uv = renderPosToMapUV(rPos, bounds, isContinent);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;

        float px = ctx.imgMin.x + uv.x * ctx.displayW;
        float py = ctx.imgMin.y + uv.y * ctx.displayH;

        ctx.drawList->AddCircleFilled(ImVec2(px, py), 5.0f, IM_COL32(0, 210, 255, 220));
        ctx.drawList->AddCircle(ImVec2(px, py), 5.0f, IM_COL32(255, 215, 0, 220), 0, 1.5f);

        if (!qp.name.empty()) {
            ImVec2 nameSz = qFont->CalcTextSizeA(ImGui::GetFontSize() * 0.85f, FLT_MAX, 0.0f, qp.name.c_str());
            float tx = px - nameSz.x * 0.5f;
            float ty = py - nameSz.y - 7.0f;
            ctx.drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                  ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 180), qp.name.c_str());
            ctx.drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                  ImVec2(tx, ty), IM_COL32(255, 230, 100, 230), qp.name.c_str());
        }
        float mdx = mp.x - px, mdy = mp.y - py;
        if (mdx * mdx + mdy * mdy < 49.0f && !qp.name.empty()) {
            ImGui::SetTooltip("%s\n(Quest Objective)", qp.name.c_str());
        }
    }
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
