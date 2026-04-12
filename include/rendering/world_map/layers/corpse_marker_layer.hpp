// corpse_marker_layer.hpp — Death corpse X marker on the world map.
#pragma once
#include "rendering/world_map/overlay_renderer.hpp"
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {
namespace world_map {

class CorpseMarkerLayer : public IOverlayLayer {
public:
    void setCorpse(bool hasCorpse, glm::vec3 renderPos) {
        hasCorpse_ = hasCorpse;
        corpseRenderPos_ = renderPos;
    }
    void render(const LayerContext& ctx) override;
private:
    bool hasCorpse_ = false;
    glm::vec3 corpseRenderPos_ = {};
};

} // namespace world_map
} // namespace rendering
} // namespace wowee
