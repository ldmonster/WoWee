// world_map_facade.hpp — Public API for the world map system.
// Drop-in replacement for the monolithic WorldMap class (Phase 10 of refactoring plan).
// Facade pattern — hides internal complexity behind the same public interface.
#pragma once

#include "rendering/world_map/world_map_types.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>
#include <vulkan/vulkan.h>

namespace wowee {
namespace rendering {
class VkContext;
}
namespace pipeline { class AssetManager; }
namespace rendering {
namespace world_map {

class WorldMapFacade {
public:
    /// Backward-compatible alias for old WorldMap::QuestPoi usage.
    using QuestPoi = QuestPOI;

    WorldMapFacade();
    ~WorldMapFacade();

    bool initialize(VkContext* ctx, pipeline::AssetManager* am);
    void shutdown();

    /// Off-screen composite pass — call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd);

    /// ImGui overlay — call INSIDE the main render pass (during ImGui frame).
    void render(const glm::vec3& playerRenderPos,
                int screenWidth, int screenHeight,
                float playerYawDeg = 0.0f);

    void setMapName(const std::string& name);
    void setServerExplorationMask(const std::vector<uint32_t>& masks, bool hasData);
    void setPartyDots(std::vector<PartyDot> dots);
    void setTaxiNodes(std::vector<TaxiNode> nodes);
    void setQuestPois(std::vector<QuestPOI> pois);
    void setCorpsePos(bool hasCorpse, glm::vec3 renderPos);

    bool isOpen() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace world_map
} // namespace rendering
} // namespace wowee

// Backward-compatible alias for gradual migration
namespace wowee {
namespace rendering {
using WorldMap = world_map::WorldMapFacade;
} // namespace rendering
} // namespace wowee
