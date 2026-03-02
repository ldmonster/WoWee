#pragma once

#include "pipeline/m2_loader.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <random>
#include <future>

namespace wowee {

namespace pipeline {
    class AssetManager;
}

namespace rendering {

class Camera;
class VkContext;
class VkTexture;

/**
 * GPU representation of an M2 model
 */
struct M2ModelGPU {
    struct BatchGPU {
        VkTexture* texture = nullptr;  // from cache, NOT owned
        VkDescriptorSet materialSet = VK_NULL_HANDLE;  // set 1
        ::VkBuffer materialUBO = VK_NULL_HANDLE;
        VmaAllocation materialUBOAlloc = VK_NULL_HANDLE;
        void* materialUBOMapped = nullptr;  // cached mapped pointer (avoids per-frame vmaGetAllocationInfo)
        uint32_t indexStart = 0;   // offset in indices (not bytes)
        uint32_t indexCount = 0;
        bool hasAlpha = false;
        bool colorKeyBlack = false;
        uint16_t textureAnimIndex = 0xFFFF; // 0xFFFF = no texture animation
        uint16_t blendMode = 0;   // 0=Opaque, 1=AlphaKey, 2=Alpha, 3=Add, etc.
        uint16_t materialFlags = 0; // M2 material flags (0x01=Unlit, 0x04=TwoSided, 0x10=NoDepthWrite)
        uint16_t submeshLevel = 0; // LOD level: 0=base, 1=LOD1, 2=LOD2, 3=LOD3
        uint8_t textureUnit = 0;  // UV set index (0=texCoords[0], 1=texCoords[1])
        uint8_t texFlags = 0;     // M2Texture.flags (bit0=WrapS, bit1=WrapT)
        bool lanternGlowHint = false; // Texture/model hints this batch is a glow-card billboard
        bool glowCardLike = false; // Batch likely is a flat emissive card that should be sprite-replaced
        uint8_t glowTint = 0; // 0=warm, 1=cool, 2=red
        float batchOpacity = 1.0f; // Resolved texture weight opacity (0=transparent, skip batch)
        glm::vec3 center = glm::vec3(0.0f); // Center of batch geometry (model space)
        float glowSize = 1.0f;              // Approx radius of batch geometry
    };

    ::VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    ::VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    std::vector<BatchGPU> batches;

    glm::vec3 boundMin;
    glm::vec3 boundMax;
    float boundRadius = 0.0f;
    bool collisionSteppedFountain = false;
    bool collisionSteppedLowPlatform = false;
    bool collisionPlanter = false;
    bool collisionBridge = false;
    bool collisionSmallSolidProp = false;
    bool collisionNarrowVerticalProp = false;
    bool collisionTreeTrunk = false;
    bool collisionNoBlock = false;
    bool collisionStatue = false;
    bool isSmallFoliage = false;  // Small foliage (bushes, grass, plants) - skip during taxi
    bool isInvisibleTrap = false; // Invisible trap objects (don't render, no collision)
    bool isGroundDetail = false;  // Ground clutter/detail doodads (special fallback render path)
    bool isWaterVegetation = false; // Cattails, reeds, kelp etc. near water (insect spawning)
    bool isFireflyEffect = false;   // Firefly/fireflies M2 (exempt from particle dampeners)

    // Collision mesh with spatial grid (from M2 bounding geometry)
    struct CollisionMesh {
        std::vector<glm::vec3> vertices;
        std::vector<uint16_t> indices;
        uint32_t triCount = 0;

        struct TriBounds { float minZ, maxZ; };
        std::vector<TriBounds> triBounds;

        static constexpr float CELL_SIZE = 4.0f;
        glm::vec2 gridOrigin{0.0f};
        int gridCellsX = 0, gridCellsY = 0;
        std::vector<std::vector<uint32_t>> cellFloorTris;
        std::vector<std::vector<uint32_t>> cellWallTris;

        void build();
        void getFloorTrisInRange(float minX, float minY, float maxX, float maxY,
                                 std::vector<uint32_t>& out) const;
        void getWallTrisInRange(float minX, float minY, float maxX, float maxY,
                                std::vector<uint32_t>& out) const;
        bool valid() const { return triCount > 0; }
    };
    CollisionMesh collision;

    std::string name;

    // Skeletal animation data (kept from M2Model for bone computation)
    std::vector<pipeline::M2Bone> bones;
    std::vector<pipeline::M2Sequence> sequences;
    std::vector<uint32_t> globalSequenceDurations;  // Loop durations for global sequence tracks
    bool hasAnimation = false;  // True if any bone has keyframes
    bool isSmoke = false;       // True for smoke models (UV scroll animation)
    bool isSpellEffect = false;  // True for spell effect models (skip particle dampeners)
    bool disableAnimation = false; // Keep foliage/tree doodads visually stable
    bool shadowWindFoliage = false; // Apply wind sway in shadow pass for foliage/tree cards
    bool isFoliageLike = false;     // Model name matches foliage/tree/bush/grass etc (precomputed)
    bool isElvenLike = false;       // Model name matches elf/elven/quel (precomputed)
    bool isLanternLike = false;     // Model name matches lantern/lamp/light (precomputed)
    bool isKoboldFlame = false;     // Model name matches kobold+(candle/torch/mine) (precomputed)
    bool hasTextureAnimation = false; // True if any batch has UV animation

    // Particle emitter data (kept from M2Model)
    std::vector<pipeline::M2ParticleEmitter> particleEmitters;
    std::vector<VkTexture*> particleTextures;  // Resolved Vulkan textures per emitter

    // Texture transform data for UV animation
    std::vector<pipeline::M2TextureTransform> textureTransforms;
    std::vector<uint16_t> textureTransformLookup;
    std::vector<int> idleVariationIndices;  // Sequence indices for idle variations (animId 0)

    bool isValid() const { return vertexBuffer != VK_NULL_HANDLE && indexCount > 0; }
};

/**
 * A single M2 particle emitted from a particle emitter
 */
struct M2Particle {
    glm::vec3 position;
    glm::vec3 velocity;
    float life;        // current age in seconds
    float maxLife;     // total lifespan
    int emitterIndex;  // which emitter spawned this
    float tileIndex = 0.0f; // texture atlas tile index
};

/**
 * Instance of an M2 model in the world
 */
struct M2Instance {
    uint32_t id = 0;     // Unique instance ID
    uint32_t modelId;
    glm::vec3 position;
    glm::vec3 rotation;  // Euler angles in degrees
    float scale;
    glm::mat4 modelMatrix;
    glm::mat4 invModelMatrix;
    glm::vec3 worldBoundsMin;
    glm::vec3 worldBoundsMax;

    // Animation state
    float animTime = 0.0f;       // Current animation time (ms)
    float animSpeed = 1.0f;      // Animation playback speed
    int currentSequenceIndex = 0;// Index into sequences array
    float animDuration = 0.0f;   // Duration of current animation (ms)
    std::vector<glm::mat4> boneMatrices;

    // Idle variation state
    int idleSequenceIndex = 0;   // Default idle sequence index
    float variationTimer = 0.0f; // Time until next variation attempt (ms)
    bool playingVariation = false;// Currently playing a one-shot variation

    // Particle emitter state
    std::vector<float> emitterAccumulators;  // fractional particle counter per emitter
    std::vector<M2Particle> particles;

    // Frame-skip optimization (update distant animations less frequently)
    uint8_t frameSkipCounter = 0;

    // Per-instance bone SSBO (double-buffered)
    ::VkBuffer boneBuffer[2] = {};
    VmaAllocation boneAlloc[2] = {};
    void* boneMapped[2] = {};
    VkDescriptorSet boneSet[2] = {};

    void updateModelMatrix();
};

/**
 * A single smoke particle emitted from a chimney or similar M2 model
 */
struct SmokeParticle {
    glm::vec3 position;
    glm::vec3 velocity;
    float life = 0.0f;
    float maxLife = 3.0f;
    float size = 1.0f;
    float isSpark = 0.0f;  // 0 = smoke, 1 = ember/spark
    uint32_t instanceId = 0;
};

// M2 material UBO — matches M2Material in m2.frag.glsl (set 1, binding 2)
struct M2MaterialUBO {
    int32_t hasTexture;
    int32_t alphaTest;
    int32_t colorKeyBlack;
    float colorKeyThreshold;
    int32_t unlit;
    int32_t blendMode;
    float fadeAlpha;
    float interiorDarken;
    float specularIntensity;
};

// M2 params UBO — matches M2Params in m2.vert.glsl (set 1, binding 1)
struct M2ParamsUBO {
    float uvOffsetX;
    float uvOffsetY;
    int32_t texCoordSet;
    int32_t useBones;
};

/**
 * M2 Model Renderer (Vulkan)
 *
 * Handles rendering of M2 models (doodads like trees, rocks, bushes)
 */
class M2Renderer {
public:
    M2Renderer();
    ~M2Renderer();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                    pipeline::AssetManager* assets);
    void shutdown();

    bool hasModel(uint32_t modelId) const;
    bool loadModel(const pipeline::M2Model& model, uint32_t modelId);

    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                            const glm::vec3& rotation = glm::vec3(0.0f),
                            float scale = 1.0f);
    uint32_t createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                       const glm::vec3& position);

    void update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection);

    /**
     * Render all visible instances (Vulkan)
     */
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera);

    /**
     * Initialize shadow pipeline (Phase 7)
     */
    bool initializeShadow(VkRenderPass shadowRenderPass);
    bool hasShadowPipeline() const { return shadowPipeline_ != VK_NULL_HANDLE; }

    /**
     * Render depth-only pass for shadow casting
     */
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, float globalTime = 0.0f,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    /**
     * Render M2 particle emitters (point sprites)
     */
    void renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /**
     * Render smoke particles from chimneys etc.
     */
    void renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    void setInstanceTransform(uint32_t instanceId, const glm::mat4& transform);
    void setInstanceAnimationFrozen(uint32_t instanceId, bool frozen);
    void removeInstance(uint32_t instanceId);
    void removeInstances(const std::vector<uint32_t>& instanceIds);
    void clear();
    void cleanupUnusedModels();

    bool checkCollision(const glm::vec3& from, const glm::vec3& to,
                        glm::vec3& adjustedPos, float playerRadius = 0.5f) const;
    std::optional<float> getFloorHeight(float glX, float glY, float glZ, float* outNormalZ = nullptr) const;
    float raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;
    void setCollisionFocus(const glm::vec3& worldPos, float radius);
    void clearCollisionFocus();

    void resetQueryStats();
    double getQueryTimeMs() const { return queryTimeMs; }
    uint32_t getQueryCallCount() const { return queryCallCount; }

    void recreatePipelines();

    // Stats
    bool isInitialized() const { return initialized_; }
    uint32_t getModelCount() const { return static_cast<uint32_t>(models.size()); }
    uint32_t getInstanceCount() const { return static_cast<uint32_t>(instances.size()); }
    uint32_t getTotalTriangleCount() const;
    uint32_t getDrawCallCount() const { return lastDrawCallCount; }

    // Lighting/fog/shadow are now in per-frame UBO; these are no-ops for API compat
    void setFog(const glm::vec3& /*color*/, float /*start*/, float /*end*/) {}
    void setLighting(const float /*lightDirIn*/[3], const float /*lightColorIn*/[3],
                     const float /*ambientColorIn*/[3]) {}
    void setShadowMap(uint32_t /*depthTex*/, const glm::mat4& /*lightSpace*/) {}
    void clearShadowMap() {}

    void setInsideInterior(bool inside) { insideInterior = inside; }
    void setOnTaxi(bool onTaxi) { onTaxi_ = onTaxi; }

    std::vector<glm::vec3> getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const;

private:
    bool initialized_ = false;
    bool insideInterior = false;
    bool onTaxi_ = false;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan context
    VkContext* vkCtx_ = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;       // blend mode 0
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;     // blend mode 1
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;         // blend mode 2
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;      // blend mode 3+
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Shadow rendering (Phase 7)
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout shadowParamsLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool shadowParamsPool_ = VK_NULL_HANDLE;
    VkDescriptorSet shadowParamsSet_ = VK_NULL_HANDLE;
    ::VkBuffer shadowParamsUBO_ = VK_NULL_HANDLE;
    VmaAllocation shadowParamsAlloc_ = VK_NULL_HANDLE;
    // Per-frame pool for foliage shadow texture descriptor sets
    VkDescriptorPool shadowTexPool_ = VK_NULL_HANDLE;

    // Particle pipelines
    VkPipeline particlePipeline_ = VK_NULL_HANDLE;       // M2 emitter particles
    VkPipeline particleAdditivePipeline_ = VK_NULL_HANDLE; // Additive particle blend
    VkPipelineLayout particlePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline smokePipeline_ = VK_NULL_HANDLE;           // Smoke particles
    VkPipelineLayout smokePipelineLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;      // set 2
    VkDescriptorSetLayout particleTexLayout_ = VK_NULL_HANDLE;  // particle set 1 (texture only)

    // Descriptor pools
    VkDescriptorPool materialDescPool_ = VK_NULL_HANDLE;
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_MATERIAL_SETS = 8192;
    static constexpr uint32_t MAX_BONE_SETS = 2048;

    // Dynamic particle buffers
    ::VkBuffer smokeVB_ = VK_NULL_HANDLE;
    VmaAllocation smokeVBAlloc_ = VK_NULL_HANDLE;
    void* smokeVBMapped_ = nullptr;
    ::VkBuffer m2ParticleVB_ = VK_NULL_HANDLE;
    VmaAllocation m2ParticleVBAlloc_ = VK_NULL_HANDLE;
    void* m2ParticleVBMapped_ = nullptr;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    std::vector<M2Instance> instances;

    uint32_t nextInstanceId = 1;
    uint32_t lastDrawCallCount = 0;
    size_t modelCacheLimit_ = 6000;
    uint32_t modelLimitRejectWarnings_ = 0;

    VkTexture* loadTexture(const std::string& path, uint32_t texFlags = 0);
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
        bool hasAlpha = true;
        bool colorKeyBlack = false;
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    std::unordered_map<VkTexture*, bool> textureHasAlphaByPtr_;
    std::unordered_map<VkTexture*, bool> textureColorKeyBlackByPtr_;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 2048ull * 1024 * 1024;
    std::unordered_set<std::string> failedTextureCache_;
    std::unordered_set<std::string> loggedTextureLoadFails_;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> glowTexture_;
    VkDescriptorSet glowTexDescSet_ = VK_NULL_HANDLE;  // cached glow texture descriptor (allocated once)

    // Optional query-space culling for collision/raycast hot paths.
    bool collisionFocusEnabled = false;
    glm::vec3 collisionFocusPos = glm::vec3(0.0f);
    float collisionFocusRadius = 0.0f;
    float collisionFocusRadiusSq = 0.0f;

    struct GridCell {
        int x;
        int y;
        int z;
        bool operator==(const GridCell& other) const {
            return x == other.x && y == other.y && z == other.z;
        }
    };
    struct GridCellHash {
        size_t operator()(const GridCell& c) const {
            size_t h1 = std::hash<int>()(c.x);
            size_t h2 = std::hash<int>()(c.y);
            size_t h3 = std::hash<int>()(c.z);
            return h1 ^ (h2 * 0x9e3779b9u) ^ (h3 * 0x85ebca6bu);
        }
    };
    GridCell toCell(const glm::vec3& p) const;
    void rebuildSpatialIndex();
    void gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax, std::vector<size_t>& outIndices) const;

    static constexpr float SPATIAL_CELL_SIZE = 64.0f;
    std::unordered_map<GridCell, std::vector<uint32_t>, GridCellHash> spatialGrid;
    std::unordered_map<uint32_t, size_t> instanceIndexById;
    mutable std::vector<size_t> candidateScratch;
    mutable std::unordered_set<uint32_t> candidateIdScratch;
    mutable std::vector<uint32_t> collisionTriScratch_;

    // Collision query profiling (per frame).
    mutable double queryTimeMs = 0.0;
    mutable uint32_t queryCallCount = 0;

    // Persistent render buffers (avoid per-frame allocation/deallocation)
    struct VisibleEntry {
        uint32_t index;
        uint32_t modelId;
        float distSq;
        float effectiveMaxDistSq;
    };
    std::vector<VisibleEntry> sortedVisible_;  // Reused each frame
    struct GlowSprite {
        glm::vec3 worldPos;
        glm::vec4 color;
        float size;
    };
    std::vector<GlowSprite> glowSprites_;  // Reused each frame

    // Animation update buffers (avoid per-frame allocation)
    std::vector<size_t> boneWorkIndices_;        // Reused each frame
    std::vector<std::future<void>> animFutures_; // Reused each frame
    bool spatialIndexDirty_ = false;

    // Smoke particle system
    std::vector<SmokeParticle> smokeParticles;
    static constexpr int MAX_SMOKE_PARTICLES = 1000;
    float smokeEmitAccum = 0.0f;
    std::mt19937 smokeRng{42};

    // M2 particle emitter system
    static constexpr size_t MAX_M2_PARTICLES = 4000;
    std::mt19937 particleRng_{123};

    // Cached camera state from update() for frustum-culling bones
    glm::vec3 cachedCamPos_ = glm::vec3(0.0f);
    float cachedMaxRenderDistSq_ = 0.0f;

    // Thread count for parallel bone animation
    uint32_t numAnimThreads_ = 1;

    float interpFloat(const pipeline::M2AnimationTrack& track, float animTime, int seqIdx,
                      const std::vector<pipeline::M2Sequence>& seqs,
                      const std::vector<uint32_t>& globalSeqDurations);
    float interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio);
    glm::vec3 interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio);
    void emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt);
    void updateParticles(M2Instance& inst, float dt);

    // Helper to allocate descriptor sets
    VkDescriptorSet allocateMaterialSet();
    VkDescriptorSet allocateBoneSet();

    // Helper to destroy model GPU resources
    void destroyModelGPU(M2ModelGPU& model);
    // Helper to destroy instance bone buffers
    void destroyInstanceBones(M2Instance& inst);
};

} // namespace rendering
} // namespace wowee
