#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <future>
#include <cstddef>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/sky_system.hpp"
#if WOWEE_HAS_AMD_FSR2
#include "ffx_fsr2.h"
#include "ffx_fsr2_vk.h"
#endif

namespace wowee {
namespace core { class Window; }
namespace rendering { class VkContext; }
namespace game { class World; class ZoneManager; class GameHandler; }
namespace audio { class MusicManager; class FootstepManager; class ActivitySoundManager; class MountSoundManager; class NpcVoiceManager; class AmbientSoundManager; class UiSoundManager; class CombatSoundManager; class SpellSoundManager; class MovementSoundManager; enum class FootstepSurface : uint8_t; enum class VoiceType; }
namespace pipeline { class AssetManager; }

namespace rendering {

class Camera;
class CameraController;
class Scene;
class TerrainRenderer;
class TerrainManager;
class PerformanceHUD;
class WaterRenderer;
class Skybox;
class Celestial;
class StarField;
class Clouds;
class LensFlare;
class Weather;
class Lightning;
class LightingManager;
class SwimEffects;
class MountDust;
class LevelUpEffect;
class ChargeEffect;
class CharacterRenderer;
class WMORenderer;
class M2Renderer;
class Minimap;
class WorldMap;
class QuestMarkerRenderer;
class CharacterPreview;
class Shader;
class AmdFsr3Runtime;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize(core::Window* window);
    void shutdown();

    void beginFrame();
    void endFrame();

    void renderWorld(game::World* world, game::GameHandler* gameHandler = nullptr);

    /**
     * Update renderer (camera, etc.)
     */
    void update(float deltaTime);

    /**
     * Load test terrain for debugging
     * @param assetManager Asset manager to load terrain data
     * @param adtPath Path to ADT file (e.g., "World\\Maps\\Azeroth\\Azeroth_32_49.adt")
     */
    bool loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath);

    /**
     * Initialize all sub-renderers (WMO, M2, Character, terrain, water, minimap, etc.)
     * without loading any ADT tile.  Used by WMO-only maps (dungeons/raids/BGs).
     */
    bool initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName);

    /**
     * Enable/disable terrain rendering
     */
    void setTerrainEnabled(bool enabled) { terrainEnabled = enabled; }

    /**
     * Enable/disable wireframe mode
     */
    void setWireframeMode(bool enabled);

    /**
     * Load terrain tiles around position
     * @param mapName Map name (e.g., "Azeroth", "Kalimdor")
     * @param centerX Center tile X coordinate
     * @param centerY Center tile Y coordinate
     * @param radius Load radius in tiles
     */
    bool loadTerrainArea(const std::string& mapName, int centerX, int centerY, int radius = 1);

    /**
     * Enable/disable terrain streaming
     */
    void setTerrainStreaming(bool enabled);

    /**
     * Render performance HUD
     */
    void renderHUD();

    Camera* getCamera() { return camera.get(); }
    CameraController* getCameraController() { return cameraController.get(); }
    Scene* getScene() { return scene.get(); }
    TerrainRenderer* getTerrainRenderer() const { return terrainRenderer.get(); }
    TerrainManager* getTerrainManager() const { return terrainManager.get(); }
    PerformanceHUD* getPerformanceHUD() { return performanceHUD.get(); }
    WaterRenderer* getWaterRenderer() const { return waterRenderer.get(); }
    Skybox* getSkybox() const { return skySystem ? skySystem->getSkybox() : nullptr; }
    Celestial* getCelestial() const { return skySystem ? skySystem->getCelestial() : nullptr; }
    StarField* getStarField() const { return skySystem ? skySystem->getStarField() : nullptr; }
    Clouds* getClouds() const { return skySystem ? skySystem->getClouds() : nullptr; }
    LensFlare* getLensFlare() const { return skySystem ? skySystem->getLensFlare() : nullptr; }
    Weather* getWeather() const { return weather.get(); }
    Lightning* getLightning() const { return lightning.get(); }
    CharacterRenderer* getCharacterRenderer() const { return characterRenderer.get(); }
    WMORenderer* getWMORenderer() const { return wmoRenderer.get(); }
    M2Renderer* getM2Renderer() const { return m2Renderer.get(); }
    Minimap* getMinimap() const { return minimap.get(); }
    WorldMap* getWorldMap() const { return worldMap.get(); }
    QuestMarkerRenderer* getQuestMarkerRenderer() const { return questMarkerRenderer.get(); }
    SkySystem* getSkySystem() const { return skySystem.get(); }
    const std::string& getCurrentZoneName() const { return currentZoneName; }
    VkContext* getVkContext() const { return vkCtx; }
    VkDescriptorSetLayout getPerFrameSetLayout() const { return perFrameSetLayout; }
    VkRenderPass getShadowRenderPass() const { return shadowRenderPass; }

    // Third-person character follow
    void setCharacterFollow(uint32_t instanceId);
    glm::vec3& getCharacterPosition() { return characterPosition; }
    uint32_t getCharacterInstanceId() const { return characterInstanceId; }
    float getCharacterYaw() const { return characterYaw; }
    void setCharacterYaw(float yawDeg) { characterYaw = yawDeg; }

    // Emote support
    void playEmote(const std::string& emoteName);
    void triggerLevelUpEffect(const glm::vec3& position);
    void cancelEmote();
    bool isEmoteActive() const { return emoteActive; }
    static std::string getEmoteText(const std::string& emoteName, const std::string* targetName = nullptr);
    static uint32_t getEmoteDbcId(const std::string& emoteName);
    static std::string getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName, const std::string* targetName = nullptr);
    static uint32_t getEmoteAnimByDbcId(uint32_t dbcId);

    // Targeting support
    void setTargetPosition(const glm::vec3* pos);
    void setInCombat(bool combat) { inCombat_ = combat; }
    void resetCombatVisualState();
    bool isMoving() const;
    void triggerMeleeSwing();
    void setEquippedWeaponType(uint32_t inventoryType) { equippedWeaponInvType_ = inventoryType; meleeAnimId = 0; }
    void setCharging(bool charging) { charging_ = charging; }
    bool isCharging() const { return charging_; }
    void startChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void emitChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void stopChargeEffect();

    // Mount rendering
    void setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath = "");
    void setTaxiFlight(bool onTaxi) { taxiFlight_ = onTaxi; }
    void setMountPitchRoll(float pitch, float roll) { mountPitch_ = pitch; mountRoll_ = roll; }
    void clearMount();
    bool isMounted() const { return mountInstanceId_ != 0; }

    // Selection circle for targeted entity
    void setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color);
    void clearSelectionCircle();

    // CPU timing stats (milliseconds, last frame).
    double getLastUpdateMs() const { return lastUpdateMs; }
    double getLastRenderMs() const { return lastRenderMs; }
    double getLastCameraUpdateMs() const { return lastCameraUpdateMs; }
    double getLastTerrainRenderMs() const { return lastTerrainRenderMs; }
    double getLastWMORenderMs() const { return lastWMORenderMs; }
    double getLastM2RenderMs() const { return lastM2RenderMs; }
    audio::MusicManager* getMusicManager() { return musicManager.get(); }
    game::ZoneManager* getZoneManager() { return zoneManager.get(); }
    audio::FootstepManager* getFootstepManager() { return footstepManager.get(); }
    audio::ActivitySoundManager* getActivitySoundManager() { return activitySoundManager.get(); }
    audio::MountSoundManager* getMountSoundManager() { return mountSoundManager.get(); }
    audio::NpcVoiceManager* getNpcVoiceManager() { return npcVoiceManager.get(); }
    audio::AmbientSoundManager* getAmbientSoundManager() { return ambientSoundManager.get(); }
    audio::UiSoundManager* getUiSoundManager() { return uiSoundManager.get(); }
    audio::CombatSoundManager* getCombatSoundManager() { return combatSoundManager.get(); }
    audio::SpellSoundManager* getSpellSoundManager() { return spellSoundManager.get(); }
    audio::MovementSoundManager* getMovementSoundManager() { return movementSoundManager.get(); }
    LightingManager* getLightingManager() { return lightingManager.get(); }

private:
    void runDeferredWorldInitStep(float deltaTime);

    core::Window* window = nullptr;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<TerrainRenderer> terrainRenderer;
    std::unique_ptr<TerrainManager> terrainManager;
    std::unique_ptr<PerformanceHUD> performanceHUD;
    std::unique_ptr<WaterRenderer> waterRenderer;
    std::unique_ptr<Skybox> skybox;
    std::unique_ptr<Celestial> celestial;
    std::unique_ptr<StarField> starField;
    std::unique_ptr<Clouds> clouds;
    std::unique_ptr<LensFlare> lensFlare;
    std::unique_ptr<Weather> weather;
    std::unique_ptr<Lightning> lightning;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<SkySystem> skySystem;  // Coordinator for sky rendering
    std::unique_ptr<SwimEffects> swimEffects;
    std::unique_ptr<MountDust> mountDust;
    std::unique_ptr<LevelUpEffect> levelUpEffect;
    std::unique_ptr<ChargeEffect> chargeEffect;
    std::unique_ptr<CharacterRenderer> characterRenderer;
    std::unique_ptr<WMORenderer> wmoRenderer;
    std::unique_ptr<M2Renderer> m2Renderer;
    std::unique_ptr<Minimap> minimap;
    std::unique_ptr<WorldMap> worldMap;
    std::unique_ptr<QuestMarkerRenderer> questMarkerRenderer;
    std::unique_ptr<audio::MusicManager> musicManager;
    std::unique_ptr<audio::FootstepManager> footstepManager;
    std::unique_ptr<audio::ActivitySoundManager> activitySoundManager;
    std::unique_ptr<audio::MountSoundManager> mountSoundManager;
    std::unique_ptr<audio::NpcVoiceManager> npcVoiceManager;
    std::unique_ptr<audio::AmbientSoundManager> ambientSoundManager;
    std::unique_ptr<audio::UiSoundManager> uiSoundManager;
    std::unique_ptr<audio::CombatSoundManager> combatSoundManager;
    std::unique_ptr<audio::SpellSoundManager> spellSoundManager;
    std::unique_ptr<audio::MovementSoundManager> movementSoundManager;
    std::unique_ptr<game::ZoneManager> zoneManager;
    // Shadow mapping (Vulkan)
    static constexpr uint32_t SHADOW_MAP_SIZE = 4096;
    // Per-frame shadow resources: each in-flight frame has its own depth image and
    // framebuffer so that frame N's shadow read and frame N+1's shadow write don't
    // race on the same image across concurrent GPU submissions.
    // Array size must match MAX_FRAMES (= 2, defined in the private section below).
    VkImage shadowDepthImage[2] = {};
    VmaAllocation shadowDepthAlloc[2] = {};
    VkImageView shadowDepthView[2] = {};
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer[2] = {};
    VkImageLayout shadowDepthLayout_[2] = {};
    glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
    glm::vec3 shadowCenter = glm::vec3(0.0f);
    bool shadowCenterInitialized = false;
    bool shadowsEnabled = true;
    float shadowDistance_ = 300.0f;  // Shadow frustum half-extent (default: 300 units)
    uint32_t shadowFrameCounter_ = 0;


public:
    // Character preview registration (for off-screen composite pass)
    void registerPreview(CharacterPreview* preview);
    void unregisterPreview(CharacterPreview* preview);

    void setShadowsEnabled(bool enabled) { shadowsEnabled = enabled; }
    bool areShadowsEnabled() const { return shadowsEnabled; }
    void setShadowDistance(float dist) { shadowDistance_ = glm::clamp(dist, 40.0f, 500.0f); }
    float getShadowDistance() const { return shadowDistance_; }
    void setMsaaSamples(VkSampleCountFlagBits samples);

    // FXAA post-process anti-aliasing (combinable with MSAA)
    void setFXAAEnabled(bool enabled);
    bool isFXAAEnabled() const { return fxaa_.enabled; }

    // FSR (FidelityFX Super Resolution) upscaling
    void setFSREnabled(bool enabled);
    bool isFSREnabled() const { return fsr_.enabled; }
    void setFSRQuality(float scaleFactor);  // 0.59=Balanced, 0.67=Quality, 0.77=UltraQuality, 1.00=Native
    void setFSRSharpness(float sharpness);  // 0.0 - 2.0
    float getFSRScaleFactor() const { return fsr_.scaleFactor; }
    float getFSRSharpness() const { return fsr_.sharpness; }
    void setFSR2Enabled(bool enabled);
    bool isFSR2Enabled() const { return fsr2_.enabled; }
    void setFSR2DebugTuning(float jitterSign, float motionVecScaleX, float motionVecScaleY);
    void setAmdFsr3FramegenEnabled(bool enabled);
    bool isAmdFsr3FramegenEnabled() const { return fsr2_.amdFsr3FramegenEnabled; }
    float getFSR2JitterSign() const { return fsr2_.jitterSign; }
    float getFSR2MotionVecScaleX() const { return fsr2_.motionVecScaleX; }
    float getFSR2MotionVecScaleY() const { return fsr2_.motionVecScaleY; }
#if WOWEE_HAS_AMD_FSR2
    bool isAmdFsr2SdkAvailable() const { return true; }
#else
    bool isAmdFsr2SdkAvailable() const { return false; }
#endif
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    bool isAmdFsr3FramegenSdkAvailable() const { return true; }
#else
    bool isAmdFsr3FramegenSdkAvailable() const { return false; }
#endif
    bool isAmdFsr3FramegenRuntimeActive() const { return fsr2_.amdFsr3FramegenRuntimeActive; }
    bool isAmdFsr3FramegenRuntimeReady() const { return fsr2_.amdFsr3FramegenRuntimeReady; }
    const char* getAmdFsr3FramegenRuntimePath() const;
    const std::string& getAmdFsr3FramegenRuntimeError() const { return fsr2_.amdFsr3RuntimeLastError; }
    size_t getAmdFsr3UpscaleDispatchCount() const { return fsr2_.amdFsr3UpscaleDispatchCount; }
    size_t getAmdFsr3FramegenDispatchCount() const { return fsr2_.amdFsr3FramegenDispatchCount; }
    size_t getAmdFsr3FallbackCount() const { return fsr2_.amdFsr3FallbackCount; }

    void setWaterRefractionEnabled(bool enabled);
    bool isWaterRefractionEnabled() const;

private:
    void applyMsaaChange();
    VkSampleCountFlagBits pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool msaaChangePending_ = false;
    void renderShadowPass();
    glm::mat4 computeLightSpaceMatrix();

    pipeline::AssetManager* cachedAssetManager = nullptr;
    uint32_t currentZoneId = 0;
    std::string currentZoneName;
    bool inTavern_ = false;
    bool inBlacksmith_ = false;
    float musicSwitchCooldown_ = 0.0f;
    bool deferredWorldInitEnabled_ = true;
    bool deferredWorldInitPending_ = false;
    uint8_t deferredWorldInitStage_ = 0;
    float deferredWorldInitCooldown_ = 0.0f;

    // Third-person character state
    glm::vec3 characterPosition = glm::vec3(0.0f);
    uint32_t characterInstanceId = 0;
    float characterYaw = 0.0f;

    // Character animation state
    enum class CharAnimState { IDLE, WALK, RUN, JUMP_START, JUMP_MID, JUMP_END, SIT_DOWN, SITTING, EMOTE, SWIM_IDLE, SWIM, MELEE_SWING, MOUNT, CHARGE, COMBAT_IDLE };
    CharAnimState charAnimState = CharAnimState::IDLE;
    float locomotionStopGraceTimer_ = 0.0f;
    bool locomotionWasSprinting_ = false;
    uint32_t lastPlayerAnimRequest_ = UINT32_MAX;
    bool lastPlayerAnimLoopRequest_ = true;
    void updateCharacterAnimation();
    bool isFootstepAnimationState() const;
    bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs);
    audio::FootstepSurface resolveFootstepSurface() const;
    uint32_t resolveMeleeAnimId();

    // Emote state
    bool emoteActive = false;
    uint32_t emoteAnimId = 0;
    bool emoteLoop = false;

    // Target facing
    const glm::vec3* targetPosition = nullptr;
    bool inCombat_ = false;

    // Selection circle rendering (Vulkan)
    VkPipeline selCirclePipeline = VK_NULL_HANDLE;
    VkPipelineLayout selCirclePipelineLayout = VK_NULL_HANDLE;
    ::VkBuffer selCircleVertBuf = VK_NULL_HANDLE;
    VmaAllocation selCircleVertAlloc = VK_NULL_HANDLE;
    ::VkBuffer selCircleIdxBuf = VK_NULL_HANDLE;
    VmaAllocation selCircleIdxAlloc = VK_NULL_HANDLE;
    int selCircleVertCount = 0;
    void initSelectionCircle();
    void renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection, VkCommandBuffer overrideCmd = VK_NULL_HANDLE);
    glm::vec3 selCirclePos{0.0f};
    glm::vec3 selCircleColor{1.0f, 0.0f, 0.0f};
    float selCircleRadius = 1.5f;
    bool selCircleVisible = false;

    // Fullscreen color overlay (underwater tint)
    VkPipeline overlayPipeline = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout = VK_NULL_HANDLE;
    void initOverlayPipeline();
    void renderOverlay(const glm::vec4& color, VkCommandBuffer overrideCmd = VK_NULL_HANDLE);

    // FSR 1.0 upscaling state
    struct FSRState {
        bool enabled = false;
        bool needsRecreate = false;
        float scaleFactor = 1.00f;  // Native default
        float sharpness = 1.6f;
        uint32_t internalWidth = 0;
        uint32_t internalHeight = 0;

        // Off-screen scene target (reduced resolution)
        AllocatedImage sceneColor{};        // 1x color (non-MSAA render target / MSAA resolve target)
        AllocatedImage sceneDepth{};        // Depth (matches current MSAA sample count)
        AllocatedImage sceneMsaaColor{};    // MSAA color target (only when MSAA > 1x)
        AllocatedImage sceneDepthResolve{}; // Depth resolve (only when MSAA + depth resolve)
        VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
        VkSampler sceneSampler = VK_NULL_HANDLE;

        // Upscale pipeline
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool descPool = VK_NULL_HANDLE;
        VkDescriptorSet descSet = VK_NULL_HANDLE;
    };
    FSRState fsr_;
    bool initFSRResources();
    void destroyFSRResources();
    void renderFSRUpscale();

    // FXAA post-process state
    struct FXAAState {
        bool enabled       = false;
        bool needsRecreate = false;

        // Off-screen scene target (same resolution as swapchain — no scaling)
        AllocatedImage sceneColor{};        // 1x resolved color target
        AllocatedImage sceneDepth{};        // Depth (matches MSAA sample count)
        AllocatedImage sceneMsaaColor{};    // MSAA color target (when MSAA > 1x)
        AllocatedImage sceneDepthResolve{}; // Depth resolve (MSAA + depth resolve)
        VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;
        VkSampler sceneSampler         = VK_NULL_HANDLE;

        // FXAA fullscreen pipeline
        VkPipeline           pipeline          = VK_NULL_HANDLE;
        VkPipelineLayout     pipelineLayout    = VK_NULL_HANDLE;
        VkDescriptorSetLayout descSetLayout    = VK_NULL_HANDLE;
        VkDescriptorPool     descPool          = VK_NULL_HANDLE;
        VkDescriptorSet      descSet           = VK_NULL_HANDLE;
    };
    FXAAState fxaa_;
    bool initFXAAResources();
    void destroyFXAAResources();
    void renderFXAAPass();

    // FSR 2.2 temporal upscaling state
    struct FSR2State {
        bool enabled = false;
        bool needsRecreate = false;
        float scaleFactor = 0.77f;
        float sharpness = 3.0f;  // Very strong RCAS to counteract upscale softness
        uint32_t internalWidth = 0;
        uint32_t internalHeight = 0;

        // Off-screen scene targets (internal resolution, no MSAA — FSR2 replaces AA)
        AllocatedImage sceneColor{};
        AllocatedImage sceneDepth{};
        VkFramebuffer sceneFramebuffer = VK_NULL_HANDLE;

        // Samplers
        VkSampler linearSampler = VK_NULL_HANDLE;   // For color
        VkSampler nearestSampler = VK_NULL_HANDLE;  // For depth / motion vectors

        // Motion vector buffer (internal resolution)
        AllocatedImage motionVectors{};

        // History buffers (display resolution, ping-pong)
        AllocatedImage history[2]{};
        AllocatedImage framegenOutput{};
        bool framegenOutputValid = false;
        uint32_t currentHistory = 0;  // Output index (0 or 1)

        // Compute pipelines
        VkPipeline motionVecPipeline = VK_NULL_HANDLE;
        VkPipelineLayout motionVecPipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout motionVecDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool motionVecDescPool = VK_NULL_HANDLE;
        VkDescriptorSet motionVecDescSet = VK_NULL_HANDLE;

        VkPipeline accumulatePipeline = VK_NULL_HANDLE;
        VkPipelineLayout accumulatePipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout accumulateDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool accumulateDescPool = VK_NULL_HANDLE;
        VkDescriptorSet accumulateDescSets[2] = {};  // Per ping-pong

        // RCAS sharpening pass (display resolution)
        VkPipeline sharpenPipeline = VK_NULL_HANDLE;
        VkPipelineLayout sharpenPipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout sharpenDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool sharpenDescPool = VK_NULL_HANDLE;
        VkDescriptorSet sharpenDescSets[2] = {};

        // Previous frame state for motion vector reprojection
        glm::mat4 prevViewProjection = glm::mat4(1.0f);
        glm::vec2 prevJitter = glm::vec2(0.0f);
        uint32_t frameIndex = 0;
        bool needsHistoryReset = true;
        bool useAmdBackend = false;
        bool amdFsr3FramegenEnabled = false;
        bool amdFsr3FramegenRuntimeActive = false;
        bool amdFsr3FramegenRuntimeReady = false;
        std::string amdFsr3RuntimePath = "Path C";
        std::string amdFsr3RuntimeLastError{};
        size_t amdFsr3UpscaleDispatchCount = 0;
        size_t amdFsr3FramegenDispatchCount = 0;
        size_t amdFsr3FallbackCount = 0;
        uint64_t amdFsr3InteropSyncValue = 1;
        float jitterSign = 0.38f;
        float motionVecScaleX = 1.0f;
        float motionVecScaleY = 1.0f;
#if WOWEE_HAS_AMD_FSR2
        FfxFsr2Context amdContext{};
        FfxFsr2Interface amdInterface{};
        void* amdScratchBuffer = nullptr;
        size_t amdScratchBufferSize = 0;
#endif
        std::unique_ptr<AmdFsr3Runtime> amdFsr3Runtime;

        // Convergent accumulation: jitter for N frames then freeze
        int convergenceFrame = 0;
        static constexpr int convergenceMaxFrames = 8;
        glm::mat4 lastStableVP = glm::mat4(1.0f);
    };
    FSR2State fsr2_;
    bool initFSR2Resources();
    void destroyFSR2Resources();
    void dispatchMotionVectors();
    void dispatchTemporalAccumulate();
    void dispatchAmdFsr2();
    void dispatchAmdFsr3Framegen();
    void renderFSR2Sharpen();
    static float halton(uint32_t index, uint32_t base);

    // Footstep event tracking (animation-driven)
    uint32_t footstepLastAnimationId = 0;
    float footstepLastNormTime = 0.0f;
    bool footstepNormInitialized = false;

    // Footstep surface cache (avoid expensive queries every step)
    mutable audio::FootstepSurface cachedFootstepSurface{};
    mutable glm::vec3 cachedFootstepPosition{0.0f, 0.0f, 0.0f};
    mutable float cachedFootstepUpdateTimer{999.0f};  // Force initial query

    // Mount footstep tracking (separate from player's)
    uint32_t mountFootstepLastAnimId = 0;
    float mountFootstepLastNormTime = 0.0f;
    bool mountFootstepNormInitialized = false;
    bool sfxStateInitialized = false;
    bool sfxPrevGrounded = true;
    bool sfxPrevJumping = false;
    bool sfxPrevFalling = false;
    bool sfxPrevSwimming = false;

    bool charging_ = false;
    float meleeSwingTimer = 0.0f;
    float meleeSwingCooldown = 0.0f;
    float meleeAnimDurationMs = 0.0f;
    uint32_t meleeAnimId = 0;
    uint32_t equippedWeaponInvType_ = 0;

    // Mount state
    // Mount animation capabilities (discovered at mount time, varies per model)
    struct MountAnimSet {
        uint32_t jumpStart = 0;  // Jump start animation
        uint32_t jumpLoop = 0;   // Jump airborne loop
        uint32_t jumpEnd = 0;    // Jump landing
        uint32_t rearUp = 0;     // Rear-up / special flourish
        uint32_t run = 0;        // Run animation (discovered, don't assume)
        uint32_t stand = 0;      // Stand animation (discovered)
        std::vector<uint32_t> fidgets;  // Idle fidget animations (head turn, tail swish, etc.)
    };

    enum class MountAction { None, Jump, RearUp };

    uint32_t mountInstanceId_ = 0;
    float mountHeightOffset_ = 0.0f;
    float mountPitch_ = 0.0f;  // Up/down tilt (radians)
    float mountRoll_ = 0.0f;   // Left/right banking (radians)
    int mountSeatAttachmentId_ = -1;  // -1 unknown, -2 unavailable
    glm::vec3 smoothedMountSeatPos_ = glm::vec3(0.0f);
    bool mountSeatSmoothingInit_ = false;
    float prevMountYaw_ = 0.0f; // Previous yaw for turn rate calculation (procedural lean)
    float lastDeltaTime_ = 0.0f; // Cached for use in updateCharacterAnimation()
    MountAction mountAction_ = MountAction::None;  // Current mount action (jump/rear-up)
    uint32_t mountActionPhase_ = 0;  // 0=start, 1=loop, 2=end (for jump chaining)
    MountAnimSet mountAnims_;  // Cached animation IDs for current mount
    float mountIdleFidgetTimer_ = 0.0f;  // Timer for random idle fidgets
    float mountIdleSoundTimer_ = 0.0f;   // Timer for ambient idle sounds
    uint32_t mountActiveFidget_ = 0;     // Currently playing fidget animation ID (0 = none)
    bool taxiFlight_ = false;
    bool taxiAnimsLogged_ = false;

    // Vulkan frame state
    VkContext* vkCtx = nullptr;
    VkCommandBuffer currentCmd = VK_NULL_HANDLE;
    uint32_t currentImageIndex = 0;

    // Per-frame UBO + descriptors (set 0)
    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorSetLayout perFrameSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet perFrameDescSets[MAX_FRAMES] = {};
    VkBuffer perFrameUBOs[MAX_FRAMES] = {};
    VmaAllocation perFrameUBOAllocs[MAX_FRAMES] = {};
    void* perFrameUBOMapped[MAX_FRAMES] = {};
    GPUPerFrameData currentFrameData{};
    float globalTime = 0.0f;

    // Per-frame reflection UBO (mirrors camera for planar reflections)
    VkBuffer reflPerFrameUBO = VK_NULL_HANDLE;
    VmaAllocation reflPerFrameUBOAlloc = VK_NULL_HANDLE;
    void* reflPerFrameUBOMapped = nullptr;
    VkDescriptorSet reflPerFrameDescSet = VK_NULL_HANDLE;

    bool createPerFrameResources();
    void destroyPerFrameResources();
    void updatePerFrameUBO();
    void setupWater1xPass();
    void renderReflectionPass();

    // ── Multithreaded secondary command buffer recording ──
    // Indices into secondaryCmds_ arrays
    static constexpr uint32_t SEC_SKY     = 0;  // sky (main thread)
    static constexpr uint32_t SEC_TERRAIN = 1;  // terrain (worker 0)
    static constexpr uint32_t SEC_WMO     = 2;  // WMO (worker 1)
    static constexpr uint32_t SEC_CHARS   = 3;  // selection circle + characters (main thread)
    static constexpr uint32_t SEC_M2      = 4;  // M2 + particles + glow (worker 2)
    static constexpr uint32_t SEC_POST    = 5;  // water + weather + effects (main thread)
    static constexpr uint32_t SEC_IMGUI   = 6;  // ImGui (main thread, non-FSR only)
    static constexpr uint32_t NUM_SECONDARIES = 7;
    static constexpr uint32_t NUM_WORKERS = 3;  // terrain, WMO, M2

    // Per-worker command pools (thread-safe: one pool per thread)
    VkCommandPool workerCmdPools_[NUM_WORKERS] = {};
    // Main-thread command pool for its secondary buffers
    VkCommandPool mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    // Pre-allocated secondary command buffers [secondaryIndex][frameInFlight]
    VkCommandBuffer secondaryCmds_[NUM_SECONDARIES][MAX_FRAMES] = {};

    bool parallelRecordingEnabled_ = false;  // set true after pools/buffers created
    bool createSecondaryCommandResources();
    void destroySecondaryCommandResources();
    VkCommandBuffer beginSecondary(uint32_t secondaryIndex);
    void setSecondaryViewportScissor(VkCommandBuffer cmd);

    // Cached render pass state for secondary buffer inheritance
    VkRenderPass activeRenderPass_ = VK_NULL_HANDLE;
    VkFramebuffer activeFramebuffer_ = VK_NULL_HANDLE;
    VkExtent2D activeRenderExtent_ = {0, 0};

    // Active character previews for off-screen rendering
    std::vector<CharacterPreview*> activePreviews_;

    bool terrainEnabled = true;
    bool terrainLoaded = false;

    bool ghostMode_ = false;  // set each frame from gameHandler->isPlayerGhost()

    // CPU timing stats (last frame/update).
    double lastUpdateMs = 0.0;
    double lastRenderMs = 0.0;
    double lastCameraUpdateMs = 0.0;
    double lastTerrainRenderMs = 0.0;
    double lastWMORenderMs = 0.0;
    double lastM2RenderMs = 0.0;
};

} // namespace rendering
} // namespace wowee
