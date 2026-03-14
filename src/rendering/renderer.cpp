#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/scene.hpp"
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
#include "rendering/shader.hpp"
#include "game/game_handler.hpp"
#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/application.hpp"
#include "core/window.hpp"
#include "core/logger.hpp"
#include "game/world.hpp"
#include "game/zone_manager.hpp"
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
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cctype>
#include <cmath>
#include <chrono>
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

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;  // EmotesText.dbc record ID (for CMSG_TEXT_EMOTE)
    bool loop = false;
    std::string textNoTarget;       // sender sees, no target: "You dance."
    std::string textTarget;         // sender sees, with target: "You dance with %s."
    std::string othersNoTarget;     // others see, no target: "%s dances."
    std::string othersTarget;       // others see, with target: "%s dances with %s."
    std::string command;
};

static std::unordered_map<std::string, EmoteInfo> EMOTE_TABLE;
static std::unordered_map<uint32_t, const EmoteInfo*> EMOTE_BY_DBCID; // reverse lookup: dbcId → EmoteInfo*
static bool emoteTableLoaded = false;

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



static std::vector<std::string> parseEmoteCommands(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool isLoopingEmote(const std::string& command) {
    static const std::unordered_set<std::string> kLooping = {
        "dance",
        "train",
    };
    return kLooping.find(command) != kLooping.end();
}

static void loadFallbackEmotes() {
    if (!EMOTE_TABLE.empty()) return;
    EMOTE_TABLE = {
        {"wave",    {67,  0, false, "You wave.", "You wave at %s.", "%s waves.", "%s waves at %s.", "wave"}},
        {"bow",     {66,  0, false, "You bow down graciously.", "You bow down before %s.", "%s bows down graciously.", "%s bows down before %s.", "bow"}},
        {"laugh",   {70,  0, false, "You laugh.", "You laugh at %s.", "%s laughs.", "%s laughs at %s.", "laugh"}},
        {"point",   {84,  0, false, "You point over yonder.", "You point at %s.", "%s points over yonder.", "%s points at %s.", "point"}},
        {"cheer",   {68,  0, false, "You cheer!", "You cheer at %s.", "%s cheers!", "%s cheers at %s.", "cheer"}},
        {"dance",   {69,  0, true,  "You burst into dance.", "You dance with %s.", "%s bursts into dance.", "%s dances with %s.", "dance"}},
        {"kneel",   {75,  0, false, "You kneel down.", "You kneel before %s.", "%s kneels down.", "%s kneels before %s.", "kneel"}},
        {"applaud", {80,  0, false, "You applaud. Bravo!", "You applaud at %s. Bravo!", "%s applauds. Bravo!", "%s applauds at %s. Bravo!", "applaud"}},
        {"shout",   {81,  0, false, "You shout.", "You shout at %s.", "%s shouts.", "%s shouts at %s.", "shout"}},
        {"chicken", {78,  0, false, "With arms flapping, you strut around. Cluck, Cluck, Chicken!",
                     "With arms flapping, you strut around %s. Cluck, Cluck, Chicken!",
                     "%s struts around. Cluck, Cluck, Chicken!", "%s struts around %s. Cluck, Cluck, Chicken!", "chicken"}},
        {"cry",     {77,  0, false, "You cry.", "You cry on %s's shoulder.", "%s cries.", "%s cries on %s's shoulder.", "cry"}},
        {"kiss",    {76,  0, false, "You blow a kiss into the wind.", "You blow a kiss to %s.", "%s blows a kiss into the wind.", "%s blows a kiss to %s.", "kiss"}},
        {"roar",    {74,  0, false, "You roar with bestial vigor. So fierce!", "You roar with bestial vigor at %s. So fierce!", "%s roars with bestial vigor. So fierce!", "%s roars with bestial vigor at %s. So fierce!", "roar"}},
        {"salute",  {113, 0, false, "You salute.", "You salute %s with respect.", "%s salutes.", "%s salutes %s with respect.", "salute"}},
        {"rude",    {73,  0, false, "You make a rude gesture.", "You make a rude gesture at %s.", "%s makes a rude gesture.", "%s makes a rude gesture at %s.", "rude"}},
        {"flex",    {82,  0, false, "You flex your muscles. Oooooh so strong!", "You flex at %s. Oooooh so strong!", "%s flexes. Oooooh so strong!", "%s flexes at %s. Oooooh so strong!", "flex"}},
        {"shy",     {83,  0, false, "You smile shyly.", "You smile shyly at %s.", "%s smiles shyly.", "%s smiles shyly at %s.", "shy"}},
        {"beg",     {79,  0, false, "You beg everyone around you. How pathetic.", "You beg %s. How pathetic.", "%s begs everyone around. How pathetic.", "%s begs %s. How pathetic.", "beg"}},
        {"eat",     {61,  0, false, "You begin to eat.", "You begin to eat in front of %s.", "%s begins to eat.", "%s begins to eat in front of %s.", "eat"}},
    };
}

static std::string replacePlaceholders(const std::string& text, const std::string* targetName) {
    if (text.empty()) return text;
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
            if (targetName && !targetName->empty()) out += *targetName;
            i++;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

static void loadEmotesFromDbc() {
    if (emoteTableLoaded) return;
    emoteTableLoaded = true;

    auto* assetManager = core::Application::getInstance().getAssetManager();
    if (!assetManager) {
        LOG_WARNING("Emotes: no AssetManager");
        loadFallbackEmotes();
        return;
    }

    auto emotesTextDbc = assetManager->loadDBC("EmotesText.dbc");
    auto emotesTextDataDbc = assetManager->loadDBC("EmotesTextData.dbc");
    if (!emotesTextDbc || !emotesTextDataDbc || !emotesTextDbc->isLoaded() || !emotesTextDataDbc->isLoaded()) {
        LOG_WARNING("Emotes: DBCs not available (EmotesText/EmotesTextData)");
        loadFallbackEmotes();
        return;
    }

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* etdL = activeLayout ? activeLayout->getLayout("EmotesTextData") : nullptr;
    const auto* emL  = activeLayout ? activeLayout->getLayout("Emotes") : nullptr;
    const auto* etL  = activeLayout ? activeLayout->getLayout("EmotesText") : nullptr;

    std::unordered_map<uint32_t, std::string> textData;
    textData.reserve(emotesTextDataDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDataDbc->getRecordCount(); ++r) {
        uint32_t id = emotesTextDataDbc->getUInt32(r, etdL ? (*etdL)["ID"] : 0);
        std::string text = emotesTextDataDbc->getString(r, etdL ? (*etdL)["Text"] : 1);
        if (!text.empty()) textData.emplace(id, std::move(text));
    }

    std::unordered_map<uint32_t, uint32_t> emoteIdToAnim;
    if (auto emotesDbc = assetManager->loadDBC("Emotes.dbc"); emotesDbc && emotesDbc->isLoaded()) {
        emoteIdToAnim.reserve(emotesDbc->getRecordCount());
        for (uint32_t r = 0; r < emotesDbc->getRecordCount(); ++r) {
            uint32_t emoteId = emotesDbc->getUInt32(r, emL ? (*emL)["ID"] : 0);
            uint32_t animId = emotesDbc->getUInt32(r, emL ? (*emL)["AnimID"] : 2);
            if (animId != 0) emoteIdToAnim[emoteId] = animId;
        }
    }

    EMOTE_TABLE.clear();
    EMOTE_TABLE.reserve(emotesTextDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDbc->getRecordCount(); ++r) {
        uint32_t recordId = emotesTextDbc->getUInt32(r, etL ? (*etL)["ID"] : 0);
        std::string cmdRaw = emotesTextDbc->getString(r, etL ? (*etL)["Command"] : 1);
        if (cmdRaw.empty()) continue;

        uint32_t emoteRef = emotesTextDbc->getUInt32(r, etL ? (*etL)["EmoteRef"] : 2);
        uint32_t animId = 0;
        auto animIt = emoteIdToAnim.find(emoteRef);
        if (animIt != emoteIdToAnim.end()) {
            animId = animIt->second;
        } else {
            animId = emoteRef;  // fallback if EmotesText stores animation id directly
        }

        uint32_t senderTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderTargetTextID"] : 5);
        uint32_t senderNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderNoTargetTextID"] : 9);
        uint32_t othersTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersTargetTextID"] : 3);
        uint32_t othersNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersNoTargetTextID"] : 7);

        std::string textTarget, textNoTarget, oTarget, oNoTarget;
        if (auto it = textData.find(senderTargetTextId); it != textData.end()) textTarget = it->second;
        if (auto it = textData.find(senderNoTargetTextId); it != textData.end()) textNoTarget = it->second;
        if (auto it = textData.find(othersTargetTextId); it != textData.end()) oTarget = it->second;
        if (auto it = textData.find(othersNoTargetTextId); it != textData.end()) oNoTarget = it->second;

        for (const std::string& cmd : parseEmoteCommands(cmdRaw)) {
            if (cmd.empty()) continue;
            EmoteInfo info;
            info.animId = animId;
            info.dbcId = recordId;
            info.loop = isLoopingEmote(cmd);
            info.textNoTarget = textNoTarget;
            info.textTarget = textTarget;
            info.othersNoTarget = oNoTarget;
            info.othersTarget = oTarget;
            info.command = cmd;
            EMOTE_TABLE.emplace(cmd, std::move(info));
        }
    }

    if (EMOTE_TABLE.empty()) {
        LOG_WARNING("Emotes: DBC loaded but no commands parsed, using fallback list");
        loadFallbackEmotes();
    } else {
        LOG_INFO("Emotes: loaded ", EMOTE_TABLE.size(), " commands from DBC");
    }

    // Build reverse lookup by dbcId (only first command per emote needed)
    EMOTE_BY_DBCID.clear();
    for (auto& [cmd, info] : EMOTE_TABLE) {
        if (info.dbcId != 0) {
            EMOTE_BY_DBCID.emplace(info.dbcId, &info);
        }
    }
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
    if (vkCreateSampler(device, &sampCI, nullptr, &shadowSampler) != VK_SUCCESS) {
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
    poolSizes[0].descriptorCount = MAX_FRAMES + 1; // +1 for reflection perFrame UBO
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = MAX_FRAMES + 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_FRAMES + 1; // +1 for reflection descriptor set
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

        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = sceneDescriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &perFrameSetLayout;

        if (vkAllocateDescriptorSets(device, &setAlloc, &reflPerFrameDescSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate reflection per-frame descriptor set");
            return false;
        }

        VkDescriptorBufferInfo descBuf{};
        descBuf.buffer = reflPerFrameUBO;
        descBuf.offset = 0;
        descBuf.range = sizeof(GPUPerFrameData);

        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler = shadowSampler;
        shadowImgInfo.imageView = shadowDepthView[0];  // reflection uses frame 0 shadow view
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = reflPerFrameDescSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &descBuf;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = reflPerFrameDescSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].pImageInfo = &shadowImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
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
    if (shadowSampler) { vkDestroySampler(device, shadowSampler, nullptr); shadowSampler = VK_NULL_HANDLE; }
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
    currentFrameData.shadowParams = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, 0.8f, 0.0f, 0.0f);

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

    // Create scene
    scene = std::make_unique<Scene>();

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

    // Initialize AudioEngine (singleton)
    if (!audio::AudioEngine::instance().initialize()) {
        LOG_WARNING("Failed to initialize AudioEngine - audio will be disabled");
    }

    // Create music manager (initialized later with asset manager)
    musicManager = std::make_unique<audio::MusicManager>();
    footstepManager = std::make_unique<audio::FootstepManager>();
    activitySoundManager = std::make_unique<audio::ActivitySoundManager>();
    mountSoundManager = std::make_unique<audio::MountSoundManager>();
    npcVoiceManager = std::make_unique<audio::NpcVoiceManager>();
    ambientSoundManager = std::make_unique<audio::AmbientSoundManager>();
    uiSoundManager = std::make_unique<audio::UiSoundManager>();
    combatSoundManager = std::make_unique<audio::CombatSoundManager>();
    spellSoundManager = std::make_unique<audio::SpellSoundManager>();
    movementSoundManager = std::make_unique<audio::MovementSoundManager>();

    // Create secondary command buffer resources for multithreaded rendering
    if (!createSecondaryCommandResources()) {
        LOG_WARNING("Failed to create secondary command buffers — falling back to single-threaded rendering");
    }

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

    LOG_WARNING("Renderer::shutdown - wmoRenderer...");
    if (wmoRenderer) {
        wmoRenderer->shutdown();
        wmoRenderer.reset();
    }

    LOG_WARNING("Renderer::shutdown - m2Renderer...");
    if (m2Renderer) {
        m2Renderer->shutdown();
        m2Renderer.reset();
    }

    LOG_WARNING("Renderer::shutdown - musicManager...");
    if (musicManager) {
        musicManager->shutdown();
        musicManager.reset();
    }
    LOG_WARNING("Renderer::shutdown - footstepManager...");
    if (footstepManager) {
        footstepManager->shutdown();
        footstepManager.reset();
    }
    LOG_WARNING("Renderer::shutdown - activitySoundManager...");
    if (activitySoundManager) {
        activitySoundManager->shutdown();
        activitySoundManager.reset();
    }

    LOG_WARNING("Renderer::shutdown - AudioEngine...");
    // Shutdown AudioEngine singleton
    audio::AudioEngine::instance().shutdown();

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

    destroyFSRResources();
    destroyFSR2Resources();
    destroyFXAAResources();
    destroyPerFrameResources();

    zoneManager.reset();

    performanceHUD.reset();
    scene.reset();
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
    if (fsr2_.enabled && samples > VK_SAMPLE_COUNT_1_BIT) return;

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

    VkSampleCountFlagBits current = vkCtx->getMsaaSamples();
    if (samples == current) return;

    LOG_INFO("Changing MSAA from ", static_cast<int>(current), "x to ", static_cast<int>(samples), "x");

    // Single GPU wait — all subsequent operations are CPU-side object creation
    vkDeviceWaitIdle(vkCtx->getDevice());

    // Set new MSAA and recreate swapchain (render pass, depth, MSAA image, framebuffers)
    vkCtx->setMsaaSamples(samples);
    if (!vkCtx->recreateSwapchain(window->getWidth(), window->getHeight())) {
        LOG_ERROR("MSAA change failed — reverting to 1x");
        vkCtx->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
        vkCtx->recreateSwapchain(window->getWidth(), window->getHeight());
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
    if (fsr_.sceneFramebuffer) destroyFSRResources();   // Will be lazily recreated in beginFrame()
    if (fsr2_.sceneFramebuffer) destroyFSR2Resources();
    if (fxaa_.sceneFramebuffer) destroyFXAAResources(); // Will be lazily recreated in beginFrame()

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
    ImGui_ImplVulkan_Init(&initInfo);

    LOG_INFO("MSAA change complete");
}

void Renderer::beginFrame() {
    if (!vkCtx) return;
    if (vkCtx->isDeviceLost()) return;

    // Apply deferred MSAA change between frames (before any rendering state is used)
    if (msaaChangePending_) {
        applyMsaaChange();
    }

    // FSR resource management (safe: between frames, no command buffer in flight)
    if (fsr_.needsRecreate && fsr_.sceneFramebuffer) {
        destroyFSRResources();
        fsr_.needsRecreate = false;
        if (!fsr_.enabled) LOG_INFO("FSR: disabled");
    }
    if (fsr_.enabled && !fsr2_.enabled && !fsr_.sceneFramebuffer) {
        if (!initFSRResources()) {
            LOG_ERROR("FSR: initialization failed, disabling");
            fsr_.enabled = false;
        }
    }

    // FSR 2.2 resource management
    if (fsr2_.needsRecreate && fsr2_.sceneFramebuffer) {
        destroyFSR2Resources();
        fsr2_.needsRecreate = false;
        if (!fsr2_.enabled) LOG_INFO("FSR2: disabled");
    }
    if (fsr2_.enabled && !fsr2_.sceneFramebuffer) {
        if (!initFSR2Resources()) {
            LOG_ERROR("FSR2: initialization failed, disabling");
            fsr2_.enabled = false;
        }
    }

    // FXAA resource management — FXAA can coexist with FSR1 and FSR3.
    // When both FXAA and FSR3 are enabled, FXAA runs as a post-FSR3 pass.
    // Do not force this pass for ghost mode; keep AA quality strictly user-controlled.
    const bool useFXAAPostPass = fxaa_.enabled;
    if ((fxaa_.needsRecreate || !useFXAAPostPass) && fxaa_.sceneFramebuffer) {
        destroyFXAAResources();
        fxaa_.needsRecreate = false;
        if (!useFXAAPostPass) LOG_INFO("FXAA: disabled");
    }
    if (useFXAAPostPass && !fxaa_.sceneFramebuffer) {
        if (!initFXAAResources()) {
            LOG_ERROR("FXAA: initialization failed, disabling");
            fxaa_.enabled = false;
        }
    }

    // Handle swapchain recreation if needed
    if (vkCtx->isSwapchainDirty()) {
        vkCtx->recreateSwapchain(window->getWidth(), window->getHeight());
        // Rebuild water resources that reference swapchain extent/views
        if (waterRenderer) {
            waterRenderer->recreatePipelines();
        }
        // Recreate FSR resources for new swapchain dimensions
        if (fsr_.enabled && !fsr2_.enabled) {
            destroyFSRResources();
            initFSRResources();
        }
        if (fsr2_.enabled) {
            destroyFSR2Resources();
            initFSR2Resources();
        }
        // Recreate FXAA resources for new swapchain dimensions.
        if (useFXAAPostPass) {
            destroyFXAAResources();
            initFXAAResources();
        }
    }

    // Acquire swapchain image and begin command buffer
    currentCmd = vkCtx->beginFrame(currentImageIndex);
    if (currentCmd == VK_NULL_HANDLE) {
        // Swapchain out of date, will retry next frame
        return;
    }

    // FSR2 jitter pattern.
    if (fsr2_.enabled && fsr2_.sceneFramebuffer && camera) {
        if (!fsr2_.useAmdBackend) {
            camera->setJitter(0.0f, 0.0f);
        } else {
#if WOWEE_HAS_AMD_FSR2
            // AMD-recommended jitter sequence in pixel space, converted to NDC projection offset.
            int32_t phaseCount = ffxFsr2GetJitterPhaseCount(
                static_cast<int32_t>(fsr2_.internalWidth),
                static_cast<int32_t>(vkCtx->getSwapchainExtent().width));
            float jitterX = 0.0f;
            float jitterY = 0.0f;
            if (phaseCount > 0 &&
                ffxFsr2GetJitterOffset(&jitterX, &jitterY, static_cast<int32_t>(fsr2_.frameIndex % static_cast<uint32_t>(phaseCount)), phaseCount) == FFX_OK) {
                float ndcJx = (2.0f * jitterX) / static_cast<float>(fsr2_.internalWidth);
                float ndcJy = (2.0f * jitterY) / static_cast<float>(fsr2_.internalHeight);
                // Keep projection jitter and FSR dispatch jitter in sync.
                camera->setJitter(fsr2_.jitterSign * ndcJx, fsr2_.jitterSign * ndcJy);
            } else {
                camera->setJitter(0.0f, 0.0f);
            }
#else
            const float jitterScale = 0.5f;
            float jx = (halton(fsr2_.frameIndex + 1, 2) - 0.5f) * 2.0f * jitterScale / static_cast<float>(fsr2_.internalWidth);
            float jy = (halton(fsr2_.frameIndex + 1, 3) - 0.5f) * 2.0f * jitterScale / static_cast<float>(fsr2_.internalHeight);
            camera->setJitter(fsr2_.jitterSign * jx, fsr2_.jitterSign * jy);
#endif
        }
    }

    // Update per-frame UBO with current camera/lighting state
    updatePerFrameUBO();

    // GPU crash diagnostic: skip all pre-passes to isolate crash source
    static const bool skipPrePasses = (std::getenv("WOWEE_SKIP_PREPASSES") != nullptr);

    if (!skipPrePasses) {
    // --- Off-screen pre-passes (before main render pass) ---
    // Minimap composite (renders 3x3 tile grid into 768x768 render target)
    if (minimap && minimap->isEnabled() && camera) {
        glm::vec3 minimapCenter = camera->getPosition();
        if (cameraController && cameraController->isThirdPerson())
            minimapCenter = characterPosition;
        minimap->compositePass(currentCmd, minimapCenter);
    }
    // World map composite (renders zone tiles into 1024x768 render target)
    if (worldMap) {
        worldMap->compositePass(currentCmd);
    }

    // Character preview composite passes
    for (auto* preview : activePreviews_) {
        if (preview && preview->isModelLoaded()) {
            preview->compositePass(currentCmd, vkCtx->getCurrentFrame());
        }
    }

    // Shadow pre-pass (before main render pass)
    if (shadowsEnabled && shadowDepthImage[0] != VK_NULL_HANDLE) {
        renderShadowPass();
    }

    // Water reflection pre-pass (renders scene from mirrored camera into 512x512 texture)
    renderReflectionPass();
    } // !skipPrePasses

    // --- Begin render pass ---
    // If FSR is enabled, render scene to off-screen target at reduced resolution.
    // Otherwise, render directly to swapchain.
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = vkCtx->getImGuiRenderPass();

    VkExtent2D renderExtent;
    if (fsr2_.enabled && fsr2_.sceneFramebuffer) {
        rpInfo.framebuffer = fsr2_.sceneFramebuffer;
        renderExtent = { fsr2_.internalWidth, fsr2_.internalHeight };
    } else if (useFXAAPostPass && fxaa_.sceneFramebuffer) {
        // FXAA takes priority over FSR1: renders at native res with AA post-process.
        // When both FSR1 and FXAA are enabled, FXAA wins (native res, no downscale).
        rpInfo.framebuffer = fxaa_.sceneFramebuffer;
        renderExtent = vkCtx->getSwapchainExtent();  // native resolution — no downscaling
    } else if (fsr_.enabled && fsr_.sceneFramebuffer) {
        rpInfo.framebuffer = fsr_.sceneFramebuffer;
        renderExtent = { fsr_.internalWidth, fsr_.internalHeight };
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
    if (!vkCtx || currentCmd == VK_NULL_HANDLE) return;

    if (fsr2_.enabled && fsr2_.sceneFramebuffer) {
        // End the off-screen scene render pass
        vkCmdEndRenderPass(currentCmd);

        if (fsr2_.useAmdBackend) {
            // Compute passes: motion vectors -> temporal accumulation
            dispatchMotionVectors();
            if (fsr2_.amdFsr3FramegenEnabled && fsr2_.amdFsr3FramegenRuntimeReady) {
                dispatchAmdFsr3Framegen();
                if (!fsr2_.amdFsr3FramegenRuntimeActive) {
                    dispatchAmdFsr2();
                }
            } else {
                dispatchAmdFsr2();
            }

            // Transition post-FSR input for sharpen pass.
            if (fsr2_.amdFsr3FramegenRuntimeActive && fsr2_.framegenOutput.image) {
                transitionImageLayout(currentCmd, fsr2_.framegenOutput.image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                fsr2_.framegenOutputValid = true;
            } else {
                transitionImageLayout(currentCmd, fsr2_.history[fsr2_.currentHistory].image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            }
        } else {
            transitionImageLayout(currentCmd, fsr2_.sceneColor.image,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        // FSR3+FXAA combined: re-point FXAA's descriptor to the FSR3 temporal output
        // so renderFXAAPass() applies spatial AA on the temporally-stabilized frame.
        // This must happen outside the render pass (descriptor updates are CPU-side).
        if (fxaa_.enabled && fxaa_.descSet && fxaa_.sceneSampler) {
            VkImageView fsr3OutputView = VK_NULL_HANDLE;
            if (fsr2_.useAmdBackend) {
                if (fsr2_.amdFsr3FramegenRuntimeActive && fsr2_.framegenOutput.image)
                    fsr3OutputView = fsr2_.framegenOutput.imageView;
                else if (fsr2_.history[fsr2_.currentHistory].image)
                    fsr3OutputView = fsr2_.history[fsr2_.currentHistory].imageView;
            } else if (fsr2_.history[fsr2_.currentHistory].image) {
                fsr3OutputView = fsr2_.history[fsr2_.currentHistory].imageView;
            }
            if (fsr3OutputView) {
                VkDescriptorImageInfo imgInfo{};
                imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imgInfo.imageView   = fsr3OutputView;
                imgInfo.sampler     = fxaa_.sceneSampler;
                VkWriteDescriptorSet write{};
                write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet           = fxaa_.descSet;
                write.dstBinding       = 0;
                write.descriptorCount  = 1;
                write.descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo       = &imgInfo;
                vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);
            }
        }

        // Begin swapchain render pass at full resolution for sharpening + ImGui
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = vkCtx->getImGuiRenderPass();
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        rpInfo.renderArea.offset = {0, 0};
        rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

        bool msaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
        VkClearValue clearValues[4]{};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        clearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[3].depthStencil = {1.0f, 0};
        rpInfo.clearValueCount = msaaOn ? (vkCtx->getDepthResolveImageView() ? 4u : 3u) : 2u;
        rpInfo.pClearValues = clearValues;

        vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        // When FXAA is also enabled: apply FXAA on the FSR3 temporal output instead
        // of RCAS sharpening. FXAA descriptor is temporarily pointed to the FSR3
        // history buffer (which is already in SHADER_READ_ONLY_OPTIMAL). This gives
        // FSR3 temporal stability + FXAA spatial edge smoothing ("ultra quality native").
        if (fxaa_.enabled && fxaa_.pipeline && fxaa_.descSet) {
            renderFXAAPass();
        } else {
            // Draw RCAS sharpening from accumulated history buffer
            renderFSR2Sharpen();
        }

        // Restore FXAA descriptor to its normal scene color source so standalone
        // FXAA frames are not affected by the FSR3 history pointer set above.
        if (fxaa_.enabled && fxaa_.descSet && fxaa_.sceneSampler && fxaa_.sceneColor.imageView) {
            VkDescriptorImageInfo restoreInfo{};
            restoreInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            restoreInfo.imageView   = fxaa_.sceneColor.imageView;
            restoreInfo.sampler     = fxaa_.sceneSampler;
            VkWriteDescriptorSet restoreWrite{};
            restoreWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            restoreWrite.dstSet          = fxaa_.descSet;
            restoreWrite.dstBinding      = 0;
            restoreWrite.descriptorCount = 1;
            restoreWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            restoreWrite.pImageInfo      = &restoreInfo;
            vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &restoreWrite, 0, nullptr);
        }

        // Maintain frame bookkeeping
        fsr2_.prevViewProjection = camera->getViewProjectionMatrix();
        fsr2_.prevJitter = camera->getJitter();
        camera->clearJitter();
        if (fsr2_.useAmdBackend) {
            fsr2_.currentHistory = 1 - fsr2_.currentHistory;
        }
        fsr2_.frameIndex = (fsr2_.frameIndex + 1) % 256;  // Wrap to keep Halton values well-distributed

    } else if (fxaa_.enabled && fxaa_.sceneFramebuffer) {
        // End the off-screen scene render pass
        vkCmdEndRenderPass(currentCmd);

        // Transition resolved scene color: PRESENT_SRC_KHR → SHADER_READ_ONLY
        transitionImageLayout(currentCmd, fxaa_.sceneColor.image,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // Begin swapchain render pass (1x — no MSAA on the output pass)
        VkRenderPassBeginInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpInfo.renderPass = vkCtx->getImGuiRenderPass();
        rpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        rpInfo.renderArea.offset = {0, 0};
        rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();
        // The swapchain render pass always has 2 attachments when MSAA is off;
        // FXAA output goes to the non-MSAA swapchain directly.
        VkClearValue fxaaClear[2]{};
        fxaaClear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        fxaaClear[1].depthStencil = {1.0f, 0};
        rpInfo.clearValueCount = 2;
        rpInfo.pClearValues = fxaaClear;

        vkCmdBeginRenderPass(currentCmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D ext = vkCtx->getSwapchainExtent();
        VkViewport vp{};
        vp.width = static_cast<float>(ext.width);
        vp.height = static_cast<float>(ext.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = ext;
        vkCmdSetScissor(currentCmd, 0, 1, &sc);

        // Draw FXAA pass
        renderFXAAPass();

    } else if (fsr_.enabled && fsr_.sceneFramebuffer) {
        // FSR1 upscale path — only runs when FXAA is not active.
        // When both FSR1 and FXAA are enabled, FXAA took priority above.
        vkCmdEndRenderPass(currentCmd);

        // Transition scene color (1x resolve/color target): PRESENT_SRC_KHR → SHADER_READ_ONLY
        transitionImageLayout(currentCmd, fsr_.sceneColor.image,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // Begin swapchain render pass at full resolution
        VkRenderPassBeginInfo fsrRpInfo{};
        fsrRpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        fsrRpInfo.renderPass = vkCtx->getImGuiRenderPass();
        fsrRpInfo.framebuffer = vkCtx->getSwapchainFramebuffers()[currentImageIndex];
        fsrRpInfo.renderArea.offset = {0, 0};
        fsrRpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

        bool fsrMsaaOn = (vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT);
        VkClearValue fsrClearValues[4]{};
        fsrClearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        fsrClearValues[1].depthStencil = {1.0f, 0};
        fsrClearValues[2].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        fsrClearValues[3].depthStencil = {1.0f, 0};
        if (fsrMsaaOn) {
            bool depthRes = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);
            fsrRpInfo.clearValueCount = depthRes ? 4 : 3;
        } else {
            fsrRpInfo.clearValueCount = 2;
        }
        fsrRpInfo.pClearValues = fsrClearValues;

        vkCmdBeginRenderPass(currentCmd, &fsrRpInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkExtent2D fsrExt = vkCtx->getSwapchainExtent();
        VkViewport fsrVp{};
        fsrVp.width = static_cast<float>(fsrExt.width);
        fsrVp.height = static_cast<float>(fsrExt.height);
        fsrVp.maxDepth = 1.0f;
        vkCmdSetViewport(currentCmd, 0, 1, &fsrVp);
        VkRect2D fsrSc{};
        fsrSc.extent = fsrExt;
        vkCmdSetScissor(currentCmd, 0, 1, &fsrSc);

        renderFSRUpscale();
    }

    // ImGui rendering — must respect subpass contents mode
    // Parallel recording only applies when no post-process pass is active.
    if (!fsr_.enabled && !fsr2_.enabled && !fxaa_.enabled && parallelRecordingEnabled_) {
        // Scene pass was begun with VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS,
        // so ImGui must be recorded into a secondary command buffer.
        VkCommandBuffer imguiCmd = beginSecondary(SEC_IMGUI);
        setSecondaryViewportScissor(imguiCmd);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), imguiCmd);
        vkEndCommandBuffer(imguiCmd);
        vkCmdExecuteCommands(currentCmd, 1, &imguiCmd);
    } else {
        // FSR swapchain pass uses INLINE mode; non-parallel also uses INLINE.
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
}

void Renderer::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    mountInstanceId_ = mountInstId;
    mountHeightOffset_ = heightOffset;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = characterPosition;
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;  // Clear mount action state
    mountActionPhase_ = 0;
    charAnimState = CharAnimState::MOUNT;
    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    // Debug: dump available mount animations
    if (characterRenderer && mountInstId > 0) {
        characterRenderer->dumpAnimations(mountInstId);
    }

    // Discover mount animation capabilities (property-based, not hardcoded IDs)
    LOG_DEBUG("=== Mount Animation Dump (Display ID ", mountDisplayId, ") ===");
    characterRenderer->dumpAnimations(mountInstId);

    // Get all sequences for property-based analysis
    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(mountInstId, sequences)) {
        LOG_WARNING("Failed to get animation sequences for mount, using fallback IDs");
        sequences.clear();
    }

    // Helper: ID-based fallback finder
    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer->hasAnimation(mountInstId, id)) {
                return id;
            }
        }
        return 0;
    };

    // Property-based jump animation discovery with chain-based scoring
    auto discoverJumpSet = [&]() {
        // Debug: log all sequences for analysis
        LOG_DEBUG("=== Full sequence table for mount ===");
        for (const auto& seq : sequences) {
            LOG_DEBUG("SEQ id=", seq.id,
                     " dur=", seq.duration,
                     " flags=0x", std::hex, seq.flags, std::dec,
                     " moveSpd=", seq.movingSpeed,
                     " blend=", seq.blendTime,
                     " next=", seq.nextAnimation,
                     " alias=", seq.aliasNext);
        }
        LOG_DEBUG("=== End sequence table ===");

        // Known combat/bad animation IDs to avoid
        std::set<uint32_t> forbiddenIds = {53, 54, 16};  // jumpkick, attack

        auto scoreNear = [](int a, int b) -> int {
            int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0; // within 8 IDs gets points
        };

        auto isForbidden = [&](uint32_t id) {
            return forbiddenIds.count(id) != 0;
        };

        auto findSeqById = [&](uint32_t id) -> const pipeline::M2Sequence* {
            for (const auto& s : sequences) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        uint32_t runId = findFirst({5, 4});
        uint32_t standId = findFirst({0});

        // Step A: Find loop candidates
        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            // Bit 0x01 NOT set = loops (0x20, 0x60), bit 0x01 set = non-looping (0x21, 0x61)
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.duration >= 350 && seq.duration <= 1000 &&
                seq.id != runId && seq.id != standId) {
                loops.push_back(seq.id);
            }
        }

        // Choose loop: prefer one near known classic IDs (38), else best duration
        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear((int)id, 38);  // classic hint
                const auto* s = findSeqById(id);
                if (s) sc += (s->duration >= 500 && s->duration <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }

        // Step B: Score start/end candidates
        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;

        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            // Only consider non-looping animations for start/end
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;

            // Start window
            if (seq.duration >= 450 && seq.duration <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear((int)seq.id, (int)loop);
                // Chain bonus: if this start points at loop or near it
                if (loop && (seq.nextAnimation == (int16_t)loop || seq.aliasNext == loop)) sc += 30;
                if (loop && scoreNear(seq.nextAnimation, (int)loop) > 0) sc += 10;
                // Penalize "stop/brake-ish": very long blendTime can be a stop transition
                if (seq.blendTime > 400) sc -= 5;

                if (sc > bestStart) {
                    bestStart = sc;
                    start = seq.id;
                }
            }

            // End window
            if (seq.duration >= 650 && seq.duration <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear((int)seq.id, (int)loop);
                // Chain bonus: end often points to run/stand or has no next
                if (seq.nextAnimation == (int16_t)runId || seq.nextAnimation == (int16_t)standId) sc += 10;
                if (seq.nextAnimation < 0) sc += 5; // no chain sometimes = terminal
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }

        LOG_DEBUG("Property-based jump discovery: start=", start, " loop=", loop, " end=", end,
                 " scores: start=", bestStart, " end=", bestEnd);
        return std::make_tuple(start, loop, end);
    };

    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    // Use discovered animations, fallback to known IDs if discovery fails
    mountAnims_.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({40, 37});
    mountAnims_.jumpLoop  = discoveredLoop > 0 ? discoveredLoop : findFirst({38});
    mountAnims_.jumpEnd   = discoveredEnd > 0 ? discoveredEnd : findFirst({39});
    mountAnims_.rearUp    = findFirst({94, 92, 40}); // RearUp/Special
    mountAnims_.run       = findFirst({5, 4});       // Run/Walk
    mountAnims_.stand     = findFirst({0});          // Stand (almost always 0)

    // Discover idle fidget animations using proper WoW M2 metadata (frequency, replay timers)
    mountAnims_.fidgets.clear();
    core::Logger::getInstance().debug("Scanning for fidget animations in ", sequences.size(), " sequences");

    // DEBUG: Log ALL non-looping, short, stationary animations to identify stamps/tosses
    core::Logger::getInstance().debug("=== ALL potential fidgets (no metadata filter) ===");
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary) {
            core::Logger::getInstance().debug("  ALL: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " flags=0x", std::hex, seq.flags, std::dec,
                " next=", seq.nextAnimation);
        }
    }

    // Proper fidget discovery: frequency > 0 + replay timers indicate random idle animations
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool hasFrequency = seq.frequency > 0;
        bool hasReplay = seq.replayMax > 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        // Log candidates with metadata
        if (!isLoop && reasonableDuration && isStationary && (hasFrequency || hasReplay)) {
            core::Logger::getInstance().debug("  Candidate: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " next=", seq.nextAnimation,
                " speed=", seq.movingSpeed);
        }

        // Exclude known problematic animations: death (5-6), wounds (7-9), combat (16-21), attacks (11-15)
        bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        bool isSpecial = (seq.id == 2 || seq.id == 3);  // Often aggressive specials

        // Select fidgets: (frequency OR replay) + exclude problematic ID ranges
        // Relaxed back to OR since some mounts may only have one metadata marker
        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration &&
            !isDeathOrWound && !isAttackOrCombat && !isSpecial) {
            // Bonus: chains back to stand (indicates idle behavior)
            bool chainsToStand = (seq.nextAnimation == (int16_t)mountAnims_.stand) ||
                                 (seq.aliasNext == mountAnims_.stand) ||
                                 (seq.nextAnimation == -1);

            mountAnims_.fidgets.push_back(seq.id);
            core::Logger::getInstance().debug("  >> Selected fidget: id=", seq.id,
                (chainsToStand ? " (chains to stand)" : ""));
        }
    }

    // Ensure we have fallbacks for movement
    if (mountAnims_.stand == 0) mountAnims_.stand = 0;  // Force 0 even if not found
    if (mountAnims_.run == 0) mountAnims_.run = mountAnims_.stand;  // Fallback to stand if no run

    core::Logger::getInstance().debug("Mount animation set: jumpStart=", mountAnims_.jumpStart,
        " jumpLoop=", mountAnims_.jumpLoop,
        " jumpEnd=", mountAnims_.jumpEnd,
        " rearUp=", mountAnims_.rearUp,
        " run=", mountAnims_.run,
        " stand=", mountAnims_.stand,
        " fidgets=", mountAnims_.fidgets.size());

    // Notify mount sound manager
    if (mountSoundManager) {
        bool isFlying = taxiFlight_;  // Taxi flights are flying mounts
        mountSoundManager->onMount(mountDisplayId, isFlying, modelPath);
    }
}

void Renderer::clearMount() {
    mountInstanceId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = glm::vec3(0.0f);
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;
    mountActionPhase_ = 0;
    charAnimState = CharAnimState::IDLE;
    if (cameraController) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }

    // Notify mount sound manager
    if (mountSoundManager) {
        mountSoundManager->onDismount();
    }
}

uint32_t Renderer::resolveMeleeAnimId() {
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId = 0;
        meleeAnimDurationMs = 0.0f;
        return 0;
    }

    if (meleeAnimId != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId)) {
        return meleeAnimId;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId = 0;
        meleeAnimDurationMs = 0.0f;
        return 0;
    }

    auto findDuration = [&](uint32_t id) -> float {
        for (const auto& seq : sequences) {
            if (seq.id == id && seq.duration > 0) {
                return static_cast<float>(seq.duration);
            }
        }
        return 0.0f;
    };

    // Select animation priority based on equipped weapon type
    // WoW inventory types: 17 = 2H weapon, 13/21 = 1H, 0 = unarmed
    // WoW anim IDs: 16 = unarmed, 17 = 1H attack, 18 = 2H attack
    const uint32_t* attackCandidates;
    size_t candidateCount;
    static const uint32_t candidates2H[] = {18, 17, 16, 19, 20, 21};
    static const uint32_t candidates1H[] = {17, 18, 16, 19, 20, 21};
    static const uint32_t candidatesUnarmed[] = {16, 17, 18, 19, 20, 21};
    if (equippedWeaponInvType_ == 17) { // INVTYPE_2HWEAPON
        attackCandidates = candidates2H;
        candidateCount = 6;
    } else if (equippedWeaponInvType_ == 0) {
        attackCandidates = candidatesUnarmed;
        candidateCount = 6;
    } else {
        attackCandidates = candidates1H;
        candidateCount = 6;
    }
    for (size_t ci = 0; ci < candidateCount; ci++) {
        uint32_t id = attackCandidates[ci];
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId = id;
            meleeAnimDurationMs = findDuration(id);
            return meleeAnimId;
        }
    }

    const uint32_t avoidIds[] = {0, 1, 4, 5, 11, 12, 13, 37, 38, 39, 41, 42, 97};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId = bestId;
    meleeAnimDurationMs = static_cast<float>(bestDuration);
    return meleeAnimId;
}

void Renderer::updateCharacterAnimation() {
    // WoW WotLK AnimationData.dbc IDs
    constexpr uint32_t ANIM_STAND      = 0;
    constexpr uint32_t ANIM_WALK       = 4;
    constexpr uint32_t ANIM_RUN        = 5;
    // Candidate locomotion clips by common WotLK IDs.
    constexpr uint32_t ANIM_STRAFE_RUN_RIGHT  = 92;
    constexpr uint32_t ANIM_STRAFE_RUN_LEFT   = 93;
    constexpr uint32_t ANIM_STRAFE_WALK_LEFT  = 11;
    constexpr uint32_t ANIM_STRAFE_WALK_RIGHT = 12;
    constexpr uint32_t ANIM_BACKPEDAL         = 13;
    constexpr uint32_t ANIM_JUMP_START = 37;
    constexpr uint32_t ANIM_JUMP_MID   = 38;
    constexpr uint32_t ANIM_JUMP_END   = 39;
    constexpr uint32_t ANIM_SIT_DOWN   = 97;  // SitGround — transition to sitting
    constexpr uint32_t ANIM_SITTING    = 97;  // Hold on same animation (no separate idle)
    constexpr uint32_t ANIM_SWIM_IDLE  = 41;  // Treading water (SwimIdle)
    constexpr uint32_t ANIM_SWIM       = 42;  // Swimming forward (Swim)
    constexpr uint32_t ANIM_MOUNT      = 91;  // Seated on mount
    // Canonical player ready stances (AnimationData.dbc)
    constexpr uint32_t ANIM_READY_UNARMED = 22;  // ReadyUnarmed
    constexpr uint32_t ANIM_READY_1H      = 23;  // Ready1H
    constexpr uint32_t ANIM_READY_2H      = 24;  // Ready2H
    constexpr uint32_t ANIM_READY_2H_L    = 25;  // Ready2HL (some 2H left-handed rigs)
    constexpr uint32_t ANIM_FLY_IDLE   = 158; // Flying mount idle/hover
    constexpr uint32_t ANIM_FLY_FORWARD = 159; // Flying mount forward

    CharAnimState newState = charAnimState;

    const bool rawMoving = cameraController->isMoving();
    const bool rawSprinting = cameraController->isSprinting();
    constexpr float kLocomotionStopGraceSec = 0.12f;
    if (rawMoving) {
        locomotionStopGraceTimer_ = kLocomotionStopGraceSec;
        locomotionWasSprinting_ = rawSprinting;
    } else {
        locomotionStopGraceTimer_ = std::max(0.0f, locomotionStopGraceTimer_ - lastDeltaTime_);
    }
    // Debounce brief input/state dropouts (notably during both-mouse steering) so
    // locomotion clips do not restart every few frames.
    bool moving = rawMoving || locomotionStopGraceTimer_ > 0.0f;
    bool movingForward = cameraController->isMovingForward();
    bool movingBackward = cameraController->isMovingBackward();
    bool autoRunning = cameraController->isAutoRunning();
    bool strafeLeft = cameraController->isStrafingLeft();
    bool strafeRight = cameraController->isStrafingRight();
    // Strafe animation only plays during *pure* strafing (no forward/backward/autorun).
    // When forward+strafe are both held, the walk/run animation plays — same as the real client.
    bool pureStrafe = !movingForward && !movingBackward && !autoRunning;
    bool anyStrafeLeft = strafeLeft && !strafeRight && pureStrafe;
    bool anyStrafeRight = strafeRight && !strafeLeft && pureStrafe;
    bool grounded = cameraController->isGrounded();
    bool jumping = cameraController->isJumping();
    bool sprinting = rawSprinting || (!rawMoving && moving && locomotionWasSprinting_);
    bool sitting = cameraController->isSitting();
    bool swim = cameraController->isSwimming();
    bool forceMelee = meleeSwingTimer > 0.0f && grounded && !swim;

    // When mounted, force MOUNT state and skip normal transitions
    if (isMounted()) {
        newState = CharAnimState::MOUNT;
        charAnimState = newState;

        // Play seated animation on player
        uint32_t currentAnimId = 0;
        float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
        bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
        if (!haveState || currentAnimId != ANIM_MOUNT) {
            characterRenderer->playAnimation(characterInstanceId, ANIM_MOUNT, true);
        }

        // Sync mount instance position and rotation
        float mountBob = 0.0f;
        float mountYawRad = glm::radians(characterYaw);
        if (mountInstanceId_ > 0) {
            characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);

            // Procedural lean into turns (ground mounts only, optional enhancement)
            if (!taxiFlight_ && moving && lastDeltaTime_ > 0.0f) {
                float currentYawDeg = characterYaw;
                float turnRate = (currentYawDeg - prevMountYaw_) / lastDeltaTime_;
                // Normalize to [-180, 180] for wrap-around
                while (turnRate > 180.0f) turnRate -= 360.0f;
                while (turnRate < -180.0f) turnRate += 360.0f;

                float targetLean = glm::clamp(turnRate * 0.15f, -0.25f, 0.25f);
                mountRoll_ = glm::mix(mountRoll_, targetLean, lastDeltaTime_ * 6.0f);
                prevMountYaw_ = currentYawDeg;
            } else {
                // Return to upright when not turning
                mountRoll_ = glm::mix(mountRoll_, 0.0f, lastDeltaTime_ * 8.0f);
            }

            // Apply pitch (up/down), roll (banking), and yaw for realistic flight
            characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll_, mountYawRad));

            // Drive mount model animation: idle when still, run when moving
            auto pickMountAnim = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
                for (uint32_t id : candidates) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, id)) {
                        return id;
                    }
                }
                return fallback;
            };

            uint32_t mountAnimId = ANIM_STAND;

            // Get current mount animation state (used throughout)
            uint32_t curMountAnim = 0;
            float curMountTime = 0, curMountDur = 0;
            bool haveMountState = characterRenderer->getAnimationState(mountInstanceId_, curMountAnim, curMountTime, curMountDur);

            // Taxi flight: use flying animations instead of ground movement
            if (taxiFlight_) {
                // Log available animations once when taxi starts
                if (!taxiAnimsLogged_) {
                    taxiAnimsLogged_ = true;
                    LOG_INFO("Taxi flight active: mountInstanceId_=", mountInstanceId_,
                             " curMountAnim=", curMountAnim, " haveMountState=", haveMountState);
                    std::vector<pipeline::M2Sequence> seqs;
                    if (characterRenderer->getAnimationSequences(mountInstanceId_, seqs)) {
                        std::string animList;
                        for (const auto& s : seqs) {
                            if (!animList.empty()) animList += ", ";
                            animList += std::to_string(s.id);
                        }
                        LOG_INFO("Taxi mount available animations: [", animList, "]");
                    }
                }

                // Try multiple flying animation IDs in priority order:
                // 159=FlyForward, 158=FlyIdle (WotLK flying mounts)
                // 234=FlyRun, 229=FlyStand (Vanilla creature fly anims)
                // 233=FlyWalk, 141=FlyMounted, 369=FlyRun (alternate IDs)
                // 6=Fly (classic creature fly)
                // Fallback: Run, then Stand (hover)
                uint32_t flyAnims[] = {ANIM_FLY_FORWARD, ANIM_FLY_IDLE, 234, 229, 233, 141, 369, 6, ANIM_RUN};
                mountAnimId = ANIM_STAND; // ultimate fallback: hover/idle
                for (uint32_t fa : flyAnims) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, fa)) {
                        mountAnimId = fa;
                        break;
                    }
                }

                if (!haveMountState || curMountAnim != mountAnimId) {
                    LOG_INFO("Taxi mount: playing animation ", mountAnimId);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                }

                // Skip all ground mount logic (jumps, fidgets, etc.)
                goto taxi_mount_done;
            } else {
                taxiAnimsLogged_ = false;
            }

            // Check for jump trigger - use cached per-mount animation IDs
            if (cameraController->isJumpKeyPressed() && grounded && mountAction_ == MountAction::None) {
                if (moving && mountAnims_.jumpLoop > 0) {
                    // Moving: skip JumpStart (looks like stopping), go straight to airborne loop
                    LOG_DEBUG("Mount jump triggered while moving: using jumpLoop anim ", mountAnims_.jumpLoop);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                    mountAction_ = MountAction::Jump;
                    mountActionPhase_ = 1;  // Start in airborne phase
                    mountAnimId = mountAnims_.jumpLoop;
                    if (mountSoundManager) {
                        mountSoundManager->playJumpSound();
                    }
                    if (cameraController) {
                        cameraController->triggerMountJump();
                    }
                } else if (!moving && mountAnims_.rearUp > 0) {
                    // Standing still: rear-up flourish
                    LOG_DEBUG("Mount rear-up triggered: playing rearUp anim ", mountAnims_.rearUp);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.rearUp, false);
                    mountAction_ = MountAction::RearUp;
                    mountActionPhase_ = 0;
                    mountAnimId = mountAnims_.rearUp;
                    // Trigger semantic rear-up sound
                    if (mountSoundManager) {
                        mountSoundManager->playRearUpSound();
                    }
                }
            }

            // Handle active mount actions (jump chaining or rear-up)
            if (mountAction_ != MountAction::None) {
                bool animFinished = haveMountState && curMountDur > 0.1f &&
                                   (curMountTime >= curMountDur - 0.05f);

                if (mountAction_ == MountAction::Jump) {
                    // Jump sequence: start → loop → end (physics-driven)
                    if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop > 0) {
                        // JumpStart finished, go to JumpLoop (airborne)
                        LOG_DEBUG("Mount jump: phase 0→1 (JumpStart→JumpLoop anim ", mountAnims_.jumpLoop, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                        mountActionPhase_ = 1;
                        mountAnimId = mountAnims_.jumpLoop;
                    } else if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop == 0) {
                        // No JumpLoop, go straight to airborne phase 1 (hold JumpStart pose)
                        LOG_DEBUG("Mount jump: phase 0→1 (no JumpLoop, holding JumpStart)");
                        mountActionPhase_ = 1;
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd > 0) {
                        // Landed after airborne phase! Go to JumpEnd (grounded-triggered)
                        LOG_DEBUG("Mount jump: phase 1→2 (landed, JumpEnd anim ", mountAnims_.jumpEnd, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpEnd, false);
                        mountActionPhase_ = 2;
                        mountAnimId = mountAnims_.jumpEnd;
                        // Trigger semantic landing sound
                        if (mountSoundManager) {
                            mountSoundManager->playLandSound();
                        }
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd == 0) {
                        // No JumpEnd animation, return directly to movement after landing
                        LOG_DEBUG("Mount jump: phase 1→done (landed, no JumpEnd, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else if (mountActionPhase_ == 2 && animFinished) {
                        // JumpEnd finished, return to movement
                        LOG_DEBUG("Mount jump: phase 2→done (JumpEnd finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;  // Keep current jump animation
                    }
                } else if (mountAction_ == MountAction::RearUp) {
                    // Rear-up: single animation, return to stand when done
                    if (animFinished) {
                        LOG_DEBUG("Mount rear-up: finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand));
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;  // Keep current rear-up animation
                    }
                }
            } else if (moving) {
                // Normal movement animations
                if (anyStrafeLeft) {
                    mountAnimId = pickMountAnim({ANIM_STRAFE_RUN_LEFT, ANIM_STRAFE_WALK_LEFT, ANIM_RUN}, ANIM_RUN);
                } else if (anyStrafeRight) {
                    mountAnimId = pickMountAnim({ANIM_STRAFE_RUN_RIGHT, ANIM_STRAFE_WALK_RIGHT, ANIM_RUN}, ANIM_RUN);
                } else if (movingBackward) {
                    mountAnimId = pickMountAnim({ANIM_BACKPEDAL}, ANIM_RUN);
                } else {
                    mountAnimId = ANIM_RUN;
                }
            }

            // Cancel active fidget immediately if movement starts
            if (moving && mountActiveFidget_ != 0) {
                mountActiveFidget_ = 0;
                // Force play run animation to stop fidget immediately
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
            }

            // Check if active fidget has completed (only when not moving)
            if (!moving && mountActiveFidget_ != 0) {
                uint32_t curAnim = 0;
                float curTime = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(mountInstanceId_, curAnim, curTime, curDur)) {
                    // If animation changed or completed, clear active fidget
                    if (curAnim != mountActiveFidget_ || curTime >= curDur * 0.95f) {
                        mountActiveFidget_ = 0;
                        LOG_DEBUG("Mount fidget completed");
                    }
                }
            }

            // Idle fidgets: random one-shot animations when standing still
            if (!moving && mountAction_ == MountAction::None && mountActiveFidget_ == 0 && !mountAnims_.fidgets.empty()) {
                mountIdleFidgetTimer_ += lastDeltaTime_;
                static float nextFidgetTime = 6.0f + (rand() % 7);  // 6-12 seconds

                if (mountIdleFidgetTimer_ >= nextFidgetTime) {
                    // Trigger random fidget animation
                    static std::mt19937 rng(std::random_device{}());
                    std::uniform_int_distribution<size_t> dist(0, mountAnims_.fidgets.size() - 1);
                    uint32_t fidgetAnim = mountAnims_.fidgets[dist(rng)];

                    characterRenderer->playAnimation(mountInstanceId_, fidgetAnim, false);
                    mountActiveFidget_ = fidgetAnim;  // Track active fidget
                    mountIdleFidgetTimer_ = 0.0f;
                    nextFidgetTime = 6.0f + (rand() % 7);  // Randomize next fidget time

                    LOG_DEBUG("Mount idle fidget: playing anim ", fidgetAnim);
                }
            }
            if (moving) {
                mountIdleFidgetTimer_ = 0.0f;  // Reset timer when moving
            }

            // Idle ambient sounds: snorts and whinnies only, infrequent
            if (!moving && mountSoundManager) {
                mountIdleSoundTimer_ += lastDeltaTime_;
                static float nextIdleSoundTime = 45.0f + (rand() % 46);  // 45-90 seconds

                if (mountIdleSoundTimer_ >= nextIdleSoundTime) {
                    mountSoundManager->playIdleSound();
                    mountIdleSoundTimer_ = 0.0f;
                    nextIdleSoundTime = 45.0f + (rand() % 46);  // Randomize next sound time
                }
            } else if (moving) {
                mountIdleSoundTimer_ = 0.0f;  // Reset timer when moving
            }

            // Only update animation if it changed and we're not in an action sequence or playing a fidget
            if (mountAction_ == MountAction::None && mountActiveFidget_ == 0 && (!haveMountState || curMountAnim != mountAnimId)) {
                bool loop = true;  // Normal movement animations loop
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, loop);
            }

            taxi_mount_done:
            // Rider bob: sinusoidal motion synced to mount's run animation (only used in fallback positioning)
            mountBob = 0.0f;
            if (moving && haveMountState && curMountDur > 1.0f) {
                // Wrap mount time preserving precision via subtraction instead of fmod
                float wrappedTime = curMountTime;
                while (wrappedTime >= curMountDur) {
                    wrappedTime -= curMountDur;
                }
                float norm = wrappedTime / curMountDur;
                // One bounce per stride cycle
                float bobSpeed = taxiFlight_ ? 2.0f : 1.0f;
                mountBob = std::sin(norm * 2.0f * 3.14159f * bobSpeed) * 0.12f;
            }
        }

        // Use mount's attachment point for proper bone-driven rider positioning.
        if (taxiFlight_) {
            glm::mat4 mountSeatTransform(1.0f);
            bool haveSeat = false;
            static constexpr uint32_t kTaxiSeatAttachmentId = 0;  // deterministic rider seat
            if (mountSeatAttachmentId_ == -1) {
                mountSeatAttachmentId_ = static_cast<int>(kTaxiSeatAttachmentId);
            }
            if (mountSeatAttachmentId_ >= 0) {
                haveSeat = characterRenderer->getAttachmentTransform(
                    mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }

            if (haveSeat) {
                glm::vec3 targetRiderPos = glm::vec3(mountSeatTransform[3]) + glm::vec3(0.0f, 0.0f, 0.02f);
                // Taxi passengers should be rigidly parented to mount attachment transforms.
                // Smoothing here introduces visible seat lag/drift on turns.
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
                characterRenderer->setInstancePosition(characterInstanceId, targetRiderPos);
            } else {
                mountSeatSmoothingInit_ = false;
                glm::vec3 playerPos = characterPosition + glm::vec3(0.0f, 0.0f, mountHeightOffset_ + 0.10f);
                characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            }

            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, mountYawRad));
            return;
        }

        // Ground mounts: try a seat attachment first.
        glm::mat4 mountSeatTransform;
        bool haveSeat = false;
        if (mountSeatAttachmentId_ >= 0) {
            haveSeat = characterRenderer->getAttachmentTransform(
                mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
        } else if (mountSeatAttachmentId_ == -1) {
            // Probe common rider seat attachment IDs once per mount.
            static constexpr uint32_t kSeatAttachments[] = {0, 5, 6, 7, 8};
            for (uint32_t attId : kSeatAttachments) {
                if (characterRenderer->getAttachmentTransform(mountInstanceId_, attId, mountSeatTransform)) {
                    mountSeatAttachmentId_ = static_cast<int>(attId);
                    haveSeat = true;
                    break;
                }
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }
        }

        if (haveSeat) {
            // Extract position from mount seat transform (attachment point already includes proper seat height)
            glm::vec3 mountSeatPos = glm::vec3(mountSeatTransform[3]);

            // Keep seat offset minimal; large offsets amplify visible bobble.
            glm::vec3 seatOffset = glm::vec3(0.0f, 0.0f, taxiFlight_ ? 0.04f : 0.08f);
            glm::vec3 targetRiderPos = mountSeatPos + seatOffset;
            // When moving, smoothing the seat position produces visible lag that looks like
            // the rider sliding toward the rump. Anchor rigidly while moving.
            if (moving) {
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
            } else if (!mountSeatSmoothingInit_) {
                smoothedMountSeatPos_ = targetRiderPos;
                mountSeatSmoothingInit_ = true;
            } else {
                float smoothHz = taxiFlight_ ? 10.0f : 14.0f;
                float alpha = 1.0f - std::exp(-smoothHz * std::max(lastDeltaTime_, 0.001f));
                smoothedMountSeatPos_ = glm::mix(smoothedMountSeatPos_, targetRiderPos, alpha);
            }

            // Position rider at mount seat
            characterRenderer->setInstancePosition(characterInstanceId, smoothedMountSeatPos_);

            // Rider uses character facing yaw, not mount bone rotation
            // (rider faces character direction, seat bone only provides position)
            float yawRad = glm::radians(characterYaw);
            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, yawRad));
        } else {
            // Fallback to old manual positioning if attachment not found
            mountSeatSmoothingInit_ = false;
            float yawRad = glm::radians(characterYaw);
            glm::mat4 mountRotation = glm::mat4(1.0f);
            mountRotation = glm::rotate(mountRotation, yawRad, glm::vec3(0.0f, 0.0f, 1.0f));
            mountRotation = glm::rotate(mountRotation, mountRoll_, glm::vec3(1.0f, 0.0f, 0.0f));
            mountRotation = glm::rotate(mountRotation, mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
            glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
            glm::vec3 playerPos = characterPosition + worldOffset;
            characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll_, yawRad));
        }
        return;
    }

    if (!forceMelee) switch (charAnimState) {
        case CharAnimState::IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (inCombat_ && grounded) {
                newState = CharAnimState::COMBAT_IDLE;
            }
            break;

        case CharAnimState::WALK:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (sprinting) {
                newState = CharAnimState::RUN;
            }
            break;

        case CharAnimState::RUN:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (!sprinting) {
                newState = CharAnimState::WALK;
            }
            break;

        case CharAnimState::JUMP_START:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            } else {
                newState = CharAnimState::JUMP_MID;
            }
            break;

        case CharAnimState::JUMP_MID:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            }
            break;

        case CharAnimState::JUMP_END:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::SIT_DOWN:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::SITTING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::EMOTE:
            if (swim) {
                cancelEmote();
                newState = CharAnimState::SWIM_IDLE;
            } else if (jumping || !grounded) {
                cancelEmote();
                newState = CharAnimState::JUMP_START;
            } else if (moving) {
                cancelEmote();
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (sitting) {
                cancelEmote();
                newState = CharAnimState::SIT_DOWN;
            } else if (!emoteLoop && characterRenderer && characterInstanceId > 0) {
                // Auto-cancel non-looping emotes once animation completes
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)
                        && curDur > 0.1f && curT >= curDur - 0.05f) {
                    cancelEmote();
                    newState = CharAnimState::IDLE;
                }
            }
            break;

        case CharAnimState::SWIM_IDLE:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (moving) {
                newState = CharAnimState::SWIM;
            }
            break;

        case CharAnimState::SWIM:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (!moving) {
                newState = CharAnimState::SWIM_IDLE;
            }
            break;

        case CharAnimState::MELEE_SWING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (sitting) {
                newState = CharAnimState::SIT_DOWN;
            } else if (inCombat_) {
                newState = CharAnimState::COMBAT_IDLE;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::MOUNT:
            // If we got here, the mount state was cleared externally but the
            // animation state hasn't been reset yet. Fall back to normal logic.
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::COMBAT_IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (!inCombat_) {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::CHARGE:
            // Stay in CHARGE until charging_ is cleared
            break;
    }

    if (forceMelee) {
        newState = CharAnimState::MELEE_SWING;
    }

    if (charging_) {
        newState = CharAnimState::CHARGE;
    }

    if (newState != charAnimState) {
        charAnimState = newState;
    }

    auto pickFirstAvailable = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer->hasAnimation(characterInstanceId, id)) {
                return id;
            }
        }
        return fallback;
    };

    uint32_t animId = ANIM_STAND;
    bool loop = true;

    switch (charAnimState) {
        case CharAnimState::IDLE:       animId = ANIM_STAND;      loop = true;  break;
        case CharAnimState::WALK:
            if (movingBackward) {
                animId = pickFirstAvailable({ANIM_BACKPEDAL}, ANIM_WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({ANIM_STRAFE_WALK_LEFT, ANIM_STRAFE_RUN_LEFT}, ANIM_WALK);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({ANIM_STRAFE_WALK_RIGHT, ANIM_STRAFE_RUN_RIGHT}, ANIM_WALK);
            } else {
                animId = pickFirstAvailable({ANIM_WALK, ANIM_RUN}, ANIM_STAND);
            }
            loop = true;
            break;
        case CharAnimState::RUN:
            if (movingBackward) {
                animId = pickFirstAvailable({ANIM_BACKPEDAL}, ANIM_WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({ANIM_STRAFE_RUN_LEFT}, ANIM_RUN);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({ANIM_STRAFE_RUN_RIGHT}, ANIM_RUN);
            } else {
                animId = pickFirstAvailable({ANIM_RUN, ANIM_WALK}, ANIM_STAND);
            }
            loop = true;
            break;
        case CharAnimState::JUMP_START: animId = ANIM_JUMP_START; loop = false; break;
        case CharAnimState::JUMP_MID:   animId = ANIM_JUMP_MID;   loop = false; break;
        case CharAnimState::JUMP_END:   animId = ANIM_JUMP_END;   loop = false; break;
        case CharAnimState::SIT_DOWN:   animId = ANIM_SIT_DOWN;   loop = false; break;
        case CharAnimState::SITTING:    animId = ANIM_SITTING;    loop = true;  break;
        case CharAnimState::EMOTE:      animId = emoteAnimId;     loop = emoteLoop; break;
        case CharAnimState::SWIM_IDLE:  animId = ANIM_SWIM_IDLE;  loop = true;  break;
        case CharAnimState::SWIM:       animId = ANIM_SWIM;       loop = true;  break;
        case CharAnimState::MELEE_SWING:
            animId = resolveMeleeAnimId();
            if (animId == 0) {
                animId = ANIM_STAND;
            }
            loop = false;
            break;
        case CharAnimState::MOUNT:      animId = ANIM_MOUNT;      loop = true;  break;
        case CharAnimState::COMBAT_IDLE:
            animId = pickFirstAvailable(
                {ANIM_READY_1H, ANIM_READY_2H, ANIM_READY_2H_L, ANIM_READY_UNARMED},
                ANIM_STAND);
            loop = true;
            break;
        case CharAnimState::CHARGE:
            animId = ANIM_RUN;
            loop = true;
            break;
    }

    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f;
    float currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    // Some frames may transiently fail getAnimationState() while resources/instance state churn.
    // Avoid reissuing the same clip on those frames, which restarts locomotion and causes hitches.
    const bool requestChanged = (lastPlayerAnimRequest_ != animId) || (lastPlayerAnimLoopRequest_ != loop);
    const bool shouldPlay = (haveState && currentAnimId != animId) || (!haveState && requestChanged);
    if (shouldPlay) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }
}

void Renderer::playEmote(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it == EMOTE_TABLE.end()) return;

    const auto& info = it->second;
    if (info.animId == 0) return;
    emoteActive = true;
    emoteAnimId = info.animId;
    emoteLoop = info.loop;
    charAnimState = CharAnimState::EMOTE;

    if (characterRenderer && characterInstanceId > 0) {
        characterRenderer->playAnimation(characterInstanceId, emoteAnimId, emoteLoop);
    }
}

void Renderer::cancelEmote() {
    emoteActive = false;
    emoteAnimId = 0;
    emoteLoop = false;
}

void Renderer::triggerLevelUpEffect(const glm::vec3& position) {
    if (!levelUpEffect) return;

    // Lazy-load the M2 model on first trigger
    if (!levelUpEffect->isModelLoaded() && m2Renderer) {
        if (!cachedAssetManager) {
            cachedAssetManager = core::Application::getInstance().getAssetManager();
        }
        if (!cachedAssetManager) {
            LOG_WARNING("LevelUpEffect: no asset manager available");
        } else {
            auto m2Data = cachedAssetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
            auto skinData = cachedAssetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
            LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
            if (!m2Data.empty()) {
                levelUpEffect->loadModel(m2Renderer.get(), m2Data, skinData);
            } else {
                LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
            }
        }
    }

    levelUpEffect->trigger(position);
}

void Renderer::startChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (!chargeEffect) return;

    // Lazy-load M2 models on first use
    if (!chargeEffect->isActive() && m2Renderer) {
        if (!cachedAssetManager) {
            cachedAssetManager = core::Application::getInstance().getAssetManager();
        }
        if (cachedAssetManager) {
            chargeEffect->tryLoadM2Models(m2Renderer.get(), cachedAssetManager);
        }
    }

    chargeEffect->start(position, direction);
}

void Renderer::emitChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (chargeEffect) {
        chargeEffect->emit(position, direction);
    }
}

void Renderer::stopChargeEffect() {
    if (chargeEffect) {
        chargeEffect->stop();
    }
}

void Renderer::triggerMeleeSwing() {
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown > 0.0f) return;
    if (emoteActive) {
        cancelEmote();
    }
    resolveMeleeAnimId();
    meleeSwingCooldown = 0.1f;
    float durationSec = meleeAnimDurationMs > 0.0f ? meleeAnimDurationMs / 1000.0f : 0.6f;
    if (durationSec < 0.25f) durationSec = 0.25f;
    if (durationSec > 1.0f) durationSec = 1.0f;
    meleeSwingTimer = durationSec;
    if (activitySoundManager) {
        activitySoundManager->playMeleeSwing();
    }
}

std::string Renderer::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        const auto& info = it->second;
        const std::string& base = (targetName ? info.textTarget : info.textNoTarget);
        if (!base.empty()) {
            return replacePlaceholders(base, targetName);
        }
        if (targetName && !targetName->empty()) {
            return "You " + info.command + " at " + *targetName + ".";
        }
        return "You " + info.command + ".";
    }
    return "";
}

uint32_t Renderer::getEmoteDbcId(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        return it->second.dbcId;
    }
    return 0;
}

std::string Renderer::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                           const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it == EMOTE_BY_DBCID.end()) return "";

    const EmoteInfo& info = *it->second;

    // Use "others see" text templates: "%s dances." / "%s dances with %s."
    if (targetName && !targetName->empty()) {
        if (!info.othersTarget.empty()) {
            // Replace first %s with sender, second %s with target
            std::string out;
            out.reserve(info.othersTarget.size() + senderName.size() + targetName->size());
            bool firstReplaced = false;
            for (size_t i = 0; i < info.othersTarget.size(); ++i) {
                if (info.othersTarget[i] == '%' && i + 1 < info.othersTarget.size() && info.othersTarget[i + 1] == 's') {
                    out += firstReplaced ? *targetName : senderName;
                    firstReplaced = true;
                    ++i;
                } else {
                    out.push_back(info.othersTarget[i]);
                }
            }
            return out;
        }
        return senderName + " " + info.command + "s at " + *targetName + ".";
    } else {
        if (!info.othersNoTarget.empty()) {
            return replacePlaceholders(info.othersNoTarget, &senderName);
        }
        return senderName + " " + info.command + "s.";
    }
}

uint32_t Renderer::getEmoteAnimByDbcId(uint32_t dbcId) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it != EMOTE_BY_DBCID.end()) {
        return it->second->animId;
    }
    return 0;
}

void Renderer::setTargetPosition(const glm::vec3* pos) {
    targetPosition = pos;
}

void Renderer::resetCombatVisualState() {
    inCombat_ = false;
    targetPosition = nullptr;
    meleeSwingTimer = 0.0f;
    meleeSwingCooldown = 0.0f;
}

bool Renderer::isMoving() const {
    return cameraController && cameraController->isMoving();
}

bool Renderer::isFootstepAnimationState() const {
    return charAnimState == CharAnimState::WALK || charAnimState == CharAnimState::RUN;
}

bool Renderer::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs) {
    if (animationDurationMs <= 1.0f) {
        footstepNormInitialized = false;
        return false;
    }

    // Wrap animation time preserving precision via subtraction instead of fmod
    float wrappedTime = animationTimeMs;
    while (wrappedTime >= animationDurationMs) {
        wrappedTime -= animationDurationMs;
    }
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    float norm = wrappedTime / animationDurationMs;

    if (animationId != footstepLastAnimationId) {
        footstepLastAnimationId = animationId;
        footstepLastNormTime = norm;
        footstepNormInitialized = true;
        return false;
    }

    if (!footstepNormInitialized) {
        footstepNormInitialized = true;
        footstepLastNormTime = norm;
        return false;
    }

    auto crossed = [&](float eventNorm) {
        if (footstepLastNormTime <= norm) {
            return footstepLastNormTime < eventNorm && eventNorm <= norm;
        }
        return footstepLastNormTime < eventNorm || eventNorm <= norm;
    };

    bool trigger = crossed(0.22f) || crossed(0.72f);
    footstepLastNormTime = norm;
    return trigger;
}

audio::FootstepSurface Renderer::resolveFootstepSurface() const {
    if (!cameraController || !cameraController->isThirdPerson()) {
        return audio::FootstepSurface::STONE;
    }

    const glm::vec3& p = characterPosition;

    // Cache footstep surface to avoid expensive queries every step
    // Only update if moved >1.5 units or timer expired (0.5s)
    float distSq = glm::dot(p - cachedFootstepPosition, p - cachedFootstepPosition);
    if (distSq < 2.25f && cachedFootstepUpdateTimer < 0.5f) {
        return cachedFootstepSurface;
    }

    // Update cache
    cachedFootstepPosition = p;
    cachedFootstepUpdateTimer = 0.0f;

    if (cameraController->isSwimming()) {
        cachedFootstepSurface = audio::FootstepSurface::WATER;
        return audio::FootstepSurface::WATER;
    }

    if (waterRenderer) {
        auto waterH = waterRenderer->getWaterHeightAt(p.x, p.y);
        if (waterH && p.z < (*waterH + 0.25f)) {
            cachedFootstepSurface = audio::FootstepSurface::WATER;
            return audio::FootstepSurface::WATER;
        }
    }

    if (wmoRenderer) {
        auto wmoFloor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
        auto terrainFloor = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
        if (wmoFloor && (!terrainFloor || *wmoFloor >= *terrainFloor - 0.1f)) {
            cachedFootstepSurface = audio::FootstepSurface::STONE;
            return audio::FootstepSurface::STONE;
        }
    }

    // Determine surface type (expensive - only done when cache needs update)
    audio::FootstepSurface surface = audio::FootstepSurface::STONE;

    if (terrainManager) {
        auto texture = terrainManager->getDominantTextureAt(p.x, p.y);
        if (texture) {
            std::string t = *texture;
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t.find("snow") != std::string::npos || t.find("ice") != std::string::npos) surface = audio::FootstepSurface::SNOW;
            else if (t.find("grass") != std::string::npos || t.find("moss") != std::string::npos || t.find("leaf") != std::string::npos) surface = audio::FootstepSurface::GRASS;
            else if (t.find("sand") != std::string::npos || t.find("dirt") != std::string::npos || t.find("mud") != std::string::npos) surface = audio::FootstepSurface::DIRT;
            else if (t.find("wood") != std::string::npos || t.find("timber") != std::string::npos) surface = audio::FootstepSurface::WOOD;
            else if (t.find("metal") != std::string::npos || t.find("iron") != std::string::npos) surface = audio::FootstepSurface::METAL;
            else if (t.find("stone") != std::string::npos || t.find("rock") != std::string::npos || t.find("cobble") != std::string::npos || t.find("brick") != std::string::npos) surface = audio::FootstepSurface::STONE;
        }
    }

    cachedFootstepSurface = surface;
    return surface;
}

void Renderer::update(float deltaTime) {
    globalTime += deltaTime;
    if (musicSwitchCooldown_ > 0.0f) {
        musicSwitchCooldown_ = std::max(0.0f, musicSwitchCooldown_ - deltaTime);
    }
    runDeferredWorldInitStep(deltaTime);

    auto updateStart = std::chrono::steady_clock::now();
    lastDeltaTime_ = deltaTime;  // Cache for use in updateCharacterAnimation()

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
        if ((cameraController->isThirdPerson() && !cameraController->isFirstPersonView()) || taxiFlight_) {
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
                else if (wType == 3) weather->setWeatherType(Weather::Type::RAIN); // thunderstorm — use rain particles
                else                 weather->setWeatherType(Weather::Type::NONE);
                weather->setIntensity(wInt);
            } else {
                // No server weather — use zone-based weather configuration
                weather->updateZoneWeather(currentZoneId, deltaTime);
            }
            weather->setEnabled(true);

            // Enable lightning during storms (wType==3) and heavy rain
            if (lightning) {
                uint32_t wType2 = gh->getWeatherType();
                float wInt2 = gh->getWeatherIntensity();
                bool stormActive = (wType2 == 3 && wInt2 > 0.1f)
                                || (wType2 == 1 && wInt2 > 0.7f);
                lightning->setEnabled(stormActive);
                if (stormActive) {
                    // Scale intensity: storm at full, heavy rain proportionally
                    float lIntensity = (wType2 == 3) ? wInt2 : (wInt2 - 0.7f) / 0.3f;
                    lightning->setIntensity(lIntensity);
                }
            }
        } else if (weather) {
            // No game handler (single-player without network) — zone weather only
            weather->updateZoneWeather(currentZoneId, deltaTime);
            weather->setEnabled(true);
        }
    }

    // Sync character model position/rotation and animation with follow target
    if (characterInstanceId > 0 && characterRenderer && cameraController) {
        if (meleeSwingCooldown > 0.0f) {
            meleeSwingCooldown = std::max(0.0f, meleeSwingCooldown - deltaTime);
        }
        if (meleeSwingTimer > 0.0f) {
            meleeSwingTimer = std::max(0.0f, meleeSwingTimer - deltaTime);
        }

        characterRenderer->setInstancePosition(characterInstanceId, characterPosition);

        // Movement-facing comes from camera controller and is decoupled from LMB orbit.
        // During taxi flights, orientation is controlled by the flight path (not player input)
        if (taxiFlight_) {
            // Taxi flight: use orientation from flight path
            characterYaw = cameraController->getFacingYaw();
        } else if (cameraController->isMoving() || cameraController->isRightMouseHeld()) {
            characterYaw = cameraController->getFacingYaw();
        } else if (inCombat_ && targetPosition && !emoteActive && !isMounted()) {
            // Face target when in combat and idle
            glm::vec3 toTarget = *targetPosition - characterPosition;
            if (glm::length(glm::vec2(toTarget.x, toTarget.y)) > 0.1f) {
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

        // Update animation based on movement state
        updateCharacterAnimation();
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
        if (isMounted() && camera && cameraController && !taxiFlight_) {
            bool isMoving = cameraController->isMoving();
            bool onGround = cameraController->isGrounded();

            if (isMoving && onGround) {
                // Calculate velocity from camera direction and speed
                glm::vec3 forward = camera->getForward();
                float speed = cameraController->getMovementSpeed();
                glm::vec3 velocity = forward * speed;
                velocity.z = 0.0f;  // Ignore vertical component

                // Spawn dust at mount's feet (slightly below character position)
                glm::vec3 dustPos = characterPosition - glm::vec3(0.0f, 0.0f, mountHeightOffset_ * 0.8f);
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

    // Footsteps: animation-event driven + surface query at event time.
    if (footstepManager) {
        footstepManager->update(deltaTime);
        cachedFootstepUpdateTimer += deltaTime;  // Update surface cache timer
        bool canPlayFootsteps = characterRenderer && characterInstanceId > 0 &&
            cameraController && cameraController->isThirdPerson() &&
            cameraController->isGrounded() && !cameraController->isSwimming();

        if (canPlayFootsteps && isMounted() && mountInstanceId_ > 0 && !taxiFlight_) {
            // Mount footsteps: use mount's animation for timing
            uint32_t animId = 0;
            float animTimeMs = 0.0f, animDurationMs = 0.0f;
            if (characterRenderer->getAnimationState(mountInstanceId_, animId, animTimeMs, animDurationMs) &&
                animDurationMs > 1.0f && cameraController->isMoving()) {
                // Wrap animation time preserving precision via subtraction instead of fmod
                float wrappedTime = animTimeMs;
                while (wrappedTime >= animDurationMs) {
                    wrappedTime -= animDurationMs;
                }
                if (wrappedTime < 0.0f) wrappedTime += animDurationMs;
                float norm = wrappedTime / animDurationMs;

                if (animId != mountFootstepLastAnimId) {
                    mountFootstepLastAnimId = animId;
                    mountFootstepLastNormTime = norm;
                    mountFootstepNormInitialized = true;
                } else if (!mountFootstepNormInitialized) {
                    mountFootstepNormInitialized = true;
                    mountFootstepLastNormTime = norm;
                } else {
                    // Mount gait: 2 hoofbeats per cycle (synced with animation)
                    auto crossed = [&](float eventNorm) {
                        if (mountFootstepLastNormTime <= norm) {
                            return mountFootstepLastNormTime < eventNorm && eventNorm <= norm;
                        }
                        return mountFootstepLastNormTime < eventNorm || eventNorm <= norm;
                    };
                    if (crossed(0.25f) || crossed(0.75f)) {
                        footstepManager->playFootstep(resolveFootstepSurface(), true);
                    }
                    mountFootstepLastNormTime = norm;
                }
            } else {
                mountFootstepNormInitialized = false;
            }
            footstepNormInitialized = false;  // Reset player footstep tracking
        } else if (canPlayFootsteps && isFootstepAnimationState()) {
            uint32_t animId = 0;
            float animTimeMs = 0.0f;
            float animDurationMs = 0.0f;
            if (characterRenderer->getAnimationState(characterInstanceId, animId, animTimeMs, animDurationMs) &&
                shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs)) {
                auto surface = resolveFootstepSurface();
                footstepManager->playFootstep(surface, cameraController->isSprinting());
                // Play additional splash sound and spawn foot splash particles when wading
                if (surface == audio::FootstepSurface::WATER) {
                    if (movementSoundManager) {
                        movementSoundManager->playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
                    }
                    if (swimEffects && waterRenderer) {
                        auto wh = waterRenderer->getWaterHeightAt(characterPosition.x, characterPosition.y);
                        if (wh) {
                            swimEffects->spawnFootSplash(characterPosition, *wh);
                        }
                    }
                }
            }
            mountFootstepNormInitialized = false;
        } else {
            footstepNormInitialized = false;
            mountFootstepNormInitialized = false;
        }
    }

    // Activity SFX: animation/state-driven jump, landing, and swim loops/splashes.
    if (activitySoundManager) {
        activitySoundManager->update(deltaTime);
        if (cameraController && cameraController->isThirdPerson()) {
            bool grounded = cameraController->isGrounded();
            bool jumping = cameraController->isJumping();
            bool falling = cameraController->isFalling();
            bool swimming = cameraController->isSwimming();
            bool moving = cameraController->isMoving();

            if (!sfxStateInitialized) {
                sfxPrevGrounded = grounded;
                sfxPrevJumping = jumping;
                sfxPrevFalling = falling;
                sfxPrevSwimming = swimming;
                sfxStateInitialized = true;
            }

            if (jumping && !sfxPrevJumping && !swimming) {
                activitySoundManager->playJump();
            }

            if (grounded && !sfxPrevGrounded) {
                bool hardLanding = sfxPrevFalling;
                activitySoundManager->playLanding(resolveFootstepSurface(), hardLanding);
            }

            if (swimming && !sfxPrevSwimming) {
                activitySoundManager->playWaterEnter();
            } else if (!swimming && sfxPrevSwimming) {
                activitySoundManager->playWaterExit();
            }

            activitySoundManager->setSwimmingState(swimming, moving);

            // Fade music underwater
            if (musicManager) {
                musicManager->setUnderwaterMode(swimming);
            }

            sfxPrevGrounded = grounded;
            sfxPrevJumping = jumping;
            sfxPrevFalling = falling;
            sfxPrevSwimming = swimming;
        } else {
            activitySoundManager->setSwimmingState(false, false);
            // Restore music volume when activity sounds disabled
            if (musicManager) {
                musicManager->setUnderwaterMode(false);
            }
            sfxStateInitialized = false;
        }
    }

    // Mount ambient sounds: wing flaps, breathing, etc.
    if (mountSoundManager) {
        mountSoundManager->update(deltaTime);
        if (cameraController && isMounted()) {
            bool moving = cameraController->isMoving();
            bool flying = taxiFlight_ || !cameraController->isGrounded();  // Flying if taxi or airborne
            mountSoundManager->setMoving(moving);
            mountSoundManager->setFlying(flying);
        }
    }

    const bool canQueryWmo = (camera && wmoRenderer);
    const glm::vec3 camPos = camera ? camera->getPosition() : glm::vec3(0.0f);
    uint32_t insideWmoId = 0;
    const bool insideWmo = canQueryWmo &&
        wmoRenderer->isInsideWMO(camPos.x, camPos.y, camPos.z, &insideWmoId);

    // Ambient environmental sounds: fireplaces, water, birds, etc.
    if (ambientSoundManager && camera && wmoRenderer && cameraController) {
        bool isIndoor = insideWmo;
        bool isSwimming = cameraController->isSwimming();

        // Check if inside blacksmith (96048 = Goldshire blacksmith)
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

            ambientSoundManager->setWeather(audioWeatherType);
        }

        ambientSoundManager->update(deltaTime, camPos, isIndoor, isSwimming, isBlacksmith);
    }

    // Wait for M2 doodad animation to finish (was launched earlier in parallel with character anim)
    if (m2AnimLaunched) {
        m2AnimFuture.get();
    }

    // Helper: play zone music, dispatching local files (file: prefix) vs MPQ paths
    auto playZoneMusic = [&](const std::string& music) {
        if (music.empty()) return;
        if (music.rfind("file:", 0) == 0) {
            musicManager->crossfadeToFile(music.substr(5));
        } else {
            musicManager->crossfadeTo(music);
        }
    };

    // Update zone detection and music
    if (zoneManager && musicManager && terrainManager && camera) {
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

                // Blacksmith detection
                if (wmoModelId == 96048) {  // Goldshire blacksmith
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
                    static int tavernTrackIndex = 0;
                    tavernMusic = tavernTracks[tavernTrackIndex % tavernTracks.size()];
                    LOG_INFO("Detected tavern WMO ", wmoModelId, ", playing: ", tavernMusic);
                }
            }
        }

        // Handle tavern music transitions
        if (insideTavern) {
            if (!inTavern_ && !tavernMusic.empty()) {
                inTavern_ = true;
                LOG_INFO("Entered tavern");
                musicManager->playMusic(tavernMusic, true);  // Immediate playback, looping
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
                musicManager->stopMusic();
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
            if (ambientSoundManager) {
                ambientSoundManager->setZoneId(zoneId);
            }
        }

        musicManager->update(deltaTime);

        // When a track finishes, pick a new random track from the current zone
        if (!musicManager->isPlaying() && !inTavern_ && !inBlacksmith_ &&
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
            if (ambientSoundManager) {
                ambientSoundManager->initialize(cachedAssetManager);
            }
            if (terrainManager && ambientSoundManager) {
                terrainManager->setAmbientSoundManager(ambientSoundManager.get());
            }
            break;
        case 1:
            if (uiSoundManager) uiSoundManager->initialize(cachedAssetManager);
            break;
        case 2:
            if (combatSoundManager) combatSoundManager->initialize(cachedAssetManager);
            break;
        case 3:
            if (spellSoundManager) spellSoundManager->initialize(cachedAssetManager);
            break;
        case 4:
            if (movementSoundManager) movementSoundManager->initialize(cachedAssetManager);
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
        .build(device);

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
        .build(device);

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

// ========================= FSR 1.0 Upscaling =========================

bool Renderer::initFSRResources() {
    if (!vkCtx) return false;

    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    VkExtent2D swapExtent = vkCtx->getSwapchainExtent();
    VkSampleCountFlagBits msaa = vkCtx->getMsaaSamples();
    bool useMsaa = (msaa > VK_SAMPLE_COUNT_1_BIT);
    bool useDepthResolve = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);

    fsr_.internalWidth = static_cast<uint32_t>(swapExtent.width * fsr_.scaleFactor);
    fsr_.internalHeight = static_cast<uint32_t>(swapExtent.height * fsr_.scaleFactor);
    fsr_.internalWidth = (fsr_.internalWidth + 1) & ~1u;
    fsr_.internalHeight = (fsr_.internalHeight + 1) & ~1u;

    LOG_INFO("FSR: initializing at ", fsr_.internalWidth, "x", fsr_.internalHeight,
             " -> ", swapExtent.width, "x", swapExtent.height,
             " (scale=", fsr_.scaleFactor, ", MSAA=", static_cast<int>(msaa), "x)");

    VkFormat colorFmt = vkCtx->getSwapchainFormat();
    VkFormat depthFmt = vkCtx->getDepthFormat();

    // sceneColor: always 1x, always sampled — this is what FSR reads
    // Non-MSAA: direct render target. MSAA: resolve target.
    fsr_.sceneColor = createImage(device, alloc, fsr_.internalWidth, fsr_.internalHeight,
        colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fsr_.sceneColor.image) {
        LOG_ERROR("FSR: failed to create scene color image");
        return false;
    }

    // sceneDepth: matches current MSAA sample count
    fsr_.sceneDepth = createImage(device, alloc, fsr_.internalWidth, fsr_.internalHeight,
        depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, msaa);
    if (!fsr_.sceneDepth.image) {
        LOG_ERROR("FSR: failed to create scene depth image");
        destroyFSRResources();
        return false;
    }

    if (useMsaa) {
        // sceneMsaaColor: multisampled color target
        fsr_.sceneMsaaColor = createImage(device, alloc, fsr_.internalWidth, fsr_.internalHeight,
            colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, msaa);
        if (!fsr_.sceneMsaaColor.image) {
            LOG_ERROR("FSR: failed to create MSAA color image");
            destroyFSRResources();
            return false;
        }

        if (useDepthResolve) {
            fsr_.sceneDepthResolve = createImage(device, alloc, fsr_.internalWidth, fsr_.internalHeight,
                depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            if (!fsr_.sceneDepthResolve.image) {
                LOG_ERROR("FSR: failed to create depth resolve image");
                destroyFSRResources();
                return false;
            }
        }
    }

    // Build framebuffer matching the main render pass attachment layout:
    //   Non-MSAA:              [color, depth]
    //   MSAA (no depth res):   [msaaColor, depth, resolve]
    //   MSAA (depth res):      [msaaColor, depth, resolve, depthResolve]
    VkImageView fbAttachments[4]{};
    uint32_t fbCount;
    if (useMsaa) {
        fbAttachments[0] = fsr_.sceneMsaaColor.imageView;
        fbAttachments[1] = fsr_.sceneDepth.imageView;
        fbAttachments[2] = fsr_.sceneColor.imageView;  // resolve target
        fbCount = 3;
        if (useDepthResolve) {
            fbAttachments[3] = fsr_.sceneDepthResolve.imageView;
            fbCount = 4;
        }
    } else {
        fbAttachments[0] = fsr_.sceneColor.imageView;
        fbAttachments[1] = fsr_.sceneDepth.imageView;
        fbCount = 2;
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = vkCtx->getImGuiRenderPass();
    fbInfo.attachmentCount = fbCount;
    fbInfo.pAttachments = fbAttachments;
    fbInfo.width = fsr_.internalWidth;
    fbInfo.height = fsr_.internalHeight;
    fbInfo.layers = 1;

    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fsr_.sceneFramebuffer) != VK_SUCCESS) {
        LOG_ERROR("FSR: failed to create scene framebuffer");
        destroyFSRResources();
        return false;
    }

    // Sampler for the resolved scene color
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &fsr_.sceneSampler) != VK_SUCCESS) {
        LOG_ERROR("FSR: failed to create sampler");
        destroyFSRResources();
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fsr_.descSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &fsr_.descPool);

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = fsr_.descPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &fsr_.descSetLayout;
    vkAllocateDescriptorSets(device, &dsAllocInfo, &fsr_.descSet);

    // Always bind the 1x sceneColor (FSR reads the resolved image)
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = fsr_.sceneSampler;
    imgInfo.imageView = fsr_.sceneColor.imageView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = fsr_.descSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Pipeline layout
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 64;
    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &fsr_.descSetLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &fsr_.pipelineLayout);

    // Load shaders
    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/fsr_easu.frag.spv")) {
        LOG_ERROR("FSR: failed to load shaders");
        destroyFSRResources();
        return false;
    }

    // FSR upscale pipeline renders into the swapchain pass at full resolution
    // Must match swapchain pass MSAA setting
    fsr_.pipeline = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(msaa)
        .setLayout(fsr_.pipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device);

    vertMod.destroy();
    fragMod.destroy();

    if (!fsr_.pipeline) {
        LOG_ERROR("FSR: failed to create upscale pipeline");
        destroyFSRResources();
        return false;
    }

    LOG_INFO("FSR: initialized successfully");
    return true;
}

void Renderer::destroyFSRResources() {
    if (!vkCtx) return;

    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    vkDeviceWaitIdle(device);

    if (fsr_.pipeline) { vkDestroyPipeline(device, fsr_.pipeline, nullptr); fsr_.pipeline = VK_NULL_HANDLE; }
    if (fsr_.pipelineLayout) { vkDestroyPipelineLayout(device, fsr_.pipelineLayout, nullptr); fsr_.pipelineLayout = VK_NULL_HANDLE; }
    if (fsr_.descPool) { vkDestroyDescriptorPool(device, fsr_.descPool, nullptr); fsr_.descPool = VK_NULL_HANDLE; fsr_.descSet = VK_NULL_HANDLE; }
    if (fsr_.descSetLayout) { vkDestroyDescriptorSetLayout(device, fsr_.descSetLayout, nullptr); fsr_.descSetLayout = VK_NULL_HANDLE; }
    if (fsr_.sceneFramebuffer) { vkDestroyFramebuffer(device, fsr_.sceneFramebuffer, nullptr); fsr_.sceneFramebuffer = VK_NULL_HANDLE; }
    if (fsr_.sceneSampler) { vkDestroySampler(device, fsr_.sceneSampler, nullptr); fsr_.sceneSampler = VK_NULL_HANDLE; }
    destroyImage(device, alloc, fsr_.sceneDepthResolve);
    destroyImage(device, alloc, fsr_.sceneMsaaColor);
    destroyImage(device, alloc, fsr_.sceneDepth);
    destroyImage(device, alloc, fsr_.sceneColor);

    fsr_.internalWidth = 0;
    fsr_.internalHeight = 0;
}

void Renderer::renderFSRUpscale() {
    if (!fsr_.pipeline || currentCmd == VK_NULL_HANDLE) return;

    VkExtent2D outExtent = vkCtx->getSwapchainExtent();
    float inW = static_cast<float>(fsr_.internalWidth);
    float inH = static_cast<float>(fsr_.internalHeight);
    float outW = static_cast<float>(outExtent.width);
    float outH = static_cast<float>(outExtent.height);

    // FSR push constants
    struct {
        glm::vec4 con0;  // inputSize.xy, 1/inputSize.xy
        glm::vec4 con1;  // inputSize.xy / outputSize.xy, 0.5 * inputSize.xy / outputSize.xy
        glm::vec4 con2;  // outputSize.xy, 1/outputSize.xy
        glm::vec4 con3;  // sharpness, 0, 0, 0
    } fsrConst;

    fsrConst.con0 = glm::vec4(inW, inH, 1.0f / inW, 1.0f / inH);
    fsrConst.con1 = glm::vec4(inW / outW, inH / outH, 0.5f * inW / outW, 0.5f * inH / outH);
    fsrConst.con2 = glm::vec4(outW, outH, 1.0f / outW, 1.0f / outH);
    fsrConst.con3 = glm::vec4(fsr_.sharpness, 0.0f, 0.0f, 0.0f);

    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr_.pipeline);
    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        fsr_.pipelineLayout, 0, 1, &fsr_.descSet, 0, nullptr);
    vkCmdPushConstants(currentCmd, fsr_.pipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, &fsrConst);
    vkCmdDraw(currentCmd, 3, 1, 0, 0);
}

void Renderer::setFSREnabled(bool enabled) {
    if (fsr_.enabled == enabled) return;
    fsr_.enabled = enabled;

    if (enabled) {
        // FSR1 upscaling renders its own AA — disable MSAA to avoid redundant work
        if (vkCtx && vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
            pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
            msaaChangePending_ = true;
        }
    } else {
        // Defer destruction to next beginFrame() — can't destroy mid-render
        fsr_.needsRecreate = true;
    }
    // Resources created/destroyed lazily in beginFrame()
}

void Renderer::setFSRQuality(float scaleFactor) {
    scaleFactor = glm::clamp(scaleFactor, 0.5f, 1.0f);
    fsr_.scaleFactor = scaleFactor;
    fsr2_.scaleFactor = scaleFactor;
    // Don't destroy/recreate mid-frame — mark for lazy recreation in next beginFrame()
    if (fsr_.enabled && fsr_.sceneFramebuffer) {
        fsr_.needsRecreate = true;
    }
    if (fsr2_.enabled && fsr2_.sceneFramebuffer) {
        fsr2_.needsRecreate = true;
        fsr2_.needsHistoryReset = true;
    }
}

void Renderer::setFSRSharpness(float sharpness) {
    fsr_.sharpness = glm::clamp(sharpness, 0.0f, 2.0f);
    fsr2_.sharpness = glm::clamp(sharpness, 0.0f, 2.0f);
}

// ========================= End FSR 1.0 =========================

// ========================= FSR 2.2 Temporal Upscaling =========================

float Renderer::halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    uint32_t current = index;
    while (current > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(current % base);
        current /= base;
    }
    return r;
}

bool Renderer::initFSR2Resources() {
    if (!vkCtx) return false;

    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    VkExtent2D swapExtent = vkCtx->getSwapchainExtent();

    // Temporary stability fallback: keep FSR2 path at native internal resolution
    // until temporal reprojection is reworked.
    fsr2_.internalWidth = static_cast<uint32_t>(swapExtent.width * fsr2_.scaleFactor);
    fsr2_.internalHeight = static_cast<uint32_t>(swapExtent.height * fsr2_.scaleFactor);
    fsr2_.internalWidth = (fsr2_.internalWidth + 1) & ~1u;
    fsr2_.internalHeight = (fsr2_.internalHeight + 1) & ~1u;

    LOG_INFO("FSR2: initializing at ", fsr2_.internalWidth, "x", fsr2_.internalHeight,
             " -> ", swapExtent.width, "x", swapExtent.height,
             " (scale=", fsr2_.scaleFactor, ")");
    fsr2_.useAmdBackend = false;
    fsr2_.amdFsr3FramegenRuntimeActive = false;
    fsr2_.amdFsr3FramegenRuntimeReady = false;
    fsr2_.framegenOutputValid = false;
    fsr2_.amdFsr3RuntimePath = "Path C";
    fsr2_.amdFsr3RuntimeLastError.clear();
    fsr2_.amdFsr3UpscaleDispatchCount = 0;
    fsr2_.amdFsr3FramegenDispatchCount = 0;
    fsr2_.amdFsr3FallbackCount = 0;
    fsr2_.amdFsr3InteropSyncValue = 1;
#if WOWEE_HAS_AMD_FSR2
    LOG_INFO("FSR2: AMD FidelityFX SDK detected at build time.");
#else
    LOG_WARNING("FSR2: AMD FidelityFX SDK not detected; using internal fallback path.");
#endif

    VkFormat colorFmt = vkCtx->getSwapchainFormat();
    VkFormat depthFmt = vkCtx->getDepthFormat();

    // Scene color (internal resolution, 1x — FSR2 replaces MSAA)
    fsr2_.sceneColor = createImage(device, alloc, fsr2_.internalWidth, fsr2_.internalHeight,
        colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fsr2_.sceneColor.image) { LOG_ERROR("FSR2: failed to create scene color"); return false; }

    // Scene depth (internal resolution, 1x, sampled for motion vectors)
    fsr2_.sceneDepth = createImage(device, alloc, fsr2_.internalWidth, fsr2_.internalHeight,
        depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fsr2_.sceneDepth.image) { LOG_ERROR("FSR2: failed to create scene depth"); destroyFSR2Resources(); return false; }

    // Motion vector buffer (internal resolution)
    fsr2_.motionVectors = createImage(device, alloc, fsr2_.internalWidth, fsr2_.internalHeight,
        VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fsr2_.motionVectors.image) { LOG_ERROR("FSR2: failed to create motion vectors"); destroyFSR2Resources(); return false; }

    // History buffers (display resolution, ping-pong)
    for (int i = 0; i < 2; i++) {
        fsr2_.history[i] = createImage(device, alloc, swapExtent.width, swapExtent.height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (!fsr2_.history[i].image) { LOG_ERROR("FSR2: failed to create history buffer ", i); destroyFSR2Resources(); return false; }
    }
    fsr2_.framegenOutput = createImage(device, alloc, swapExtent.width, swapExtent.height,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fsr2_.framegenOutput.image) { LOG_ERROR("FSR2: failed to create framegen output"); destroyFSR2Resources(); return false; }

    // Scene framebuffer (non-MSAA: [color, depth])
    // Must use the same render pass as the swapchain — which must be non-MSAA when FSR2 is active
    VkImageView fbAttachments[2] = { fsr2_.sceneColor.imageView, fsr2_.sceneDepth.imageView };
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = vkCtx->getImGuiRenderPass();
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = fbAttachments;
    fbInfo.width = fsr2_.internalWidth;
    fbInfo.height = fsr2_.internalHeight;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fsr2_.sceneFramebuffer) != VK_SUCCESS) {
        LOG_ERROR("FSR2: failed to create scene framebuffer");
        destroyFSR2Resources();
        return false;
    }

    // Samplers
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device, &samplerInfo, nullptr, &fsr2_.linearSampler);

    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    vkCreateSampler(device, &samplerInfo, nullptr, &fsr2_.nearestSampler);

#if WOWEE_HAS_AMD_FSR2
    // Initialize AMD FSR2 context; fall back to internal path on any failure.
    fsr2_.amdScratchBufferSize = ffxFsr2GetScratchMemorySizeVK(vkCtx->getPhysicalDevice());
    if (fsr2_.amdScratchBufferSize > 0) {
        fsr2_.amdScratchBuffer = std::malloc(fsr2_.amdScratchBufferSize);
    }
    if (!fsr2_.amdScratchBuffer) {
        LOG_WARNING("FSR2 AMD: failed to allocate scratch buffer, using internal fallback.");
    } else {
        FfxErrorCode ifaceErr = ffxFsr2GetInterfaceVK(
            &fsr2_.amdInterface,
            fsr2_.amdScratchBuffer,
            fsr2_.amdScratchBufferSize,
            vkCtx->getPhysicalDevice(),
            vkGetDeviceProcAddr);
        if (ifaceErr != FFX_OK) {
            LOG_WARNING("FSR2 AMD: ffxFsr2GetInterfaceVK failed (", static_cast<int>(ifaceErr), "), using internal fallback.");
            std::free(fsr2_.amdScratchBuffer);
            fsr2_.amdScratchBuffer = nullptr;
            fsr2_.amdScratchBufferSize = 0;
        } else {
            FfxFsr2ContextDescription ctxDesc{};
            ctxDesc.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
            ctxDesc.maxRenderSize.width = fsr2_.internalWidth;
            ctxDesc.maxRenderSize.height = fsr2_.internalHeight;
            ctxDesc.displaySize.width = swapExtent.width;
            ctxDesc.displaySize.height = swapExtent.height;
            ctxDesc.callbacks = fsr2_.amdInterface;
            ctxDesc.device = ffxGetDeviceVK(vkCtx->getDevice());
            ctxDesc.fpMessage = nullptr;

            FfxErrorCode ctxErr = ffxFsr2ContextCreate(&fsr2_.amdContext, &ctxDesc);
            if (ctxErr == FFX_OK) {
                fsr2_.useAmdBackend = true;
                LOG_INFO("FSR2 AMD: context created successfully.");
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
                if (fsr2_.amdFsr3FramegenEnabled) {
                    fsr2_.amdFsr3FramegenRuntimeActive = false;
                    if (!fsr2_.amdFsr3Runtime) fsr2_.amdFsr3Runtime = std::make_unique<AmdFsr3Runtime>();
                    AmdFsr3RuntimeInitDesc fgInit{};
                    fgInit.physicalDevice = vkCtx->getPhysicalDevice();
                    fgInit.device = vkCtx->getDevice();
                    fgInit.getDeviceProcAddr = vkGetDeviceProcAddr;
                    fgInit.maxRenderWidth = fsr2_.internalWidth;
                    fgInit.maxRenderHeight = fsr2_.internalHeight;
                    fgInit.displayWidth = swapExtent.width;
                    fgInit.displayHeight = swapExtent.height;
                    fgInit.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
                    fgInit.hdrInput = false;
                    fgInit.depthInverted = false;
                    fgInit.enableFrameGeneration = true;
                    fsr2_.amdFsr3FramegenRuntimeReady = fsr2_.amdFsr3Runtime->initialize(fgInit);
                    if (fsr2_.amdFsr3FramegenRuntimeReady) {
                        fsr2_.amdFsr3RuntimeLastError.clear();
                        fsr2_.amdFsr3RuntimePath = "Path A";
                        LOG_INFO("FSR3 framegen runtime library loaded from ", fsr2_.amdFsr3Runtime->loadedLibraryPath(),
                                 " (upscale+framegen dispatch enabled)");
                    } else {
                        fsr2_.amdFsr3RuntimePath = "Path C";
                        fsr2_.amdFsr3RuntimeLastError = fsr2_.amdFsr3Runtime->lastError();
                        LOG_WARNING("FSR3 framegen toggle is enabled, but runtime initialization failed. ",
                                    "path=", fsr2_.amdFsr3RuntimePath,
                                    " error=", fsr2_.amdFsr3RuntimeLastError.empty() ? "(none)" : fsr2_.amdFsr3RuntimeLastError,
                                    " runtimeLib=", fsr2_.amdFsr3Runtime->loadedLibraryPath().empty() ? "(not loaded)" : fsr2_.amdFsr3Runtime->loadedLibraryPath());
                    }
                }
#endif
            } else {
                LOG_WARNING("FSR2 AMD: context creation failed (", static_cast<int>(ctxErr), "), using internal fallback.");
                std::free(fsr2_.amdScratchBuffer);
                fsr2_.amdScratchBuffer = nullptr;
                fsr2_.amdScratchBufferSize = 0;
            }
        }
    }
#endif

    // --- Motion Vector Compute Pipeline ---
    {
        // Descriptor set layout: binding 0 = depth (sampler), binding 1 = motion vectors (storage image)
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fsr2_.motionVecDescSetLayout);

        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.offset = 0;
        pc.size = 2 * sizeof(glm::mat4);  // 128 bytes

        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount = 1;
        plCI.pSetLayouts = &fsr2_.motionVecDescSetLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges = &pc;
        vkCreatePipelineLayout(device, &plCI, nullptr, &fsr2_.motionVecPipelineLayout);

        VkShaderModule compMod;
        if (!compMod.loadFromFile(device, "assets/shaders/fsr2_motion.comp.spv")) {
            LOG_ERROR("FSR2: failed to load motion vector compute shader");
            destroyFSR2Resources();
            return false;
        }

        VkComputePipelineCreateInfo cpCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = compMod.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = fsr2_.motionVecPipelineLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &fsr2_.motionVecPipeline) != VK_SUCCESS) {
            LOG_ERROR("FSR2: failed to create motion vector pipeline");
            compMod.destroy();
            destroyFSR2Resources();
            return false;
        }
        compMod.destroy();

        // Descriptor pool + set
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &fsr2_.motionVecDescPool);

        VkDescriptorSetAllocateInfo dsAI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAI.descriptorPool = fsr2_.motionVecDescPool;
        dsAI.descriptorSetCount = 1;
        dsAI.pSetLayouts = &fsr2_.motionVecDescSetLayout;
        vkAllocateDescriptorSets(device, &dsAI, &fsr2_.motionVecDescSet);

        // Write descriptors
        VkDescriptorImageInfo depthImgInfo{};
        depthImgInfo.sampler = fsr2_.nearestSampler;
        depthImgInfo.imageView = fsr2_.sceneDepth.imageView;
        depthImgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo mvImgInfo{};
        mvImgInfo.imageView = fsr2_.motionVectors.imageView;
        mvImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = fsr2_.motionVecDescSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &depthImgInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = fsr2_.motionVecDescSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &mvImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // --- Temporal Accumulation Compute Pipeline ---
    {
        // bindings: 0=sceneColor, 1=depth, 2=motionVectors, 3=historyInput, 4=historyOutput
        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (int i = 0; i < 4; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 5;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fsr2_.accumulateDescSetLayout);

        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.offset = 0;
        pc.size = 4 * sizeof(glm::vec4);  // 64 bytes

        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount = 1;
        plCI.pSetLayouts = &fsr2_.accumulateDescSetLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges = &pc;
        vkCreatePipelineLayout(device, &plCI, nullptr, &fsr2_.accumulatePipelineLayout);

        VkShaderModule compMod;
        if (!compMod.loadFromFile(device, "assets/shaders/fsr2_accumulate.comp.spv")) {
            LOG_ERROR("FSR2: failed to load accumulation compute shader");
            destroyFSR2Resources();
            return false;
        }

        VkComputePipelineCreateInfo cpCI{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpCI.stage = compMod.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
        cpCI.layout = fsr2_.accumulatePipelineLayout;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &fsr2_.accumulatePipeline) != VK_SUCCESS) {
            LOG_ERROR("FSR2: failed to create accumulation pipeline");
            compMod.destroy();
            destroyFSR2Resources();
            return false;
        }
        compMod.destroy();

        // Descriptor pool: 2 sets (ping-pong), each with 4 samplers + 1 storage image
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &fsr2_.accumulateDescPool);

        // Allocate 2 descriptor sets (one per ping-pong direction)
        VkDescriptorSetLayout layouts[2] = { fsr2_.accumulateDescSetLayout, fsr2_.accumulateDescSetLayout };
        VkDescriptorSetAllocateInfo dsAI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAI.descriptorPool = fsr2_.accumulateDescPool;
        dsAI.descriptorSetCount = 2;
        dsAI.pSetLayouts = layouts;
        vkAllocateDescriptorSets(device, &dsAI, fsr2_.accumulateDescSets);

        // Write descriptors for both ping-pong sets
        for (int pp = 0; pp < 2; pp++) {
            int inputHistory = 1 - pp;   // Read from the other
            int outputHistory = pp;       // Write to this one

            // The accumulation shader already performs custom Lanczos reconstruction.
            // Use nearest here to avoid double filtering (linear + Lanczos) softening.
            VkDescriptorImageInfo colorInfo{fsr2_.nearestSampler, fsr2_.sceneColor.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo depthInfo{fsr2_.nearestSampler, fsr2_.sceneDepth.imageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo mvInfo{fsr2_.nearestSampler, fsr2_.motionVectors.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo histInInfo{fsr2_.linearSampler, fsr2_.history[inputHistory].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorImageInfo histOutInfo{VK_NULL_HANDLE, fsr2_.history[outputHistory].imageView, VK_IMAGE_LAYOUT_GENERAL};

            VkWriteDescriptorSet writes[5] = {};
            for (int w = 0; w < 5; w++) {
                writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[w].dstSet = fsr2_.accumulateDescSets[pp];
                writes[w].dstBinding = w;
                writes[w].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &colorInfo;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &depthInfo;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[2].pImageInfo = &mvInfo;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &histInInfo;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          writes[4].pImageInfo = &histOutInfo;

            vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
        }
    }

    // --- RCAS Sharpening Pipeline (fragment shader, fullscreen pass) ---
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fsr2_.sharpenDescSetLayout);

        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.offset = 0;
        pc.size = sizeof(glm::vec4);

        VkPipelineLayoutCreateInfo plCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCI.setLayoutCount = 1;
        plCI.pSetLayouts = &fsr2_.sharpenDescSetLayout;
        plCI.pushConstantRangeCount = 1;
        plCI.pPushConstantRanges = &pc;
        vkCreatePipelineLayout(device, &plCI, nullptr, &fsr2_.sharpenPipelineLayout);

        VkShaderModule vertMod, fragMod;
        if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
            !fragMod.loadFromFile(device, "assets/shaders/fsr2_sharpen.frag.spv")) {
            LOG_ERROR("FSR2: failed to load sharpen shaders");
            destroyFSR2Resources();
            return false;
        }

        fsr2_.sharpenPipeline = PipelineBuilder()
            .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({}, {})
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setMultisample(VK_SAMPLE_COUNT_1_BIT)
            .setLayout(fsr2_.sharpenPipelineLayout)
            .setRenderPass(vkCtx->getImGuiRenderPass())
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .build(device);

        vertMod.destroy();
        fragMod.destroy();

        if (!fsr2_.sharpenPipeline) {
            LOG_ERROR("FSR2: failed to create sharpen pipeline");
            destroyFSR2Resources();
            return false;
        }

        // Descriptor pool + sets for sharpen pass (double-buffered to avoid race condition)
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &fsr2_.sharpenDescPool);

        VkDescriptorSetLayout layouts[2] = {fsr2_.sharpenDescSetLayout, fsr2_.sharpenDescSetLayout};
        VkDescriptorSetAllocateInfo dsAI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsAI.descriptorPool = fsr2_.sharpenDescPool;
        dsAI.descriptorSetCount = 2;
        dsAI.pSetLayouts = layouts;
        vkAllocateDescriptorSets(device, &dsAI, fsr2_.sharpenDescSets);
        // Descriptors updated dynamically each frame to point at the correct history buffer
    }

    fsr2_.needsHistoryReset = true;
    fsr2_.frameIndex = 0;
    LOG_INFO("FSR2: initialized successfully");
    return true;
}

void Renderer::destroyFSR2Resources() {
    if (!vkCtx) return;

    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    vkDeviceWaitIdle(device);

#if WOWEE_HAS_AMD_FSR2
    if (fsr2_.useAmdBackend) {
        ffxFsr2ContextDestroy(&fsr2_.amdContext);
        fsr2_.useAmdBackend = false;
    }
    if (fsr2_.amdScratchBuffer) {
        std::free(fsr2_.amdScratchBuffer);
        fsr2_.amdScratchBuffer = nullptr;
    }
    fsr2_.amdScratchBufferSize = 0;
#endif
    fsr2_.amdFsr3FramegenRuntimeActive = false;
    fsr2_.amdFsr3FramegenRuntimeReady = false;
    fsr2_.framegenOutputValid = false;
    fsr2_.amdFsr3RuntimePath = "Path C";
    fsr2_.amdFsr3RuntimeLastError.clear();
    fsr2_.amdFsr3InteropSyncValue = 1;
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    if (fsr2_.amdFsr3Runtime) {
        fsr2_.amdFsr3Runtime->shutdown();
        fsr2_.amdFsr3Runtime.reset();
    }
#endif

    if (fsr2_.sharpenPipeline) { vkDestroyPipeline(device, fsr2_.sharpenPipeline, nullptr); fsr2_.sharpenPipeline = VK_NULL_HANDLE; }
    if (fsr2_.sharpenPipelineLayout) { vkDestroyPipelineLayout(device, fsr2_.sharpenPipelineLayout, nullptr); fsr2_.sharpenPipelineLayout = VK_NULL_HANDLE; }
    if (fsr2_.sharpenDescPool) { vkDestroyDescriptorPool(device, fsr2_.sharpenDescPool, nullptr); fsr2_.sharpenDescPool = VK_NULL_HANDLE; fsr2_.sharpenDescSets[0] = fsr2_.sharpenDescSets[1] = VK_NULL_HANDLE; }
    if (fsr2_.sharpenDescSetLayout) { vkDestroyDescriptorSetLayout(device, fsr2_.sharpenDescSetLayout, nullptr); fsr2_.sharpenDescSetLayout = VK_NULL_HANDLE; }

    if (fsr2_.accumulatePipeline) { vkDestroyPipeline(device, fsr2_.accumulatePipeline, nullptr); fsr2_.accumulatePipeline = VK_NULL_HANDLE; }
    if (fsr2_.accumulatePipelineLayout) { vkDestroyPipelineLayout(device, fsr2_.accumulatePipelineLayout, nullptr); fsr2_.accumulatePipelineLayout = VK_NULL_HANDLE; }
    if (fsr2_.accumulateDescPool) { vkDestroyDescriptorPool(device, fsr2_.accumulateDescPool, nullptr); fsr2_.accumulateDescPool = VK_NULL_HANDLE; fsr2_.accumulateDescSets[0] = fsr2_.accumulateDescSets[1] = VK_NULL_HANDLE; }
    if (fsr2_.accumulateDescSetLayout) { vkDestroyDescriptorSetLayout(device, fsr2_.accumulateDescSetLayout, nullptr); fsr2_.accumulateDescSetLayout = VK_NULL_HANDLE; }

    if (fsr2_.motionVecPipeline) { vkDestroyPipeline(device, fsr2_.motionVecPipeline, nullptr); fsr2_.motionVecPipeline = VK_NULL_HANDLE; }
    if (fsr2_.motionVecPipelineLayout) { vkDestroyPipelineLayout(device, fsr2_.motionVecPipelineLayout, nullptr); fsr2_.motionVecPipelineLayout = VK_NULL_HANDLE; }
    if (fsr2_.motionVecDescPool) { vkDestroyDescriptorPool(device, fsr2_.motionVecDescPool, nullptr); fsr2_.motionVecDescPool = VK_NULL_HANDLE; fsr2_.motionVecDescSet = VK_NULL_HANDLE; }
    if (fsr2_.motionVecDescSetLayout) { vkDestroyDescriptorSetLayout(device, fsr2_.motionVecDescSetLayout, nullptr); fsr2_.motionVecDescSetLayout = VK_NULL_HANDLE; }

    if (fsr2_.sceneFramebuffer) { vkDestroyFramebuffer(device, fsr2_.sceneFramebuffer, nullptr); fsr2_.sceneFramebuffer = VK_NULL_HANDLE; }
    if (fsr2_.linearSampler) { vkDestroySampler(device, fsr2_.linearSampler, nullptr); fsr2_.linearSampler = VK_NULL_HANDLE; }
    if (fsr2_.nearestSampler) { vkDestroySampler(device, fsr2_.nearestSampler, nullptr); fsr2_.nearestSampler = VK_NULL_HANDLE; }

    destroyImage(device, alloc, fsr2_.motionVectors);
    for (int i = 0; i < 2; i++) destroyImage(device, alloc, fsr2_.history[i]);
    destroyImage(device, alloc, fsr2_.framegenOutput);
    destroyImage(device, alloc, fsr2_.sceneDepth);
    destroyImage(device, alloc, fsr2_.sceneColor);

    fsr2_.internalWidth = 0;
    fsr2_.internalHeight = 0;
}

void Renderer::dispatchMotionVectors() {
    if (!fsr2_.motionVecPipeline || currentCmd == VK_NULL_HANDLE) return;

    // Transition depth: DEPTH_STENCIL_ATTACHMENT → DEPTH_STENCIL_READ_ONLY
    transitionImageLayout(currentCmd, fsr2_.sceneDepth.image,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Transition motion vectors: UNDEFINED → GENERAL
    transitionImageLayout(currentCmd, fsr2_.motionVectors.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsr2_.motionVecPipeline);
    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        fsr2_.motionVecPipelineLayout, 0, 1, &fsr2_.motionVecDescSet, 0, nullptr);

    // Reprojection with jittered matrices:
    // reconstruct world position from current depth, then project into previous clip.
    struct {
        glm::mat4 prevViewProjection;
        glm::mat4 invCurrentViewProj;
    } pc;

    glm::mat4 currentVP = camera->getViewProjectionMatrix();
    pc.prevViewProjection = fsr2_.prevViewProjection;
    pc.invCurrentViewProj = glm::inverse(currentVP);

    vkCmdPushConstants(currentCmd, fsr2_.motionVecPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t gx = (fsr2_.internalWidth + 7) / 8;
    uint32_t gy = (fsr2_.internalHeight + 7) / 8;
    vkCmdDispatch(currentCmd, gx, gy, 1);

    // Transition motion vectors: GENERAL → SHADER_READ_ONLY for accumulation
    transitionImageLayout(currentCmd, fsr2_.motionVectors.image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void Renderer::dispatchTemporalAccumulate() {
    if (!fsr2_.accumulatePipeline || currentCmd == VK_NULL_HANDLE) return;

    VkExtent2D swapExtent = vkCtx->getSwapchainExtent();
    uint32_t outputIdx = fsr2_.currentHistory;
    uint32_t inputIdx = 1 - outputIdx;

    // Transition scene color: PRESENT_SRC_KHR → SHADER_READ_ONLY
    transitionImageLayout(currentCmd, fsr2_.sceneColor.image,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // History layout lifecycle:
    //   First frame: both in UNDEFINED
    //   Subsequent frames: both in SHADER_READ_ONLY (output was transitioned for sharpen,
    //                      input was left in SHADER_READ_ONLY from its sharpen read)
    VkImageLayout historyOldLayout = fsr2_.needsHistoryReset
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Transition history input: SHADER_READ_ONLY → SHADER_READ_ONLY (barrier for sync)
    transitionImageLayout(currentCmd, fsr2_.history[inputIdx].image,
        historyOldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,  // sharpen read in previous frame
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Transition history output: SHADER_READ_ONLY → GENERAL (for compute write)
    transitionImageLayout(currentCmd, fsr2_.history[outputIdx].image,
        historyOldLayout, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_COMPUTE, fsr2_.accumulatePipeline);
    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        fsr2_.accumulatePipelineLayout, 0, 1, &fsr2_.accumulateDescSets[outputIdx], 0, nullptr);

    // Push constants
    struct {
        glm::vec4 internalSize;
        glm::vec4 displaySize;
        glm::vec4 jitterOffset;
        glm::vec4 params;
    } pc;

    pc.internalSize = glm::vec4(
        static_cast<float>(fsr2_.internalWidth), static_cast<float>(fsr2_.internalHeight),
        1.0f / fsr2_.internalWidth, 1.0f / fsr2_.internalHeight);
    pc.displaySize = glm::vec4(
        static_cast<float>(swapExtent.width), static_cast<float>(swapExtent.height),
        1.0f / swapExtent.width, 1.0f / swapExtent.height);
    glm::vec2 jitter = camera->getJitter();
    pc.jitterOffset = glm::vec4(jitter.x, jitter.y, 0.0f, 0.0f);
    pc.params = glm::vec4(
        fsr2_.needsHistoryReset ? 1.0f : 0.0f,
        fsr2_.sharpness,
        static_cast<float>(fsr2_.convergenceFrame),
        0.0f);

    vkCmdPushConstants(currentCmd, fsr2_.accumulatePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t gx = (swapExtent.width + 7) / 8;
    uint32_t gy = (swapExtent.height + 7) / 8;
    vkCmdDispatch(currentCmd, gx, gy, 1);

    fsr2_.needsHistoryReset = false;
}

void Renderer::dispatchAmdFsr2() {
    if (currentCmd == VK_NULL_HANDLE || !camera) return;
#if WOWEE_HAS_AMD_FSR2
    if (!fsr2_.useAmdBackend) return;

    VkExtent2D swapExtent = vkCtx->getSwapchainExtent();
    uint32_t outputIdx = fsr2_.currentHistory;

    transitionImageLayout(currentCmd, fsr2_.sceneColor.image,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.motionVectors.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.sceneDepth.image,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.history[outputIdx].image,
        fsr2_.needsHistoryReset ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    FfxFsr2DispatchDescription desc{};
    desc.commandList = ffxGetCommandListVK(currentCmd);
    desc.color = ffxGetTextureResourceVK(&fsr2_.amdContext,
        fsr2_.sceneColor.image, fsr2_.sceneColor.imageView,
        fsr2_.internalWidth, fsr2_.internalHeight, vkCtx->getSwapchainFormat(),
        L"FSR2_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    desc.depth = ffxGetTextureResourceVK(&fsr2_.amdContext,
        fsr2_.sceneDepth.image, fsr2_.sceneDepth.imageView,
        fsr2_.internalWidth, fsr2_.internalHeight, vkCtx->getDepthFormat(),
        L"FSR2_InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors = ffxGetTextureResourceVK(&fsr2_.amdContext,
        fsr2_.motionVectors.image, fsr2_.motionVectors.imageView,
        fsr2_.internalWidth, fsr2_.internalHeight, VK_FORMAT_R16G16_SFLOAT,
        L"FSR2_InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    desc.output = ffxGetTextureResourceVK(&fsr2_.amdContext,
        fsr2_.history[outputIdx].image, fsr2_.history[outputIdx].imageView,
        swapExtent.width, swapExtent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
        L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    // Camera jitter is stored as NDC projection offsets; convert to render-pixel offsets.
    // Do not apply jitterSign again here: it is already baked into camera jitter.
    glm::vec2 jitterNdc = camera->getJitter();
    desc.jitterOffset.x = jitterNdc.x * 0.5f * static_cast<float>(fsr2_.internalWidth);
    desc.jitterOffset.y = jitterNdc.y * 0.5f * static_cast<float>(fsr2_.internalHeight);
    desc.motionVectorScale.x = static_cast<float>(fsr2_.internalWidth) * fsr2_.motionVecScaleX;
    desc.motionVectorScale.y = static_cast<float>(fsr2_.internalHeight) * fsr2_.motionVecScaleY;
    desc.renderSize.width = fsr2_.internalWidth;
    desc.renderSize.height = fsr2_.internalHeight;
    desc.enableSharpening = false;  // Keep existing RCAS post pass.
    desc.sharpness = 0.0f;
    desc.frameTimeDelta = glm::max(0.001f, lastDeltaTime_ * 1000.0f);
    desc.preExposure = 1.0f;
    desc.reset = fsr2_.needsHistoryReset;
    desc.cameraNear = camera->getNearPlane();
    desc.cameraFar = camera->getFarPlane();
    desc.cameraFovAngleVertical = glm::radians(camera->getFovDegrees());
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.enableAutoReactive = false;

    FfxErrorCode dispatchErr = ffxFsr2ContextDispatch(&fsr2_.amdContext, &desc);
    if (dispatchErr != FFX_OK) {
        LOG_WARNING("FSR2 AMD: dispatch failed (", static_cast<int>(dispatchErr), "), forcing history reset.");
        fsr2_.needsHistoryReset = true;
    } else {
        fsr2_.needsHistoryReset = false;
    }
#endif
}

void Renderer::dispatchAmdFsr3Framegen() {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    if (!fsr2_.amdFsr3FramegenEnabled) {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    if (!fsr2_.amdFsr3Runtime || !fsr2_.amdFsr3FramegenRuntimeReady) {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    uint32_t outputIdx = fsr2_.currentHistory;
    transitionImageLayout(currentCmd, fsr2_.sceneColor.image,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.motionVectors.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.sceneDepth.image,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    transitionImageLayout(currentCmd, fsr2_.history[outputIdx].image,
        fsr2_.needsHistoryReset ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    if (fsr2_.amdFsr3FramegenEnabled && fsr2_.framegenOutput.image) {
        transitionImageLayout(currentCmd, fsr2_.framegenOutput.image,
            fsr2_.framegenOutputValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    AmdFsr3RuntimeDispatchDesc fgDispatch{};
    fgDispatch.commandBuffer = currentCmd;
    fgDispatch.colorImage = fsr2_.sceneColor.image;
    fgDispatch.depthImage = fsr2_.sceneDepth.image;
    fgDispatch.motionVectorImage = fsr2_.motionVectors.image;
    fgDispatch.outputImage = fsr2_.history[fsr2_.currentHistory].image;
    fgDispatch.renderWidth = fsr2_.internalWidth;
    fgDispatch.renderHeight = fsr2_.internalHeight;
    fgDispatch.outputWidth = vkCtx->getSwapchainExtent().width;
    fgDispatch.outputHeight = vkCtx->getSwapchainExtent().height;
    fgDispatch.colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    fgDispatch.depthFormat = vkCtx->getDepthFormat();
    fgDispatch.motionVectorFormat = VK_FORMAT_R16G16_SFLOAT;
    fgDispatch.outputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    fgDispatch.frameGenOutputImage = fsr2_.framegenOutput.image;
    glm::vec2 jitterNdc = camera ? camera->getJitter() : glm::vec2(0.0f);
    fgDispatch.jitterX = jitterNdc.x * 0.5f * static_cast<float>(fsr2_.internalWidth);
    fgDispatch.jitterY = jitterNdc.y * 0.5f * static_cast<float>(fsr2_.internalHeight);
    fgDispatch.motionScaleX = static_cast<float>(fsr2_.internalWidth) * fsr2_.motionVecScaleX;
    fgDispatch.motionScaleY = static_cast<float>(fsr2_.internalHeight) * fsr2_.motionVecScaleY;
    fgDispatch.frameTimeDeltaMs = glm::max(0.001f, lastDeltaTime_ * 1000.0f);
    fgDispatch.cameraNear = camera ? camera->getNearPlane() : 0.1f;
    fgDispatch.cameraFar = camera ? camera->getFarPlane() : 1000.0f;
    fgDispatch.cameraFovYRadians = camera ? glm::radians(camera->getFovDegrees()) : 1.0f;
    fgDispatch.reset = fsr2_.needsHistoryReset;


    if (!fsr2_.amdFsr3Runtime->dispatchUpscale(fgDispatch)) {
        static bool warnedRuntimeDispatch = false;
        if (!warnedRuntimeDispatch) {
            warnedRuntimeDispatch = true;
            LOG_WARNING("FSR3 runtime upscale dispatch failed; falling back to FSR2 dispatch output.");
        }
        fsr2_.amdFsr3RuntimeLastError = fsr2_.amdFsr3Runtime->lastError();
        fsr2_.amdFsr3FallbackCount++;
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    fsr2_.amdFsr3RuntimeLastError.clear();
    fsr2_.amdFsr3UpscaleDispatchCount++;

    if (!fsr2_.amdFsr3FramegenEnabled) {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    if (!fsr2_.amdFsr3Runtime->isFrameGenerationReady()) {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    if (!fsr2_.amdFsr3Runtime->dispatchFrameGeneration(fgDispatch)) {
        static bool warnedFgDispatch = false;
        if (!warnedFgDispatch) {
            warnedFgDispatch = true;
            LOG_WARNING("FSR3 runtime frame generation dispatch failed; using upscaled output only.");
        }
        fsr2_.amdFsr3RuntimeLastError = fsr2_.amdFsr3Runtime->lastError();
        fsr2_.amdFsr3FallbackCount++;
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        return;
    }
    fsr2_.amdFsr3RuntimeLastError.clear();
    fsr2_.amdFsr3FramegenDispatchCount++;
    fsr2_.framegenOutputValid = true;
    fsr2_.amdFsr3FramegenRuntimeActive = true;
#else
    fsr2_.amdFsr3FramegenRuntimeActive = false;
#endif
}

void Renderer::renderFSR2Sharpen() {
    if (!fsr2_.sharpenPipeline || currentCmd == VK_NULL_HANDLE) return;

    VkExtent2D ext = vkCtx->getSwapchainExtent();
    uint32_t outputIdx = fsr2_.currentHistory;

    // Use per-frame descriptor set to avoid race with in-flight command buffers
    uint32_t frameIdx = vkCtx->getCurrentFrame();
    VkDescriptorSet descSet = fsr2_.sharpenDescSets[frameIdx];

    // Update sharpen descriptor to point at current history output
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = fsr2_.linearSampler;
    if (fsr2_.useAmdBackend) {
        imgInfo.imageView = (fsr2_.amdFsr3FramegenEnabled && fsr2_.amdFsr3FramegenRuntimeActive && fsr2_.framegenOutput.imageView)
            ? fsr2_.framegenOutput.imageView
            : fsr2_.history[outputIdx].imageView;
    } else {
        imgInfo.imageView = fsr2_.sceneColor.imageView;
    }
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &write, 0, nullptr);

    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr2_.sharpenPipeline);
    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        fsr2_.sharpenPipelineLayout, 0, 1, &descSet, 0, nullptr);

    glm::vec4 params(1.0f / ext.width, 1.0f / ext.height, fsr2_.sharpness, 0.0f);
    vkCmdPushConstants(currentCmd, fsr2_.sharpenPipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4), &params);

    vkCmdDraw(currentCmd, 3, 1, 0, 0);
}

void Renderer::setFSR2Enabled(bool enabled) {
    if (fsr2_.enabled == enabled) return;
    fsr2_.enabled = enabled;

    if (enabled) {
        static bool initFramegenToggleFromEnv = false;
        if (!initFramegenToggleFromEnv) {
            initFramegenToggleFromEnv = true;
            if (std::getenv("WOWEE_ENABLE_AMD_FSR3_FRAMEGEN_RUNTIME") != nullptr) {
                fsr2_.amdFsr3FramegenEnabled = true;
            }
        }
        // FSR2 replaces both FSR1 and MSAA
        if (fsr_.enabled) {
            fsr_.enabled = false;
            fsr_.needsRecreate = true;
        }
        // FSR2 requires non-MSAA render pass (its framebuffer has 2 attachments)
        if (vkCtx && vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
            pendingMsaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
            msaaChangePending_ = true;
        }
        // Use FSR1's scale factor and sharpness as defaults
        fsr2_.scaleFactor = fsr_.scaleFactor;
        fsr2_.sharpness = fsr_.sharpness;
        fsr2_.needsHistoryReset = true;
    } else {
        fsr2_.needsRecreate = true;
        if (camera) camera->clearJitter();
    }
}

void Renderer::setFSR2DebugTuning(float jitterSign, float motionVecScaleX, float motionVecScaleY) {
    fsr2_.jitterSign = glm::clamp(jitterSign, -2.0f, 2.0f);
    fsr2_.motionVecScaleX = glm::clamp(motionVecScaleX, -2.0f, 2.0f);
    fsr2_.motionVecScaleY = glm::clamp(motionVecScaleY, -2.0f, 2.0f);
}

void Renderer::setAmdFsr3FramegenEnabled(bool enabled) {
    if (fsr2_.amdFsr3FramegenEnabled == enabled) return;
    fsr2_.amdFsr3FramegenEnabled = enabled;
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    if (enabled) {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        fsr2_.framegenOutputValid = false;
        fsr2_.needsRecreate = true;
        fsr2_.needsHistoryReset = true;
        fsr2_.amdFsr3FramegenRuntimeReady = false;
        fsr2_.amdFsr3RuntimePath = "Path C";
        fsr2_.amdFsr3RuntimeLastError.clear();
        LOG_INFO("FSR3 framegen requested; runtime will initialize on next FSR2 resource creation.");
    } else {
        fsr2_.amdFsr3FramegenRuntimeActive = false;
        fsr2_.amdFsr3FramegenRuntimeReady = false;
        fsr2_.framegenOutputValid = false;
        fsr2_.needsHistoryReset = true;
        fsr2_.needsRecreate = true;
        fsr2_.amdFsr3RuntimePath = "Path C";
        fsr2_.amdFsr3RuntimeLastError = "disabled by user";
        if (fsr2_.amdFsr3Runtime) {
            fsr2_.amdFsr3Runtime->shutdown();
            fsr2_.amdFsr3Runtime.reset();
        }
        LOG_INFO("FSR3 framegen disabled; forcing FSR2-only path rebuild.");
    }
#else
    fsr2_.amdFsr3FramegenRuntimeActive = false;
    fsr2_.amdFsr3FramegenRuntimeReady = false;
    fsr2_.framegenOutputValid = false;
    if (enabled) {
        LOG_WARNING("FSR3 framegen requested, but AMD FSR3 framegen SDK headers are unavailable in this build.");
    }
#endif
}

// ========================= End FSR 2.2 =========================

// ========================= FXAA Post-Process =========================

bool Renderer::initFXAAResources() {
    if (!vkCtx) return false;

    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    VkExtent2D ext = vkCtx->getSwapchainExtent();
    VkSampleCountFlagBits msaa = vkCtx->getMsaaSamples();
    bool useMsaa = (msaa > VK_SAMPLE_COUNT_1_BIT);
    bool useDepthResolve = (vkCtx->getDepthResolveImageView() != VK_NULL_HANDLE);

    LOG_INFO("FXAA: initializing at ", ext.width, "x", ext.height,
             " (MSAA=", static_cast<int>(msaa), "x)");

    VkFormat colorFmt = vkCtx->getSwapchainFormat();
    VkFormat depthFmt = vkCtx->getDepthFormat();

    // sceneColor: 1x resolved color target — FXAA reads from here
    fxaa_.sceneColor = createImage(device, alloc, ext.width, ext.height,
        colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!fxaa_.sceneColor.image) {
        LOG_ERROR("FXAA: failed to create scene color image");
        return false;
    }

    // sceneDepth: depth buffer at current MSAA sample count
    fxaa_.sceneDepth = createImage(device, alloc, ext.width, ext.height,
        depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, msaa);
    if (!fxaa_.sceneDepth.image) {
        LOG_ERROR("FXAA: failed to create scene depth image");
        destroyFXAAResources();
        return false;
    }

    if (useMsaa) {
        fxaa_.sceneMsaaColor = createImage(device, alloc, ext.width, ext.height,
            colorFmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, msaa);
        if (!fxaa_.sceneMsaaColor.image) {
            LOG_ERROR("FXAA: failed to create MSAA color image");
            destroyFXAAResources();
            return false;
        }
        if (useDepthResolve) {
            fxaa_.sceneDepthResolve = createImage(device, alloc, ext.width, ext.height,
                depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            if (!fxaa_.sceneDepthResolve.image) {
                LOG_ERROR("FXAA: failed to create depth resolve image");
                destroyFXAAResources();
                return false;
            }
        }
    }

    // Framebuffer — same attachment layout as main render pass
    VkImageView fbAttachments[4]{};
    uint32_t fbCount;
    if (useMsaa) {
        fbAttachments[0] = fxaa_.sceneMsaaColor.imageView;
        fbAttachments[1] = fxaa_.sceneDepth.imageView;
        fbAttachments[2] = fxaa_.sceneColor.imageView;  // resolve target
        fbCount = 3;
        if (useDepthResolve) {
            fbAttachments[3] = fxaa_.sceneDepthResolve.imageView;
            fbCount = 4;
        }
    } else {
        fbAttachments[0] = fxaa_.sceneColor.imageView;
        fbAttachments[1] = fxaa_.sceneDepth.imageView;
        fbCount = 2;
    }

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = vkCtx->getImGuiRenderPass();
    fbInfo.attachmentCount = fbCount;
    fbInfo.pAttachments = fbAttachments;
    fbInfo.width = ext.width;
    fbInfo.height = ext.height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fxaa_.sceneFramebuffer) != VK_SUCCESS) {
        LOG_ERROR("FXAA: failed to create scene framebuffer");
        destroyFXAAResources();
        return false;
    }

    // Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &fxaa_.sceneSampler) != VK_SUCCESS) {
        LOG_ERROR("FXAA: failed to create sampler");
        destroyFXAAResources();
        return false;
    }

    // Descriptor set layout: binding 0 = combined image sampler
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fxaa_.descSetLayout);

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &fxaa_.descPool);

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = fxaa_.descPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &fxaa_.descSetLayout;
    vkAllocateDescriptorSets(device, &dsAllocInfo, &fxaa_.descSet);

    // Bind the resolved 1x sceneColor
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = fxaa_.sceneSampler;
    imgInfo.imageView = fxaa_.sceneColor.imageView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = fxaa_.descSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Pipeline layout — push constant holds vec4(rcpFrame.xy, sharpness, pad)
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 16;  // vec4
    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount = 1;
    plCI.pSetLayouts = &fxaa_.descSetLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(device, &plCI, nullptr, &fxaa_.pipelineLayout);

    // FXAA pipeline — fullscreen triangle into the swapchain render pass
    // Uses VK_SAMPLE_COUNT_1_BIT: it always runs after MSAA resolve.
    VkShaderModule vertMod, fragMod;
    if (!vertMod.loadFromFile(device, "assets/shaders/postprocess.vert.spv") ||
        !fragMod.loadFromFile(device, "assets/shaders/fxaa.frag.spv")) {
        LOG_ERROR("FXAA: failed to load shaders");
        destroyFXAAResources();
        return false;
    }

    fxaa_.pipeline = PipelineBuilder()
        .setShaders(vertMod.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragMod.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({}, {})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(VK_SAMPLE_COUNT_1_BIT)  // swapchain pass is always 1x
        .setLayout(fxaa_.pipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device);

    vertMod.destroy();
    fragMod.destroy();

    if (!fxaa_.pipeline) {
        LOG_ERROR("FXAA: failed to create pipeline");
        destroyFXAAResources();
        return false;
    }

    LOG_INFO("FXAA: initialized successfully");
    return true;
}

void Renderer::destroyFXAAResources() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();
    vkDeviceWaitIdle(device);

    if (fxaa_.pipeline)       { vkDestroyPipeline(device, fxaa_.pipeline, nullptr);             fxaa_.pipeline = VK_NULL_HANDLE; }
    if (fxaa_.pipelineLayout) { vkDestroyPipelineLayout(device, fxaa_.pipelineLayout, nullptr); fxaa_.pipelineLayout = VK_NULL_HANDLE; }
    if (fxaa_.descPool)       { vkDestroyDescriptorPool(device, fxaa_.descPool, nullptr);       fxaa_.descPool = VK_NULL_HANDLE; fxaa_.descSet = VK_NULL_HANDLE; }
    if (fxaa_.descSetLayout)  { vkDestroyDescriptorSetLayout(device, fxaa_.descSetLayout, nullptr); fxaa_.descSetLayout = VK_NULL_HANDLE; }
    if (fxaa_.sceneFramebuffer) { vkDestroyFramebuffer(device, fxaa_.sceneFramebuffer, nullptr); fxaa_.sceneFramebuffer = VK_NULL_HANDLE; }
    if (fxaa_.sceneSampler)   { vkDestroySampler(device, fxaa_.sceneSampler, nullptr);          fxaa_.sceneSampler = VK_NULL_HANDLE; }
    destroyImage(device, alloc, fxaa_.sceneDepthResolve);
    destroyImage(device, alloc, fxaa_.sceneMsaaColor);
    destroyImage(device, alloc, fxaa_.sceneDepth);
    destroyImage(device, alloc, fxaa_.sceneColor);
}

void Renderer::renderFXAAPass() {
    if (!fxaa_.pipeline || currentCmd == VK_NULL_HANDLE) return;
    VkExtent2D ext = vkCtx->getSwapchainExtent();

    vkCmdBindPipeline(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_.pipeline);
    vkCmdBindDescriptorSets(currentCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            fxaa_.pipelineLayout, 0, 1, &fxaa_.descSet, 0, nullptr);

    // Pass rcpFrame + sharpness + effect flag (vec4, 16 bytes).
    // When FSR2/FSR3 is active alongside FXAA, forward FSR2's sharpness so the
    // post-FXAA unsharp-mask step restores the crispness that FXAA's blur removes.
    float sharpness = fsr2_.enabled ? fsr2_.sharpness : 0.0f;
    float pc[4] = {
        1.0f / static_cast<float>(ext.width),
        1.0f / static_cast<float>(ext.height),
        sharpness,
        0.0f
    };
    vkCmdPushConstants(currentCmd, fxaa_.pipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);

    vkCmdDraw(currentCmd, 3, 1, 0, 0);  // fullscreen triangle
}

void Renderer::setFXAAEnabled(bool enabled) {
    if (fxaa_.enabled == enabled) return;
    // FXAA is a post-process AA pass intended to supplement FSR temporal output.
    // It conflicts with MSAA (which resolves AA during the scene render pass), so
    // refuse to enable FXAA when hardware MSAA is active.
    if (enabled && vkCtx && vkCtx->getMsaaSamples() > VK_SAMPLE_COUNT_1_BIT) {
        LOG_INFO("FXAA: blocked while MSAA is active — disable MSAA first");
        return;
    }
    fxaa_.enabled = enabled;
    if (!enabled) {
        fxaa_.needsRecreate = true;  // defer destruction to next beginFrame()
    }
}

// ========================= End FXAA =========================

void Renderer::renderWorld(game::World* world, game::GameHandler* gameHandler) {
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
    float timeOfDay = (skySystem && skySystem->getSkybox()) ? skySystem->getSkybox()->getTimeOfDay() : 12.0f;

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
        if (terrainFuture.valid()) lastTerrainRenderMs = terrainFuture.get();
        if (wmoFuture.valid()) lastWMORenderMs = wmoFuture.get();
        if (m2Future.valid()) lastM2RenderMs = m2Future.get();

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
        if (ambientSoundManager) {
            terrainManager->setAmbientSoundManager(ambientSoundManager.get());
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
    if (musicManager && assetManager && !cachedAssetManager) {
        audio::AudioEngine::instance().setAssetManager(assetManager);
        musicManager->initialize(assetManager);
        if (footstepManager) {
            footstepManager->initialize(assetManager);
        }
        if (activitySoundManager) {
            activitySoundManager->initialize(assetManager);
        }
        if (mountSoundManager) {
            mountSoundManager->initialize(assetManager);
        }
        if (npcVoiceManager) {
            npcVoiceManager->initialize(assetManager);
        }
        if (!deferredWorldInitEnabled_) {
            if (ambientSoundManager) {
                ambientSoundManager->initialize(assetManager);
            }
            if (uiSoundManager) {
                uiSoundManager->initialize(assetManager);
            }
            if (combatSoundManager) {
                combatSoundManager->initialize(assetManager);
            }
            if (spellSoundManager) {
                spellSoundManager->initialize(assetManager);
            }
            if (movementSoundManager) {
                movementSoundManager->initialize(assetManager);
            }
            if (questMarkerRenderer) {
                questMarkerRenderer->initialize(vkCtx, perFrameSetLayout, assetManager);
            }

            if (envFlagEnabled("WOWEE_PREWARM_ZONE_MUSIC", false)) {
                if (zoneManager) {
                    for (const auto& musicPath : zoneManager->getAllMusicPaths()) {
                        musicManager->preloadMusic(musicPath);
                    }
                }
                static const std::vector<std::string> tavernTracks = {
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance01.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernAlliance\\TavernAlliance02.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern1A.mp3",
                    "Sound\\Music\\ZoneMusic\\TavernHuman\\RA_HumanTavern2A.mp3",
                };
                for (const auto& musicPath : tavernTracks) {
                    musicManager->preloadMusic(musicPath);
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
                        tileX = std::stoi(filename.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1));
                        tileY = std::stoi(filename.substr(secondUnderscore + 1, dot - secondUnderscore - 1));
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
    if (musicManager && cachedAssetManager) {
        if (!musicManager->isInitialized()) {
            musicManager->initialize(cachedAssetManager);
        }
    }
    if (footstepManager && cachedAssetManager) {
        if (!footstepManager->isInitialized()) {
            footstepManager->initialize(cachedAssetManager);
        }
    }
    if (activitySoundManager && cachedAssetManager) {
        if (!activitySoundManager->isInitialized()) {
            activitySoundManager->initialize(cachedAssetManager);
        }
    }
    if (mountSoundManager && cachedAssetManager) {
        mountSoundManager->initialize(cachedAssetManager);
    }
    if (npcVoiceManager && cachedAssetManager) {
        npcVoiceManager->initialize(cachedAssetManager);
    }
    if (!deferredWorldInitEnabled_) {
        if (ambientSoundManager && cachedAssetManager) {
            ambientSoundManager->initialize(cachedAssetManager);
        }
        if (uiSoundManager && cachedAssetManager) {
            uiSoundManager->initialize(cachedAssetManager);
        }
        if (combatSoundManager && cachedAssetManager) {
            combatSoundManager->initialize(cachedAssetManager);
        }
        if (spellSoundManager && cachedAssetManager) {
            spellSoundManager->initialize(cachedAssetManager);
        }
        if (movementSoundManager && cachedAssetManager) {
            movementSoundManager->initialize(cachedAssetManager);
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
    if (terrainManager && ambientSoundManager) {
        terrainManager->setAmbientSoundManager(ambientSoundManager.get());
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

const char* Renderer::getAmdFsr3FramegenRuntimePath() const {
    return fsr2_.amdFsr3RuntimePath.c_str();
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
        if (glm::length(lighting.directionalDir) > 0.001f) {
            sunDir = glm::normalize(-lighting.directionalDir);
        }
    }
    // Shadow camera expects light rays pointing downward in render space (Z up).
    // Some profiles/opcode paths provide the opposite convention; normalize here.
    if (sunDir.z > 0.0f) {
        sunDir = -sunDir;
    }
    // Keep a minimum downward component so the frustum doesn't collapse at grazing angles.
    if (sunDir.z > -0.08f) {
        sunDir.z = -0.08f;
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

    // Reflection pass uses 1x MSAA. Scene pipelines must be render-pass-compatible,
    // which requires matching sample counts. Only render scene into reflection when MSAA is off.
    bool canRenderScene = (vkCtx->getMsaaSamples() == VK_SAMPLE_COUNT_1_BIT);

    // Find dominant water height near camera
    auto waterH = waterRenderer->getDominantWaterHeight(camera->getPosition());
    if (!waterH) return;

    float waterHeight = *waterH;

    // Skip reflection if camera is underwater (Z is up)
    if (camera->getPosition().z < waterHeight + 0.5f) return;

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
    glm::vec3 reflPos = camera->getPosition();
    reflPos.z = 2.0f * waterHeight - reflPos.z;
    reflData.viewPos = glm::vec4(reflPos, 1.0f);
    std::memcpy(reflPerFrameUBOMapped, &reflData, sizeof(GPUPerFrameData));

    // Begin reflection render pass (clears to black; scene rendered if pipeline-compatible)
    if (!waterRenderer->beginReflectionPass(currentCmd)) return;

    if (canRenderScene) {
        // Render scene into reflection texture (sky + terrain + WMO only for perf)
        if (skySystem) {
            rendering::SkyParams skyParams;
            skyParams.timeOfDay = (skySystem->getSkybox()) ? skySystem->getSkybox()->getTimeOfDay() : 12.0f;
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
            skySystem->render(currentCmd, reflPerFrameDescSet, *camera, skyParams);
        }
        if (terrainRenderer && terrainEnabled) {
            terrainRenderer->render(currentCmd, reflPerFrameDescSet, *camera);
        }
        if (wmoRenderer) {
            wmoRenderer->render(currentCmd, reflPerFrameDescSet, *camera);
        }
    }

    waterRenderer->endReflectionPass(currentCmd);
}

void Renderer::renderShadowPass() {
    static const bool skipShadows = (std::getenv("WOWEE_SKIP_SHADOWS") != nullptr);
    if (skipShadows) return;
    if (!shadowsEnabled || shadowDepthImage[0] == VK_NULL_HANDLE) return;
    if (currentCmd == VK_NULL_HANDLE) return;

    // Shadows render every frame — throttling causes visible flicker on player/NPCs

    // Compute and store light space matrix; write to per-frame UBO
    lightSpaceMatrix = computeLightSpaceMatrix();
    // Zero matrix means character position isn't set yet — skip shadow pass entirely.
    if (lightSpaceMatrix == glm::mat4(0.0f)) return;
    uint32_t frame = vkCtx->getCurrentFrame();
    auto* ubo = reinterpret_cast<GPUPerFrameData*>(perFrameUBOMapped[frame]);
    if (ubo) {
        ubo->lightSpaceMatrix = lightSpaceMatrix;
        ubo->shadowParams.x = shadowsEnabled ? 1.0f : 0.0f;
        ubo->shadowParams.y = 0.8f;
    }

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

} // namespace rendering
} // namespace wowee
