// taxi_node_layer.hpp — Flight master diamond icons on the world map.
#pragma once
#include "rendering/world_map/overlay_renderer.hpp"
#include "rendering/world_map/world_map_types.hpp"
#include <vector>

namespace wowee {
namespace rendering {
namespace world_map {

class TaxiNodeLayer : public IOverlayLayer {
public:
    void setNodes(const std::vector<TaxiNode>& nodes) { nodes_ = &nodes; }
    void render(const LayerContext& ctx) override;
private:
    const std::vector<TaxiNode>* nodes_ = nullptr;
};

} // namespace world_map
} // namespace rendering
} // namespace wowee
