#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/weather.hpp"
#include "rendering/lightning.hpp"
#include "rendering/lighting_manager.hpp"
#include "core/profiler.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/mount_dust.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "game/game_handler.hpp"
#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "core/logger.hpp"
#include "game/world.hpp"
#include "game/zone_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/amd_fsr3_runtime.hpp"
#include "rendering/spell_visual_system.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/render_graph.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cctype>
#include <cmath>
#include <chrono>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <future>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace wowee {
namespace rendering {

static bool envFlagEnabled(const char* key, bool defaultValue) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}

static int envIntOrDefault(const char* key, int defaultValue) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    char* end = nullptr;
    long n = std::strtol(raw, &end, 10);
    if (end == raw) return defaultValue;
    return static_cast<int>(n);
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::createPerFrameResources() {
    VkDevice device = vkCtx->getDevice();

    // --- Create per-frame shadow depth images (one per in-flight frame) ---
    // Each frame slot has its own depth image so that frame N's shadow read and
    // frame N+1's shadow write cannot race on the same image.
    VkImageCreateInfo imgCI{};
    imgCI.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType = VK_IMAGE_TYPE_2D;
    imgCI.format = VK_FORMAT_D32_SFLOAT;
    imgCI.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
    imgCI.mipLevels = 1;
    imgCI.arrayLayers = 1;
    imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo imgAllocCI{};
    imgAllocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (vmaCreateImage(vkCtx->getAllocator(), &imgCI, &imgAllocCI,
                &shadowDepthImage[i], &shadowDepthAlloc[i], nullptr) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image [", i, "]");
            return false;
        }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // --- Create per-frame shadow depth image views ---
    VkImageViewCreateInfo viewCI{};
    viewCI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format = VK_FORMAT_D32_SFLOAT;
    viewCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        viewCI.image = shadowDepthImage[i];
        if (vkCreateImageView(device, &viewCI, nullptr, &shadowDepthView[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow depth image view [", i, "]");
            return false;
        }
    }

    // --- Create shadow sampler (shared — read-only, no per-frame needed) ---
    VkSamplerCreateInfo sampCI{};
    sampCI.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter = VK_FILTER_LINEAR;
    sampCI.minFilter = VK_FILTER_LINEAR;
    sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampCI.compareEnable = VK_TRUE;
    sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSampler = vkCtx->getOrCreateSampler(sampCI);
    if (shadowSampler == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to create shadow sampler");
        return false;
    }

    // --- Create shadow render pass (depth-only) ---
    VkAttachmentDescription depthAtt{};
    depthAtt.format = VK_FORMAT_D32_SFLOAT;
    depthAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments = &depthAtt;
    rpCI.subpassCount = 1;
    rpCI.pSubpasses = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rpCI, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shadow render pass");
        return false;
    }

    // --- Create per-frame shadow framebuffers ---
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass = shadowRenderPass;
    fbCI.attachmentCount = 1;
    fbCI.width = SHADOW_MAP_SIZE;
    fbCI.height = SHADOW_MAP_SIZE;
    fbCI.layers = 1;
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        fbCI.pAttachments = &shadowDepthView[i];
        if (vkCreateFramebuffer(device, &fbCI, nullptr, &shadowFramebuffer[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create shadow framebuffer [", i, "]");
            return false;
        }
    }

    // --- Create descriptor set layout for set 0 (per-frame UBO + shadow sampler) ---
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &perFrameSetLayout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create per-frame descriptor set layout");
        return false;
    }

    // --- Create descriptor pool for UBO + image sampler (normal frames + reflection) ---
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES * 2; // normal frames + reflection frames
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES * 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES * 2; // normal frames + reflection frames
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &sceneDescriptorPool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create scene descriptor pool");
        return false;
    }

    // --- Create per-frame UBOs and descriptor sets ---
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &perFrameUBOs[i], &perFrameUBOAllocs[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create per-frame UBO ", i);
            return false;
        }
        perFrameUBOMapped[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout;

        if (vkAllocateDescriptorSets(device, &setAlloc, &perFrameDescSets[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate per-frame descriptor set ", i);
            return false;
        }

        // Write binding 0 (UBO) and binding 1 (shadow sampler)
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = perFrameUBOs[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler = shadowSampler;
        shadowImgInfo.imageView = shadowDepthView[i];
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = perFrameDescSets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = perFrameDescSets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // --- Create reflection per-frame UBO and descriptor set ---
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(vkCtx->getAllocator(), &bufInfo, &allocInfo,
                &reflPerFrameUBO, &reflPerFrameUBOAlloc, &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create reflection per-frame UBO");
            return false;
        }
        reflPerFrameUBOMapped = mapInfo.pMappedData;

        VkDescriptorSetLayout layouts[MAX_FRAMES];
        for (uint32_t i = 0; i < MAX_FRAMES; i++) layouts[i] = perFrameSetLayout;

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = MAX_FRAMES;
        setAlloc.pSetLayouts = layouts;

        if (vkAllocateDescriptorSets(device, &setAlloc, reflPerFrameDescSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate reflection per-frame descriptor sets");
            return false;
        }

        // Bind each reflection descriptor to the same UBO but its own frame's shadow view
        for (uint32_t i = 0; i < MAX_FRAMES; i++) {
            VkDescriptorBufferInfo descBuf{};
            descBuf.buffer = reflPerFrameUBO;
            descBuf.offset = 0;
            descBuf.range = sizeof(GPUPerFrameData);

            VkDescriptorImageInfo shadowImgInfo{};
            shadowImgInfo.sampler = shadowSampler;
            shadowImgInfo.imageView = shadowDepthView[i];
            shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = reflPerFrameDescSet[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &descBuf;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = reflPerFrameDescSet[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &shadowImgInfo;

            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        }
    }

    LOG_INFO("Per-frame Vulkan resources created (shadow map ", SHADOW_MAP_SIZE, "x", SHADOW_MAP_SIZE, ")");
    return true;
}

void Renderer::destroyPerFrameResources() {
    if (!vkCtx) return;
    vkDeviceWaitIdle(vkCtx->getDevice());
    VkDevice device = vkCtx->getDevice();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (perFrameUBOs[i]) {
            vmaDestroyBuffer(vkCtx->getAllocator(), perFrameUBOs[i], perFrameUBOAllocs[i]);
            perFrameUBOs[i] = VK_NULL_HANDLE;
        }
    }
    if (reflPerFrameUBO) {
        vmaDestroyBuffer(vkCtx->getAllocator(), reflPerFrameUBO, reflPerFrameUBOAlloc);
        reflPerFrameUBO = VK_NULL_HANDLE;
        reflPerFrameUBOMapped = nullptr;
    }
    if (sceneDescriptorPool) {
        vkDestroyDescriptorPool(device, sceneDescriptorPool, nullptr);
        sceneDescriptorPool = VK_NULL_HANDLE;
    }
    if (perFrameSetLayout) {
        vkDestroyDescriptorSetLayout(device, perFrameSetLayout, nullptr);
        perFrameSetLayout = VK_NULL_HANDLE;
    }

    // Destroy per-frame shadow resources
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (shadowFramebuffer[i]) { vkDestroyFramebuffer(device, shadowFramebuffer[i], nullptr); shadowFramebuffer[i] = VK_NULL_HANDLE; }
        if (shadowDepthView[i]) { vkDestroyImageView(device, shadowDepthView[i], nullptr); shadowDepthView[i] = VK_NULL_HANDLE; }
        if (shadowDepthImage[i]) { vmaDestroyImage(vkCtx->getAllocator(), shadowDepthImage[i], shadowDepthAlloc[i]); shadowDepthImage[i] = VK_NULL_HANDLE; shadowDepthAlloc[i] = VK_NULL_HANDLE; }
        shadowDepthLayout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (shadowRenderPass) { vkDestroyRenderPass(device, shadowRenderPass, nullptr); shadowRenderPass = VK_NULL_HANDLE; }
    shadowSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
}

void Renderer::updatePerFrameUBO() {
    if (!camera) return;

    currentFrameData.view = camera->getViewMatrix();
    currentFrameData.projection = camera->getProjectionMatrix();
    currentFrameData.viewPos = glm::vec4(camera->getPosition(), 1.0f);
    currentFrameData.fogParams.z = globalTime;

    // Lighting from LightingManager
    if (lightingManager) {
        const auto& lp = lightingManager->getLightingParams();
        currentFrameData.lightDir = glm::vec4(lp.directionalDir, 0.0f);
        currentFrameData.lightColor = glm::vec4(lp.diffuseColor, 1.0f);
        currentFrameData.ambientColor = glm::vec4(lp.ambientColor, 1.0f);
        currentFrameData.fogColor = glm::vec4(lp.fogColor, 1.0f);
        currentFrameData.fogParams.x = lp.fogStart;
        currentFrameData.fogParams.y = lp.fogEnd;

        // Shift fog to blue when camera is significantly underwater (terrain water only).
        if (waterRenderer && camera) {
            glm::vec3 camPos = camera->getPosition();
            auto waterH = waterRenderer->getNearestWaterHeightAt(camPos.x, camPos.y, camPos.z);
            constexpr float MIN_SUBMERSION = 2.0f;
            if (waterH && camPos.z < (*waterH - MIN_SUBMERSION)
                       && !waterRenderer->isWmoWaterAt(camPos.x, camPos.y)) {
                float depth = *waterH - camPos.z - MIN_SUBMERSION;
                float blend = glm::clamp(1.0f - std::exp(-depth * 0.08f), 0.0f, 0.7f);
                glm::vec3 underwaterFog(0.03f, 0.09f, 0.18f);
                glm::vec3 blendedFog = glm::mix(lp.fogColor, underwaterFog, blend);
                currentFrameData.fogColor = glm::vec4(blendedFog, 1.0f);
                currentFrameData.fogParams.x = glm::mix(lp.fogStart, 20.0f, blend);
                currentFrameData.fogParams.y = glm::mix(lp.fogEnd, 200.0f, blend);
            }
        }
    }

    currentFrameData.lightSpaceMatrix = lightSpaceMatrix;
    // Scale shadow bias proportionally to ortho extent to avoid acne at close range / gaps at far range
    float shadowBias = glm::clamp(0.8f * (shadowDistance_ / 300.0f), 0.0f, 1.0f);
    currentFrameData.shadowParams = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, shadowBias, 0.0f, 0.0f);

    // Player water ripple data: pack player XY into shadowParams.zw, ripple strength into fogParams.w
    if (cameraController) {
        currentFrameData.shadowParams.z = characterPosition.x;
        currentFrameData.shadowParams.w = characterPosition.y;
        bool inWater = cameraController->isSwimming();
        bool moving = cameraController->isMoving();
        currentFrameData.fogParams.w = (inWater && moving) ? 1.0f : 0.0f;
    } else {
        currentFrameData.fogParams.w = 0.0f;
    }

    // Copy to current frame's mapped UBO
    uint32_t frame = vkCtx->getCurrentFrame();
    std::memcpy(perFrameUBOMapped[frame], &currentFrameData, sizeof(GPUPerFrameData));
}

bool Renderer::initialize(core::Window* win) {
    window = win;
    vkCtx = win->getVkContext();
    deferredWorldInitEnabled_ = envFlagEnabled("WOWEE_DEFER_WORLD_SYSTEMS", true);
    LOG_INFO("Initializing renderer (Vulkan)");

    // Create camera (in front of Stormwind gate, looking north)
    camera = std::make_unique<Camera>();
    camera->setPosition(glm::vec3(-8900.0f, -170.0f, 150.0f));
    camera->setRotation(0.0f, -5.0f);
    camera->setAspectRatio(window->getAspectRatio());
    camera->setFov(60.0f);

    // Create camera controller
    cameraController = std::make_unique<CameraController>(camera.get());
    cameraController->setUseWoWSpeed(true);  // Use realistic WoW movement speed
    cameraController->setMouseSensitivity(0.15f);

    // Create performance HUD
    performanceHUD = std::make_unique<PerformanceHUD>();
    performanceHUD->setPosition(PerformanceHUD::Position::TOP_LEFT);

    // Create per-frame UBO and descriptor sets
    if (!createPerFrameResources()) {
        LOG_ERROR("Failed to create per-frame Vulkan resources");
        return false;
    }

    // Initialize Vulkan sub-renderers (Phase 3)

    // Sky system (owns skybox, starfield, celestial, clouds, lens flare)
    skySystem = std::make_unique<SkySystem>();
    if (!skySystem->initialize(vkCtx, perFrameSetLayout)) {
        LOG_ERROR("Failed to initialize sky system");
        return false;
    }
    // Expose sub-components via renderer accessors
    skybox = nullptr;  // Owned by skySystem; access via skySystem->getSkybox()
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    weather = std::make_unique<Weather>();
    weather->initialize(vkCtx, perFrameSetLayout);

    lightning = std::make_unique<Lightning>();
    lightning->initialize(vkCtx, perFrameSetLayout);

    swimEffects = std::make_unique<SwimEffects>();
    swimEffects->initialize(vkCtx, perFrameSetLayout);

    mountDust = std::make_unique<MountDust>();
    mountDust->initialize(vkCtx, perFrameSetLayout);

    chargeEffect = std::make_unique<ChargeEffect>();
    chargeEffect->initialize(vkCtx, perFrameSetLayout);

    levelUpEffect = std::make_unique<LevelUpEffect>();

    questMarkerRenderer = std::make_unique<QuestMarkerRenderer>();

    LOG_INFO("Vulkan sub-renderers initialized (Phase 3)");

    // LightingManager doesn't use GL — initialize for data-only use
    lightingManager = std::make_unique<LightingManager>();
    [[maybe_unused]] auto* assetManager = core::Application::getInstance().getAssetManager();

    // Create zone manager; enrich music paths from DBC if available
    zoneManager = std::make_unique<game::ZoneManager>();
    zoneManager->initialize();
    if (assetManager) {
        zoneManager->enrichFromDBC(assetManager);
    }

    // Audio is now owned by AudioCoordinator (created by Application).
    // Renderer receives AudioCoordinator* via setAudioCoordinator().

    // Create secondary command buffer resources for multithreaded rendering
    if (!createSecondaryCommandResources()) {
        LOG_WARNING("Failed to create secondary command buffers — falling back to single-threaded rendering");
    }

    // Create PostProcessPipeline (§4.3 — owns FSR/FXAA/FSR2/FSR3/brightness)
    postProcessPipeline_ = std::make_unique<PostProcessPipeline>();
    postProcessPipeline_->initialize(vkCtx);

    // Create render graph and register virtual resources
    renderGraph_ = std::make_unique<RenderGraph>();
    renderGraph_->registerResource("shadow_depth");
    renderGraph_->registerResource("reflection_texture");
    renderGraph_->registerResource("cull_visibility");
    renderGraph_->registerResource("scene_color");
    renderGraph_->registerResource("scene_depth");
    renderGraph_->registerResource("final_image");

    LOG_INFO("Renderer initialized");
    return true;
}

void Renderer::shutdown() {
    destroySecondaryCommandResources();

    LOG_WARNING("Renderer::shutdown - terrainManager stopWorkers...");
    if (terrainManager) {
        terrainManager->stopWorkers();
        LOG_WARNING("Renderer::shutdown - terrainManager reset...");
        terrainManager.reset();
    }

    LOG_WARNING("Renderer::shutdown - terrainRenderer...");
    if (terrainRenderer) {
        terrainRenderer->shutdown();
        terrainRenderer.reset();
    }

    LOG_WARNING("Renderer::shutdown - waterRenderer...");
    if (waterRenderer) {
        waterRenderer->shutdown();
        waterRenderer.reset();
    }

    LOG_WARNING("Renderer::shutdown - minimap...");
    if (minimap) {
        minimap->shutdown();
        minimap.reset();
    }

    LOG_WARNING("Renderer::shutdown - worldMap...");
    if (worldMap) {
        worldMap->shutdown();
        worldMap.reset();
    }

    LOG_WARNING("Renderer::shutdown - skySystem...");
    if (skySystem) {
        skySystem->shutdown();
        skySystem.reset();
    }

    // Individual sky components are owned by skySystem; just null the aliases
    skybox = nullptr;
    celestial = nullptr;
    starField = nullptr;
    clouds = nullptr;
    lensFlare = nullptr;

    if (weather) {
        weather.reset();
    }

    if (lightning) {
        lightning->shutdown();
        lightning.reset();
    }

    if (swimEffects) {
        swimEffects->shutdown();
        swimEffects.reset();
    }

    LOG_WARNING("Renderer::shutdown - characterRenderer...");
    if (characterRenderer) {
        characterRenderer->shutdown();
        characterRenderer.reset();
    }

    // Shutdown AnimationController before renderers it references (§4.2)
    animationController_.reset();

    LOG_WARNING("Renderer::shutdown - wmoRenderer...");
    if (wmoRenderer) {
        wmoRenderer->shutdown();
        wmoRenderer.reset();
    }

    // Shutdown SpellVisualSystem before M2Renderer (it holds M2Renderer pointer) (§4.4)
    if (spellVisualSystem_) {
        spellVisualSystem_->shutdown();
        spellVisualSystem_.reset();
    }

    LOG_WARNING("Renderer::shutdown - m2Renderer...");
    if (m2Renderer) {
        m2Renderer->shutdown();
        m2Renderer.reset();
    }

    // Audio shutdown is handled by AudioCoordinator (owned by Application).
    audioCoordinator_ = nullptr;

    // Cleanup Vulkan selection circle resources
    if (vkCtx) {
        VkDevice device = vkCtx->getDevice();
        if (selCirclePipeline) { vkDestroyPipeline(device, selCirclePipeline, nullptr); selCirclePipeline = VK_NULL_HANDLE; }
        if (selCirclePipelineLayout) { vkDestroyPipelineLayout(device, selCirclePipelineLayout, nullptr); selCirclePipelineLayout = VK_NULL_HANDLE; }
        if (selCircleVertBuf) { vmaDestroyBuffer(vkCtx->getAllocator(), selCircleVertBuf, selCircleVertAlloc); selCircleVertBuf = VK_NULL_HANDLE; selCircleVertAlloc = VK_NULL_HANDLE; }
        if (selCircleIdxBuf) { vmaDestroyBuffer(vkCtx->getAllocator(), selCircleIdxBuf, selCircleIdxAlloc); selCircleIdxBuf = VK_NULL_HANDLE; selCircleIdxAlloc = VK_NULL_HANDLE; }
        if (overlayPipeline) { vkDestroyPipeline(device, overlayPipeline, nullptr); overlayPipeline = VK_NULL_HANDLE; }
        if (overlayPipelineLayout) { vkDestroyPipelineLayout(device, overlayPipelineLayout, nullptr); overlayPipelineLayout = VK_NULL_HANDLE; }
    }

    // Shutdown post-process pipeline (FSR/FXAA/FSR2 resources) (§4.3)
    if (postProcessPipeline_) {
        postProcessPipeline_->shutdown();
        postProcessPipeline_.reset();
    }

    // Destroy render graph
    renderGraph_.reset();

    destroyPerFrameResources();

    zoneManager.reset();

    performanceHUD.reset();
    cameraController.reset();
    camera.reset();

    LOG_INFO("Renderer shutdown");
}

void Renderer::registerPreview(CharacterPreview* preview) {
    if (!preview) return;
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it == activePreviews_.end()) {
        activePreviews_.push_back(preview);
    }
}

void Renderer::unregisterPreview(CharacterPreview* preview) {
    auto it = std::find(activePreviews_.begin(), activePreviews_.end(), preview);
    if (it != activePreviews_.end()) {
        activePreviews_.erase(it);
    }
}

void Renderer::setWaterRefractionEnabled(bool enabled) {
    if (waterRenderer) waterRenderer->setRefractionEnabled(enabled);
}

bool Renderer::isWaterRefractionEnabled() const {
    return waterRenderer && waterRenderer->isRefractionEnabled();
}

void Renderer::setMsaaSamples(VkSampleCountFlagBits samples) {
    if (!vkCtx) return;

    // FSR2 requires non-MSAA render pass — block MSAA changes while FSR2 is active
    if (postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa() && samples > VK_SAMPLE_COUNT_1_BIT) return;

    // Clamp to device maximum
    VkSampleCountFlagBits maxSamples = vkCtx->getMaxUsableSampleCount();
    if (samples > maxSamples) samples = maxSamples;

    if (samples == vkCtx->getMsaaSamples()) return;

    // Defer to between frames — cannot destroy render pass/framebuffers mid-frame
    pendingMsaaSamples_ = samples;
    msaaChangePending_ = true;
}

void Renderer::applyMsaaChange() {
    VkSampleCountFlagBits samples = pendingMsaaSamples_;
    msaaChangePending_ = false;

    // FSR2 requires non-MSAA render pass — if FSR2 was enabled after the MSAA
    // change was queued (startup race), force 1x to avoid framebuffer mismatch.
    if (samples > VK_SAMPLE_COUNT_1_BIT &&
        postProcessPipeline_ && postProcessPipeline_->isFsr2BlockingMsaa()) {
        samples = VK_SAMPLE_COUNT_1_BIT;
    }

    VkSampleCountFlagBits current = vkCtx->getMsaaSamples();
    if (samples == current) return;

    // Single GPU wait — all subsequent operations are CPU-side object creation
    vkDeviceWaitIdle(vkCtx->getDevice());

    // Set new MSAA and recreate swapchain (render pass, depth, MSAA image, framebuffers)
    vkCtx->setMsaaSamples(samples);
    if (!vkCtx->recreateSwapchain(window->getWidth(), window->getHeight())) {
        LOG_ERROR("MSAA change failed — reverting to 1x");
        vkCtx->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
        (void)vkCtx->recreateSwapchain(window->getWidth(), window->getHeight());
    }

    // Recreate all sub-renderer pipelines (they embed sample count from render pass)
    if (terrainRenderer) terrainRenderer->recreatePipelines();
    if (waterRenderer) {
        waterRenderer->recreatePipelines();
        waterRenderer->destroyWater1xResources();  // no longer used
    }
    if (wmoRenderer) wmoRenderer->recreatePipelines();
    if (m2Renderer) m2Renderer->recreatePipelines();
    if (characterRenderer) characterRenderer->recreatePipelines();
    if (questMarkerRenderer) questMarkerRenderer->recreatePipelines();
    if (weather) weather->recreatePipelines();
    if (lightning) lightning->recreatePipelines();
    if (swimEffects) swimEffects->recreatePipelines();
    if (mountDust) mountDust->recreatePipelines();
    if (chargeEffect) chargeEffect->recreatePipelines();

    // Sky system sub-renderers
    if (skySystem) {
        if (auto* sb = skySystem->getSkybox()) sb->recreatePipelines();
        if (auto* sf = skySystem->getStarField()) sf->recreatePipelines();
        if (auto* ce = skySystem->getCelestial()) ce->recreatePipelines();
        if (auto* cl = skySystem->getClouds()) cl->recreatePipelines();
        if (auto* lf = skySystem->getLensFlare()) lf->recreatePipelines();
    }

    if (minimap) minimap->recreatePipelines();

    // Selection circle + overlay + FSR use lazy init, just destroy them
    VkDevice device = vkCtx->getDevice();
    if (selCirclePipeline) { vkDestroyPipeline(device, selCirclePipeline, nullptr); selCirclePipeline = VK_NULL_HANDLE; }
    if (overlayPipeline) { vkDestroyPipeline(device, overlayPipeline, nullptr); overlayPipeline = VK_NULL_HANDLE; }
    if (postProcessPipeline_) postProcessPipeline_->destroyAllResources(); // Will be lazily recreated in beginFrame()

    // Reinitialize ImGui Vulkan backend with new MSAA sample count
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_1;
    initInfo.Instance = vkCtx->getInstance();
    initInfo.PhysicalDevice = vkCtx->getPhysicalDevice();
    initInfo.Device = vkCtx->getDevice();
    initInfo.QueueFamily = vkCtx->getGraphicsQueueFamily();
    initInfo.Queue = vkCtx->getGraphicsQueue();
    initInfo.DescriptorPool = vkCtx->getImGuiDescriptorPool();
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = vkCtx->getSwapchainImageCount();
    initInfo.PipelineInfoMain.RenderPass = vkCtx->getImGuiRenderPass();
    initInfo.PipelineInfoMain.MSAASamples = vkCtx->getMsaaSamples();
    initInfo.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS)
            LOG_ERROR("ImGui Vulkan error: ", static_cast<int>(err));
    };
    ImGui_ImplVulkan_Init(&initInfo);

}

void Renderer::beginFrame() {
    ZoneScopedN("Renderer::beginFrame");
    if (!vkCtx) return;
    if (vkCtx->isDeviceLost()) return;

    // Apply deferred MSAA change between frames (before any rendering state is used)
    if (msaaChangePending_) {
        applyMsaaChange();
    }

    // Post-process resource management (§4.3 — delegates to PostProcessPipeline)
    if (postProcessPipeline_) postProcessPipeline_->manageResources();

    // Handle swapchain recreation if needed
    if (vkCtx->isSwapchainDirty()) {
        (void)vkCtx->recreateSwapchain(window->getWidth(), window->getHeight());
        // Rebuild water resources that reference swapchain extent/views
        if (waterRenderer) {
            waterRenderer->recreatePipelines();
        }
        // Recreate post-process resources for new swapchain dimensions
        if (postProcessPipeline_) postProcessPipeline_->handleSwapchainResize();
    }

    // Acquire swapchain image and begin command buffer
    currentCmd = vkCtx->beginFrame(currentImageIndex);
    if (currentCmd == VK_NULL_HANDLE) {
        // Swapchain out of date, will retry next frame
        return;
    }

    // FSR2 jitter pattern (§4.3 — delegates to PostProcessPipeline)
    if (postProcessPipeline_ && camera) postProcessPipeline_->applyJitter(camera.get());

    // Compute fresh shadow matrix BEFORE UBO update so shaders get current-frame data.
    lightSpaceMatrix = computeLightSpaceMatrix();

    // Update per-frame UBO with current camera/lighting state
    updatePerFrameUBO();

    // --- Off-screen pre-passes ---
    // Build frame graph: registers pre-passes as graph nodes with dependencies.
    // compile() topologically sorts; execute() runs them with auto barriers.
    buildFrameGraph(nullptr);
    if (renderGraph_) {
        renderGraph_->execute(currentCmd);
    }

    // --- Begin render pass ---
    // Select framebuffer: PP off-screen target or swapchain (§4.3 — PostProcessPipeline)
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = vkCtx->getImGuiRenderPass();

    VkExtent2D renderExtent;
    VkFramebuffer ppFB = postProcessPipeline_ ? postProcessPipeline_->getSceneFramebuffer() : VK_NULL_HANDLE;
    if (ppFB != VK_NULL_HANDLE) {
        rpInfo.framebuffer = ppFB;
        renderExtent = postProcessPipeline_->getSceneRenderExtent();
    } else {
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        renderExtent = vkCtx->getSwapchainExtent();
    }

    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = renderExtent;

    // Clear values must match attachment count: 2 (no MSAA), 3 (MSAA), or 4 (MSAA+depth resolve)
    VkClearValue clearValues[4]{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[3].depthStencil = {1.0f, 0};
    bool msaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
    if (msaaOn) {
        bool depthRes = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);
        rpInfo.clearValueCount = depthRes ? 4 : 3;
    } else {
        rpInfo.clearValueCount = 2;
    }
    rpInfo.pClearValues = clearValues;

    // Cache render pass state for secondary command buffer inheritance
    activeRenderPass_ = rpInfo.renderPass;
    activeFramebuffer_ = rpInfo.framebuffer;
    activeRenderExtent_ = renderExtent;

    VkSubpassContents subpassMode = parallelRecordingEnabled_
        ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
        : VK_SUBPASS_CONTENTS_INLINE;
    vkCmdBeginRenderPass(currentCmd, &rpInfo, subpassMode);

    if (!parallelRecordingEnabled_) {
        // Fallback: set dynamic viewport and scissor on primary (inline mode)
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderExtent.width);
        viewport.height = static_cast<float>(renderExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = renderExtent;
        vkCmdSetScissor(currentCmd, 0, 1, &scissor);
    }
}

void Renderer::endFrame() {
    ZoneScopedN("Renderer::endFrame");
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) return;

    // Track whether a post-processing path switched to an INLINE render pass.
    // beginFrame() may have started the scene pass with SECONDARY_COMMAND_BUFFERS;
    // post-proc paths end it and begin a new INLINE pass for the swapchain output.
    endFrameInlineMode_ = false;

    // Post-process execution (§4.3 — delegates to PostProcessPipeline)
    if (postProcessPipeline_) {
        endFrameInlineMode_ = postProcessPipeline_->executePostProcessing(
            currentCmd, currentImageIndex, camera.get(), lastDeltaTime_);
    }

    // ImGui rendering — must respect the subpass contents mode of the
    // CURRENT render pass. Post-processing paths (FSR/FXAA) end the scene
    // pass and begin a new INLINE pass; if none ran, we're still inside the
    // scene pass which may be SECONDARY_COMMAND_BUFFERS when parallel recording
    // is active. Track this via endFrameInlineMode_ (set true by any post-proc
    // path that started an INLINE render pass).
    if (parallelRecordingEnabled_ && !endFrameInlineMode_) {
        // Still in the scene pass with SECONDARY_COMMAND_BUFFERS — record
        // ImGui into a secondary command buffer.
        VkCommandBuffer imguiCmd = beginSecondary(SEC_IMGUI);
        setSecondaryViewportScissor(imguiCmd);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), imguiCmd);
        vkEndCommandBuffer(imguiCmd);
        vkCmdExecuteCommands(currentCmd, 1, &imguiCmd);
    } else {
        // INLINE render pass (post-process pass or non-parallel mode).
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), currentCmd);
    }

    vkCmdEndRenderPass(currentCmd);

    uint32_t frame = vkCtx->getCurrentFrame();

    // Capture scene color/depth into per-frame history images for water refraction
    if (waterRenderer && waterRenderer->isRefractionEnabled() && waterRenderer->hasSurfaces()
        && currentImageIndex < vkCtx->getSwapchainImages().size()) {
        waterRenderer->captureSceneHistory(
            currentCmd,
            vkCtx->getSwapchainImages()[currentImageIndex],
            vkCtx->getDepthCopySourceImage(),
            vkCtx->getSwapchainExtent(),
            vkCtx->isDepthCopySourceMsaa(),
            frame);
    }

    // Water now renders in the main pass (renderWorld), no separate 1x pass needed.

    // Submit and present
    vkCtx->endFrame(currentCmd, currentImageIndex);
    currentCmd = VK_NULL_HANDLE;
}

void Renderer::setCharacterFollow(uint32_t instanceId) {
    characterInstanceId = instanceId;
    if (cameraController && instanceId > 0) {
        cameraController->setFollowTarget(&characterPosition);
    }
    if (animationController_) animationController_->onCharacterFollow(instanceId);
}

void Renderer::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    if (animationController_) animationController_->setMounted(mountInstId, mountDisplayId, heightOffset, modelPath);
}

void Renderer::clearMount() {
    if (animationController_) animationController_->clearMount();
}



void Renderer::playEmote(const std::string& emoteName) {
    if (animationController_) animationController_->playEmote(emoteName);
}

void Renderer::cancelEmote() {
    if (animationController_) animationController_->cancelEmote();
}

bool Renderer::isEmoteActive() const {
    return animationController_ && animationController_->isEmoteActive();
}

void Renderer::setInCombat(bool combat) {
    if (animationController_) animationController_->setInCombat(combat);
}

void Renderer::setEquippedWeaponType(uint32_t inventoryType, bool is2HLoose, bool isFist,
                                     bool isDagger, bool hasOffHand, bool hasShield) {
    if (animationController_) animationController_->setEquippedWeaponType(inventoryType, is2HLoose, isFist, isDagger, hasOffHand, hasShield);
}

void Renderer::triggerSpecialAttack(uint32_t spellId) {
    if (animationController_) animationController_->triggerSpecialAttack(spellId);
}

void Renderer::setEquippedRangedType(RangedWeaponType type) {
    if (animationController_) animationController_->setEquippedRangedType(type);
}

void Renderer::triggerRangedShot() {
    if (animationController_) animationController_->triggerRangedShot();
}

RangedWeaponType Renderer::getEquippedRangedType() const {
    return animationController_ ? animationController_->getEquippedRangedType()
                                : RangedWeaponType::NONE;
}

void Renderer::setCharging(bool c) {
    if (animationController_) animationController_->setCharging(c);
}

bool Renderer::isCharging() const {
    return animationController_ && animationController_->isCharging();
}

void Renderer::setTaxiFlight(bool taxi) {
    if (animationController_) animationController_->setTaxiFlight(taxi);
}

void Renderer::setMountPitchRoll(float pitch, float roll) {
    if (animationController_) animationController_->setMountPitchRoll(pitch, roll);
}

bool Renderer::isMounted() const {
    return animationController_ && animationController_->isMounted();
}

bool Renderer::captureScreenshot(const std::string& outputPath) {
    if (!vkCtx) return false;

    VkDevice device     = vkCtx->getDevice();
    VmaAllocator alloc  = vkCtx->getAllocator();
    VkExtent2D extent   = vkCtx->getSwapchainExtent();
    const auto& images  = vkCtx->getSwapchainImages();

    if (images.empty() || currentImageIndex >= images.size()) return false;

    VkImage srcImage = images[currentImageIndex];
    uint32_t w = extent.width;
    uint32_t h = extent.height;
    VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

    // Stall GPU so the swapchain image is idle
    vkDeviceWaitIdle(device);

    // Create staging buffer
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size  = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    if (vmaCreateBuffer(alloc, &bufInfo, &allocCI, &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS) {
        LOG_WARNING("Screenshot: failed to create staging buffer");
        return false;
    }

    // Record copy commands
    VkCommandBuffer cmd = vkCtx->beginSingleTimeCommands();

    // Transition swapchain image: PRESENT_SRC → TRANSFER_SRC
    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.image               = srcImage;
    toTransfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Copy image to buffer
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuf, 1, &region);

    // Transition back: TRANSFER_SRC → PRESENT_SRC
    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPresent);

    vkCtx->endSingleTimeCommands(cmd);

    // Map and convert BGRA → RGBA
    void* mapped = nullptr;
    vmaMapMemory(alloc, stagingAlloc, &mapped);
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t i = 0; i < w * h; ++i) {
        std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]); // B ↔ R
    }

    // Ensure output directory exists
    std::filesystem::path outPath(outputPath);
    if (outPath.has_parent_path())
        std::filesystem::create_directories(outPath.parent_path());

    int ok = stbi_write_png(outputPath.c_str(),
                            static_cast<int>(w), static_cast<int>(h),
                            4, pixels, static_cast<int>(w * 4));

    vmaUnmapMemory(alloc, stagingAlloc);
    vmaDestroyBuffer(alloc, stagingBuf, stagingAlloc);

    if (ok) {
        LOG_INFO("Screenshot saved: ", outputPath);
    } else {
        LOG_WARNING("Screenshot: stbi_write_png failed for ", outputPath);
    }
    return ok != 0;
}

void Renderer::triggerLevelUpEffect(const glm::vec3& position) {
    if (animationController_) animationController_->triggerLevelUpEffect(position);
}

void Renderer::startChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (animationController_) animationController_->startChargeEffect(position, direction);
}

void Renderer::emitChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (animationController_) animationController_->emitChargeEffect(position, direction);
}

void Renderer::stopChargeEffect() {
    if (animationController_) animationController_->stopChargeEffect();
}

// ─── Spell Visual Effects — delegated to SpellVisualSystem (§4.4) ────────────

void Renderer::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                bool useImpactKit) {
    if (spellVisualSystem_) spellVisualSystem_->playSpellVisual(visualId, worldPosition, useImpactKit);
}

void Renderer::triggerMeleeSwing() {
    if (animationController_) animationController_->triggerMeleeSwing();
}

std::string Renderer::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    return AnimationController::getEmoteText(emoteName, targetName);
}

uint32_t Renderer::getEmoteDbcId(const std::string& emoteName) {
    return AnimationController::getEmoteDbcId(emoteName);
}

std::string Renderer::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                           const std::string* targetName) {
    return AnimationController::getEmoteTextByDbcId(dbcId, senderName, targetName);
}

uint32_t Renderer::getEmoteAnimByDbcId(uint32_t dbcId) {
    return AnimationController::getEmoteAnimByDbcId(dbcId);
}

void Renderer::setTargetPosition(const glm::vec3* pos) {
    if (animationController_) animationController_->setTargetPosition(pos);
}

void Renderer::resetCombatVisualState() {
    if (animationController_) animationController_->resetCombatVisualState();
    if (spellVisualSystem_) spellVisualSystem_->reset();
}

bool Renderer::isMoving() const {
    return cameraController && cameraController->isMoving();
}

void Renderer::update(float deltaTime) {
    ZoneScopedN("Renderer::update");
    globalTime += deltaTime;
    if (musicSwitchCooldown_ > 0.0f) {
        musicSwitchCooldown_ = std::max(0.0f, musicSwitchCooldown_ - deltaTime);
    }
    runDeferredWorldInitStep(deltaTime);

    auto updateStart = std::chrono::steady_clock::now();
    lastDeltaTime_ = deltaTime;

    if (wmoRenderer) wmoRenderer->resetQueryStats();
    if (m2Renderer) m2Renderer->resetQueryStats();

    if (cameraController) {
        auto cameraStart = std::chrono::steady_clock::now();
        cameraController->update(deltaTime);
        auto cameraEnd = std::chrono::steady_clock::now();
        lastCameraUpdateMs = std::chrono::duration<double, std::milli>(cameraEnd - cameraStart).count();
        if (lastCameraUpdateMs > 50.0) {
            LOG_WARNING("SLOW cameraController->update: ", lastCameraUpdateMs, "ms");
        }

        // Update 3D audio listener position/orientation to match camera
        if (camera) {
            audio::AudioEngine::instance().setListenerPosition(camera->getPosition());
            audio::AudioEngine::instance().setListenerOrientation(camera->getForward(), camera->getUp());
        }
    } else {
        lastCameraUpdateMs = 0.0;
    }

    // Visibility hardening: ensure player instance cannot stay hidden after
    // taxi/camera transitions, but preserve first-person self-hide.
    if (characterRenderer && characterInstanceId > 0 && cameraController) {
        if ((cameraController->isThirdPerson() && !cameraController->isFirstPersonView()) || (animationController_ && animationController_->isTaxiFlight())) {
            characterRenderer->setInstanceVisible(characterInstanceId, true);
        }
    }

    // Update lighting system
    if (lightingManager) {
        const auto* gh = core::Application::getInstance().getGameHandler();
        uint32_t mapId    = gh ? gh->getCurrentMapId() : 0;
        float gameTime    = gh ? gh->getGameTime() : -1.0f;
        bool isRaining    = gh ? gh->isRaining() : false;
        bool isUnderwater = cameraController ? cameraController->isSwimming() : false;

        lightingManager->update(characterPosition, mapId, gameTime, isRaining, isUnderwater);

        // Sync weather visual renderer with game state
        if (weather && gh) {
            uint32_t wType = gh->getWeatherType();
            float wInt = gh->getWeatherIntensity();
            if (wType != 0) {
                // Server-driven weather (SMSG_WEATHER) — authoritative
                if (wType == 1)      weather->setWeatherType(Weather::Type::RAIN);
                else if (wType == 2) weather->setWeatherType(Weather::Type::SNOW);
                else if (wType == 3) weather->setWeatherType(Weather::Type::STORM);
                else                 weather->setWeatherType(Weather::Type::NONE);
                weather->setIntensity(wInt);
            } else {
                // No server weather — use zone-based weather configuration
                weather->updateZoneWeather(currentZoneId, deltaTime);
            }
            weather->setEnabled(true);

            // Lightning flash disabled
            if (lightning) {
                lightning->setEnabled(false);
            }
        } else if (weather) {
            // No game handler (single-player without network) — zone weather only
            weather->updateZoneWeather(currentZoneId, deltaTime);
            weather->setEnabled(true);
        }
    }

    // Sync character model position/rotation and animation with follow target
    if (characterInstanceId > 0 && characterRenderer && cameraController) {
        characterRenderer->setInstancePosition(characterInstanceId, characterPosition);

        // Movement-facing comes from camera controller and is decoupled from LMB orbit.
        bool taxiFlight = animationController_ && animationController_->isTaxiFlight();
        if (taxiFlight) {
            characterYaw = cameraController->getFacingYaw();
        } else if (cameraController->isMoving() || cameraController->isRightMouseHeld()) {
            characterYaw = cameraController->getFacingYaw();
        } else if (animationController_ && animationController_->isInCombat() &&
                   animationController_->getTargetPosition() && !animationController_->isEmoteActive() && !isMounted()) {
            glm::vec3 toTarget = *animationController_->getTargetPosition() - characterPosition;
            if (toTarget.x * toTarget.x + toTarget.y * toTarget.y > 0.01f) {
                float targetYaw = glm::degrees(std::atan2(toTarget.y, toTarget.x));
                float diff = targetYaw - characterYaw;
                while (diff > 180.0f) diff -= 360.0f;
                while (diff < -180.0f) diff += 360.0f;
                float rotSpeed = 360.0f * deltaTime;
                if (std::abs(diff) < rotSpeed) {
                    characterYaw = targetYaw;
                } else {
                    characterYaw += (diff > 0 ? rotSpeed : -rotSpeed);
                }
            }
        }
        float yawRad = glm::radians(characterYaw);
        characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(0.0f, 0.0f, yawRad));

        // Update animation based on movement state (delegated to AnimationController §4.2)
        if (animationController_) {
            animationController_->updateMeleeTimers(deltaTime);
            animationController_->setDeltaTime(deltaTime);
            animationController_->updateCharacterAnimation();
        }
    }

    // Update terrain streaming
    if (terrainManager && camera) {
        auto terrStart = std::chrono::steady_clock::now();
        terrainManager->update(*camera, deltaTime);
        float terrMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - terrStart).count();
        if (terrMs > 50.0f) {
            LOG_WARNING("SLOW terrainManager->update: ", terrMs, "ms");
        }
    }

    // Update sky system (skybox time, star twinkle, clouds, celestial moon phases)
    if (skySystem) {
        skySystem->update(deltaTime);
    }

    // Update weather particles
    if (weather && camera) {
        weather->update(*camera, deltaTime);
    }

    // Update lightning (storm / heavy rain)
    if (lightning && camera && lightning->isEnabled()) {
        lightning->update(deltaTime, *camera);
    }

    // Update swim effects
    if (swimEffects && camera && cameraController && waterRenderer) {
        swimEffects->update(*camera, *cameraController, *waterRenderer, deltaTime);
    }

    // Update mount dust effects
    if (mountDust) {
        mountDust->update(deltaTime);

        // Spawn dust when mounted and moving on ground
        if (isMounted() && camera && cameraController && !(animationController_ && animationController_->isTaxiFlight())) {
            bool isMoving = cameraController->isMoving();
            bool onGround = cameraController->isGrounded();

            if (isMoving && onGround) {
                // Calculate velocity from camera direction and speed
                glm::vec3 forward = camera->getForward();
                float speed = cameraController->getMovementSpeed();
                glm::vec3 velocity = forward * speed;
                velocity.z = 0.0f;  // Ignore vertical component

                // Spawn dust at mount's feet (slightly below character position)
                float mho = animationController_ ? animationController_->getMountHeightOffset() : 0.0f;
                glm::vec3 dustPos = characterPosition - glm::vec3(0.0f, 0.0f, mho * 0.8f);
                mountDust->spawnDust(dustPos, velocity, isMoving);
            }
        }
    }
    // Update level-up effect
    if (levelUpEffect) {
        levelUpEffect->update(deltaTime);
    }
    // Update charge effect
    if (chargeEffect) {
        chargeEffect->update(deltaTime);
    }
    // Update transient spell visual instances (delegated to SpellVisualSystem §4.4)
    if (spellVisualSystem_) spellVisualSystem_->update(deltaTime);


    // Launch M2 doodad animation on background thread (overlaps with character animation + audio)
    std::future<void> m2AnimFuture;
    bool m2AnimLaunched = false;
    if (m2Renderer && camera) {
        float m2DeltaTime = deltaTime;
        glm::vec3 m2CamPos = camera->getPosition();
        glm::mat4 m2ViewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
        m2AnimFuture = std::async(std::launch::async,
            [this, m2DeltaTime, m2CamPos, m2ViewProj]() {
                m2Renderer->update(m2DeltaTime, m2CamPos, m2ViewProj);
            });
        m2AnimLaunched = true;
    }

    // Update character animations (runs in parallel with M2 animation above)
    if (characterRenderer && camera) {
        characterRenderer->update(deltaTime, camera->getPosition());
    }

    // Update AudioEngine (cleanup finished sounds, etc.)
    audio::AudioEngine::instance().update(deltaTime);

    // Footsteps: delegated to AnimationController (§4.2)
    if (animationController_) animationController_->updateFootsteps(deltaTime);

    // Activity SFX + mount ambient sounds: delegated to AnimationController (§4.2)
    if (animationController_) animationController_->updateSfxState(deltaTime);

    const bool canQueryWmo = (camera && wmoRenderer);
    const glm::vec3 camPos = camera ? camera->getPosition() : glm::vec3(0.0f);
    uint32_t insideWmoId = 0;
    const bool insideWmo = canQueryWmo &&
        wmoRenderer->isInsideWMO(camPos.x, camPos.y, camPos.z, &insideWmoId);
    playerIndoors_ = insideWmo;

    // Ambient environmental sounds: fireplaces, water, birds, etc.
    if (audioCoordinator_->getAmbientSoundManager() && camera && wmoRenderer && cameraController) {
        bool isIndoor = insideWmo;
        bool isSwimming = cameraController->isSwimming();

        // Detect blacksmith buildings to play ambient forge/anvil sounds.
        // 96048 is the WMO group ID for the Goldshire blacksmith interior.
        // TODO: extend to other smithy WMO IDs (Ironforge, Orgrimmar, etc.)
        bool isBlacksmith = (insideWmoId == 96048);

        // Sync weather audio with visual weather system
        if (weather) {
            auto weatherType = weather->getWeatherType();
            float intensity = weather->getIntensity();

            audio::AmbientSoundManager::WeatherType audioWeatherType = audio::AmbientSoundManager::WeatherType::NONE;

            if (weatherType == Weather::Type::RAIN) {
                if (intensity < 0.33f) {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::RAIN_LIGHT;
                } else if (intensity < 0.66f) {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::RAIN_MEDIUM;
                } else {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::RAIN_HEAVY;
                }
            } else if (weatherType == Weather::Type::SNOW) {
                if (intensity < 0.33f) {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::SNOW_LIGHT;
                } else if (intensity < 0.66f) {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::SNOW_MEDIUM;
                } else {
                    audioWeatherType = audio::AmbientSoundManager::WeatherType::SNOW_HEAVY;
                }
            }

            audioCoordinator_->getAmbientSoundManager()->setWeather(audioWeatherType);
        }

        audioCoordinator_->getAmbientSoundManager()->update(deltaTime, camPos, isIndoor, isSwimming, isBlacksmith);
    }

    // Wait for M2 doodad animation to finish (was launched earlier in parallel with character anim)
    if (m2AnimLaunched) {
        try { m2AnimFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 animation worker: ", e.what()); }
    }

    // Helper: play zone music, dispatching local files (file: prefix) vs MPQ paths
    auto playZoneMusic = [&](const std::string& music) {
        if (music.empty()) return;
        if (music.rfind("file:", 0) == 0) {
            audioCoordinator_->getMusicManager()->crossfadeToFile(music.substr(5));
        } else {
            audioCoordinator_->getMusicManager()->crossfadeTo(music);
        }
    };

    // Update zone detection and music
    if (zoneManager && audioCoordinator_->getMusicManager() && terrainManager && camera) {
        // Prefer server-authoritative zone ID (from SMSG_INIT_WORLD_STATES);
        // fall back to tile-based lookup for single-player / offline mode.
        const auto* gh = core::Application::getInstance().getGameHandler();
        uint32_t serverZoneId = gh ? gh->getWorldStateZoneId() : 0;
        auto tile = terrainManager->getCurrentTile();
        uint32_t zoneId = (serverZoneId != 0) ? serverZoneId : zoneManager->getZoneId(tile.x, tile.y);

        bool insideTavern = false;
        bool insideBlacksmith = false;
        std::string tavernMusic;

        // Override with WMO-based detection (e.g., inside Stormwind, taverns, blacksmiths)
        if (wmoRenderer) {
            uint32_t wmoModelId = insideWmoId;
            if (insideWmo) {
                // Check if inside Stormwind WMO (model ID 10047)
                if (wmoModelId == 10047) {
                    zoneId = 1519;  // Stormwind City
                }

                // Detect taverns/inns/blacksmiths by WMO model ID
                // Log WMO ID for debugging
                static uint32_t lastLoggedWmoId = 0;
                if (wmoModelId != lastLoggedWmoId) {
                    LOG_INFO("Inside WMO model ID: ", wmoModelId);
                    lastLoggedWmoId = wmoModelId;
                }

                // Detect blacksmith WMO for ambient forge sounds
                if (wmoModelId == 96048) {  // Goldshire blacksmith interior
                    insideBlacksmith = true;
                    LOG_INFO("Detected blacksmith WMO ", wmoModelId);
                }

                // These IDs represent typical Alliance and Horde inn buildings
                if (wmoModelId == 191 ||    // Goldshire inn (old ID)
                    wmoModelId == 71414 ||  // Goldshire inn (actual)
                    wmoModelId == 190 ||    // Small inn (common)
                    wmoModelId == 220 ||    // Tavern building
                    wmoModelId == 221 ||    // Large tavern
                    wmoModelId == 5392 ||   // Horde inn
                    wmoModelId == 5393) {   // Another inn variant
                    insideTavern = true;
                    // WoW tavern music (cozy ambient tracks) - FIXED PATHS
                    static const std::vector<std::string> tavernTracks = {
                        "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3",
                        "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                        "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                        "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
                    };
                    // Rotate through tracks so the player doesn't always hear the same one.
                    // Post-increment: first visit plays index 0, next plays 1, etc.
                    static int tavernTrackIndex = 0;
                    tavernMusic = tavernTracks[tavernTrackIndex++ % tavernTracks.size()];
                    LOG_INFO("Detected tavern WMO ", wmoModelId, ", playing: ", tavernMusic);
                }
            }
        }

        // Handle tavern music transitions
        if (insideTavern) {
            if (!inTavern_ && !tavernMusic.empty()) {
                inTavern_ = true;
                LOG_INFO("Entered tavern");
                audioCoordinator_->getMusicManager()->playMusic(tavernMusic, true);  // Immediate playback, looping
                musicSwitchCooldown_ = 6.0f;
            }
        } else if (inTavern_) {
            // Exited tavern - restore zone music with crossfade
            inTavern_ = false;
            LOG_INFO("Exited tavern");
            auto* info = zoneManager->getZoneInfo(currentZoneId);
            if (info) {
                std::string music = zoneManager->getRandomMusic(currentZoneId);
                if (!music.empty()) {
                    playZoneMusic(music);
                    musicSwitchCooldown_ = 6.0f;
                }
            }
        }

        // Handle blacksmith music (stop music when entering blacksmith, let ambience play)
        if (insideBlacksmith) {
            if (!inBlacksmith_) {
                inBlacksmith_ = true;
                LOG_INFO("Entered blacksmith - stopping music");
                audioCoordinator_->getMusicManager()->stopMusic();
            }
        } else if (inBlacksmith_) {
            // Exited blacksmith - restore zone music with crossfade
            inBlacksmith_ = false;
            LOG_INFO("Exited blacksmith - restoring music");
            auto* info = zoneManager->getZoneInfo(currentZoneId);
            if (info) {
                std::string music = zoneManager->getRandomMusic(currentZoneId);
                if (!music.empty()) {
                    playZoneMusic(music);
                    musicSwitchCooldown_ = 6.0f;
                }
            }
        }

        // Handle normal zone transitions (only if not in tavern or blacksmith)
        if (!insideTavern && !insideBlacksmith && zoneId != currentZoneId && zoneId != 0) {
            currentZoneId = zoneId;
            auto* info = zoneManager->getZoneInfo(zoneId);
            if (info) {
                currentZoneName = info->name;
                LOG_INFO("Entered zone: ", info->name);
                if (musicSwitchCooldown_ <= 0.0f) {
                    std::string music = zoneManager->getRandomMusic(zoneId);
                    if (!music.empty()) {
                        playZoneMusic(music);
                        musicSwitchCooldown_ = 6.0f;
                    }
                }
            }
            // Update ambient sound manager zone type
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->setZoneId(zoneId);
            }
        }

        audioCoordinator_->getMusicManager()->update(deltaTime);

        // When a track finishes, pick a new random track from the current zone
        if (!audioCoordinator_->getMusicManager()->isPlaying() && !inTavern_ && !inBlacksmith_ &&
            currentZoneId != 0 && musicSwitchCooldown_ <= 0.0f) {
            std::string music = zoneManager->getRandomMusic(currentZoneId);
            if (!music.empty()) {
                playZoneMusic(music);
                musicSwitchCooldown_ = 2.0f;
            }
        }
    }

    // Update performance HUD
    if (performanceHUD) {
        performanceHUD->update(deltaTime);
    }

    // Periodic cache hygiene: drop model GPU data no longer referenced by active instances.
    static float modelCleanupTimer = 0.0f;
    modelCleanupTimer += deltaTime;
    if (modelCleanupTimer >= 5.0f) {
        if (wmoRenderer) {
            wmoRenderer->cleanupUnusedModels();
        }
        if (m2Renderer) {
            m2Renderer->cleanupUnusedModels();
        }
        modelCleanupTimer = 0.0f;
    }

    auto updateEnd = std::chrono::steady_clock::now();
    lastUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
}

void Renderer::runDeferredWorldInitStep(float deltaTime) {
    if (!deferredWorldInitEnabled_ || !deferredWorldInitPending_ || !cachedAssetManager) return;
    if (deferredWorldInitCooldown_ > 0.0f) {
        deferredWorldInitCooldown_ = std::max(0.0f, deferredWorldInitCooldown_ - deltaTime);
        if (deferredWorldInitCooldown_ > 0.0f) return;
    }

    switch (deferredWorldInitStage_) {
        case 0:
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(cachedAssetManager);
            }
            if (terrainManager && audioCoordinator_->getAmbientSoundManager()) {
                terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
            }
            break;
        case 1:
            if (audioCoordinator_->getUiSoundManager()) audioCoordinator_->getUiSoundManager()->initialize(cachedAssetManager);
            break;
        case 2:
            if (audioCoordinator_->getCombatSoundManager()) audioCoordinator_->getCombatSoundManager()->initialize(cachedAssetManager);
            break;
        case 3:
            if (audioCoordinator_->getSpellSoundManager()) audioCoordinator_->getSpellSoundManager()->initialize(cachedAssetManager);
            break;
        case 4:
            if (audioCoordinator_->getMovementSoundManager()) audioCoordinator_->getMovementSoundManager()->initialize(cachedAssetManager);
            break;
        case 5:
            if (questMarkerRenderer) questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, cachedAssetManager);
            break;
        default:
            deferredWorldInitPending_ = false;
            return;
    }

    deferredWorldInitStage_++;
    deferredWorldInitCooldown_ = 0.12f;
}

// ============================================================
// Selection Circle
// ============================================================

void Renderer::initSelectionCircle() {
    if (selCirclePipeline != VK_NULL_HANDLE) return;
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Load shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/selection_circle.vert.spv")) {
        LOG_ERROR("initSelectionCircle: failed to load vertex shader");
        return;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/selection_circle.frag.spv")) {
        LOG_ERROR("initSelectionCircle: failed to load fragment shader");
        vertShader.destroy();
        return;
    }

    // Pipeline layout: push constants only (mat4 mvp=64 + vec4 color=16), VERTEX|FRAGMENT
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.offset = 0;
    pcRange.size = 80;
    selCirclePipelineLayout = createPipelineLayout(device, {}, {pcRange});

    // Vertex input: binding 0, stride 12, vec3 at location 0
    VkVertexInputBindingDescription vertBind{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    // Build disc geometry as TRIANGLE_LIST (replaces GL_TRIANGLE_FAN)
    // N=48 segments: center at origin + ring verts
    constexpr int SEGMENTS = 48;
    std::vector<float> verts;
    verts.reserve((SEGMENTS + 1) * 3);
    // Center vertex
    verts.insert(verts.end(), {0.0f, 0.0f, 0.0f});
    // Ring vertices
    for (int i = 0; i <= SEGMENTS; ++i) {
        float angle = 2.0f * 3.14159265f * static_cast<float>(i) / static_cast<float>(SEGMENTS);
        verts.push_back(std::cos(angle));
        verts.push_back(std::sin(angle));
        verts.push_back(0.0f);
    }

    // Build TRIANGLE_LIST indices: N triangles (center=0, ring[i]=i+1, ring[i+1]=i+2)
    std::vector<uint16_t> indices;
    indices.reserve(SEGMENTS * 3);
    for (int i = 0; i < SEGMENTS; ++i) {
        indices.push_back(0);
        indices.push_back(static_cast<uint16_t>(i + 1));
        indices.push_back(static_cast<uint16_t>(i + 2));
    }
    selCircleVertCount = SEGMENTS * 3; // index count for drawing

    // Upload vertex buffer
    AllocatedBuffer vbuf = uploadBuffer(*vkCtx, verts.data(),
        verts.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    selCircleVertBuf = vbuf.buffer;
    selCircleVertAlloc = vbuf.allocation;

    // Upload index buffer
    AllocatedBuffer ibuf = uploadBuffer(*vkCtx, indices.data(),
        indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    selCircleIdxBuf = ibuf.buffer;
    selCircleIdxAlloc = ibuf.allocation;

    // Build pipeline: alpha blend, no depth write/test, TRIANGLE_LIST, CULL_NONE
    selCirclePipeline = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, {vertAttr})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(selCirclePipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, vkCtx->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!selCirclePipeline) {
        LOG_ERROR("initSelectionCircle: failed to build pipeline");
    }
}

void Renderer::setSelectionCircle(const glm::vec3& pos, float radius, const glm::vec3& color) {
    selCirclePos = pos;
    selCircleRadius = radius;
    selCircleColor = color;
    selCircleVisible = true;
}

void Renderer::clearSelectionCircle() {
    selCircleVisible = false;
}

void Renderer::renderSelectionCircle(const glm::mat4& view, const glm::mat4& projection, VkCommandBuffer overrideCmd) {
    if (!selCircleVisible) return;
    initSelectionCircle();
    VkCommandBuffer cmd = (overrideCmd != VK_NULL_HANDLE) ? overrideCmd : currentCmd;
    if (selCirclePipeline == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;

    // Keep circle anchored near target foot Z. Accept nearby floor probes only,
    // so distant upper/lower WMO planes don't yank the ring away from feet.
    const float baseZ = selCirclePos.z;
    float floorZ = baseZ;
    auto considerFloor = [&](std::optional<float> sample) {
        if (!sample) return;
        const float h = *sample;
        // Ignore unrelated floors/ceilings far from target feet.
        if (h < baseZ - 1.25f || h > baseZ + 0.85f) return;
        floorZ = std::max(floorZ, h);
    };

    if (terrainManager) {
        considerFloor(terrainManager->getHeightAt(selCirclePos.x, selCirclePos.y));
    }
    if (wmoRenderer) {
        considerFloor(wmoRenderer->getFloorHeight(selCirclePos.x, selCirclePos.y, selCirclePos.z + 3.0f));
    }
    if (m2Renderer) {
        considerFloor(m2Renderer->getFloorHeight(selCirclePos.x, selCirclePos.y, selCirclePos.z + 2.0f));
    }

    glm::vec3 raisedPos = selCirclePos;
    raisedPos.z = floorZ + 0.17f;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), raisedPos);
    model = glm::scale(model, glm::vec3(selCircleRadius));

    glm::mat4 mvp = projection * view * model;
    glm::vec4 color4(selCircleColor, 1.0f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selCirclePipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &selCircleVertBuf, &offset);
    vkCmdBindIndexBuffer(cmd, selCircleIdxBuf, 0, VK_INDEX_TYPE_UINT16);
    // Push mvp (64 bytes) at offset 0
    vkCmdPushConstants(cmd, selCirclePipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, 64, &mvp[0][0]);
    // Push color (16 bytes) at offset 64
    vkCmdPushConstants(cmd, selCirclePipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        64, 16, &color4[0]);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(selCircleVertCount), 1, 0, 0, 0);
}

// ──────────────────────────────────────────────────────────────
// Fullscreen overlay pipeline (underwater tint, etc.)
// ──────────────────────────────────────────────────────────────

void Renderer::initOverlayPipeline() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();

    // Push constant: vec4 color (16 bytes), visible to both stages
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 16;

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &overlayPipelineLayout);

    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/overlay.frag.spv")) {
        LOG_ERROR("Renderer: failed to load overlay shaders");
        vertMod.destroy(); fragMod.destroy();
        return;
    }

    overlayPipeline = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})                             // fullscreen triangle, no VBOs
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(overlayPipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, vkCtx->getPipelineCache());

    vertMod.destroy(); fragMod.destroy();

    if (overlayPipeline) LOG_INFO("Renderer: overlay pipeline initialized");
}

void Renderer::renderOverlay(const glm::vec4& color, VkCommandBuffer overrideCmd) {
    if (!overlayPipeline) initOverlayPipeline();
    VkCommandBuffer cmd = (overrideCmd != VK_NULL_HANDLE) ? overrideCmd : currentCmd;
    if (!overlayPipeline || cmd == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline);
    vkCmdPushConstants(cmd, overlayPipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &color[0]);
    vkCmdDraw(cmd, 3, 1, 0, 0); // fullscreen triangle
}

// ========================= PostProcessPipeline delegation stubs (§4.3) =========================

PostProcessPipeline* Renderer::getPostProcessPipeline() const {
    return postProcessPipeline_.get();
}

void Renderer::setFXAAEnabled(bool enabled) {
    if (postProcessPipeline_) postProcessPipeline_->setFXAAEnabled(enabled);
}
bool Renderer::isFXAAEnabled() const {
    return postProcessPipeline_ && postProcessPipeline_->isFXAAEnabled();
}

void Renderer::setFSREnabled(bool enabled) {
    if (!postProcessPipeline_) return;
    auto req = postProcessPipeline_->setFSREnabled(enabled);
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
}
bool Renderer::isFSREnabled() const {
    return postProcessPipeline_ && postProcessPipeline_->isFSREnabled();
}
void Renderer::setFSRQuality(float scaleFactor) {
    if (postProcessPipeline_) postProcessPipeline_->setFSRQuality(scaleFactor);
}
void Renderer::setFSRSharpness(float sharpness) {
    if (postProcessPipeline_) postProcessPipeline_->setFSRSharpness(sharpness);
}
float Renderer::getFSRScaleFactor() const {
    return postProcessPipeline_ ? postProcessPipeline_->getFSRScaleFactor() : 1.0f;
}
float Renderer::getFSRSharpness() const {
    return postProcessPipeline_ ? postProcessPipeline_->getFSRSharpness() : 0.0f;
}

void Renderer::setFSR2Enabled(bool enabled) {
    if (!postProcessPipeline_) return;
    auto req = postProcessPipeline_->setFSR2Enabled(enabled, camera.get());
    if (req.requested) {
        pendingMsaaSamples_ = req.samples;
        msaaChangePending_ = true;
    }
    // If enabling FSR2 and there's already a pending MSAA change to >1x
    // (e.g. startup settings loaded MSAA before FSR2), override it to 1x.
    if (enabled && msaaChangePending_ && pendingMsaaSamples_ > VK_SAMPLE_COUNT_1_BIT) {
        pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    }
}
bool Renderer::isFSR2Enabled() const {
    return postProcessPipeline_ && postProcessPipeline_->isFSR2Enabled();
}
void Renderer::setFSR2DebugTuning(float jitterSign, float motionVecScaleX, float motionVecScaleY) {
    if (postProcessPipeline_) postProcessPipeline_->setFSR2DebugTuning(jitterSign, motionVecScaleX, motionVecScaleY);
}

void Renderer::setAmdFsr3FramegenEnabled(bool enabled) {
    if (postProcessPipeline_) postProcessPipeline_->setAmdFsr3FramegenEnabled(enabled);
}
bool Renderer::isAmdFsr3FramegenEnabled() const {
    return postProcessPipeline_ && postProcessPipeline_->isAmdFsr3FramegenEnabled();
}
float Renderer::getFSR2JitterSign() const {
    return postProcessPipeline_ ? postProcessPipeline_->getFSR2JitterSign() : 1.0f;
}
float Renderer::getFSR2MotionVecScaleX() const {
    return postProcessPipeline_ ? postProcessPipeline_->getFSR2MotionVecScaleX() : 1.0f;
}
float Renderer::getFSR2MotionVecScaleY() const {
    return postProcessPipeline_ ? postProcessPipeline_->getFSR2MotionVecScaleY() : 1.0f;
}
bool Renderer::isAmdFsr2SdkAvailable() const {
    return postProcessPipeline_ && postProcessPipeline_->isAmdFsr2SdkAvailable();
}
bool Renderer::isAmdFsr3FramegenSdkAvailable() const {
    return postProcessPipeline_ && postProcessPipeline_->isAmdFsr3FramegenSdkAvailable();
}
bool Renderer::isAmdFsr3FramegenRuntimeActive() const {
    return postProcessPipeline_ && postProcessPipeline_->isAmdFsr3FramegenRuntimeActive();
}
bool Renderer::isAmdFsr3FramegenRuntimeReady() const {
    return postProcessPipeline_ && postProcessPipeline_->isAmdFsr3FramegenRuntimeReady();
}
const char* Renderer::getAmdFsr3FramegenRuntimePath() const {
    return postProcessPipeline_ ? postProcessPipeline_->getAmdFsr3FramegenRuntimePath() : "";
}
const std::string& Renderer::getAmdFsr3FramegenRuntimeError() const {
    static const std::string empty;
    return postProcessPipeline_ ? postProcessPipeline_->getAmdFsr3FramegenRuntimeError() : empty;
}
size_t Renderer::getAmdFsr3UpscaleDispatchCount() const {
    return postProcessPipeline_ ? postProcessPipeline_->getAmdFsr3UpscaleDispatchCount() : 0;
}
size_t Renderer::getAmdFsr3FramegenDispatchCount() const {
    return postProcessPipeline_ ? postProcessPipeline_->getAmdFsr3FramegenDispatchCount() : 0;
}
size_t Renderer::getAmdFsr3FallbackCount() const {
    return postProcessPipeline_ ? postProcessPipeline_->getAmdFsr3FallbackCount() : 0;
}
void Renderer::setBrightness(float b) {
    if (postProcessPipeline_) postProcessPipeline_->setBrightness(b);
}
float Renderer::getBrightness() const {
    return postProcessPipeline_ ? postProcessPipeline_->getBrightness() : 1.0f;
}

void Renderer::renderWorld(game::World* world, game::GameHandler* gameHandler) {
    ZoneScopedN("Renderer::renderWorld");
    (void)world;

    // Guard against null command buffer (e.g. after VK_ERROR_DEVICE_LOST)
    if (currentCmd == VK_NULL_HANDLE) return;

    // GPU crash diagnostic: skip ALL world rendering to isolate crash source
    static const bool skipAll = (std::getenv("WOWEE_SKIP_ALL_RENDER") != nullptr);
    if (skipAll) return;

    auto renderStart = std::chrono::steady_clock::now();
    lastTerrainRenderMs = 0.0;
    lastWMORenderMs = 0.0;
    lastM2RenderMs = 0.0;

    // Cache ghost state for use in overlay and FXAA passes this frame.
    ghostMode_ = (gameHandler && gameHandler->isPlayerGhost());

    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDescriptorSet perFrameSet = perFrameDescSets[frameIdx];
    const glm::mat4& view = camera ? camera->getViewMatrix() : glm::mat4(1.0f);
    const glm::mat4& projection = camera ? camera->getProjectionMatrix() : glm::mat4(1.0f);

    // GPU crash diagnostic: skip individual renderers to isolate which one faults
    static const bool skipWMO = (std::getenv("WOWEE_SKIP_WMO") != nullptr);
    static const bool skipChars = (std::getenv("WOWEE_SKIP_CHARS") != nullptr);
    static const bool skipM2 = (std::getenv("WOWEE_SKIP_M2") != nullptr);
    static const bool skipTerrain = (std::getenv("WOWEE_SKIP_TERRAIN") != nullptr);
    static const bool skipSky = (std::getenv("WOWEE_SKIP_SKY") != nullptr);

    // Get time of day for sky-related rendering
    auto* skybox = skySystem ? skySystem->getSkybox() : nullptr;
    float timeOfDay = skybox ? skybox->getTimeOfDay() : 12.0f;

    // ── Multithreaded secondary command buffer recording ──
    // Terrain, WMO, and M2 record on worker threads while main thread handles
    // sky, characters, water, and effects.  prepareRender() on main thread first
    // to handle thread-unsafe GPU allocations (descriptor pools, bone SSBOs).
    if (parallelRecordingEnabled_) {
        // --- Pre-compute state + GPU allocations on main thread (not thread-safe) ---
        if (m2Renderer && cameraController) {
            m2Renderer->setInsideInterior(cameraController->isInsideWMO());
            m2Renderer->setOnTaxi(cameraController->isOnTaxi());
        }
        if (wmoRenderer) wmoRenderer->prepareRender();
        if (m2Renderer && camera) m2Renderer->prepareRender(frameIdx, *camera);
        if (characterRenderer) characterRenderer->prepareRender(frameIdx);

        // --- Dispatch worker threads (terrain + WMO + M2) ---
        std::future<double> terrainFuture, wmoFuture, m2Future;

        if (terrainRenderer && camera && terrainEnabled && !skipTerrain) {
            terrainFuture = std::async(std::launch::async, [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_TERRAIN);
                setSecondaryViewportScissor(cmd);
                terrainRenderer->render(cmd, perFrameSet, *camera);
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        if (wmoRenderer && camera && !skipWMO) {
            wmoFuture = std::async(std::launch::async, [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_WMO);
                setSecondaryViewportScissor(cmd);
                wmoRenderer->render(cmd, perFrameSet, *camera, &characterPosition);
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        if (m2Renderer && camera && !skipM2) {
            m2Future = std::async(std::launch::async, [&]() -> double {
                auto t0 = std::chrono::steady_clock::now();
                VkCommandBuffer cmd = beginSecondary(SEC_M2);
                setSecondaryViewportScissor(cmd);
                m2Renderer->render(cmd, perFrameSet, *camera);
                m2Renderer->renderSmokeParticles(cmd, perFrameSet);
                m2Renderer->renderM2Particles(cmd, perFrameSet);
                m2Renderer->renderM2Ribbons(cmd, perFrameSet);
                vkEndCommandBuffer(cmd);
                return std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            });
        }

        // --- Main thread: record sky (SEC_SKY) ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_SKY);
            setSecondaryViewportScissor(cmd);
            if (skySystem && camera && !skipSky) {
                rendering::SkyParams skyParams;
                skyParams.timeOfDay = timeOfDay;
                skyParams.gameTime = gameHandler ? gameHandler->getGameTime() : -1.0f;
                if (lightingManager) {
                    const auto& lighting = lightingManager->getLightingParams();
                    skyParams.directionalDir = lighting.directionalDir;
                    skyParams.sunColor = lighting.diffuseColor;
                    skyParams.skyTopColor = lighting.skyTopColor;
                    skyParams.skyMiddleColor = lighting.skyMiddleColor;
                    skyParams.skyBand1Color = lighting.skyBand1Color;
                    skyParams.skyBand2Color = lighting.skyBand2Color;
                    skyParams.cloudDensity = lighting.cloudDensity;
                    skyParams.fogDensity = lighting.fogDensity;
                    skyParams.horizonGlow = lighting.horizonGlow;
                }
                if (gameHandler) skyParams.weatherIntensity = gameHandler->getWeatherIntensity();
                skyParams.skyboxModelId = 0;
                skyParams.skyboxHasStars = false;
                skySystem->render(cmd, perFrameSet, *camera, skyParams);
            }
            vkEndCommandBuffer(cmd);
        }

        // --- Main thread: record characters + selection circle (SEC_CHARS) ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_CHARS);
            setSecondaryViewportScissor(cmd);
            renderSelectionCircle(view, projection, cmd);
            if (characterRenderer && camera && !skipChars) {
                characterRenderer->render(cmd, perFrameSet, *camera);
            }
            vkEndCommandBuffer(cmd);
        }

        // --- Wait for workers ---
        // Guard with try-catch: future::get() re-throws any exception from the
        // async task. Without this, a single bad_alloc in a render worker would
        // propagate as an unhandled exception and terminate the process.
        try { if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("Terrain render worker: ", e.what()); }
        try { if (wmoFuture.valid()) lastWMORenderMs = wmoFuture.get(); }
        catch (const std::exception& e) { LOG_ERROR("WMO render worker: ", e.what()); }
        try { if (m2Future.valid()) lastM2RenderMs = m2Future.get(); }
        catch (const std::exception& e) { LOG_ERROR("M2 render worker: ", e.what()); }

        // --- Main thread: record post-opaque (SEC_POST) ---
        {
            VkCommandBuffer cmd = beginSecondary(SEC_POST);
            setSecondaryViewportScissor(cmd);
            if (waterRenderer && camera)
                waterRenderer->render(cmd, perFrameSet, *camera, globalTime, false, frameIdx);
            if (weather && camera) weather->render(cmd, perFrameSet);
            if (lightning && camera && lightning->isEnabled()) lightning->render(cmd, perFrameSet);
            if (swimEffects && camera) swimEffects->render(cmd, perFrameSet);
            if (mountDust && camera) mountDust->render(cmd, perFrameSet);
            if (chargeEffect && camera) chargeEffect->render(cmd, perFrameSet);
            if (questMarkerRenderer && camera) questMarkerRenderer->render(cmd, perFrameSet, *camera);

            // Underwater overlay + minimap
            if (overlayPipeline && waterRenderer && camera) {
                glm::vec3 camPos = camera->getPosition();
                auto waterH = waterRenderer->getNearestWaterHeightAt(camPos.x, camPos.y, camPos.z);
                constexpr float MIN_SUBMERSION_OVERLAY = 1.5f;
                if (waterH && camPos.z < (*waterH - MIN_SUBMERSION_OVERLAY)
                           && !waterRenderer->isWmoWaterAt(camPos.x, camPos.y)) {
                    float depth = *waterH - camPos.z - MIN_SUBMERSION_OVERLAY;
                    bool canal = false;
                    if (auto lt = waterRenderer->getWaterTypeAt(camPos.x, camPos.y))
                        canal = (*lt == 5 || *lt == 13 || *lt == 17);
                    float fogStrength = 1.0f - std::exp(-depth * (canal ? 0.25f : 0.12f));
                    fogStrength = glm::clamp(fogStrength, 0.0f, 0.75f);
                    glm::vec4 tint = canal
                        ? glm::vec4(0.01f, 0.04f, 0.10f, fogStrength)
                        : glm::vec4(0.03f, 0.09f, 0.18f, fogStrength);
                    renderOverlay(tint, cmd);
                }
            }
            // Ghost mode desaturation: cold blue-grey overlay when dead/ghost
            if (ghostMode_) {
                renderOverlay(glm::vec4(0.30f, 0.35f, 0.42f, 0.45f), cmd);
            }
            // Brightness overlay (applied before minimap so it doesn't affect UI)
            {
                float br = postProcessPipeline_ ? postProcessPipeline_->getBrightness() : 1.0f;
                if (br < 0.99f) {
                    renderOverlay(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - br), cmd);
                } else if (br > 1.01f) {
                    float alpha = (br - 1.0f) / 1.0f;
                    renderOverlay(glm::vec4(1.0f, 1.0f, 1.0f, alpha), cmd);
                }
            }
            if (minimap && minimap->isEnabled() && camera && window) {
                glm::vec3 minimapCenter = camera->getPosition();
                if (cameraController && cameraController->isThirdPerson())
                    minimapCenter = characterPosition;
                float minimapPlayerOrientation = 0.0f;
                bool hasMinimapPlayerOrientation = false;
                if (cameraController) {
                    float facingRad = glm::radians(characterYaw);
                    glm::vec3 facingFwd(std::cos(facingRad), std::sin(facingRad), 0.0f);
                    // atan2(-x,y) = canonical yaw (0=North); negate for shader convention.
                    minimapPlayerOrientation = -std::atan2(-facingFwd.x, facingFwd.y);
                    hasMinimapPlayerOrientation = true;
                } else if (gameHandler) {
                    // movementInfo.orientation is canonical yaw: 0=North, π/2=East.
                    // Minimap shader: arrowRotation=0 points up (North), positive rotates CW
                    // (π/2=West, -π/2=East). Correct mapping: arrowRotation = -canonical_yaw.
                    minimapPlayerOrientation = -gameHandler->getMovementInfo().orientation;
                    hasMinimapPlayerOrientation = true;
                }
                minimap->render(cmd, *camera, minimapCenter,
                                window->getWidth(), window->getHeight(),
                                minimapPlayerOrientation, hasMinimapPlayerOrientation);
            }
            vkEndCommandBuffer(cmd);
        }

        // --- Execute all secondary buffers in correct draw order ---
        VkCommandBuffer validCmds[6];
        uint32_t numCmds = 0;
        validCmds[numCmds++] = secondaryCmds_[SEC_SKY][frameIdx];
        if (terrainRenderer && camera && terrainEnabled && !skipTerrain)
            validCmds[numCmds++] = secondaryCmds_[SEC_TERRAIN][frameIdx];
        if (wmoRenderer && camera && !skipWMO)
            validCmds[numCmds++] = secondaryCmds_[SEC_WMO][frameIdx];
        validCmds[numCmds++] = secondaryCmds_[SEC_CHARS][frameIdx];
        if (m2Renderer && camera && !skipM2)
            validCmds[numCmds++] = secondaryCmds_[SEC_M2][frameIdx];
        validCmds[numCmds++] = secondaryCmds_[SEC_POST][frameIdx];

        vkCmdExecuteCommands(currentCmd, numCmds, validCmds);

    } else {
        // ── Fallback: single-threaded inline recording (original path) ──

        if (skySystem && camera && !skipSky) {
            rendering::SkyParams skyParams;
            skyParams.timeOfDay = timeOfDay;
            skyParams.gameTime = gameHandler ? gameHandler->getGameTime() : -1.0f;
            if (lightingManager) {
                const auto& lighting = lightingManager->getLightingParams();
                skyParams.directionalDir = lighting.directionalDir;
                skyParams.sunColor = lighting.diffuseColor;
                skyParams.skyTopColor = lighting.skyTopColor;
                skyParams.skyMiddleColor = lighting.skyMiddleColor;
                skyParams.skyBand1Color = lighting.skyBand1Color;
                skyParams.skyBand2Color = lighting.skyBand2Color;
                skyParams.cloudDensity = lighting.cloudDensity;
                skyParams.fogDensity = lighting.fogDensity;
                skyParams.horizonGlow = lighting.horizonGlow;
            }
            if (gameHandler) skyParams.weatherIntensity = gameHandler->getWeatherIntensity();
            skyParams.skyboxModelId = 0;
            skyParams.skyboxHasStars = false;
            skySystem->render(currentCmd, perFrameSet, *camera, skyParams);
        }

        if (terrainRenderer && camera && terrainEnabled && !skipTerrain) {
            auto terrainStart = std::chrono::steady_clock::now();
            terrainRenderer->render(currentCmd, perFrameSet, *camera);
            lastTerrainRenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - terrainStart).count();
        }

        if (wmoRenderer && camera && !skipWMO) {
            wmoRenderer->prepareRender();
            auto wmoStart = std::chrono::steady_clock::now();
            wmoRenderer->render(currentCmd, perFrameSet, *camera, &characterPosition);
            lastWMORenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wmoStart).count();
        }

        renderSelectionCircle(view, projection);

        if (characterRenderer && camera && !skipChars) {
            characterRenderer->prepareRender(frameIdx);
            characterRenderer->render(currentCmd, perFrameSet, *camera);
        }

        if (m2Renderer && camera && !skipM2) {
            if (cameraController) {
                m2Renderer->setInsideInterior(cameraController->isInsideWMO());
                m2Renderer->setOnTaxi(cameraController->isOnTaxi());
            }
            m2Renderer->prepareRender(frameIdx, *camera);
            auto m2Start = std::chrono::steady_clock::now();
            m2Renderer->render(currentCmd, perFrameSet, *camera);
            m2Renderer->renderSmokeParticles(currentCmd, perFrameSet);
            m2Renderer->renderM2Particles(currentCmd, perFrameSet);
            m2Renderer->renderM2Ribbons(currentCmd, perFrameSet);
            lastM2RenderMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m2Start).count();
        }

        if (waterRenderer && camera)
            waterRenderer->render(currentCmd, perFrameSet, *camera, globalTime, false, frameIdx);
        if (weather && camera) weather->render(currentCmd, perFrameSet);
        if (lightning && camera && lightning->isEnabled()) lightning->render(currentCmd, perFrameSet);
        if (swimEffects && camera) swimEffects->render(currentCmd, perFrameSet);
        if (mountDust && camera) mountDust->render(currentCmd, perFrameSet);
        if (chargeEffect && camera) chargeEffect->render(currentCmd, perFrameSet);
        if (questMarkerRenderer && camera) questMarkerRenderer->render(currentCmd, perFrameSet, *camera);
    }

    // Underwater overlay and minimap — in the fallback path these run inline;
    // in the parallel path they were already recorded into SEC_POST above.
    if (!parallelRecordingEnabled_) {
        if (overlayPipeline && waterRenderer && camera) {
            glm::vec3 camPos = camera->getPosition();
            auto waterH = waterRenderer->getNearestWaterHeightAt(camPos.x, camPos.y, camPos.z);
            constexpr float MIN_SUBMERSION_OVERLAY = 1.5f;
            if (waterH && camPos.z < (*waterH - MIN_SUBMERSION_OVERLAY)
                       && !waterRenderer->isWmoWaterAt(camPos.x, camPos.y)) {
                float depth = *waterH - camPos.z - MIN_SUBMERSION_OVERLAY;
                bool canal = false;
                if (auto lt = waterRenderer->getWaterTypeAt(camPos.x, camPos.y))
                    canal = (*lt == 5 || *lt == 13 || *lt == 17);
                float fogStrength = 1.0f - std::exp(-depth * (canal ? 0.25f : 0.12f));
                fogStrength = glm::clamp(fogStrength, 0.0f, 0.75f);
                glm::vec4 tint = canal
                    ? glm::vec4(0.01f, 0.04f, 0.10f, fogStrength)
                    : glm::vec4(0.03f, 0.09f, 0.18f, fogStrength);
                renderOverlay(tint);
            }
        }
        // Ghost mode desaturation: cold blue-grey overlay when dead/ghost
        if (ghostMode_) {
            renderOverlay(glm::vec4(0.30f, 0.35f, 0.42f, 0.45f));
        }
        // Brightness overlay (applied before minimap so it doesn't affect UI)
        {
            float br = postProcessPipeline_ ? postProcessPipeline_->getBrightness() : 1.0f;
            if (br < 0.99f) {
                renderOverlay(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f - br));
            } else if (br > 1.01f) {
                float alpha = (br - 1.0f) / 1.0f;
                renderOverlay(glm::vec4(1.0f, 1.0f, 1.0f, alpha));
            }
        }
        if (minimap && minimap->isEnabled() && camera && window) {
            glm::vec3 minimapCenter = camera->getPosition();
            if (cameraController && cameraController->isThirdPerson())
                minimapCenter = characterPosition;
            float minimapPlayerOrientation = 0.0f;
            bool hasMinimapPlayerOrientation = false;
            if (cameraController) {
                float facingRad = glm::radians(characterYaw);
                glm::vec3 facingFwd(std::cos(facingRad), std::sin(facingRad), 0.0f);
                // atan2(-x,y) = canonical yaw (0=North); negate for shader convention.
                minimapPlayerOrientation = -std::atan2(-facingFwd.x, facingFwd.y);
                hasMinimapPlayerOrientation = true;
            } else if (gameHandler) {
                // movementInfo.orientation is canonical yaw: 0=North, π/2=East.
                // Minimap shader: arrowRotation=0 points up (North), positive rotates CW
                // (π/2=West, -π/2=East). Correct mapping: arrowRotation = -canonical_yaw.
                minimapPlayerOrientation = -gameHandler->getMovementInfo().orientation;
                hasMinimapPlayerOrientation = true;
            }
            minimap->render(currentCmd, *camera, minimapCenter,
                            window->getWidth(), window->getHeight(),
                            minimapPlayerOrientation, hasMinimapPlayerOrientation);
        }
    }

    auto renderEnd = std::chrono::steady_clock::now();
    lastRenderMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
}

// initPostProcess(), resizePostProcess(), shutdownPostProcess() removed —
// post-process pipeline is now handled by Vulkan (Phase 6 cleanup).

bool Renderer::initializeRenderers(pipeline::AssetManager* assetManager, const std::string& mapName) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Initializing renderers for map: ", mapName);

    // Create terrain renderer if not already created
    if (!terrainRenderer) {
        terrainRenderer = std::make_unique<TerrainRenderer>();
        if (!terrainRenderer->initialize(vkCtx, perFrameSetLayout, assetManager)) {
            LOG_ERROR("Failed to initialize terrain renderer");
            terrainRenderer.reset();
            return false;
        }
        if (shadowRenderPass != VK_NULL_HANDLE) {
            terrainRenderer->initializeShadow(shadowRenderPass);
        }
    } else if (!terrainRenderer->hasShadowPipeline() && shadowRenderPass != VK_NULL_HANDLE) {
        terrainRenderer->initializeShadow(shadowRenderPass);
    }

    // Create water renderer if not already created
    if (!waterRenderer) {
        waterRenderer = std::make_unique<WaterRenderer>();
        if (!waterRenderer->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize water renderer");
            waterRenderer.reset();
        }
    }

    // Create minimap if not already created
    if (!minimap) {
        minimap = std::make_unique<Minimap>();
        if (!minimap->initialize(vkCtx, perFrameSetLayout)) {
            LOG_ERROR("Failed to initialize minimap");
            minimap.reset();
        }
    }

    // Create world map if not already created
    if (!worldMap) {
        worldMap = std::make_unique<WorldMap>();
        if (!worldMap->initialize(vkCtx, assetManager)) {
            LOG_ERROR("Failed to initialize world map");
            worldMap.reset();
        }
    }

    // Create M2, WMO, and Character renderers
    if (!m2Renderer) {
        m2Renderer = std::make_unique<M2Renderer>();
        m2Renderer->initialize(vkCtx, perFrameSetLayout, assetManager);
        if (swimEffects) {
            swimEffects->setM2Renderer(m2Renderer.get());
        }
        // Initialize SpellVisualSystem once M2Renderer is available (§4.4)
        if (!spellVisualSystem_) {
            spellVisualSystem_ = std::make_unique<SpellVisualSystem>();
            spellVisualSystem_->initialize(m2Renderer.get());
        }
    }
    if (!wmoRenderer) {
        wmoRenderer = std::make_unique<WMORenderer>();
        wmoRenderer->initialize(vkCtx, perFrameSetLayout, assetManager);
        if (shadowRenderPass != VK_NULL_HANDLE) {
            wmoRenderer->initializeShadow(shadowRenderPass);
        }
    }

    // Initialize shadow pipelines for M2 if not yet done
    if (m2Renderer && shadowRenderPass != VK_NULL_HANDLE && !m2Renderer->hasShadowPipeline()) {
        m2Renderer->initializeShadow(shadowRenderPass);
    }
    if (!characterRenderer) {
        characterRenderer = std::make_unique<CharacterRenderer>();
        characterRenderer->initialize(vkCtx, perFrameSetLayout, assetManager);
        if (shadowRenderPass != VK_NULL_HANDLE) {
            characterRenderer->initializeShadow(shadowRenderPass);
        }
    }

    // Initialize AnimationController (§4.2)
    if (!animationController_) {
        animationController_ = std::make_unique<AnimationController>();
        animationController_->initialize(this);
    }

    // Create and initialize terrain manager
    if (!terrainManager) {
        terrainManager = std::make_unique<TerrainManager>();
        if (!terrainManager->initialize(assetManager, terrainRenderer.get())) {
            LOG_ERROR("Failed to initialize terrain manager");
            terrainManager.reset();
            return false;
        }
        // Set water renderer for terrain streaming
        if (waterRenderer) {
            terrainManager->setWaterRenderer(waterRenderer.get());
        }
        // Set M2 renderer for doodad loading during streaming
        if (m2Renderer) {
            terrainManager->setM2Renderer(m2Renderer.get());
        }
        // Set WMO renderer for building loading during streaming
        if (wmoRenderer) {
            terrainManager->setWMORenderer(wmoRenderer.get());
        }
        // Set ambient sound manager for environmental audio emitters
        if (audioCoordinator_->getAmbientSoundManager()) {
            terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
        }
        // Pass asset manager to character renderer for texture loading
        if (characterRenderer) {
            characterRenderer->setAssetManager(assetManager);
        }
        // Wire asset manager to minimap for tile texture loading
        if (minimap) {
            minimap->setAssetManager(assetManager);
        }
        // Wire terrain manager, WMO renderer, and water renderer to camera controller
        if (cameraController) {
            cameraController->setTerrainManager(terrainManager.get());
            if (wmoRenderer) {
                cameraController->setWMORenderer(wmoRenderer.get());
            }
            if (m2Renderer) {
                cameraController->setM2Renderer(m2Renderer.get());
            }
            if (waterRenderer) {
                cameraController->setWaterRenderer(waterRenderer.get());
            }
        }
    }

    // Set map name on sub-renderers
    if (terrainManager) terrainManager->setMapName(mapName);
    if (minimap) minimap->setMapName(mapName);
    if (worldMap) worldMap->setMapName(mapName);

    // Initialize audio managers
    if (audioCoordinator_->getMusicManager() && assetManager && !cachedAssetManager) {
        audio::AudioEngine::instance().setAssetManager(assetManager);
        audioCoordinator_->getMusicManager()->initialize(assetManager);
        if (audioCoordinator_->getFootstepManager()) {
            audioCoordinator_->getFootstepManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getActivitySoundManager()) {
            audioCoordinator_->getActivitySoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getMountSoundManager()) {
            audioCoordinator_->getMountSoundManager()->initialize(assetManager);
        }
        if (audioCoordinator_->getNpcVoiceManager()) {
            audioCoordinator_->getNpcVoiceManager()->initialize(assetManager);
        }
        if (!deferredWorldInitEnabled_) {
            if (audioCoordinator_->getAmbientSoundManager()) {
                audioCoordinator_->getAmbientSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getUiSoundManager()) {
                audioCoordinator_->getUiSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getCombatSoundManager()) {
                audioCoordinator_->getCombatSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getSpellSoundManager()) {
                audioCoordinator_->getSpellSoundManager()->initialize(assetManager);
            }
            if (audioCoordinator_->getMovementSoundManager()) {
                audioCoordinator_->getMovementSoundManager()->initialize(assetManager);
            }
            if (questMarkerRenderer) {
                questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, assetManager);
            }

            if (envFlagEnabled("WOWEE_PREWARM_ZONE_MUSIC", false)) {
                if (zoneManager) {
                    for (const auto& musicPath : zoneManager->getAllMusicPaths()) {
                        audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                    }
                }
                static const std::vector<std::string> tavernTracks = {
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
                };
                for (const auto& musicPath : tavernTracks) {
                    audioCoordinator_->getMusicManager()->preloadMusic(musicPath);
                }
            }
        } else {
            deferredWorldInitPending_ = true;
            deferredWorldInitStage_ = 0;
            deferredWorldInitCooldown_ = 0.25f;
        }

        cachedAssetManager = assetManager;

        // Enrich zone music from DBC if not already done (e.g. asset manager was null at init).
        if (zoneManager && assetManager) {
            zoneManager->enrichFromDBC(assetManager);
        }
    }

    // Snap camera to ground
    if (cameraController) {
        cameraController->reset();
    }

    return true;
}

bool Renderer::loadTestTerrain(pipeline::AssetManager* assetManager, const std::string& adtPath) {
    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    LOG_INFO("Loading test terrain: ", adtPath);

    // Extract map name from ADT path for renderer initialization
    std::string mapName;
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            mapName = filename.substr(0, firstUnderscore != std::string::npos ? firstUnderscore : filename.size());
        }
    }

    // Initialize all sub-renderers
    if (!initializeRenderers(assetManager, mapName)) {
        return false;
    }

    // Parse tile coordinates from ADT path
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    int tileX = 32, tileY = 49;  // defaults
    {
        size_t lastSep = adtPath.find_last_of("\\/");
        if (lastSep != std::string::npos) {
            std::string filename = adtPath.substr(lastSep + 1);
            size_t firstUnderscore = filename.find('_');
            if (firstUnderscore != std::string::npos) {
                size_t secondUnderscore = filename.find('_', firstUnderscore + 1);
                if (secondUnderscore != std::string::npos) {
                    size_t dot = filename.find('.', secondUnderscore);
                    if (dot != std::string::npos) {
                        try {
                            tileX = std::stoi(filename.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1));
                            tileY = std::stoi(filename.substr(secondUnderscore + 1, dot - secondUnderscore - 1));
                        } catch (...) {
                            LOG_WARNING("Failed to parse tile coords from: ", filename);
                        }
                    }
                }
            }
        }
    }

    LOG_INFO("Enqueuing initial tile [", tileX, ",", tileY, "] via terrain manager");

    // Enqueue the initial tile for async loading (avoids long sync stalls)
    if (!terrainManager->enqueueTile(tileX, tileY)) {
        LOG_ERROR("Failed to enqueue initial tile [", tileX, ",", tileY, "]");
        return false;
    }

    terrainLoaded = true;

    LOG_INFO("Test terrain loaded successfully!");
    LOG_INFO("  Chunks: ", terrainRenderer->getChunkCount());
    LOG_INFO("  Triangles: ", terrainRenderer->getTriangleCount());

    return true;
}

void Renderer::setWireframeMode(bool enabled) {
    if (terrainRenderer) {
        terrainRenderer->setWireframe(enabled);
    }
}

bool Renderer::loadTerrainArea(const std::string& mapName, int centerX, int centerY, int radius) {
    // Create terrain renderer if not already created
    if (!terrainRenderer) {
        LOG_ERROR("Terrain renderer not initialized");
        return false;
    }

    // Create terrain manager if not already created
    if (!terrainManager) {
        terrainManager = std::make_unique<TerrainManager>();
        // Wire terrain manager to camera controller for grounding
        if (cameraController) {
            cameraController->setTerrainManager(terrainManager.get());
        }
    }

    LOG_INFO("Loading terrain area: ", mapName, " [", centerX, ",", centerY, "] radius=", radius);

    terrainManager->setMapName(mapName);
    terrainManager->setLoadRadius(radius);
    terrainManager->setUnloadRadius(radius + 1);

    // Load tiles in radius
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int tileX = centerX + dx;
            int tileY = centerY + dy;

            if (tileX >= 0 && tileX <= 63 && tileY >= 0 && tileY <= 63) {
                terrainManager->loadTile(tileX, tileY);
            }
        }
    }

    terrainLoaded = true;

    // Get asset manager from Application if not cached yet
    if (!cachedAssetManager) {
        cachedAssetManager = core::Application::getInstance().getAssetManager();
    }

    // Initialize music manager with asset manager
    if (audioCoordinator_->getMusicManager() && cachedAssetManager) {
        if (!audioCoordinator_->getMusicManager()->isInitialized()) {
            audioCoordinator_->getMusicManager()->initialize(cachedAssetManager);
        }
    }
    if (audioCoordinator_->getFootstepManager() && cachedAssetManager) {
        if (!audioCoordinator_->getFootstepManager()->isInitialized()) {
            audioCoordinator_->getFootstepManager()->initialize(cachedAssetManager);
        }
    }
    if (audioCoordinator_->getActivitySoundManager() && cachedAssetManager) {
        if (!audioCoordinator_->getActivitySoundManager()->isInitialized()) {
            audioCoordinator_->getActivitySoundManager()->initialize(cachedAssetManager);
        }
    }
    if (audioCoordinator_->getMountSoundManager() && cachedAssetManager) {
        audioCoordinator_->getMountSoundManager()->initialize(cachedAssetManager);
    }
    if (audioCoordinator_->getNpcVoiceManager() && cachedAssetManager) {
        audioCoordinator_->getNpcVoiceManager()->initialize(cachedAssetManager);
    }
    if (!deferredWorldInitEnabled_) {
        if (audioCoordinator_->getAmbientSoundManager() && cachedAssetManager) {
            audioCoordinator_->getAmbientSoundManager()->initialize(cachedAssetManager);
        }
        if (audioCoordinator_->getUiSoundManager() && cachedAssetManager) {
            audioCoordinator_->getUiSoundManager()->initialize(cachedAssetManager);
        }
        if (audioCoordinator_->getCombatSoundManager() && cachedAssetManager) {
            audioCoordinator_->getCombatSoundManager()->initialize(cachedAssetManager);
        }
        if (audioCoordinator_->getSpellSoundManager() && cachedAssetManager) {
            audioCoordinator_->getSpellSoundManager()->initialize(cachedAssetManager);
        }
        if (audioCoordinator_->getMovementSoundManager() && cachedAssetManager) {
            audioCoordinator_->getMovementSoundManager()->initialize(cachedAssetManager);
        }
        if (questMarkerRenderer && cachedAssetManager) {
            questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, cachedAssetManager);
        }
    } else {
        deferredWorldInitPending_ = true;
        deferredWorldInitStage_ = 0;
        deferredWorldInitCooldown_ = 0.1f;
    }

    // Wire ambient sound manager to terrain manager for emitter registration
    if (terrainManager && audioCoordinator_->getAmbientSoundManager()) {
        terrainManager->setAmbientSoundManager(audioCoordinator_->getAmbientSoundManager());
    }

    // Wire WMO, M2, and water renderer to camera controller
    if (cameraController && wmoRenderer) {
        cameraController->setWMORenderer(wmoRenderer.get());
    }
    if (cameraController && m2Renderer) {
        cameraController->setM2Renderer(m2Renderer.get());
    }
    if (cameraController && waterRenderer) {
        cameraController->setWaterRenderer(waterRenderer.get());
    }

    // Snap camera to ground now that terrain is loaded
    if (cameraController) {
        cameraController->reset();
    }

    LOG_INFO("Terrain area loaded: ", terrainManager->getLoadedTileCount(), " tiles");

    return true;
}

void Renderer::setTerrainStreaming(bool enabled) {
    if (terrainManager) {
        terrainManager->setStreamingEnabled(enabled);
        LOG_INFO("Terrain streaming: ", enabled ? "ON" : "OFF");
    }
}

void Renderer::renderHUD() {
    if (currentCmd == VK_NULL_HANDLE) return;
    if (performanceHUD && camera) {
        performanceHUD->render(this, camera.get());
    }
}

// ──────────────────────────────────────────────────────
// Shadow mapping helpers
// ──────────────────────────────────────────────────────

// initShadowMap() and compileShadowShader() removed — shadow resources now created
// in createPerFrameResources() as part of the Vulkan shadow infrastructure.

glm::mat4 Renderer::computeLightSpaceMatrix() {
    const float kShadowHalfExtent = shadowDistance_;
    const float kShadowLightDistance = shadowDistance_ * 3.0f;
    constexpr float kShadowNearPlane = 1.0f;
    const float kShadowFarPlane = shadowDistance_ * 6.5f;

    // Use active lighting direction so shadow projection matches main shading.
    // Fragment shaders derive lighting with `ldir = normalize(-lightDir.xyz)`,
    // therefore shadow rays must use -directionalDir to stay aligned.
    glm::vec3 sunDir = glm::normalize(glm::vec3(-0.3f, -0.7f, -0.6f));
    if (lightingManager) {
        const auto& lighting = lightingManager->getLightingParams();
        float ldirLenSq = glm::dot(lighting.directionalDir, lighting.directionalDir);
        if (ldirLenSq > 1e-6f) {
            sunDir = -lighting.directionalDir * glm::inversesqrt(ldirLenSq);
        }
    }
    // Shadow camera expects light rays pointing downward in render space (Z up).
    // Some profiles/opcode paths provide the opposite convention; normalize here.
    if (sunDir.z > 0.0f) {
        sunDir = -sunDir;
    }
    // Keep a minimum downward component so the frustum doesn't collapse at grazing angles.
    if (sunDir.z > -0.15f) {
        sunDir.z = -0.15f;
        sunDir = glm::normalize(sunDir);
    }

    // Shadow center follows the player directly; texel snapping below
    // prevents shimmer without needing to freeze the projection.
    glm::vec3 desiredCenter = characterPosition;
    if (!shadowCenterInitialized) {
        if (glm::dot(desiredCenter, desiredCenter) < 1.0f) {
            return glm::mat4(0.0f);
        }
        shadowCenterInitialized = true;
    }
    shadowCenter = desiredCenter;
    glm::vec3 center = shadowCenter;

    // Snap shadow frustum to texel grid so the projection is perfectly stable
    // while moving. We compute the light's right/up axes from the sun direction
    // (these are constant per frame regardless of center) and snap center along
    // them before building the view matrix.
    float halfExtent = kShadowHalfExtent;
    float texelWorld = (2.0f * halfExtent) / static_cast<float>(SHADOW_MAP_SIZE);

    // Stable light-space axes (independent of center position)
    glm::vec3 up(0.0f, 0.0f, 1.0f);
    if (std::abs(glm::dot(sunDir, up)) > 0.99f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    glm::vec3 lightRight = glm::normalize(glm::cross(sunDir, up));
    glm::vec3 lightUp = glm::normalize(glm::cross(lightRight, sunDir));

    // Snap center along light's right and up axes to align with texel grid.
    // This eliminates sub-texel shifts that cause shadow shimmer.
    float dotR = glm::dot(center, lightRight);
    float dotU = glm::dot(center, lightUp);
    dotR = std::floor(dotR / texelWorld) * texelWorld;
    dotU = std::floor(dotU / texelWorld) * texelWorld;
    float dotD = glm::dot(center, sunDir);  // depth axis unchanged
    center = lightRight * dotR + lightUp * dotU + sunDir * dotD;
    shadowCenter = center;

    glm::mat4 lightView = glm::lookAt(center - sunDir * kShadowLightDistance, center, up);
    glm::mat4 lightProj = glm::ortho(-halfExtent, halfExtent, -halfExtent, halfExtent,
                                     kShadowNearPlane, kShadowFarPlane);
    lightProj[1][1] *= -1.0f; // Vulkan Y-flip for shadow pass

    return lightProj * lightView;
}

void Renderer::setupWater1xPass() {
    if (!waterRenderer || !vkCtx) return;
    VkImageView depthView = vkCtx->getDepthResolveImageView();
    if (!depthView) {
        LOG_WARNING("No depth resolve image available - cannot create 1x water pass");
        return;
    }

    waterRenderer->createWater1xPass(vkCtx->getSwapchainFormat(), vkCtx->getDepthFormat());
    waterRenderer->createWater1xFramebuffers(
        vkCtx->getSwapchainImageViews(), depthView, vkCtx->getSwapchainExtent());
}

// ========================= Multithreaded Secondary Command Buffers =========================

bool Renderer::createSecondaryCommandResources() {
    if (!vkCtx) return false;
    VkDevice device = vkCtx->getDevice();
    uint32_t queueFamily = vkCtx->getGraphicsQueueFamily();

    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCI.queueFamilyIndex = queueFamily;

    // Create worker command pools (one per worker thread)
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        if (vkCreateCommandPool(device, &poolCI, nullptr, &workerCmdPools_[w]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create worker command pool ", w);
            return false;
        }
    }

    // Create main-thread secondary command pool
    if (vkCreateCommandPool(device, &poolCI, nullptr, &mainSecondaryCmdPool_) != VK_SUCCESS) {
        LOG_ERROR("Failed to create main secondary command pool");
        return false;
    }

    // Allocate secondary command buffers
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    allocInfo.commandBufferCount = 1;

    // Worker secondaries: SEC_TERRAIN=1, SEC_WMO=2, SEC_M2=4 → worker pools 0,1,2
    const uint32_t workerSecondaries[] = { SEC_TERRAIN, SEC_WMO, SEC_M2 };
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        allocInfo.commandPool = workerCmdPools_[w];
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[workerSecondaries[w]][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate worker secondary buffer w=", w, " f=", f);
                return false;
            }
        }
    }

    // Main-thread secondaries: SEC_SKY=0, SEC_CHARS=3, SEC_POST=5, SEC_IMGUI=6
    const uint32_t mainSecondaries[] = { SEC_SKY, SEC_CHARS, SEC_POST, SEC_IMGUI };
    for (uint32_t idx : mainSecondaries) {
        allocInfo.commandPool = mainSecondaryCmdPool_;
        for (uint32_t f = 0; f < MAX_FRAMES; ++f) {
            if (vkAllocateCommandBuffers(device, &allocInfo, &secondaryCmds_[idx][f]) != VK_SUCCESS) {
                LOG_ERROR("Failed to allocate main secondary buffer idx=", idx, " f=", f);
                return false;
            }
        }
    }

    parallelRecordingEnabled_ = true;
    LOG_INFO("Multithreaded rendering: ", NUM_WORKERS, " worker threads, ",
             NUM_SECONDARIES, " secondary buffers [ENABLED]");
    return true;
}

void Renderer::destroySecondaryCommandResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    vkDeviceWaitIdle(device);

    // Secondary buffers are freed when their pool is destroyed
    for (uint32_t w = 0; w < NUM_WORKERS; ++w) {
        if (workerCmdPools_[w]) {
            vkDestroyCommandPool(device, workerCmdPools_[w], nullptr);
            workerCmdPools_[w] = VK_NULL_HANDLE;
        }
    }
    if (mainSecondaryCmdPool_) {
        vkDestroyCommandPool(device, mainSecondaryCmdPool_, nullptr);
        mainSecondaryCmdPool_ = VK_NULL_HANDLE;
    }

    for (auto& arr : secondaryCmds_)
        for (auto& cmd : arr)
            cmd = VK_NULL_HANDLE;

    parallelRecordingEnabled_ = false;
}

VkCommandBuffer Renderer::beginSecondary(uint32_t secondaryIndex) {
    uint32_t frame = vkCtx->getCurrentFrame();
    VkCommandBuffer cmd = secondaryCmds_[secondaryIndex][frame];

    VkCommandBufferInheritanceInfo inheritInfo{};
    inheritInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritInfo.renderPass = activeRenderPass_;
    inheritInfo.subpass = 0;
    inheritInfo.framebuffer = activeFramebuffer_;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                    | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    beginInfo.pInheritanceInfo = &inheritInfo;

    VkResult result = vkBeginCommandBuffer(cmd, &beginInfo);
    if (result != VK_SUCCESS) {
        LOG_ERROR("vkBeginCommandBuffer failed for secondary ", secondaryIndex,
                  " frame ", frame, " result=", static_cast<int>(result));
    }
    return cmd;
}

void Renderer::setSecondaryViewportScissor(VkCommandBuffer cmd) {
    VkViewport vp{};
    vp.width = static_cast<float>(activeRenderExtent_.width);
    vp.height = static_cast<float>(activeRenderExtent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.extent = activeRenderExtent_;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void Renderer::renderReflectionPass() {
    if (!waterRenderer || !camera || !waterRenderer->hasReflectionPass() || !waterRenderer->hasSurfaces()) return;
    if (currentCmd == VK_NULL_HANDLE || !reflPerFrameUBOMapped) return;

    // Select the current frame's pre-bound reflection descriptor set
    // (each frame's set was bound to its own shadow depth view at init).
    uint32_t frame = vkCtx->getCurrentFrame();
    VkDescriptorSet reflDescSet = reflPerFrameDescSet[frame];

    // Reflection pass uses 1x MSAA. Scene pipelines must be render-pass-compatible,
    // which requires matching sample counts. Only render scene into reflection when MSAA is off.
    bool canRenderScene = (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT);

    // Find dominant water height near camera
    const glm::vec3 camPos = camera->getPosition();
    auto waterH = waterRenderer->getDominantWaterHeight(camPos);
    if (!waterH) return;

    float waterHeight = *waterH;

    // Skip reflection if camera is underwater (Z is up)
    if (camPos.z < waterHeight + 0.5f) return;

    // Compute reflected view and oblique projection
    glm::mat4 reflView = WaterRenderer::computeReflectedView(*camera, waterHeight);
    glm::mat4 reflProj = WaterRenderer::computeObliqueProjection(
        camera->getProjectionMatrix(), reflView, waterHeight);

    // Update water renderer's reflection UBO with the reflected viewProj
    waterRenderer->updateReflectionUBO(reflProj * reflView);

    // Fill the reflection per-frame UBO (same as normal but with reflected matrices)
    GPUPerFrameData reflData = currentFrameData;
    reflData.view = reflView;
    reflData.projection = reflProj;
    // Reflected camera position (Z is up)
    glm::vec3 reflPos = camPos;
    reflPos.z = 2.0f * waterHeight - reflPos.z;
    reflData.viewPos = glm::vec4(reflPos, 1.0f);
    std::memcpy(reflPerFrameUBOMapped, &reflData, sizeof(GPUPerFrameData));

    // Begin reflection render pass (clears to black; scene rendered if pipeline-compatible)
    if (!waterRenderer->beginReflectionPass(currentCmd)) return;

    if (canRenderScene) {
        // Render scene into reflection texture (sky + terrain + WMO only for perf)
        if (skySystem) {
            rendering::SkyParams skyParams;
            auto* reflSkybox = skySystem->getSkybox();
            skyParams.timeOfDay = reflSkybox ? reflSkybox->getTimeOfDay() : 12.0f;
            if (lightingManager) {
                const auto& lp = lightingManager->getLightingParams();
                skyParams.directionalDir = lp.directionalDir;
                skyParams.sunColor = lp.diffuseColor;
                skyParams.skyTopColor = lp.skyTopColor;
                skyParams.skyMiddleColor = lp.skyMiddleColor;
                skyParams.skyBand1Color = lp.skyBand1Color;
                skyParams.skyBand2Color = lp.skyBand2Color;
                skyParams.cloudDensity = lp.cloudDensity;
                skyParams.fogDensity = lp.fogDensity;
                skyParams.horizonGlow = lp.horizonGlow;
            }
            // weatherIntensity left at default 0 for reflection pass (no game handler in scope)
            skySystem->render(currentCmd, reflDescSet, *camera, skyParams);
        }
        if (terrainRenderer && terrainEnabled) {
            terrainRenderer->render(currentCmd, reflDescSet, *camera);
        }
        if (wmoRenderer) {
            wmoRenderer->render(currentCmd, reflDescSet, *camera);
        }
    }

    waterRenderer->endReflectionPass(currentCmd);
}

void Renderer::renderShadowPass() {
    ZoneScopedN("Renderer::renderShadowPass");
    static const bool skipShadows = (std::getenv("WOWEE_SKIP_SHADOWS") != nullptr);
    if (skipShadows) return;
    if (!shadowsEnabled || shadowDepthImage[0] == VK_NULL_HANDLE) return;
    if (currentCmd == VK_NULL_HANDLE) return;

    // Shadows render every frame — throttling causes visible flicker on player/NPCs

    // lightSpaceMatrix was already computed at frame start (before updatePerFrameUBO).
    // Zero matrix means character position isn't set yet — skip shadow pass entirely.
    if (lightSpaceMatrix == glm::mat4(0.0f)) return;
    uint32_t frame = vkCtx->getCurrentFrame();

    // Barrier 1: transition this frame's shadow map into writable depth layout.
    VkImageMemoryBarrier b1{};
    b1.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.oldLayout = shadowDepthLayout_[frame];
    b1.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.srcAccessMask = (shadowDepthLayout_[frame] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_ACCESS_SHADER_READ_BIT
        : 0;
    b1.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b1.image = shadowDepthImage[frame];
    b1.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkPipelineStageFlags srcStage = (shadowDepthLayout_[frame] == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    vkCmdPipelineBarrier(currentCmd,
        srcStage, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b1);

    // Begin shadow render pass
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = shadowRenderPass;
    rpInfo.framebuffer = shadowFramebuffer[frame];
    rpInfo.renderArea = {{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0, 0, static_cast<float>(SHADOW_MAP_SIZE), static_cast<float>(SHADOW_MAP_SIZE), 0.0f, 1.0f};
    vkCmdSetViewport(currentCmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    vkCmdSetScissor(currentCmd, 0, 1, &sc);

    // Phase 7/8: render shadow casters
    const float shadowCullRadius = shadowDistance_ * 1.35f;
    if (terrainRenderer) {
        terrainRenderer->renderShadow(currentCmd, lightSpaceMatrix, shadowCenter, shadowCullRadius);
    }
    if (wmoRenderer) {
        wmoRenderer->renderShadow(currentCmd, lightSpaceMatrix, shadowCenter, shadowCullRadius);
    }
    if (m2Renderer) {
        m2Renderer->renderShadow(currentCmd, lightSpaceMatrix, globalTime, shadowCenter, shadowCullRadius);
    }
    if (characterRenderer) {
        characterRenderer->renderShadow(currentCmd, lightSpaceMatrix, shadowCenter, shadowCullRadius);
    }

    vkCmdEndRenderPass(currentCmd);

    // Barrier 2: DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier b2{};
    b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.image = shadowDepthImage[frame];
    b2.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(currentCmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b2);
    shadowDepthLayout_[frame] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Build the per-frame render graph for off-screen pre-passes.
// Declares passes as graph nodes with input/output dependencies.
// compile() performs topological sort; execute() runs them with auto barriers.
void Renderer::buildFrameGraph(game::GameHandler* gameHandler) {
    (void)gameHandler;
    if (!renderGraph_) return;

    renderGraph_->reset();

    auto shadowDepth = renderGraph_->findResource("shadow_depth");
    auto reflTex = renderGraph_->findResource("reflection_texture");
    auto cullVis = renderGraph_->findResource("cull_visibility");

    // Minimap composites (no dependencies — standalone off-screen render target)
    renderGraph_->addPass("minimap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (minimap && minimap->isEnabled() && camera) {
                glm::vec3 minimapCenter = camera->getPosition();
                if (cameraController && cameraController->isThirdPerson())
                    minimapCenter = characterPosition;
                minimap->compositePass(cmd, minimapCenter);
            }
        });

    // World map composite (standalone)
    renderGraph_->addPass("worldmap_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            if (worldMap) worldMap->compositePass(cmd);
        });

    // Character preview composites (standalone)
    renderGraph_->addPass("preview_composite", {}, {},
        [this](VkCommandBuffer cmd) {
            uint32_t frame = vkCtx->getCurrentFrame();
            for (auto* preview : activePreviews_) {
                if (preview && preview->isModelLoaded())
                    preview->compositePass(cmd, frame);
            }
        });

    // Shadow pre-pass → outputs shadow_depth
    renderGraph_->addPass("shadow_pass", {}, {shadowDepth},
        [this](VkCommandBuffer) {
            if (shadowsEnabled && shadowDepthImage[0] != VK_NULL_HANDLE)
                renderShadowPass();
        });
    renderGraph_->setPassEnabled("shadow_pass", shadowsEnabled && shadowDepthImage[0] != VK_NULL_HANDLE);

    // Reflection pre-pass → outputs reflection_texture (reads scene, so after shadow)
    renderGraph_->addPass("reflection_pass", {shadowDepth}, {reflTex},
        [this](VkCommandBuffer) {
            renderReflectionPass();
        });

    // GPU frustum cull compute → outputs cull_visibility
    renderGraph_->addPass("compute_cull", {}, {cullVis},
        [this](VkCommandBuffer cmd) {
            if (m2Renderer && camera)
                m2Renderer->dispatchCullCompute(cmd, vkCtx->getCurrentFrame(), *camera);
        });

    renderGraph_->compile();
}

} // namespace rendering
} // namespace wowee
