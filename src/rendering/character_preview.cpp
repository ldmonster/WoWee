#include "rendering/character_preview.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/animation_ids.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <unordered_set>
#include <cstring>

namespace wowee {
namespace rendering {

CharacterPreview::CharacterPreview() = default;

CharacterPreview::~CharacterPreview() {
    shutdown();
}

bool CharacterPreview::initialize(pipeline::AssetManager* am) {
    assetManager_ = am;

    // If already initialized with valid resources, reuse them.
    // This avoids destroying GPU resources that may still be referenced by
    // an in-flight command buffer (compositePass recorded earlier this frame).
    if (renderTarget_ && renderTarget_->isValid() && charRenderer_ && camera_) {
        // Mark model as not loaded — loadCharacter() will handle instance cleanup
        modelLoaded_ = false;
        return true;
    }

    auto* appRenderer = core::Application::getInstance().getRenderer();
    vkCtx_ = appRenderer ? appRenderer->getVkContext() : nullptr;
    VkDescriptorSetLayout perFrameLayout = appRenderer ? appRenderer->getPerFrameSetLayout() : VK_NULL_HANDLE;

    if (!vkCtx_ || perFrameLayout == VK_NULL_HANDLE) {
        LOG_ERROR("CharacterPreview: no VkContext or perFrameLayout available");
        return false;
    }

    // Create off-screen render target first (need its render pass for pipeline creation)
    createFBO();
    if (!renderTarget_ || !renderTarget_->isValid()) {
        LOG_ERROR("CharacterPreview: failed to create off-screen render target");
        return false;
    }

    // Initialize CharacterRenderer with our off-screen render pass
    charRenderer_ = std::make_unique<CharacterRenderer>();
    if (!charRenderer_->initialize(vkCtx_, perFrameLayout, am, renderTarget_->getRenderPass(),
                                   renderTarget_->getSampleCount())) {
        LOG_ERROR("CharacterPreview: failed to initialize CharacterRenderer");
        return false;
    }

    // Configure lighting for character preview
    // Use distant fog to avoid clipping, enable shadows for visual depth
    charRenderer_->setFog(glm::vec3(0.05f, 0.05f, 0.1f), 9999.0f, 10000.0f);

    camera_ = std::make_unique<Camera>();
    // Portrait-style camera: WoW Z-up coordinate system
    // Model at origin, camera positioned along +Y looking toward -Y
    camera_->setFov(30.0f);
    camera_->setAspectRatio(static_cast<float>(fboWidth_) / static_cast<float>(fboHeight_));
    // Pull camera back far enough to see full body + head with margin
    camera_->setPosition(glm::vec3(0.0f, 4.5f, 0.9f));
    camera_->setRotation(270.0f, 0.0f);

    LOG_INFO("CharacterPreview initialized (", fboWidth_, "x", fboHeight_, ")");
    return true;
}

void CharacterPreview::shutdown() {
    // Unregister from renderer before destroying resources
    auto* appRenderer = core::Application::getInstance().getRenderer();
    if (appRenderer) appRenderer->unregisterPreview(this);

    if (charRenderer_) {
        charRenderer_->shutdown();
        charRenderer_.reset();
    }
    camera_.reset();
    destroyFBO();
    modelLoaded_ = false;
    compositeRendered_ = false;
    instanceId_ = 0;
}

void CharacterPreview::createFBO() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    // 1. Create off-screen render target with depth
    renderTarget_ = std::make_unique<VkRenderTarget>();
    if (!renderTarget_->create(*vkCtx_, fboWidth_, fboHeight_, VK_FORMAT_R8G8B8A8_UNORM, true,
                               VK_SAMPLE_COUNT_4_BIT)) {
        LOG_ERROR("CharacterPreview: failed to create render target");
        renderTarget_.reset();
        return;
    }

    // 1b. Transition the color image from UNDEFINED to SHADER_READ_ONLY_OPTIMAL
    // so that ImGui::Image doesn't sample an image in UNDEFINED layout before
    // the first compositePass runs.
    {
        VkCommandBuffer cmd = vkCtx_->beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = renderTarget_->getColorImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        vkCtx_->endSingleTimeCommands(cmd);
    }

    // 2. Create 1x1 dummy depth texture (shadow map placeholder, depth=1.0 = no shadow).
    //    Must be a depth format for sampler2DShadow compatibility.
    {
        VkImageCreateInfo imgCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgCI.imageType = VK_IMAGE_TYPE_2D;
        imgCI.format = VK_FORMAT_D16_UNORM;
        imgCI.extent = {1, 1, 1};
        imgCI.mipLevels = 1;
        imgCI.arrayLayers = 1;
        imgCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocCI{};
        allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vkCtx_->getAllocator(), &imgCI, &allocCI,
                &dummyShadowImage_, &dummyShadowAlloc_, nullptr) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create dummy shadow image");
            return;
        }
        VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewCI.image = dummyShadowImage_;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format = VK_FORMAT_D16_UNORM;
        viewCI.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &viewCI, nullptr, &dummyShadowView_) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create dummy shadow image view");
            return;
        }
        // Clear to depth 1.0 and transition to shader-read layout
        vkCtx_->immediateSubmit([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.image = dummyShadowImage_;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
            VkClearDepthStencilValue clearVal{1.0f, 0};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, dummyShadowImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);
            VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toRead.image = dummyShadowImage_;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &toRead);
        });
        // Comparison sampler for sampler2DShadow
        VkSamplerCreateInfo sampCI{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampCI.magFilter = VK_FILTER_NEAREST;
        sampCI.minFilter = VK_FILTER_NEAREST;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampCI.compareEnable = VK_TRUE;
        sampCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        dummyShadowSampler_ = vkCtx_->getOrCreateSampler(sampCI);
    }

    // 3. Create descriptor pool for per-frame sets (2 UBO + 2 sampler)
    {
        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = MAX_FRAMES;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = MAX_FRAMES;

        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_FRAMES;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &ci, nullptr, &previewDescPool_) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create descriptor pool");
            return;
        }
    }

    // 4. Create per-frame UBOs and descriptor sets
    auto* appRenderer = core::Application::getInstance().getRenderer();
    VkDescriptorSetLayout perFrameLayout = appRenderer->getPerFrameSetLayout();

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        // Create mapped UBO
        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = sizeof(GPUPerFrameData);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo mapInfo{};
        if (vmaCreateBuffer(allocator, &bufInfo, &allocInfo,
                &previewUBO_[i], &previewUBOAlloc_[i], &mapInfo) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to create UBO ", i);
            return;
        }
        previewUBOMapped_[i] = mapInfo.pMappedData;

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo setAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = previewDescPool_;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameLayout;
        if (vkAllocateDescriptorSets(device, &setAlloc, &previewPerFrameSet_[i]) != VK_SUCCESS) {
            LOG_ERROR("CharacterPreview: failed to allocate descriptor set ", i);
            return;
        }

        // Write UBO binding (0) and shadow sampler binding (1) using dummy white texture
        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = previewUBO_[i];
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImg{};
        shadowImg.sampler = dummyShadowSampler_;
        shadowImg.imageView = dummyShadowView_;
        shadowImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = previewPerFrameSet_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = previewPerFrameSet_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImg;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // 5. Register the color attachment as an ImGui texture
    imguiTextureId_ = ImGui_ImplVulkan_AddTexture(
        renderTarget_->getSampler(),
        renderTarget_->getColorImageView(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    LOG_INFO("CharacterPreview: off-screen FBO created (", fboWidth_, "x", fboHeight_, ")");
}

void CharacterPreview::destroyFBO() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();

    if (imguiTextureId_) {
        ImGui_ImplVulkan_RemoveTexture(imguiTextureId_);
        imguiTextureId_ = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        if (previewUBO_[i]) {
            vmaDestroyBuffer(allocator, previewUBO_[i], previewUBOAlloc_[i]);
            previewUBO_[i] = VK_NULL_HANDLE;
        }
    }

    if (previewDescPool_) {
        vkDestroyDescriptorPool(device, previewDescPool_, nullptr);
        previewDescPool_ = VK_NULL_HANDLE;
    }

    // dummyShadowSampler_ is owned by VkContext sampler cache — do NOT destroy
    if (dummyShadowView_) { vkDestroyImageView(device, dummyShadowView_, nullptr); dummyShadowView_ = VK_NULL_HANDLE; }
    if (dummyShadowImage_) { vmaDestroyImage(allocator, dummyShadowImage_, dummyShadowAlloc_); dummyShadowImage_ = VK_NULL_HANDLE; dummyShadowAlloc_ = VK_NULL_HANDLE; }

    if (renderTarget_) {
        renderTarget_->destroy(device, allocator);
        renderTarget_.reset();
    }
}

bool CharacterPreview::loadCharacter(game::Race race, game::Gender gender,
                                      uint8_t skin, uint8_t face,
                                      uint8_t hairStyle, uint8_t hairColor,
                                      uint8_t facialHair, bool useFemaleModel) {
    if (!charRenderer_ || !assetManager_ || !assetManager_->isInitialized()) {
        return false;
    }

    // Remove existing instance.
    // Must wait for GPU to finish — compositePass() may have recorded draw commands
    // referencing this instance's bone buffers earlier in the current frame.
    if (instanceId_ > 0) {
        if (vkCtx_) vkDeviceWaitIdle(vkCtx_->getDevice());
        charRenderer_->removeInstance(instanceId_);
        instanceId_ = 0;
        modelLoaded_ = false;
    }

    std::string m2Path = game::getPlayerModelPath(race, gender, useFemaleModel);
    std::string modelDir;
    std::string baseName;
    {
        size_t slash = m2Path.rfind('\\');
        if (slash != std::string::npos) {
            modelDir = m2Path.substr(0, slash + 1);
            baseName = m2Path.substr(slash + 1);
        } else {
            baseName = m2Path;
        }
        size_t dot = baseName.rfind('.');
        if (dot != std::string::npos) {
            baseName = baseName.substr(0, dot);
        }
    }

    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) {
        LOG_WARNING("CharacterPreview: failed to read M2: ", m2Path);
        return false;
    }

    auto model = pipeline::M2Loader::load(m2Data);

    // M2 version 264+ (WotLK) stores submesh/bone data in external .skin files.
    // Earlier versions (Classic ≤256, TBC ≤263) have skin data embedded in the M2.
    std::string skinPath = modelDir + baseName + "00.skin";
    auto skinData = assetManager_->readFile(skinPath);
    if (!skinData.empty() && model.version >= 264) {
        pipeline::M2Loader::loadSkin(skinData, model);
    }

    if (!model.isValid()) {
        LOG_WARNING("CharacterPreview: invalid model: ", m2Path);
        return false;
    }

    // Look up CharSections.dbc for all appearance textures
    uint32_t targetRaceId = static_cast<uint32_t>(race);
    uint32_t targetSexId = (gender == game::Gender::FEMALE) ? 1u : 0u;

    std::string faceLowerPath;
    std::string faceUpperPath;
    std::string hairScalpPath;
    std::vector<std::string> underwearPaths;
    bodySkinPath_.clear();
    baseLayers_.clear();

    auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc");
    if (charSectionsDbc) {
        bool foundSkin = false;
        bool foundFace = false;
        bool foundHair = false;
        bool foundUnderwear = false;

        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);

        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
            uint32_t raceId = charSectionsDbc->getUInt32(r, csF.raceId);
            uint32_t sexId = charSectionsDbc->getUInt32(r, csF.sexId);
            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

            if (raceId != targetRaceId || sexId != targetSexId) continue;

            // Section 0: Body skin (variation=0, colorIndex = skin color)
            if (baseSection == 0 && !foundSkin &&
                variationIndex == 0 && colorIndex == static_cast<uint32_t>(skin)) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) {
                    bodySkinPath_ = tex1;
                    foundSkin = true;
                }
            }
            // Section 1: Face (variation = face index, colorIndex = skin color)
            else if (baseSection == 1 && !foundFace &&
                     variationIndex == static_cast<uint32_t>(face) &&
                     colorIndex == static_cast<uint32_t>(skin)) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) faceLowerPath = tex1;
                if (!tex2.empty()) faceUpperPath = tex2;
                foundFace = true;
            }
            // Section 3: Hair (variation = hair style, colorIndex = hair color)
            else if (baseSection == 3 && !foundHair &&
                     variationIndex == static_cast<uint32_t>(hairStyle) &&
                     colorIndex == static_cast<uint32_t>(hairColor)) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) {
                    hairScalpPath = tex1;
                    foundHair = true;
                }
            }
            // Section 4: Underwear (variation=0, colorIndex = skin color)
            else if (baseSection == 4 && !foundUnderwear &&
                     variationIndex == 0 && colorIndex == static_cast<uint32_t>(skin)) {
                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                    std::string tex = charSectionsDbc->getString(r, f);
                    if (!tex.empty()) {
                        underwearPaths.push_back(tex);
                    }
                }
                foundUnderwear = true;
            }
        }

        LOG_INFO("CharSections lookup: skin=", foundSkin ? bodySkinPath_ : "(not found)",
                 " face=", foundFace ? (faceLowerPath.empty() ? "(empty)" : faceLowerPath) : "(not found)",
                 " hair=", foundHair ? (hairScalpPath.empty() ? "(empty)" : hairScalpPath) : "(not found)",
                 " underwear=", foundUnderwear, " (", underwearPaths.size(), " textures)");
    } else {
        LOG_WARNING("CharSections.dbc not loaded — no character textures");
    }

    // Assign texture filenames on model before GPU upload
    for (size_t ti = 0; ti < model.textures.size(); ti++) {
        auto& tex = model.textures[ti];
        LOG_INFO("  Model texture[", ti, "]: type=", tex.type,
                 " filename='", tex.filename, "'");
        // M2 texture types: 1=character skin, 6=hair/scalp. Empty filename means
        // the texture is resolved at runtime via CharSections.dbc lookup.
        if (tex.type == 1 && tex.filename.empty() && !bodySkinPath_.empty()) {
            tex.filename = bodySkinPath_;
        } else if (tex.type == 6 && tex.filename.empty() && !hairScalpPath.empty()) {
            tex.filename = hairScalpPath;
        }
    }

    // Load external .anim files for sequences that store keyframes outside the M2.
    // Flag 0x20 = embedded data; when clear, animation lives in {ModelName}{SeqID}-{Var}.anim
    for (uint32_t si = 0; si < model.sequences.size(); si++) {
        if (!(model.sequences[si].flags & 0x20)) {
            char animFileName[256];
            snprintf(animFileName, sizeof(animFileName),
                "%s%s%04u-%02u.anim",
                modelDir.c_str(),
                baseName.c_str(),
                model.sequences[si].id,
                model.sequences[si].variationIndex);
            auto animFileData = assetManager_->readFileOptional(animFileName);
            if (!animFileData.empty()) {
                pipeline::M2Loader::loadAnimFile(m2Data, animFileData, si, model);
            }
        }
    }

    if (!charRenderer_->loadModel(model, PREVIEW_MODEL_ID)) {
        LOG_WARNING("CharacterPreview: failed to load model to GPU");
        return false;
    }
    // Composite body skin + face + underwear overlays
    if (!bodySkinPath_.empty()) {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath_);
        // Face lower texture composited onto body at the face region
        if (!faceLowerPath.empty()) {
            layers.push_back(faceLowerPath);
        }
        if (!faceUpperPath.empty()) {
            layers.push_back(faceUpperPath);
        }
        for (const auto& up : underwearPaths) {
            layers.push_back(up);
        }

        // Cache for later equipment compositing.
        // Keep baseLayers_ without the base skin (compositeWithRegions takes basePath separately).
        if (!faceLowerPath.empty()) baseLayers_.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) baseLayers_.push_back(faceUpperPath);
        for (const auto& up : underwearPaths) baseLayers_.push_back(up);

        if (layers.size() > 1) {
            VkTexture* compositeTex = charRenderer_->compositeTextures(layers);
            if (compositeTex != nullptr) {
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    if (model.textures[ti].type == 1) {
                        charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), compositeTex);
                        break;
                    }
                }
            }
        } else {
            // Single layer (body skin only, no face/underwear overlays) — load directly
            VkTexture* skinTex = charRenderer_->loadTexture(bodySkinPath_);
            if (skinTex != nullptr) {
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    if (model.textures[ti].type == 1) {
                        charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), skinTex);
                        break;
                    }
                }
            }
        }
    }

    // If hair scalp texture was found, ensure it's loaded for type-6 slot
    if (!hairScalpPath.empty()) {
        VkTexture* hairTex = charRenderer_->loadTexture(hairScalpPath);
        if (hairTex != nullptr) {
            for (size_t ti = 0; ti < model.textures.size(); ti++) {
                if (model.textures[ti].type == 6) {
                    charRenderer_->setModelTexture(PREVIEW_MODEL_ID, static_cast<uint32_t>(ti), hairTex);
                    break;
                }
            }
        }
    }

    // Create instance at origin with current yaw
    instanceId_ = charRenderer_->createInstance(PREVIEW_MODEL_ID,
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, modelYaw_),
        1.0f);

    if (instanceId_ == 0) {
        LOG_WARNING("CharacterPreview: failed to create instance");
        return false;
    }

    // Set default geosets (naked character)
    std::unordered_set<uint16_t> activeGeosets;
    // Body parts (group 0: IDs 0-99, vanilla models use up to 27)
    for (uint16_t i = 0; i <= 99; i++) {
        activeGeosets.insert(i);
    }
    // Hair style geoset: group 1 = 100 + variation + 1
    activeGeosets.insert(static_cast<uint16_t>(100 + hairStyle + 1));
    // Facial hair geoset: group 2 = 200 + variation + 1
    activeGeosets.insert(static_cast<uint16_t>(200 + facialHair + 1));
    activeGeosets.insert(401);   // Bare forearms (no gloves) — group 4
    activeGeosets.insert(502);   // Bare shins (no boots) — group 5
    activeGeosets.insert(702);   // Ears: default
    activeGeosets.insert(801);   // Bare wrists (no sleeves) — group 8
    activeGeosets.insert(902);   // Kneepads: default — group 9
    activeGeosets.insert(1301);  // Bare legs (no pants) — group 13
    activeGeosets.insert(1502);  // No cloak — group 15
    activeGeosets.insert(2002);  // Bare feet mesh — group 20
    charRenderer_->setActiveGeosets(instanceId_, activeGeosets);

    // Play idle animation (Stand = animation ID 0)
    charRenderer_->playAnimation(instanceId_, rendering::anim::STAND, true);

    // Cache core appearance for later equipment geosets.
    race_ = race;
    gender_ = gender;
    useFemaleModel_ = useFemaleModel;
    hairStyle_ = hairStyle;
    facialHair_ = facialHair;

    // Cache the type-1 texture slot index so applyEquipment can update it.
    skinTextureSlotIndex_ = 0;
    for (size_t ti = 0; ti < model.textures.size(); ti++) {
        if (model.textures[ti].type == 1) {
            skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
            break;
        }
    }

    modelLoaded_ = true;
    LOG_INFO("CharacterPreview: loaded ", m2Path,
             " skin=", static_cast<int>(skin), " face=", static_cast<int>(face),
             " hair=", static_cast<int>(hairStyle), " hairColor=", static_cast<int>(hairColor),
             " facial=", static_cast<int>(facialHair));
    return true;
}

bool CharacterPreview::applyEquipment(const std::vector<game::EquipmentItem>& equipment) {
    if (!modelLoaded_ || instanceId_ == 0 || !charRenderer_ || !assetManager_ || !assetManager_->isInitialized()) {
        return false;
    }

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc || !displayInfoDbc->isLoaded()) {
        LOG_WARNING("applyEquipment: ItemDisplayInfo.dbc not loaded");
        return false;
    }

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (const auto& it : equipment) {
            if (it.displayModel == 0) continue;
            for (uint8_t t : types) {
                if (it.inventoryType == t) return true;
            }
        }
        return false;
    };

    auto findDisplayId = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (const auto& it : equipment) {
            if (it.displayModel == 0) continue;
            for (uint8_t t : types) {
                if (it.inventoryType == t) return it.displayModel;
            }
        }
        return 0;
    };

    auto getGeosetGroup = [&](uint32_t displayInfoId, int groupField) -> uint32_t {
        if (displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        uint32_t val = displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), 7 + groupField);
        return val;
    };

    // --- Geosets ---
    // M2 geoset IDs encode body part group × 100 + variant (e.g., 801 = group 8
    // (sleeves) variant 1, 1301 = group 13 (pants) variant 1). ItemDisplayInfo.dbc
    // provides the variant offset per equipped item; base IDs are per-group constants.
    std::unordered_set<uint16_t> geosets;
    for (uint16_t i = 0; i <= 99; i++) geosets.insert(i);
    geosets.insert(static_cast<uint16_t>(100 + hairStyle_ + 1));    // Hair style
    geosets.insert(static_cast<uint16_t>(200 + facialHair_ + 1));  // Facial hair
    geosets.insert(701);   // Ears
    geosets.insert(902);   // Kneepads: default (group 9)
    geosets.insert(2002);  // Bare feet mesh (group 20 = CG_FEET)

    // CharGeosets: group 4=gloves(forearm), 5=boots(shin), 8=sleeves, 13=pants
    uint16_t geosetGloves = 401;   // Bare forearms (group 4)
    uint16_t geosetBoots = 502;    // Bare shins (group 5)
    uint16_t geosetSleeves = 801;  // Bare wrists (group 8)
    uint16_t geosetPants = 1301;   // Bare legs (group 13)

    // Chest/Shirt/Robe → group 8 (sleeves)
    {
        uint32_t did = findDisplayId({4, 5, 20});
        uint32_t gg = getGeosetGroup(did, 0);
        if (gg > 0) geosetSleeves = static_cast<uint16_t>(801 + gg);
        // Robe kilt legs
        uint32_t gg3 = getGeosetGroup(did, 2);
        if (gg3 > 0) geosetPants = static_cast<uint16_t>(1301 + gg3);
    }
    // Legs → group 13 (trousers)
    {
        uint32_t did = findDisplayId({7});
        uint32_t gg = getGeosetGroup(did, 0);
        if (gg > 0) geosetPants = static_cast<uint16_t>(1301 + gg);
    }
    // Boots → group 5 (shins)
    {
        uint32_t did = findDisplayId({8});
        uint32_t gg = getGeosetGroup(did, 0);
        if (gg > 0) geosetBoots = static_cast<uint16_t>(501 + gg);
    }
    // Gloves → group 4 (forearms)
    {
        uint32_t did = findDisplayId({10});
        uint32_t gg = getGeosetGroup(did, 0);
        if (gg > 0) geosetGloves = static_cast<uint16_t>(401 + gg);
    }
    // Wrists/Bracers → group 8 (sleeves, only if chest/shirt didn't set it)
    {
        uint32_t did = findDisplayId({9});
        if (did != 0 && geosetSleeves == 801) {
            uint32_t gg = getGeosetGroup(did, 0);
            if (gg > 0) geosetSleeves = static_cast<uint16_t>(801 + gg);
        }
    }
    // Belt → group 18 (buckle)
    uint16_t geosetBelt = 0;
    {
        uint32_t did = findDisplayId({6});
        uint32_t gg = getGeosetGroup(did, 0);
        if (gg > 0) geosetBelt = static_cast<uint16_t>(1801 + gg);
    }

    geosets.insert(geosetGloves);
    geosets.insert(geosetBoots);
    geosets.insert(geosetSleeves);
    geosets.insert(geosetPants);
    if (geosetBelt != 0) geosets.insert(geosetBelt);
    geosets.insert(hasInvType({16}) ? 1502 : 1501); // Cloak mesh toggle (visual may still be limited)
    if (hasInvType({19})) geosets.insert(1201);     // Tabard mesh toggle

    // Hide hair under helmets (helmets are separate models; this still avoids hair clipping)
    if (hasInvType({1})) {
        geosets.erase(static_cast<uint16_t>(100 + hairStyle_ + 1));
        geosets.insert(1);   // Bald scalp cap
        geosets.insert(101); // Default group-1 connector
    }

    charRenderer_->setActiveGeosets(instanceId_, geosets);

    // --- Textures (equipment overlays onto body skin) ---
    if (bodySkinPath_.empty()) return true; // geosets applied, but can't composite

    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
        "TorsoUpperTexture", "TorsoLowerTexture",
        "LegUpperTexture", "LegLowerTexture", "FootTexture",
    };

    // Texture component region fields — use DBC layout when available, fall back to binary offsets.
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    std::vector<std::pair<int, std::string>> regionLayers;
    regionLayers.reserve(32);

    for (const auto& it : equipment) {
        if (it.displayModel == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(it.displayModel);
        if (recIdx < 0) continue;

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            std::string base = "Item\\TextureComponents\\" +
                std::string(componentDirs[region]) + "\\" + texName;

            std::string genderSuffix = (gender_ == game::Gender::FEMALE) ? "_F.blp" : "_M.blp";
            std::string genderPath = base + genderSuffix;
            std::string unisexPath = base + "_U.blp";
            std::string fullPath;
            std::string basePath = base + ".blp";
            if (assetManager_->fileExists(genderPath)) {
                fullPath = genderPath;
            } else if (assetManager_->fileExists(unisexPath)) {
                fullPath = unisexPath;
            } else if (assetManager_->fileExists(basePath)) {
                fullPath = basePath;
            } else {
                continue;
            }
            regionLayers.emplace_back(region, fullPath);
        }
    }

    if (!regionLayers.empty()) {
        VkTexture* newTex = charRenderer_->compositeWithRegions(bodySkinPath_, baseLayers_, regionLayers);
        if (newTex != nullptr) {
            charRenderer_->setModelTexture(PREVIEW_MODEL_ID, skinTextureSlotIndex_, newTex);
        }
    }

    // Cloak texture (group 15) is separate from body compositing.
    if (hasInvType({16})) {
        uint32_t capeDisplayId = findDisplayId({16});
        if (capeDisplayId != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDisplayId);
            if (capeRecIdx >= 0) {
                std::vector<std::string> capeNames;
                auto addName = [&](const std::string& n) {
                    if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                        capeNames.push_back(n);
                    }
                };
                std::string leftName = displayInfoDbc->getString(static_cast<uint32_t>(capeRecIdx), 3);
                std::string rightName = displayInfoDbc->getString(static_cast<uint32_t>(capeRecIdx), 4);
                if (gender_ == game::Gender::FEMALE) {
                    addName(rightName);
                    addName(leftName);
                } else {
                    addName(leftName);
                    addName(rightName);
                }

                auto hasBlpExt = [](const std::string& p) {
                    if (p.size() < 4) return false;
                    std::string ext = p.substr(p.size() - 4);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return ext == ".blp";
                };
                std::vector<std::string> candidates;
                auto addCandidate = [&](const std::string& p) {
                    if (!p.empty() && std::find(candidates.begin(), candidates.end(), p) == candidates.end()) {
                        candidates.push_back(p);
                    }
                };
                for (const auto& nameRaw : capeNames) {
                    std::string name = nameRaw;
                    std::replace(name.begin(), name.end(), '/', '\\');
                    bool hasDir = (name.find('\\') != std::string::npos);
                    bool hasExt = hasBlpExt(name);
                    if (hasDir) {
                        addCandidate(name);
                        if (!hasExt) addCandidate(name + ".blp");
                    } else {
                        std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name;
                        std::string baseTex = "Item\\TextureComponents\\Cape\\" + name;
                        addCandidate(baseObj);
                        addCandidate(baseTex);
                        if (!hasExt) {
                            addCandidate(baseObj + ".blp");
                            addCandidate(baseTex + ".blp");
                        }
                        addCandidate(baseObj + (gender_ == game::Gender::FEMALE ? "_F.blp" : "_M.blp"));
                        addCandidate(baseObj + "_U.blp");
                        addCandidate(baseTex + (gender_ == game::Gender::FEMALE ? "_F.blp" : "_M.blp"));
                        addCandidate(baseTex + "_U.blp");
                    }
                }
                VkTexture* whiteTex = charRenderer_->loadTexture("");
                for (const auto& c : candidates) {
                    VkTexture* capeTex = charRenderer_->loadTexture(c);
                    if (capeTex != nullptr && capeTex != whiteTex) {
                        charRenderer_->setGroupTextureOverride(instanceId_, 15, capeTex);
                        if (const auto* md = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer_->setTextureSlotOverride(instanceId_, static_cast<uint16_t>(ti), capeTex);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
    } else {
        if (const auto* md = charRenderer_->getModelData(PREVIEW_MODEL_ID)) {
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                if (md->textures[ti].type == 2) {
                    charRenderer_->clearTextureSlotOverride(instanceId_, static_cast<uint16_t>(ti));
                }
            }
        }
    }

    return true;
}

void CharacterPreview::update(float deltaTime) {
    if (charRenderer_ && modelLoaded_) {
        charRenderer_->update(deltaTime);
    }
}

void CharacterPreview::render() {
    // No-op — actual rendering happens in compositePass() called from Renderer::beginFrame()
}

void CharacterPreview::compositePass(VkCommandBuffer cmd, uint32_t frameIndex) {
    // Only composite when a UI screen actually requested it this frame
    if (!compositeRequested_) return;
    compositeRequested_ = false;

    if (!charRenderer_ || !camera_ || !modelLoaded_ || !renderTarget_ || !renderTarget_->isValid()) {
        return;
    }

    uint32_t fi = frameIndex % MAX_FRAMES;

    // Update per-frame UBO with preview camera matrices and studio lighting
    GPUPerFrameData ubo{};
    ubo.view = camera_->getViewMatrix();
    ubo.projection = camera_->getProjectionMatrix();
    ubo.lightSpaceMatrix = glm::mat4(1.0f);
    // Studio lighting: key light from upper-right-front
    ubo.lightDir = glm::vec4(glm::normalize(glm::vec3(0.5f, -0.7f, 0.5f)), 0.0f);
    ubo.lightColor = glm::vec4(1.0f, 0.95f, 0.9f, 0.0f);
    ubo.ambientColor = glm::vec4(0.35f, 0.35f, 0.4f, 0.0f);
    ubo.viewPos = glm::vec4(camera_->getPosition(), 0.0f);
    // No fog in preview
    ubo.fogColor = glm::vec4(0.05f, 0.05f, 0.1f, 0.0f);
    ubo.fogParams = glm::vec4(9999.0f, 10000.0f, 0.0f, 0.0f);
    // Enable shadows for visual depth in preview (strength=0.5 for subtle effect)
    ubo.shadowParams = glm::vec4(1.0f, 0.5f, 0.0f, 0.0f);

    std::memcpy(previewUBOMapped_[fi], &ubo, sizeof(GPUPerFrameData));

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{0.05f, 0.05f, 0.1f, 1.0f}};
    renderTarget_->beginPass(cmd, clearColor);

    // Render the character model
    charRenderer_->render(cmd, previewPerFrameSet_[fi], *camera_);

    renderTarget_->endPass(cmd);

    compositeRendered_ = true;
}

void CharacterPreview::rotate(float yawDelta) {
    modelYaw_ += yawDelta;
    if (instanceId_ > 0 && charRenderer_) {
        charRenderer_->setInstanceRotation(instanceId_, glm::vec3(0.0f, 0.0f, modelYaw_));
    }
}

} // namespace rendering
} // namespace wowee
