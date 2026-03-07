#pragma once

#include "pipeline/terrain_mesh.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/camera.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace wowee {

// Forward declarations
namespace pipeline { class AssetManager; }

namespace rendering {

class VkContext;
class VkTexture;
class Frustum;

/**
 * GPU-side terrain chunk data (Vulkan)
 */
struct TerrainChunkGPU {
    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;

    // Material descriptor set (set 1: 7 samplers + params UBO)
    VkDescriptorSet materialSet = VK_NULL_HANDLE;

    // Per-chunk params UBO (hasLayer1/2/3)
    ::VkBuffer paramsUBO = VK_NULL_HANDLE;
    VmaAllocation paramsAlloc = VK_NULL_HANDLE;

    // Texture handles (owned by cache, NOT destroyed per-chunk)
    VkTexture* baseTexture = nullptr;
    VkTexture* layerTextures[3] = {nullptr, nullptr, nullptr};
    VkTexture* alphaTextures[3] = {nullptr, nullptr, nullptr};
    int layerCount = 0;

    // Per-chunk alpha textures (owned by this chunk, destroyed on removal)
    std::vector<std::unique_ptr<VkTexture>> ownedAlphaTextures;

    // World position for culling
    float worldX = 0.0f;
    float worldY = 0.0f;
    float worldZ = 0.0f;

    // Owning tile coordinates (for per-tile removal)
    int tileX = -1, tileY = -1;

    // Bounding sphere for frustum culling
    float boundingSphereRadius = 0.0f;
    glm::vec3 boundingSphereCenter = glm::vec3(0.0f);

    bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexBuffer != VK_NULL_HANDLE; }
};

/**
 * Terrain renderer (Vulkan)
 */
class TerrainRenderer {
public:
    TerrainRenderer();
    ~TerrainRenderer();

    /**
     * Initialize terrain renderer
     * @param ctx Vulkan context
     * @param perFrameLayout Descriptor set layout for set 0 (per-frame UBO)
     * @param assetManager Asset manager for loading textures
     */
    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assetManager);

    void shutdown();

    bool loadTerrain(const pipeline::TerrainMesh& mesh,
                     const std::vector<std::string>& texturePaths,
                     int tileX = -1, int tileY = -1);

    /// Upload a batch of terrain chunks incrementally. Returns true when all chunks done.
    /// chunkIndex is updated to the next chunk to process (0-255 row-major).
    bool loadTerrainIncremental(const pipeline::TerrainMesh& mesh,
                                const std::vector<std::string>& texturePaths,
                                int tileX, int tileY,
                                int& chunkIndex, int maxChunksPerCall = 16);

    void removeTile(int tileX, int tileY);

    void uploadPreloadedTextures(const std::unordered_map<std::string, pipeline::BLPImage>& textures);

    /**
     * Render terrain
     * @param cmd Command buffer to record into
     * @param perFrameSet Per-frame descriptor set (set 0)
     * @param camera Camera for frustum culling
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /**
     * Render terrain into shadow depth map (Phase 6 stub)
     */
    void renderShadow(VkCommandBuffer cmd, const glm::vec3& shadowCenter, float halfExtent);

    void clear();

    void recreatePipelines();

    void setWireframe(bool enabled) { wireframe = enabled; }
    void setFrustumCulling(bool enabled) { frustumCullingEnabled = enabled; }
    void setFogEnabled(bool enabled) { fogEnabled = enabled; }
    bool isFogEnabled() const { return fogEnabled; }

    // Shadow mapping stubs (Phase 6)
    void setShadowMap(VkDescriptorImageInfo /*depthInfo*/, const glm::mat4& /*lightSpaceMat*/) {}
    void clearShadowMap() {}

    int getChunkCount() const { return static_cast<int>(chunks.size()); }
    int getRenderedChunkCount() const { return renderedChunks; }
    int getCulledChunkCount() const { return culledChunks; }
    int getTriangleCount() const;

private:
    TerrainChunkGPU uploadChunk(const pipeline::ChunkMesh& chunk);
    VkTexture* loadTexture(const std::string& path);
    VkTexture* createAlphaTexture(const std::vector<uint8_t>& alphaData);
    bool isChunkVisible(const TerrainChunkGPU& chunk, const Frustum& frustum);
    void calculateBoundingSphere(TerrainChunkGPU& chunk, const pipeline::ChunkMesh& meshChunk);
    VkDescriptorSet allocateMaterialSet();
    void writeMaterialDescriptors(VkDescriptorSet set, const TerrainChunkGPU& chunk);
    void destroyChunkGPU(TerrainChunkGPU& chunk);

    VkContext* vkCtx = nullptr;
    pipeline::AssetManager* assetManager = nullptr;

    // Pipeline
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline wireframePipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;

    // Descriptor pool for material sets
    VkDescriptorPool materialDescPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_MATERIAL_SETS = 16384;

    // Loaded terrain chunks
    std::vector<TerrainChunkGPU> chunks;

    // Texture cache (path -> VkTexture)
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 4096ull * 1024 * 1024;
    std::unordered_set<std::string> failedTextureCache_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint32_t textureBudgetRejectWarnings_ = 0;

    // Fallback textures
    std::unique_ptr<VkTexture> whiteTexture;
    std::unique_ptr<VkTexture> opaqueAlphaTexture;

    // Rendering state
    bool wireframe = false;
    bool frustumCullingEnabled = true;
    bool fogEnabled = true;
    int renderedChunks = 0;
    int culledChunks = 0;
};

} // namespace rendering
} // namespace wowee
