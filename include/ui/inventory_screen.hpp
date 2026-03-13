#pragma once

#include "game/inventory.hpp"
#include "game/character.hpp"
#include "game/world_packets.hpp"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <array>
#include <functional>
#include <memory>
#include <unordered_map>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterPreview; class CharacterRenderer; }
namespace game { class GameHandler; }
namespace ui {

class InventoryScreen {
public:
    ~InventoryScreen();

    /// Render bags window (B key). Positioned at bottom of screen.
    void render(game::Inventory& inventory, uint64_t moneyCopper);

    /// Render character screen (C key). Standalone equipment window.
    void renderCharacterScreen(game::GameHandler& gameHandler);

    bool isOpen() const { return open; }
    void toggle() { open = !open; }
    void setOpen(bool o) { open = o; }

    // Separate bag window controls
    void toggleBackpack();
    void toggleBag(int idx);
    void openAllBags();
    void closeAllBags();
    void setSeparateBags(bool sep) { separateBags_ = sep; }
    bool isSeparateBags() const { return separateBags_; }
    void toggleCompactBags() { compactBags_ = !compactBags_; }
    bool isCompactBags() const { return compactBags_; }
    bool isBackpackOpen() const { return backpackOpen_; }
    bool isBagOpen(int idx) const { return idx >= 0 && idx < 4 ? bagOpen_[idx] : false; }

    bool isCharacterOpen() const { return characterOpen; }
    void toggleCharacter() { characterOpen = !characterOpen; }
    void setCharacterOpen(bool o) { characterOpen = o; }

    /// Enable vendor mode: right-clicking bag items sells them.
    void setVendorMode(bool enabled, game::GameHandler* handler) {
        vendorMode_ = enabled;
        gameHandler_ = handler;
    }
    void setGameHandler(game::GameHandler* handler) { gameHandler_ = handler; }

    /// Set asset manager for icon/model loading
    void setAssetManager(pipeline::AssetManager* am) { assetManager_ = am; }

    /// Store player appearance for character preview
    void setPlayerAppearance(game::Race race, game::Gender gender,
                             uint8_t skin, uint8_t face,
                             uint8_t hairStyle, uint8_t hairColor,
                             uint8_t facialHair);

    /// Mark the character preview as needing equipment update
    void markPreviewDirty() { previewDirty_ = true; }

    /// Update the preview animation (call each frame)
    void updatePreview(float deltaTime);

    /// Returns true if equipment changed since last call, and clears the flag.
    bool consumeEquipmentDirty() { bool d = equipmentDirty; equipmentDirty = false; return d; }
    /// Returns true if any inventory slot changed since last call, and clears the flag.
    bool consumeInventoryDirty() { bool d = inventoryDirty; inventoryDirty = false; return d; }

private:
    bool open = false;
    bool characterOpen = false;
    bool bKeyWasDown = false;
    bool separateBags_ = true;
    bool compactBags_ = false;
    bool backpackOpen_ = false;
    std::array<bool, 4> bagOpen_{};
    bool cKeyWasDown = false;
    bool equipmentDirty = false;
    bool inventoryDirty = false;

    // Vendor sell mode
    bool vendorMode_ = false;
    game::GameHandler* gameHandler_ = nullptr;

    // Asset manager for icons and preview
    pipeline::AssetManager* assetManager_ = nullptr;

    // Item icon cache: displayInfoId -> GL texture
    std::unordered_map<uint32_t, VkDescriptorSet> iconCache_;
public:
    VkDescriptorSet getItemIcon(uint32_t displayInfoId);
    void renderItemTooltip(const game::ItemQueryResponseData& info, const game::Inventory* inventory = nullptr);
private:

    // Character model preview
    std::unique_ptr<rendering::CharacterPreview> charPreview_;
    bool previewInitialized_ = false;
    bool previewDirty_ = false;

    // Stored player appearance for preview
    game::Race playerRace_ = game::Race::HUMAN;
    game::Gender playerGender_ = game::Gender::MALE;
    uint8_t playerSkin_ = 0;
    uint8_t playerFace_ = 0;
    uint8_t playerHairStyle_ = 0;
    uint8_t playerHairColor_ = 0;
    uint8_t playerFacialHair_ = 0;

    void initPreview();
    void updatePreviewEquipment(game::Inventory& inventory);

    // Drag-and-drop held item state
    bool holdingItem = false;
    game::ItemDef heldItem;
    enum class HeldSource { NONE, BACKPACK, BAG, EQUIPMENT, BANK, BANK_BAG, BANK_BAG_EQUIP };
    HeldSource heldSource = HeldSource::NONE;
    int heldBackpackIndex = -1;
    int heldBagIndex = -1;
    int heldBagSlotIndex = -1;
    int heldBankIndex = -1;
    int heldBankBagIndex = -1;
    int heldBankBagSlotIndex = -1;
    game::EquipSlot heldEquipSlot = game::EquipSlot::NUM_SLOTS;

    // Slot rendering with interaction support
    enum class SlotKind { BACKPACK, EQUIPMENT };

    // Click-and-hold pickup tracking
    bool pickupPending_ = false;
    float pickupPressTime_ = 0.0f;
    SlotKind pickupSlotKind_ = SlotKind::BACKPACK;
    int pickupBackpackIndex_ = -1;
    int pickupBagIndex_ = -1;
    int pickupBagSlotIndex_ = -1;
    game::EquipSlot pickupEquipSlot_ = game::EquipSlot::NUM_SLOTS;
    static constexpr float kPickupHoldThreshold = 0.10f; // seconds

    void renderSeparateBags(game::Inventory& inventory, uint64_t moneyCopper);
    void renderAggregateBags(game::Inventory& inventory, uint64_t moneyCopper);
    void renderBagWindow(const char* title, bool& isOpen, game::Inventory& inventory,
                         int bagIndex, float defaultX, float defaultY, uint64_t moneyCopper);
    void renderEquipmentPanel(game::Inventory& inventory);
    void renderBackpackPanel(game::Inventory& inventory, bool collapseEmptySections = false);
    void renderStatsPanel(game::Inventory& inventory, uint32_t playerLevel, int32_t serverArmor = 0,
                          const int32_t* serverStats = nullptr, const int32_t* serverResists = nullptr,
                          const game::GameHandler* gh = nullptr);
    void renderReputationPanel(game::GameHandler& gameHandler);

    void renderItemSlot(game::Inventory& inventory, const game::ItemSlot& slot,
                        float size, const char* label,
                        SlotKind kind, int backpackIndex,
                        game::EquipSlot equipSlot,
                        int bagIndex = -1, int bagSlotIndex = -1);
    void renderItemTooltip(const game::ItemDef& item, const game::Inventory* inventory = nullptr);

    // Held item helpers
    void pickupFromBackpack(game::Inventory& inv, int index);
    void pickupFromBag(game::Inventory& inv, int bagIndex, int slotIndex);
    void pickupFromEquipment(game::Inventory& inv, game::EquipSlot slot);
    void placeInBackpack(game::Inventory& inv, int index);
    void placeInBag(game::Inventory& inv, int bagIndex, int slotIndex);
    void placeInEquipment(game::Inventory& inv, game::EquipSlot slot);
    void cancelPickup(game::Inventory& inv);
    game::EquipSlot getEquipSlotForType(uint8_t inventoryType, game::Inventory& inv);
    void renderHeldItem();
    bool bagHasAnyItems(const game::Inventory& inventory, int bagIndex) const;

    // Drop confirmation (drag-outside-window destroy)
    bool dropConfirmOpen_ = false;
    int dropBackpackIndex_ = -1;
    std::string dropItemName_;

    // Destroy confirmation (Shift+right-click destroy)
    bool destroyConfirmOpen_ = false;
    uint8_t destroyBag_ = 0xFF;
    uint8_t destroySlot_ = 0;
    uint8_t destroyCount_ = 1;
    std::string destroyItemName_;

    // Pending chat item link from shift-click
    std::string pendingChatItemLink_;

public:
    static ImVec4 getQualityColor(game::ItemQuality quality);

    /// Returns true if the user is currently holding an item (pickup cursor).
    bool isHoldingItem() const { return holdingItem; }
    /// Returns the item being held (only valid when isHoldingItem() is true).
    const game::ItemDef& getHeldItem() const { return heldItem; }
    /// Begin pickup from an equipment slot (e.g., bag bar slot) into held cursor.
    bool beginPickupFromEquipSlot(game::Inventory& inv, game::EquipSlot slot);
    /// Cancel the pickup, returning the item to its original slot.
    void returnHeldItem(game::Inventory& inv) { cancelPickup(inv); }
    /// Drop the currently held item into a specific equipment slot.
    /// Returns true if the drop was accepted and consumed.
    bool dropHeldItemToEquipSlot(game::Inventory& inv, game::EquipSlot slot);
    /// Returns a WoW item link string if the user shift-clicked a bag item, then clears it.
    std::string getAndClearPendingChatLink() {
        std::string out = std::move(pendingChatItemLink_);
        pendingChatItemLink_.clear();
        return out;
    }

    /// Drop the currently held item into a bank slot via CMSG_SWAP_ITEM.
    void dropIntoBankSlot(game::GameHandler& gh, uint8_t dstBag, uint8_t dstSlot);
    /// Pick up an item from main bank slot (click-and-hold from bank window).
    void pickupFromBank(game::Inventory& inv, int bankIndex);
    /// Pick up an item from a bank bag slot (click-and-hold from bank window).
    void pickupFromBankBag(game::Inventory& inv, int bagIndex, int slotIndex);
    /// Pick up a bag from a bank bag equip slot (click-and-hold from bank window).
    void pickupFromBankBagEquip(game::Inventory& inv, int bagIndex);
};

} // namespace ui
} // namespace wowee
