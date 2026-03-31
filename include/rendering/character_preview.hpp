#pragma once

#include "game/character.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class CharacterRenderer;
class Camera;
class VkContext;
class VkTexture;
class VkRenderTarget;

class CharacterPreview {
public:
    CharacterPreview();
    ~CharacterPreview();

    bool initialize(pipeline::AssetManager* am);
    void shutdown();

    bool loadCharacter(game::Race race, game::Gender gender,
                       uint8_t skin, uint8_t face,
                       uint8_t hairStyle, uint8_t hairColor,
                       uint8_t facialHair, bool useFemaleModel = false);

    // Apply equipment overlays/geosets using SMSG_CHAR_ENUM equipment data (ItemDisplayInfo.dbc).
    bool applyEquipment(const std::vector<game::EquipmentItem>& equipment);

    void update(float deltaTime);
    void render();
    void rotate(float yawDelta);

    // Off-screen composite pass — call from Renderer::beginFrame() before main render pass
    void compositePass(VkCommandBuffer cmd, uint32_t frameIndex);

    // Mark that the preview needs compositing this frame (call from UI each frame)
    void requestComposite() { compositeRequested_ = true; }

    // Returns the ImGui texture handle. Returns VK_NULL_HANDLE until the first
    // compositePass has run (image is in UNDEFINED layout before that).
    VkDescriptorSet getTextureId() const { return compositeRendered_ ? imguiTextureId_ : VK_NULL_HANDLE; }
    int getWidth() const { return fboWidth_; }
    int getHeight() const { return fboHeight_; }

    CharacterRenderer* getCharacterRenderer() { return charRenderer_.get(); }
    uint32_t getInstanceId() const { return instanceId_; }
    uint32_t getModelId() const { return PREVIEW_MODEL_ID; }
    bool isModelLoaded() const { return modelLoaded_; }

private:
    void createFBO();
    void destroyFBO();

    pipeline::AssetManager* assetManager_ = nullptr;
    VkContext* vkCtx_ = nullptr;
    std::unique_ptr<CharacterRenderer> charRenderer_;
    std::unique_ptr<Camera> camera_;

    // Off-screen render target (color + depth)
    std::unique_ptr<VkRenderTarget> renderTarget_;

    // Per-frame UBO for preview camera/lighting (double-buffered)
    static constexpr uint32_t MAX_FRAMES = 2;
    VkDescriptorPool previewDescPool_ = VK_NULL_HANDLE;
    VkBuffer previewUBO_[MAX_FRAMES] = {};
    VmaAllocation previewUBOAlloc_[MAX_FRAMES] = {};
    void* previewUBOMapped_[MAX_FRAMES] = {};
    VkDescriptorSet previewPerFrameSet_[MAX_FRAMES] = {};

    // Dummy 1x1 white texture for shadow map placeholder
    std::unique_ptr<VkTexture> dummyWhiteTex_;

    // ImGui texture handle for displaying the preview (VkDescriptorSet in Vulkan backend)
    VkDescriptorSet imguiTextureId_ = VK_NULL_HANDLE;

    // 4:5 portrait aspect ratio — taller than wide to show full character body
    // from head to feet in the character creation/selection screen
    static constexpr int fboWidth_ = 400;
    static constexpr int fboHeight_ = 500;

    static constexpr uint32_t PREVIEW_MODEL_ID = 9999;
    uint32_t instanceId_ = 0;
    bool modelLoaded_ = false;
    bool compositeRequested_ = false;
    bool compositeRendered_ = false;  // True after first successful compositePass
    float modelYaw_ = 90.0f;

    // Cached info from loadCharacter() for later recompositing.
    game::Race race_ = game::Race::HUMAN;
    game::Gender gender_ = game::Gender::MALE;
    bool useFemaleModel_ = false;
    uint8_t hairStyle_ = 0;
    uint8_t facialHair_ = 0;
    std::string bodySkinPath_;
    std::vector<std::string> baseLayers_; // face + underwear, etc.
    uint32_t skinTextureSlotIndex_ = 0;
};

} // namespace rendering
} // namespace wowee
