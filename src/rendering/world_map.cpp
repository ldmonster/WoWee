#include "rendering/world_map.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "core/logger.hpp"
#include "ui/ui_colors.hpp"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>

namespace wowee {
namespace rendering {

namespace {
bool isRootContinent(const std::vector<WorldMapZone>& zones, int idx) {
    if (idx < 0 || idx >= static_cast<int>(zones.size())) return false;
    const auto& c = zones[idx];
    if (c.areaID != 0 || c.wmaID == 0) return false;
    for (const auto& z : zones) {
        if (z.areaID == 0 && z.parentWorldMapID == c.wmaID) {
            return true;
        }
    }
    return false;
}

bool isLeafContinent(const std::vector<WorldMapZone>& zones, int idx) {
    if (idx < 0 || idx >= static_cast<int>(zones.size())) return false;
    const auto& c = zones[idx];
    if (c.areaID != 0) return false;
    return c.parentWorldMapID != 0;
}
} // namespace

// Push constant for world map tile composite vertex shader
struct WorldMapTilePush {
    glm::vec2 gridOffset;  // 8 bytes
    float gridCols;          // 4 bytes
    float gridRows;          // 4 bytes
};  // 16 bytes

WorldMap::WorldMap() = default;

WorldMap::~WorldMap() {
    shutdown();
}

bool WorldMap::initialize(VkContext* ctx, pipeline::AssetManager* am) {
    if (initialized) return true;
    vkCtx = ctx;
    assetManager = am;
    VkDevice device = vkCtx->getDevice();

    // --- Composite render target (1024x768) ---
    compositeTarget = std::make_unique<VkRenderTarget>();
    if (!compositeTarget->create(*vkCtx, FBO_W, FBO_H)) {
        LOG_ERROR("WorldMap: failed to create composite render target");
        return false;
    }

    // --- Quad vertex buffer (unit quad: pos2 + uv2) ---
    float quadVerts[] = {
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    auto quadBuf = uploadBuffer(*vkCtx, quadVerts, sizeof(quadVerts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB = quadBuf.buffer;
    quadVBAlloc = quadBuf.allocation;

    // --- Descriptor set layout: 1 combined image sampler at binding 0 ---
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerSetLayout = createDescriptorSetLayout(device, { samplerBinding });

    // --- Descriptor pool (24 tile + 1 display = 25) ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_DESC_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_DESC_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

    // --- Allocate descriptor sets: 12*2 tile + 1 display = 25 ---
    constexpr uint32_t totalSets = 25;
    std::vector<VkDescriptorSetLayout> layouts(totalSets, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = totalSets;
    allocInfo.pSetLayouts = layouts.data();

    VkDescriptorSet allSets[25];
    vkAllocateDescriptorSets(device, &allocInfo, allSets);

    for (int f = 0; f < 2; f++)
        for (int t = 0; t < 12; t++)
            tileDescSets[f][t] = allSets[f * 12 + t];
    imguiDisplaySet = allSets[24];

    // --- Write display descriptor set → composite render target ---
    VkDescriptorImageInfo compositeImgInfo = compositeTarget->descriptorInfo();
    VkWriteDescriptorSet displayWrite{};
    displayWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    displayWrite.dstSet = imguiDisplaySet;
    displayWrite.dstBinding = 0;
    displayWrite.descriptorCount = 1;
    displayWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    displayWrite.pImageInfo = &compositeImgInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);

    // --- Pipeline layout: samplerSetLayout + push constant (16 bytes, vertex) ---
    VkPushConstantRange tilePush{};
    tilePush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    tilePush.offset = 0;
    tilePush.size = sizeof(WorldMapTilePush);
    tilePipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { tilePush });

    // --- Vertex input: pos2 (loc 0) + uv2 (loc 1), stride 16 ---
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) };

    // --- Load tile shaders and build pipeline ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/world_map.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/world_map.frag.spv")) {
            LOG_ERROR("WorldMap: failed to load tile shaders");
            return false;
        }

        tilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR })
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    if (!tilePipeline) {
        LOG_ERROR("WorldMap: failed to create tile pipeline");
        return false;
    }

    initialized = true;
    LOG_INFO("WorldMap initialized (", FBO_W, "x", FBO_H, " composite)");
    return true;
}

void WorldMap::shutdown() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    vkDeviceWaitIdle(device);

    if (tilePipeline) { vkDestroyPipeline(device, tilePipeline, nullptr); tilePipeline = VK_NULL_HANDLE; }
    if (tilePipelineLayout) { vkDestroyPipelineLayout(device, tilePipelineLayout, nullptr); tilePipelineLayout = VK_NULL_HANDLE; }
    if (descPool) { vkDestroyDescriptorPool(device, descPool, nullptr); descPool = VK_NULL_HANDLE; }
    if (samplerSetLayout) { vkDestroyDescriptorSetLayout(device, samplerSetLayout, nullptr); samplerSetLayout = VK_NULL_HANDLE; }
    if (quadVB) { vmaDestroyBuffer(alloc, quadVB, quadVBAlloc); quadVB = VK_NULL_HANDLE; }

    destroyZoneTextures();

    if (compositeTarget) { compositeTarget->destroy(device, alloc); compositeTarget.reset(); }

    zones.clear();
    initialized = false;
    vkCtx = nullptr;
}

void WorldMap::destroyZoneTextures() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    for (auto& tex : zoneTextures) {
        if (tex) tex->destroy(device, alloc);
    }
    zoneTextures.clear();

    for (auto& zone : zones) {
        for (auto& tex : zone.tileTextures) tex = nullptr;
        zone.tilesLoaded = false;
    }
}

void WorldMap::setMapName(const std::string& name) {
    if (mapName == name && !zones.empty()) return;
    mapName = name;

    destroyZoneTextures();
    zones.clear();
    continentIdx = -1;
    currentIdx = -1;
    compositedIdx = -1;
    pendingCompositeIdx = -1;
    viewLevel = ViewLevel::WORLD;
}

void WorldMap::setServerExplorationMask(const std::vector<uint32_t>& masks, bool hasData) {
    if (!hasData || masks.empty()) {
        // New session or no data yet — reset both server mask and local accumulation
        if (hasServerExplorationMask) {
            locallyExploredZones_.clear();
        }
        hasServerExplorationMask = false;
        serverExplorationMask.clear();
        return;
    }
    hasServerExplorationMask = true;
    serverExplorationMask = masks;
}

// --------------------------------------------------------
// DBC zone loading (identical to GL version)
// --------------------------------------------------------

void WorldMap::loadZonesFromDBC() {
    if (!zones.empty() || !assetManager) return;

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* mapL = activeLayout ? activeLayout->getLayout("Map") : nullptr;

    int mapID = -1;
    auto mapDbc = assetManager->loadDBC("Map.dbc");
    if (mapDbc && mapDbc->isLoaded()) {
        for (uint32_t i = 0; i < mapDbc->getRecordCount(); i++) {
            std::string dir = mapDbc->getString(i, mapL ? (*mapL)["InternalName"] : 1);
            if (dir == mapName) {
                mapID = static_cast<int>(mapDbc->getUInt32(i, mapL ? (*mapL)["ID"] : 0));
                LOG_INFO("WorldMap: Map.dbc '", mapName, "' -> mapID=", mapID);
                break;
            }
        }
    }

    if (mapID < 0) {
        if (mapName == "Azeroth") mapID = 0;
        else if (mapName == "Kalimdor") mapID = 1;
        else if (mapName == "Expansion01") mapID = 530;
        else if (mapName == "Northrend") mapID = 571;
        else {
            LOG_WARNING("WorldMap: unknown map '", mapName, "'");
            return;
        }
    }

    // Use expansion-aware DBC layout when available; fall back to WotLK stock field
    // indices (ID=0, ParentAreaNum=2, ExploreFlag=3) when layout metadata is missing.
    // Incorrect field indices silently return wrong data, so these defaults must match
    // the most common AreaTable.dbc layout to minimize breakage.
    const auto* atL = activeLayout ? activeLayout->getLayout("AreaTable") : nullptr;
    std::unordered_map<uint32_t, uint32_t> exploreFlagByAreaId;
    std::unordered_map<uint32_t, std::vector<uint32_t>> childBitsByParent;
    auto areaDbc = assetManager->loadDBC("AreaTable.dbc");
    if (areaDbc && areaDbc->isLoaded() && areaDbc->getFieldCount() > 3) {
        const uint32_t parentField = atL ? (*atL)["ParentAreaNum"] : 2;
        for (uint32_t i = 0; i < areaDbc->getRecordCount(); i++) {
            const uint32_t areaId = areaDbc->getUInt32(i, atL ? (*atL)["ID"] : 0);
            const uint32_t exploreFlag = areaDbc->getUInt32(i, atL ? (*atL)["ExploreFlag"] : 3);
            const uint32_t parentArea = areaDbc->getUInt32(i, parentField);
            if (areaId != 0) exploreFlagByAreaId[areaId] = exploreFlag;
            if (parentArea != 0) childBitsByParent[parentArea].push_back(exploreFlag);
        }
    }

    auto wmaDbc = assetManager->loadDBC("WorldMapArea.dbc");
    if (!wmaDbc || !wmaDbc->isLoaded()) {
        LOG_WARNING("WorldMap: WorldMapArea.dbc not found");
        return;
    }

    const auto* wmaL = activeLayout ? activeLayout->getLayout("WorldMapArea") : nullptr;

    for (uint32_t i = 0; i < wmaDbc->getRecordCount(); i++) {
        uint32_t recMapID = wmaDbc->getUInt32(i, wmaL ? (*wmaL)["MapID"] : 1);
        if (static_cast<int>(recMapID) != mapID) continue;

        WorldMapZone zone;
        zone.wmaID   = wmaDbc->getUInt32(i, wmaL ? (*wmaL)["ID"] : 0);
        zone.areaID  = wmaDbc->getUInt32(i, wmaL ? (*wmaL)["AreaID"] : 2);
        zone.areaName = wmaDbc->getString(i, wmaL ? (*wmaL)["AreaName"] : 3);
        zone.locLeft   = wmaDbc->getFloat(i, wmaL ? (*wmaL)["LocLeft"] : 4);
        zone.locRight  = wmaDbc->getFloat(i, wmaL ? (*wmaL)["LocRight"] : 5);
        zone.locTop    = wmaDbc->getFloat(i, wmaL ? (*wmaL)["LocTop"] : 6);
        zone.locBottom = wmaDbc->getFloat(i, wmaL ? (*wmaL)["LocBottom"] : 7);
        zone.displayMapID = wmaDbc->getUInt32(i, wmaL ? (*wmaL)["DisplayMapID"] : 8);
        zone.parentWorldMapID = wmaDbc->getUInt32(i, wmaL ? (*wmaL)["ParentWorldMapID"] : 10);
        // Collect the zone's own AreaBit plus all subzone AreaBits
        auto exploreIt = exploreFlagByAreaId.find(zone.areaID);
        if (exploreIt != exploreFlagByAreaId.end())
            zone.exploreBits.push_back(exploreIt->second);
        auto childIt = childBitsByParent.find(zone.areaID);
        if (childIt != childBitsByParent.end()) {
            for (uint32_t bit : childIt->second)
                zone.exploreBits.push_back(bit);
        }

        int idx = static_cast<int>(zones.size());

        LOG_INFO("WorldMap: zone[", idx, "] areaID=", zone.areaID,
                 " '", zone.areaName, "' L=", zone.locLeft,
                 " R=", zone.locRight, " T=", zone.locTop,
                 " B=", zone.locBottom);

        if (zone.areaID == 0 && continentIdx < 0)
            continentIdx = idx;

        zones.push_back(std::move(zone));
    }

    // Derive continent bounds from child zones if missing
    for (int ci = 0; ci < static_cast<int>(zones.size()); ci++) {
        auto& cont = zones[ci];
        if (cont.areaID != 0) continue;
        if (std::abs(cont.locLeft) > 0.001f || std::abs(cont.locRight) > 0.001f ||
            std::abs(cont.locTop) > 0.001f || std::abs(cont.locBottom) > 0.001f)
            continue;

        bool first = true;
        for (const auto& z : zones) {
            if (z.areaID == 0) continue;
            if (std::abs(z.locLeft - z.locRight) < 0.001f ||
                std::abs(z.locTop - z.locBottom) < 0.001f)
                continue;
            if (z.parentWorldMapID != 0 && cont.wmaID != 0 && z.parentWorldMapID != cont.wmaID)
                continue;

            if (first) {
                cont.locLeft = z.locLeft; cont.locRight = z.locRight;
                cont.locTop = z.locTop; cont.locBottom = z.locBottom;
                first = false;
            } else {
                cont.locLeft = std::min(cont.locLeft, z.locLeft);
                cont.locRight = std::max(cont.locRight, z.locRight);
                cont.locTop = std::min(cont.locTop, z.locTop);
                cont.locBottom = std::max(cont.locBottom, z.locBottom);
            }
        }
    }

    currentMapId_ = mapID;
    LOG_INFO("WorldMap: loaded ", zones.size(), " zones for mapID=", mapID,
             ", continentIdx=", continentIdx);
}

int WorldMap::findBestContinentForPlayer(const glm::vec3& playerRenderPos) const {
    float wowX = playerRenderPos.y;
    float wowY = playerRenderPos.x;

    int bestIdx = -1;
    float bestArea = std::numeric_limits<float>::max();
    float bestCenterDist2 = std::numeric_limits<float>::max();

    bool hasLeafContinent = false;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (zones[i].areaID == 0 && !isRootContinent(zones, i)) {
            hasLeafContinent = true;
            break;
        }
    }

    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        const auto& z = zones[i];
        if (z.areaID != 0) continue;
        if (hasLeafContinent && isRootContinent(zones, i)) continue;

        float minX = std::min(z.locLeft, z.locRight);
        float maxX = std::max(z.locLeft, z.locRight);
        float minY = std::min(z.locTop, z.locBottom);
        float maxY = std::max(z.locTop, z.locBottom);
        float spanX = maxX - minX;
        float spanY = maxY - minY;
        if (spanX < 0.001f || spanY < 0.001f) continue;

        bool contains = (wowX >= minX && wowX <= maxX && wowY >= minY && wowY <= maxY);
        float area = spanX * spanY;
        if (contains) {
            if (area < bestArea) { bestArea = area; bestIdx = i; }
        } else if (bestIdx < 0) {
            float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
            float dist2 = (wowX - cx) * (wowX - cx) + (wowY - cy) * (wowY - cy);
            if (dist2 < bestCenterDist2) { bestCenterDist2 = dist2; bestIdx = i; }
        }
    }
    return bestIdx;
}

int WorldMap::findZoneForPlayer(const glm::vec3& playerRenderPos) const {
    float wowX = playerRenderPos.y;
    float wowY = playerRenderPos.x;

    int bestIdx = -1;
    float bestArea = std::numeric_limits<float>::max();

    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        const auto& z = zones[i];
        if (z.areaID == 0) continue;

        float minX = std::min(z.locLeft, z.locRight);
        float maxX = std::max(z.locLeft, z.locRight);
        float minY = std::min(z.locTop, z.locBottom);
        float maxY = std::max(z.locTop, z.locBottom);
        float spanX = maxX - minX, spanY = maxY - minY;
        if (spanX < 0.001f || spanY < 0.001f) continue;

        if (wowX >= minX && wowX <= maxX && wowY >= minY && wowY <= maxY) {
            float area = spanX * spanY;
            if (area < bestArea) { bestArea = area; bestIdx = i; }
        }
    }
    return bestIdx;
}

bool WorldMap::zoneBelongsToContinent(int zoneIdx, int contIdx) const {
    if (zoneIdx < 0 || zoneIdx >= static_cast<int>(zones.size())) return false;
    if (contIdx < 0 || contIdx >= static_cast<int>(zones.size())) return false;

    const auto& z = zones[zoneIdx];
    const auto& cont = zones[contIdx];
    if (z.areaID == 0) return false;

    if (z.parentWorldMapID != 0 && cont.wmaID != 0)
        return z.parentWorldMapID == cont.wmaID;

    auto rectMinX = [](const WorldMapZone& a) { return std::min(a.locLeft, a.locRight); };
    auto rectMaxX = [](const WorldMapZone& a) { return std::max(a.locLeft, a.locRight); };
    auto rectMinY = [](const WorldMapZone& a) { return std::min(a.locTop, a.locBottom); };
    auto rectMaxY = [](const WorldMapZone& a) { return std::max(a.locTop, a.locBottom); };

    float zMinX = rectMinX(z), zMaxX = rectMaxX(z);
    float zMinY = rectMinY(z), zMaxY = rectMaxY(z);
    if ((zMaxX - zMinX) < 0.001f || (zMaxY - zMinY) < 0.001f) return false;

    int bestContIdx = -1;
    float bestOverlap = 0.0f;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        const auto& c = zones[i];
        if (c.areaID != 0) continue;
        float cMinX = rectMinX(c), cMaxX = rectMaxX(c);
        float cMinY = rectMinY(c), cMaxY = rectMaxY(c);
        if ((cMaxX - cMinX) < 0.001f || (cMaxY - cMinY) < 0.001f) continue;

        float ox = std::max(0.0f, std::min(zMaxX, cMaxX) - std::max(zMinX, cMinX));
        float oy = std::max(0.0f, std::min(zMaxY, cMaxY) - std::max(zMinY, cMinY));
        float overlap = ox * oy;
        if (overlap > bestOverlap) { bestOverlap = overlap; bestContIdx = i; }
    }
    if (bestContIdx >= 0) return bestContIdx == contIdx;

    float centerX = (z.locLeft + z.locRight) * 0.5f;
    float centerY = (z.locTop + z.locBottom) * 0.5f;
    return centerX >= rectMinX(cont) && centerX <= rectMaxX(cont) &&
           centerY >= rectMinY(cont) && centerY <= rectMaxY(cont);
}

bool WorldMap::getContinentProjectionBounds(int contIdx, float& left, float& right,
                                            float& top, float& bottom) const {
    if (contIdx < 0 || contIdx >= static_cast<int>(zones.size())) return false;
    const auto& cont = zones[contIdx];
    if (cont.areaID != 0) return false;

    if (std::abs(cont.locLeft - cont.locRight) > 0.001f &&
        std::abs(cont.locTop - cont.locBottom) > 0.001f) {
        left = cont.locLeft; right = cont.locRight;
        top = cont.locTop; bottom = cont.locBottom;
        return true;
    }

    std::vector<float> northEdges, southEdges, westEdges, eastEdges;
    for (int zi = 0; zi < static_cast<int>(zones.size()); zi++) {
        if (!zoneBelongsToContinent(zi, contIdx)) continue;
        const auto& z = zones[zi];
        if (std::abs(z.locLeft - z.locRight) < 0.001f ||
            std::abs(z.locTop - z.locBottom) < 0.001f) continue;
        northEdges.push_back(std::max(z.locLeft, z.locRight));
        southEdges.push_back(std::min(z.locLeft, z.locRight));
        westEdges.push_back(std::max(z.locTop, z.locBottom));
        eastEdges.push_back(std::min(z.locTop, z.locBottom));
    }

    if (northEdges.size() < 3) {
        left = cont.locLeft; right = cont.locRight;
        top = cont.locTop; bottom = cont.locBottom;
        return std::abs(left - right) > 0.001f && std::abs(top - bottom) > 0.001f;
    }

    left = *std::max_element(northEdges.begin(), northEdges.end());
    right = *std::min_element(southEdges.begin(), southEdges.end());
    top = *std::max_element(westEdges.begin(), westEdges.end());
    bottom = *std::min_element(eastEdges.begin(), eastEdges.end());

    if (left <= right || top <= bottom) {
        left = cont.locLeft; right = cont.locRight;
        top = cont.locTop; bottom = cont.locBottom;
    }
    return std::abs(left - right) > 0.001f && std::abs(top - bottom) > 0.001f;
}

// --------------------------------------------------------
// Per-zone texture loading (Vulkan)
// --------------------------------------------------------

void WorldMap::loadZoneTextures(int zoneIdx) {
    if (zoneIdx < 0 || zoneIdx >= static_cast<int>(zones.size())) return;
    auto& zone = zones[zoneIdx];
    if (zone.tilesLoaded) return;
    zone.tilesLoaded = true;

    const std::string& folder = zone.areaName;
    if (folder.empty()) return;

    std::vector<std::string> candidateFolders;
    candidateFolders.push_back(folder);
    if (zone.areaID == 0 && mapName == "Azeroth") {
        if (folder != "Azeroth") candidateFolders.push_back("Azeroth");
        if (folder != "EasternKingdoms") candidateFolders.push_back("EasternKingdoms");
    }

    VkDevice device = vkCtx->getDevice();
    int loaded = 0;

    for (int i = 0; i < 12; i++) {
        pipeline::BLPImage blpImage;
        bool found = false;
        for (const auto& testFolder : candidateFolders) {
            std::string path = "Interface\\WorldMap\\" + testFolder + "\\" +
                               testFolder + std::to_string(i + 1) + ".blp";
            blpImage = assetManager->loadTexture(path);
            if (blpImage.isValid()) { found = true; break; }
        }

        if (!found) {
            zone.tileTextures[i] = nullptr;
            continue;
        }

        auto tex = std::make_unique<VkTexture>();
        tex->upload(*vkCtx, blpImage.data.data(), blpImage.width, blpImage.height,
                    VK_FORMAT_R8G8B8A8_UNORM, false);
        tex->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                           VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

        zone.tileTextures[i] = tex.get();
        zoneTextures.push_back(std::move(tex));
        loaded++;
    }

    LOG_INFO("WorldMap: loaded ", loaded, "/12 tiles for '", folder, "'");
}

// --------------------------------------------------------
// Request composite (deferred to compositePass)
// --------------------------------------------------------

void WorldMap::requestComposite(int zoneIdx) {
    if (zoneIdx < 0 || zoneIdx >= static_cast<int>(zones.size())) return;
    pendingCompositeIdx = zoneIdx;
}

// --------------------------------------------------------
// Off-screen composite pass (call BEFORE main render pass)
// --------------------------------------------------------

void WorldMap::compositePass(VkCommandBuffer cmd) {
    if (!initialized || pendingCompositeIdx < 0 || !compositeTarget) return;
    if (pendingCompositeIdx >= static_cast<int>(zones.size())) {
        pendingCompositeIdx = -1;
        return;
    }

    int zoneIdx = pendingCompositeIdx;
    pendingCompositeIdx = -1;

    if (compositedIdx == zoneIdx) return;

    const auto& zone = zones[zoneIdx];
    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDevice device = vkCtx->getDevice();

    // Update tile descriptor sets for this frame
    for (int i = 0; i < 12; i++) {
        VkTexture* tileTex = zone.tileTextures[i];
        if (!tileTex || !tileTex->isValid()) continue;

        VkDescriptorImageInfo imgInfo = tileTex->descriptorInfo();
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = tileDescSets[frameIdx][i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{ 0.05f, 0.08f, 0.12f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Draw 4x3 tile grid
    for (int i = 0; i < 12; i++) {
        if (!zone.tileTextures[i] || !zone.tileTextures[i]->isValid()) continue;

        int col = i % GRID_COLS;
        int row = i / GRID_COLS;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                tilePipelineLayout, 0, 1,
                                &tileDescSets[frameIdx][i], 0, nullptr);

        WorldMapTilePush push{};
        push.gridOffset = glm::vec2(static_cast<float>(col), static_cast<float>(row));
        push.gridCols = static_cast<float>(GRID_COLS);
        push.gridRows = static_cast<float>(GRID_ROWS);
        vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);

        vkCmdDraw(cmd, 6, 1, 0, 0);
    }

    compositeTarget->endPass(cmd);
    compositedIdx = zoneIdx;
}

void WorldMap::enterWorldView() {
    viewLevel = ViewLevel::WORLD;

    int rootIdx = -1;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (isRootContinent(zones, i)) { rootIdx = i; break; }
    }

    if (rootIdx >= 0) {
        loadZoneTextures(rootIdx);
        bool hasAnyTile = false;
        for (VkTexture* tex : zones[rootIdx].tileTextures) {
            if (tex != nullptr) { hasAnyTile = true; break; }
        }
        if (hasAnyTile) {
            requestComposite(rootIdx);
            currentIdx = rootIdx;
            return;
        }
    }

    int fallbackContinent = -1;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (isLeafContinent(zones, i)) { fallbackContinent = i; break; }
    }
    if (fallbackContinent < 0) {
        for (int i = 0; i < static_cast<int>(zones.size()); i++) {
            if (zones[i].areaID == 0 && !isRootContinent(zones, i)) {
                fallbackContinent = i; break;
            }
        }
    }
    if (fallbackContinent >= 0) {
        loadZoneTextures(fallbackContinent);
        requestComposite(fallbackContinent);
        currentIdx = fallbackContinent;
        return;
    }

    currentIdx = -1;
    compositedIdx = -1;
    // Render target will be cleared by next compositePass
    pendingCompositeIdx = -2;  // Signal "clear only"
}

// --------------------------------------------------------
// Coordinate projection
// --------------------------------------------------------

glm::vec2 WorldMap::renderPosToMapUV(const glm::vec3& renderPos, int zoneIdx) const {
    if (zoneIdx < 0 || zoneIdx >= static_cast<int>(zones.size()))
        return glm::vec2(0.5f, 0.5f);

    const auto& zone = zones[zoneIdx];
    float wowX = renderPos.y;
    float wowY = renderPos.x;

    float left = zone.locLeft, right = zone.locRight;
    float top = zone.locTop, bottom = zone.locBottom;
    if (zone.areaID == 0) {
        float l, r, t, b;
        if (getContinentProjectionBounds(zoneIdx, l, r, t, b)) {
            left = l; right = r; top = t; bottom = b;
        }
    }

    float denom_h = left - right;
    float denom_v = top - bottom;
    if (std::abs(denom_h) < 0.001f || std::abs(denom_v) < 0.001f)
        return glm::vec2(0.5f, 0.5f);

    float u = (left - wowX) / denom_h;
    float v = (top  - wowY) / denom_v;

    if (zone.areaID == 0) {
        constexpr float kVScale = 1.0f;
        constexpr float kVOffset = -0.15f;
        v = (v - 0.5f) * kVScale + 0.5f + kVOffset;
    }
    return glm::vec2(u, v);
}

// --------------------------------------------------------
// Exploration tracking (identical to GL version)
// --------------------------------------------------------

void WorldMap::updateExploration(const glm::vec3& playerRenderPos) {
    auto isBitSet = [this](uint32_t bitIndex) -> bool {
        if (!hasServerExplorationMask || serverExplorationMask.empty()) return false;
        const size_t word = bitIndex / 32;
        if (word >= serverExplorationMask.size()) return false;
        return (serverExplorationMask[word] & (1u << (bitIndex % 32))) != 0;
    };

    if (hasServerExplorationMask) {
        exploredZones.clear();
        for (int i = 0; i < static_cast<int>(zones.size()); i++) {
            const auto& z = zones[i];
            if (z.areaID == 0 || z.exploreBits.empty()) continue;
            for (uint32_t bit : z.exploreBits) {
                if (isBitSet(bit)) {
                    exploredZones.insert(i);
                    break;
                }
            }
        }
        // Always trust the server mask when available — even if empty (unexplored character).
        // Also reveal the zone the player is currently standing in so the map isn't pitch-black
        // the moment they first enter a new zone (the server bit arrives on the next update).
        int curZone = findZoneForPlayer(playerRenderPos);
        if (curZone >= 0) exploredZones.insert(curZone);
        return;
    }

    // Server mask unavailable — fall back to locally-accumulated position tracking.
    // Add the zone the player is currently in to the local set and display that.
    float wowX = playerRenderPos.y;
    float wowY = playerRenderPos.x;

    bool foundPos = false;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        const auto& z = zones[i];
        if (z.areaID == 0) continue;
        float minX = std::min(z.locLeft, z.locRight), maxX = std::max(z.locLeft, z.locRight);
        float minY = std::min(z.locTop, z.locBottom), maxY = std::max(z.locTop, z.locBottom);
        if (maxX - minX < 0.001f || maxY - minY < 0.001f) continue;
        if (wowX >= minX && wowX <= maxX && wowY >= minY && wowY <= maxY) {
            locallyExploredZones_.insert(i);
            foundPos = true;
        }
    }

    if (!foundPos) {
        int zoneIdx = findZoneForPlayer(playerRenderPos);
        if (zoneIdx >= 0) locallyExploredZones_.insert(zoneIdx);
    }

    // Display the accumulated local set
    exploredZones = locallyExploredZones_;
}

void WorldMap::zoomIn(const glm::vec3& playerRenderPos) {
    if (viewLevel == ViewLevel::WORLD) {
        if (continentIdx >= 0) {
            loadZoneTextures(continentIdx);
            requestComposite(continentIdx);
            currentIdx = continentIdx;
            viewLevel = ViewLevel::CONTINENT;
        }
    } else if (viewLevel == ViewLevel::CONTINENT) {
        int zoneIdx = findZoneForPlayer(playerRenderPos);
        if (zoneIdx >= 0 && zoneBelongsToContinent(zoneIdx, continentIdx)) {
            loadZoneTextures(zoneIdx);
            requestComposite(zoneIdx);
            currentIdx = zoneIdx;
            viewLevel = ViewLevel::ZONE;
        }
    }
}

void WorldMap::zoomOut() {
    if (viewLevel == ViewLevel::ZONE) {
        if (continentIdx >= 0) {
            requestComposite(continentIdx);
            currentIdx = continentIdx;
            viewLevel = ViewLevel::CONTINENT;
        }
    } else if (viewLevel == ViewLevel::CONTINENT) {
        enterWorldView();
    }
}

// --------------------------------------------------------
// Main render (input + ImGui overlay)
// --------------------------------------------------------

void WorldMap::render(const glm::vec3& playerRenderPos, int screenWidth, int screenHeight,
                      float playerYawDeg) {
    if (!initialized || !assetManager) return;

    auto& input = core::Input::getInstance();

    if (!zones.empty()) updateExploration(playerRenderPos);

    // game_screen owns the open/close toggle (via showWorldMap_ + TOGGLE_WORLD_MAP keybinding).
    // render() is only called when showWorldMap_ is true, so treat each call as "should be open".
    if (!open) {
        // First time shown: load zones and navigate to player's location.
        open = true;
        if (zones.empty()) loadZonesFromDBC();

        int bestContinent = findBestContinentForPlayer(playerRenderPos);
        if (bestContinent >= 0 && bestContinent != continentIdx) {
            continentIdx = bestContinent;
            compositedIdx = -1;
        }

        int playerZone = findZoneForPlayer(playerRenderPos);
        if (playerZone >= 0 && continentIdx >= 0 &&
            zoneBelongsToContinent(playerZone, continentIdx)) {
            loadZoneTextures(playerZone);
            requestComposite(playerZone);
            currentIdx = playerZone;
            viewLevel = ViewLevel::ZONE;
        } else if (continentIdx >= 0) {
            loadZoneTextures(continentIdx);
            requestComposite(continentIdx);
            currentIdx = continentIdx;
            viewLevel = ViewLevel::CONTINENT;
        }
    }

    // ESC closes the map; game_screen will sync showWorldMap_ via wm->isOpen() next frame.
    if (input.isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        open = false;
        return;
    }

    {
        auto& io = ImGui::GetIO();
        float wheelDelta = io.MouseWheel;
        if (std::abs(wheelDelta) < 0.001f)
            wheelDelta = input.getMouseWheelDelta();
        if (wheelDelta > 0.0f) zoomIn(playerRenderPos);
        else if (wheelDelta < 0.0f) zoomOut();
    }

    if (!open) return;
    renderImGuiOverlay(playerRenderPos, screenWidth, screenHeight, playerYawDeg);
}

// --------------------------------------------------------
// ImGui overlay
// --------------------------------------------------------

void WorldMap::renderImGuiOverlay(const glm::vec3& playerRenderPos, int screenWidth, int screenHeight, float playerYawDeg) {
    float sw = static_cast<float>(screenWidth);
    float sh = static_cast<float>(screenHeight);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(sw, sh));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    if (ImGui::Begin("##WorldMap", nullptr, flags)) {
        float mapAspect = static_cast<float>(FBO_W) / static_cast<float>(FBO_H);
        float availW = sw * 0.85f;
        float availH = sh * 0.85f;
        float displayW, displayH;
        if (availW / availH > mapAspect) {
            displayH = availH;
            displayW = availH * mapAspect;
        } else {
            displayW = availW;
            displayH = availW / mapAspect;
        }

        float mapX = (sw - displayW) / 2.0f;
        float mapY = (sh - displayH) / 2.0f;

        ImGui::SetCursorPos(ImVec2(mapX, mapY));
        // Display composite render target via ImGui (VkDescriptorSet as ImTextureID)
        ImGui::Image(reinterpret_cast<ImTextureID>(imguiDisplaySet),
                     ImVec2(displayW, displayH), ImVec2(0, 0), ImVec2(1, 1));

        ImVec2 imgMin = ImGui::GetItemRectMin();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        std::vector<int> continentIndices;
        bool hasLeafContinents = false;
        for (int i = 0; i < static_cast<int>(zones.size()); i++) {
            if (isLeafContinent(zones, i)) { hasLeafContinents = true; break; }
        }
        for (int i = 0; i < static_cast<int>(zones.size()); i++) {
            if (zones[i].areaID != 0) continue;
            if (hasLeafContinents) {
                if (isLeafContinent(zones, i)) continentIndices.push_back(i);
            } else if (!isRootContinent(zones, i)) {
                continentIndices.push_back(i);
            }
        }
        if (continentIndices.size() > 1) {
            std::vector<int> filtered;
            filtered.reserve(continentIndices.size());
            for (int idx : continentIndices) {
                if (zones[idx].areaName == mapName) continue;
                filtered.push_back(idx);
            }
            if (!filtered.empty()) continentIndices = std::move(filtered);
        }
        if (continentIndices.empty()) {
            for (int i = 0; i < static_cast<int>(zones.size()); i++) {
                if (zones[i].areaID == 0) continentIndices.push_back(i);
            }
        }

        // World-level continent selection UI
        if (viewLevel == ViewLevel::WORLD && !continentIndices.empty()) {
            ImVec2 titleSz = ImGui::CalcTextSize("World");
            ImGui::SetCursorPos(ImVec2((sw - titleSz.x) * 0.5f, mapY + 8.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 0.95f), "World");

            ImGui::SetCursorPos(ImVec2(mapX + 8.0f, mapY + 32.0f));
            for (size_t i = 0; i < continentIndices.size(); i++) {
                int ci = continentIndices[i];
                if (i > 0) ImGui::SameLine();
                const bool selected = (ci == continentIdx);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.25f, 0.05f, 0.9f));

                std::string rawName = zones[ci].areaName.empty() ? "Continent" : zones[ci].areaName;
                if (rawName == "Azeroth") rawName = "Eastern Kingdoms";
                std::string label = rawName + "##" + std::to_string(ci);
                if (ImGui::Button(label.c_str())) {
                    continentIdx = ci;
                    loadZoneTextures(continentIdx);
                    requestComposite(continentIdx);
                    currentIdx = continentIdx;
                    viewLevel = ViewLevel::CONTINENT;
                }
                if (selected) ImGui::PopStyleColor();
            }
        } else if (viewLevel == ViewLevel::CONTINENT && continentIndices.size() > 1) {
            ImGui::SetCursorPos(ImVec2(mapX + 8.0f, mapY + 8.0f));
            for (size_t i = 0; i < continentIndices.size(); i++) {
                int ci = continentIndices[i];
                if (i > 0) ImGui::SameLine();
                const bool selected = (ci == continentIdx);
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.25f, 0.05f, 0.9f));

                std::string rawName = zones[ci].areaName.empty() ? "Continent" : zones[ci].areaName;
                if (rawName == "Azeroth") rawName = "Eastern Kingdoms";
                std::string label = rawName + "##" + std::to_string(ci);
                if (ImGui::Button(label.c_str())) {
                    continentIdx = ci;
                    loadZoneTextures(continentIdx);
                    requestComposite(continentIdx);
                    currentIdx = continentIdx;
                }
                if (selected) ImGui::PopStyleColor();
            }
        }

        // Player marker
        if (currentIdx >= 0 && viewLevel != ViewLevel::WORLD) {
            glm::vec2 playerUV = renderPosToMapUV(playerRenderPos, currentIdx);
            if (playerUV.x >= 0.0f && playerUV.x <= 1.0f &&
                playerUV.y >= 0.0f && playerUV.y <= 1.0f) {
                float px = imgMin.x + playerUV.x * displayW;
                float py = imgMin.y + playerUV.y * displayH;
                // Directional arrow: render-space (cos,sin) maps to screen (-dx,-dy)
                // because render+X=west=left and render+Y=north=up (screen Y is down).
                float yawRad = glm::radians(playerYawDeg);
                float adx = -std::cos(yawRad);   // screen-space arrow X
                float ady = -std::sin(yawRad);   // screen-space arrow Y
                float apx = -ady, apy = adx;     // perpendicular (left/right of arrow)
                constexpr float TIP  = 9.0f;     // tip distance from center
                constexpr float TAIL = 4.0f;     // tail distance from center
                constexpr float HALF = 5.0f;     // half base width
                ImVec2 tip(px + adx * TIP,  py + ady * TIP);
                ImVec2 bl (px - adx * TAIL + apx * HALF,  py - ady * TAIL + apy * HALF);
                ImVec2 br (px - adx * TAIL - apx * HALF,  py - ady * TAIL - apy * HALF);
                drawList->AddTriangleFilled(tip, bl, br, IM_COL32(255, 40, 40, 255));
                drawList->AddTriangle(tip, bl, br, IM_COL32(0, 0, 0, 200), 1.5f);
            }
        }

        // Party member dots
        if (currentIdx >= 0 && viewLevel != ViewLevel::WORLD) {
            ImFont* font = ImGui::GetFont();
            for (const auto& dot : partyDots_) {
                glm::vec2 uv = renderPosToMapUV(dot.renderPos, currentIdx);
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;
                float px = imgMin.x + uv.x * displayW;
                float py = imgMin.y + uv.y * displayH;
                drawList->AddCircleFilled(ImVec2(px, py), 5.0f, dot.color);
                drawList->AddCircle(ImVec2(px, py), 5.0f, IM_COL32(0, 0, 0, 200), 0, 1.5f);
                // Name tooltip on hover
                if (!dot.name.empty()) {
                    ImVec2 mp = ImGui::GetMousePos();
                    float dx = mp.x - px, dy = mp.y - py;
                    if (dx * dx + dy * dy <= 49.0f) {  // radius 7 px hit area
                        ImGui::SetTooltip("%s", dot.name.c_str());
                    }
                    // Draw name label above the dot
                    ImVec2 nameSz = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, dot.name.c_str());
                    float tx = px - nameSz.x * 0.5f;
                    float ty = py - nameSz.y - 7.0f;
                    drawList->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 180), dot.name.c_str());
                    drawList->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 220), dot.name.c_str());
                }
            }
        }

        // Taxi node markers — flight master icons on the map
        if (currentIdx >= 0 && viewLevel != ViewLevel::WORLD && !taxiNodes_.empty()) {
            ImVec2 mp = ImGui::GetMousePos();
            for (const auto& node : taxiNodes_) {
                if (!node.known) continue;
                if (static_cast<int>(node.mapId) != currentMapId_) continue;

                glm::vec3 rPos = core::coords::canonicalToRender(
                    glm::vec3(node.wowX, node.wowY, node.wowZ));
                glm::vec2 uv = renderPosToMapUV(rPos, currentIdx);
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;

                float px = imgMin.x + uv.x * displayW;
                float py = imgMin.y + uv.y * displayH;

                // Flight-master icon: yellow diamond with dark border
                constexpr float H = 5.0f;  // half-size of diamond
                ImVec2 top2(px,     py - H);
                ImVec2 right2(px + H, py    );
                ImVec2 bot2(px,     py + H);
                ImVec2 left2(px - H, py    );
                drawList->AddQuadFilled(top2, right2, bot2, left2,
                                        IM_COL32(255, 215, 0, 230));
                drawList->AddQuad(top2, right2, bot2, left2,
                                  IM_COL32(80, 50, 0, 200), 1.2f);

                // Tooltip on hover
                if (!node.name.empty()) {
                    float mdx = mp.x - px, mdy = mp.y - py;
                    if (mdx * mdx + mdy * mdy < 49.0f) {
                        ImGui::SetTooltip("%s\n(Flight Master)", node.name.c_str());
                    }
                }
            }
        }

        // Quest POI markers — golden exclamation marks / question marks
        if (currentIdx >= 0 && viewLevel != ViewLevel::WORLD && !questPois_.empty()) {
            ImVec2 mp = ImGui::GetMousePos();
            ImFont* qFont = ImGui::GetFont();
            for (const auto& qp : questPois_) {
                glm::vec3 rPos = core::coords::canonicalToRender(
                    glm::vec3(qp.wowX, qp.wowY, 0.0f));
                glm::vec2 uv = renderPosToMapUV(rPos, currentIdx);
                if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;

                float px = imgMin.x + uv.x * displayW;
                float py = imgMin.y + uv.y * displayH;

                // Cyan circle with golden ring (matches minimap POI style)
                drawList->AddCircleFilled(ImVec2(px, py), 5.0f, IM_COL32(0, 210, 255, 220));
                drawList->AddCircle(ImVec2(px, py), 5.0f, IM_COL32(255, 215, 0, 220), 0, 1.5f);

                // Quest name label
                if (!qp.name.empty()) {
                    ImVec2 nameSz = qFont->CalcTextSizeA(ImGui::GetFontSize() * 0.85f, FLT_MAX, 0.0f, qp.name.c_str());
                    float tx = px - nameSz.x * 0.5f;
                    float ty = py - nameSz.y - 7.0f;
                    drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                      ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 180), qp.name.c_str());
                    drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                      ImVec2(tx, ty), IM_COL32(255, 230, 100, 230), qp.name.c_str());
                }
                // Tooltip on hover
                float mdx = mp.x - px, mdy = mp.y - py;
                if (mdx * mdx + mdy * mdy < 49.0f && !qp.name.empty()) {
                    ImGui::SetTooltip("%s\n(Quest Objective)", qp.name.c_str());
                }
            }
        }

        // Corpse marker — skull X shown when player is a ghost with unclaimed corpse
        if (hasCorpse_ && currentIdx >= 0 && viewLevel != ViewLevel::WORLD) {
            glm::vec2 uv = renderPosToMapUV(corpseRenderPos_, currentIdx);
            if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f) {
                float cx = imgMin.x + uv.x * displayW;
                float cy = imgMin.y + uv.y * displayH;
                constexpr float R = 5.0f;   // cross arm half-length
                constexpr float T = 1.8f;   // line thickness
                // Dark outline
                drawList->AddLine(ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R),
                                  IM_COL32(0, 0, 0, 220), T + 1.5f);
                drawList->AddLine(ImVec2(cx + R, cy - R), ImVec2(cx - R, cy + R),
                                  IM_COL32(0, 0, 0, 220), T + 1.5f);
                // Bone-white X
                drawList->AddLine(ImVec2(cx - R, cy - R), ImVec2(cx + R, cy + R),
                                  IM_COL32(230, 220, 200, 240), T);
                drawList->AddLine(ImVec2(cx + R, cy - R), ImVec2(cx - R, cy + R),
                                  IM_COL32(230, 220, 200, 240), T);
                // Tooltip on hover
                ImVec2 mp = ImGui::GetMousePos();
                float dx = mp.x - cx, dy = mp.y - cy;
                if (dx * dx + dy * dy < 64.0f) {
                    ImGui::SetTooltip("Your corpse");
                }
            }
        }

        // Hover coordinate display — show WoW coordinates under cursor
        if (currentIdx >= 0 && viewLevel != ViewLevel::WORLD) {
            auto& io = ImGui::GetIO();
            ImVec2 mp = io.MousePos;
            if (mp.x >= imgMin.x && mp.x <= imgMin.x + displayW &&
                mp.y >= imgMin.y && mp.y <= imgMin.y + displayH) {
                float mu = (mp.x - imgMin.x) / displayW;
                float mv = (mp.y - imgMin.y) / displayH;

                const auto& zone = zones[currentIdx];
                float left = zone.locLeft, right = zone.locRight;
                float top = zone.locTop, bottom = zone.locBottom;
                if (zone.areaID == 0) {
                    float l, r, t, b;
                    getContinentProjectionBounds(currentIdx, l, r, t, b);
                    left = l; right = r; top = t; bottom = b;
                    // Undo the kVOffset applied during renderPosToMapUV for continent
                    constexpr float kVOffset = -0.15f;
                    mv -= kVOffset;
                }

                float hWowX = left - mu * (left - right);
                float hWowY = top  - mv * (top  - bottom);

                char coordBuf[32];
                snprintf(coordBuf, sizeof(coordBuf), "%.0f, %.0f", hWowX, hWowY);
                ImVec2 coordSz = ImGui::CalcTextSize(coordBuf);
                float cx = imgMin.x + displayW - coordSz.x - 8.0f;
                float cy = imgMin.y + displayH - coordSz.y - 8.0f;
                drawList->AddText(ImVec2(cx + 1.0f, cy + 1.0f), IM_COL32(0, 0, 0, 180), coordBuf);
                drawList->AddText(ImVec2(cx, cy), IM_COL32(220, 210, 150, 230), coordBuf);
            }
        }

        // Continent view: clickable zone overlays
        if (viewLevel == ViewLevel::CONTINENT && continentIdx >= 0) {
            const auto& cont = zones[continentIdx];
            float cLeft = cont.locLeft, cRight = cont.locRight;
            float cTop = cont.locTop, cBottom = cont.locBottom;
            getContinentProjectionBounds(continentIdx, cLeft, cRight, cTop, cBottom);
            float cDenomU = cLeft - cRight;
            float cDenomV = cTop - cBottom;

            ImVec2 mousePos = ImGui::GetMousePos();
            int hoveredZone = -1;

            if (std::abs(cDenomU) > 0.001f && std::abs(cDenomV) > 0.001f) {
                for (int zi = 0; zi < static_cast<int>(zones.size()); zi++) {
                    if (!zoneBelongsToContinent(zi, continentIdx)) continue;
                    const auto& z = zones[zi];
                    if (std::abs(z.locLeft - z.locRight) < 0.001f ||
                        std::abs(z.locTop - z.locBottom) < 0.001f) continue;

                    float zuMin = (cLeft - z.locLeft) / cDenomU;
                    float zuMax = (cLeft - z.locRight) / cDenomU;
                    float zvMin = (cTop - z.locTop) / cDenomV;
                    float zvMax = (cTop - z.locBottom) / cDenomV;

                    constexpr float kOverlayShrink = 0.92f;
                    float cu = (zuMin + zuMax) * 0.5f, cv = (zvMin + zvMax) * 0.5f;
                    float hu = (zuMax - zuMin) * 0.5f * kOverlayShrink;
                    float hv = (zvMax - zvMin) * 0.5f * kOverlayShrink;
                    zuMin = cu - hu; zuMax = cu + hu;
                    zvMin = cv - hv; zvMax = cv + hv;

                    constexpr float kVOffset = -0.15f;
                    zvMin = (zvMin - 0.5f) + 0.5f + kVOffset;
                    zvMax = (zvMax - 0.5f) + 0.5f + kVOffset;

                    zuMin = std::clamp(zuMin, 0.0f, 1.0f);
                    zuMax = std::clamp(zuMax, 0.0f, 1.0f);
                    zvMin = std::clamp(zvMin, 0.0f, 1.0f);
                    zvMax = std::clamp(zvMax, 0.0f, 1.0f);
                    if (zuMax - zuMin < 0.001f || zvMax - zvMin < 0.001f) continue;

                    float sx0 = imgMin.x + zuMin * displayW;
                    float sy0 = imgMin.y + zvMin * displayH;
                    float sx1 = imgMin.x + zuMax * displayW;
                    float sy1 = imgMin.y + zvMax * displayH;

                    bool explored = exploredZones.count(zi) > 0;
                    bool hovered = (mousePos.x >= sx0 && mousePos.x <= sx1 &&
                                    mousePos.y >= sy0 && mousePos.y <= sy1);

                    if (!explored) {
                        drawList->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                                IM_COL32(0, 0, 0, 160));
                    }
                    if (hovered) {
                        hoveredZone = zi;
                        drawList->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                                IM_COL32(255, 255, 200, 40));
                        drawList->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                          IM_COL32(255, 215, 0, 180), 0.0f, 0, 2.0f);
                    } else if (explored) {
                        drawList->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1),
                                          IM_COL32(255, 255, 255, 30), 0.0f, 0, 1.0f);
                    }

                    // Zone name label — only if the rect is large enough to fit it
                    if (!z.areaName.empty()) {
                        ImVec2 textSz = ImGui::CalcTextSize(z.areaName.c_str());
                        float rectW = sx1 - sx0;
                        float rectH = sy1 - sy0;
                        if (rectW > textSz.x + 4.0f && rectH > textSz.y + 2.0f) {
                            float tx = (sx0 + sx1) * 0.5f - textSz.x * 0.5f;
                            float ty = (sy0 + sy1) * 0.5f - textSz.y * 0.5f;
                            ImU32 labelCol = explored
                                ? IM_COL32(255, 230, 150, 210)
                                : IM_COL32(160, 160, 160, 80);
                            drawList->AddText(ImVec2(tx + 1.0f, ty + 1.0f),
                                              IM_COL32(0, 0, 0, 130), z.areaName.c_str());
                            drawList->AddText(ImVec2(tx, ty), labelCol, z.areaName.c_str());
                        }
                    }
                }
            }

            if (hoveredZone >= 0) {
                ImGui::SetTooltip("%s", zones[hoveredZone].areaName.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    loadZoneTextures(hoveredZone);
                    requestComposite(hoveredZone);
                    currentIdx = hoveredZone;
                    viewLevel = ViewLevel::ZONE;
                }
            }
        }

        // Zone view: back to continent
        if (viewLevel == ViewLevel::ZONE && continentIdx >= 0) {
            auto& io = ImGui::GetIO();
            bool goBack = io.MouseClicked[1];

            ImGui::SetCursorPos(ImVec2(mapX + 8.0f, mapY + 8.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
            if (ImGui::Button("< Back")) goBack = true;
            ImGui::PopStyleColor(3);

            if (goBack) {
                requestComposite(continentIdx);
                currentIdx = continentIdx;
                viewLevel = ViewLevel::CONTINENT;
            }

            const char* zoneName = zones[currentIdx].areaName.c_str();
            ImVec2 nameSize = ImGui::CalcTextSize(zoneName);
            float nameY = mapY - nameSize.y - 8.0f;
            if (nameY > 0.0f) {
                ImGui::SetCursorPos(ImVec2((sw - nameSize.x) / 2.0f, nameY));
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 0.9f), "%s", zoneName);
            }
        }

        // Continent view: back to world
        if (viewLevel == ViewLevel::CONTINENT) {
            auto& io = ImGui::GetIO();
            bool goWorld = io.MouseClicked[1];

            float worldBtnY = mapY + (continentIndices.size() > 1 ? 40.0f : 8.0f);
            ImGui::SetCursorPos(ImVec2(mapX + 8.0f, worldBtnY));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
            if (ImGui::Button("< World")) goWorld = true;
            ImGui::PopStyleColor(3);

            if (goWorld) enterWorldView();
        }

        // Help text
        const char* helpText;
        if (viewLevel == ViewLevel::ZONE)
            helpText = "Scroll out or right-click to zoom out | M or Escape to close";
        else if (viewLevel == ViewLevel::WORLD)
            helpText = "Select a continent | Scroll in to zoom | M or Escape to close";
        else
            helpText = "Click zone or scroll in to zoom | Scroll out / right-click for World | M or Escape to close";

        ImVec2 textSize = ImGui::CalcTextSize(helpText);
        float textY = mapY + displayH + 8.0f;
        if (textY + textSize.y < sh) {
            ImGui::SetCursorPos(ImVec2((sw - textSize.x) / 2.0f, textY));
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.8f), "%s", helpText);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace rendering
} // namespace wowee
