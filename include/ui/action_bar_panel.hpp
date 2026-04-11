// ============================================================
// ActionBarPanel — extracted from GameScreen
// Owns all action bar rendering: main bar, stance bar, bag bar,
// XP bar, reputation bar, macro resolution.
// ============================================================
#pragma once
#include "ui/ui_services.hpp"
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <vulkan/vulkan.h>

namespace wowee {
namespace game { class GameHandler; }
namespace pipeline { class AssetManager; }
namespace ui {

class ChatPanel;
class SettingsPanel;
class InventoryScreen;
class SpellbookScreen;
class QuestLogScreen;

class ActionBarPanel {
public:
    // Callback type for resolving spell icons (spellId, assetMgr) → VkDescriptorSet
    using SpellIconFn = std::function<VkDescriptorSet(uint32_t, pipeline::AssetManager*)>;

    // ---- Action bar render methods ----
    void renderActionBar(game::GameHandler& gameHandler,
                         SettingsPanel& settingsPanel,
                         ChatPanel& chatPanel,
                         InventoryScreen& inventoryScreen,
                         SpellbookScreen& spellbookScreen,
                         QuestLogScreen& questLogScreen,
                         SpellIconFn getSpellIcon);
    void renderStanceBar(game::GameHandler& gameHandler,
                         SettingsPanel& settingsPanel,
                         SpellbookScreen& spellbookScreen,
                         SpellIconFn getSpellIcon);
    void renderBagBar(game::GameHandler& gameHandler,
                      SettingsPanel& settingsPanel,
                      InventoryScreen& inventoryScreen);
    void renderXpBar(game::GameHandler& gameHandler,
                     SettingsPanel& settingsPanel);
    void renderRepBar(game::GameHandler& gameHandler,
                      SettingsPanel& settingsPanel);

    // ---- State owned by this panel ----

    // Action bar error-flash: spellId → wall-clock time (seconds) when the flash ends
    std::unordered_map<uint32_t, float> actionFlashEndTimes_;
    static constexpr float kActionFlashDuration = 0.5f;

    // Action bar drag state (-1 = not dragging)
    int actionBarDragSlot_ = -1;
    VkDescriptorSet actionBarDragIcon_ = VK_NULL_HANDLE;

    // Bag bar state
    VkDescriptorSet backpackIconTexture_ = VK_NULL_HANDLE;
    VkDescriptorSet emptyBagSlotTexture_ = VK_NULL_HANDLE;
    int bagBarPickedSlot_ = -1;
    int bagBarDragSource_ = -1;

    // Macro editor popup state
    uint32_t macroEditorId_   = 0;
    bool     macroEditorOpen_ = false;
    char     macroEditorBuf_[256] = {};

    // Macro cooldown cache: maps macro ID → resolved primary spell ID
    std::unordered_map<uint32_t, uint32_t> macroPrimarySpellCache_;
    size_t macroCacheSpellCount_ = 0;

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    UIServices services_;
    uint32_t resolveMacroPrimarySpellId(uint32_t macroId, game::GameHandler& gameHandler);
};

} // namespace ui
} // namespace wowee
