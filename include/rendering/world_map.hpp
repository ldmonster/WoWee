#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;
class VkTexture;
class VkRenderTarget;

/// Party member dot passed in from the UI layer for world map overlay.
struct WorldMapPartyDot {
    glm::vec3 renderPos;   ///< Position in render-space coordinates
    uint32_t  color;       ///< RGBA packed color (IM_COL32 format)
    std::string name;      ///< Member name (shown as tooltip on hover)
};

struct WorldMapZone {
    uint32_t wmaID = 0;
    uint32_t areaID = 0;       // 0 = continent level
    std::string areaName;      // texture folder name (from DBC)
    float locLeft = 0, locRight = 0, locTop = 0, locBottom = 0;
    uint32_t displayMapID = 0;
    uint32_t parentWorldMapID = 0;
    std::vector<uint32_t> exploreBits;  // all AreaBit indices (zone + subzones)

    // Per-zone cached textures (owned by WorldMap::zoneTextures)
    VkTexture* tileTextures[12] = {};
    bool tilesLoaded = false;
};

class WorldMap {
public:
    WorldMap();
    ~WorldMap();

    bool initialize(VkContext* ctx, pipeline::AssetManager* assetManager);
    void shutdown();

    /// Off-screen composite pass — call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd);

    /// ImGui overlay — call INSIDE the main render pass (during ImGui frame).
    void render(const glm::vec3& playerRenderPos, int screenWidth, int screenHeight,
                float playerYawDeg = 0.0f);

    void setMapName(const std::string& name);
    void setServerExplorationMask(const std::vector<uint32_t>& masks, bool hasData);
    void setPartyDots(std::vector<WorldMapPartyDot> dots) { partyDots_ = std::move(dots); }
    bool isOpen() const { return open; }
    void close() { open = false; }

private:
    enum class ViewLevel { WORLD, CONTINENT, ZONE };

    void enterWorldView();
    void loadZonesFromDBC();
    int findBestContinentForPlayer(const glm::vec3& playerRenderPos) const;
    int findZoneForPlayer(const glm::vec3& playerRenderPos) const;
    bool zoneBelongsToContinent(int zoneIdx, int contIdx) const;
    bool getContinentProjectionBounds(int contIdx, float& left, float& right,
                                      float& top, float& bottom) const;
    void loadZoneTextures(int zoneIdx);
    void requestComposite(int zoneIdx);
    void renderImGuiOverlay(const glm::vec3& playerRenderPos, int screenWidth, int screenHeight,
                            float playerYawDeg);
    void updateExploration(const glm::vec3& playerRenderPos);
    void zoomIn(const glm::vec3& playerRenderPos);
    void zoomOut();
    glm::vec2 renderPosToMapUV(const glm::vec3& renderPos, int zoneIdx) const;
    void destroyZoneTextures();

    VkContext* vkCtx = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    bool initialized = false;
    bool open = false;

    std::string mapName = "Azeroth";

    // All zones for current map
    std::vector<WorldMapZone> zones;
    int continentIdx = -1;
    int currentIdx = -1;
    ViewLevel viewLevel = ViewLevel::CONTINENT;
    int compositedIdx = -1;
    int pendingCompositeIdx = -1;

    // FBO replacement (4x3 tiles = 1024x768)
    static constexpr int GRID_COLS = 4;
    static constexpr int GRID_ROWS = 3;
    static constexpr int TILE_PX = 256;
    static constexpr int FBO_W = GRID_COLS * TILE_PX;
    static constexpr int FBO_H = GRID_ROWS * TILE_PX;

    std::unique_ptr<VkRenderTarget> compositeTarget;

    // Quad vertex buffer (pos2 + uv2)
    ::VkBuffer quadVB = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc = VK_NULL_HANDLE;

    // Descriptor resources
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_DESC_SETS = 32;

    // Tile composite pipeline
    VkPipeline tilePipeline = VK_NULL_HANDLE;
    VkPipelineLayout tilePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet tileDescSets[2][12] = {};  // [frameInFlight][tileSlot]

    // ImGui display descriptor set (points to composite render target)
    VkDescriptorSet imguiDisplaySet = VK_NULL_HANDLE;

    // Texture storage (owns all VkTexture objects for zone tiles)
    std::vector<std::unique_ptr<VkTexture>> zoneTextures;

    // Party member dots (set each frame from the UI layer)
    std::vector<WorldMapPartyDot> partyDots_;

    // Exploration / fog of war
    std::vector<uint32_t> serverExplorationMask;
    bool hasServerExplorationMask = false;
    std::unordered_set<int> exploredZones;
    // Locally accumulated exploration (used as fallback when server mask is unavailable)
    std::unordered_set<int> locallyExploredZones_;
};

} // namespace rendering
} // namespace wowee
