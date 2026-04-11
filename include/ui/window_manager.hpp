// ============================================================
// WindowManager — extracted from GameScreen
// Owns all NPC interaction windows, popup dialogs, and misc
// overlay UI: loot, gossip, quest, vendor, trainer, mail, bank,
// auction house, barber, stable, taxi, escape menu, death screen,
// instance lockouts, achievements, GM ticket, books, titles,
// equipment sets, skills.
// ============================================================
#pragma once
#include "ui/ui_services.hpp"
#include <cstdint>
#include <string>
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

class WindowManager {
public:
    // Callback type for resolving spell icons (spellId, assetMgr) → VkDescriptorSet
    using SpellIconFn = std::function<VkDescriptorSet(uint32_t, pipeline::AssetManager*)>;

    // ---- NPC interaction windows ----
    void renderLootWindow(game::GameHandler& gameHandler,
                          InventoryScreen& inventoryScreen,
                          ChatPanel& chatPanel);
    void renderGossipWindow(game::GameHandler& gameHandler,
                            ChatPanel& chatPanel);
    void renderQuestDetailsWindow(game::GameHandler& gameHandler,
                                  ChatPanel& chatPanel,
                                  InventoryScreen& inventoryScreen);
    void renderQuestRequestItemsWindow(game::GameHandler& gameHandler,
                                       ChatPanel& chatPanel,
                                       InventoryScreen& inventoryScreen);
    void renderQuestOfferRewardWindow(game::GameHandler& gameHandler,
                                      ChatPanel& chatPanel,
                                      InventoryScreen& inventoryScreen);
    void renderVendorWindow(game::GameHandler& gameHandler,
                            InventoryScreen& inventoryScreen,
                            ChatPanel& chatPanel);
    void renderTrainerWindow(game::GameHandler& gameHandler,
                             SpellIconFn getSpellIcon);
    void renderBarberShopWindow(game::GameHandler& gameHandler);
    void renderStableWindow(game::GameHandler& gameHandler);
    void renderTaxiWindow(game::GameHandler& gameHandler);

    // ---- Mail and banking ----
    void renderMailWindow(game::GameHandler& gameHandler,
                          InventoryScreen& inventoryScreen,
                          ChatPanel& chatPanel);
    void renderMailComposeWindow(game::GameHandler& gameHandler,
                                 InventoryScreen& inventoryScreen);
    void renderBankWindow(game::GameHandler& gameHandler,
                          InventoryScreen& inventoryScreen,
                          ChatPanel& chatPanel);
    void renderGuildBankWindow(game::GameHandler& gameHandler,
                               InventoryScreen& inventoryScreen,
                               ChatPanel& chatPanel);
    void renderAuctionHouseWindow(game::GameHandler& gameHandler,
                                  InventoryScreen& inventoryScreen,
                                  ChatPanel& chatPanel);

    // ---- Popup / overlay windows ----
    void renderEscapeMenu(SettingsPanel& settingsPanel);
    void renderLogoutCountdown(game::GameHandler& gameHandler);
    void renderDeathScreen(game::GameHandler& gameHandler);
    void renderReclaimCorpseButton(game::GameHandler& gameHandler);
    void renderInstanceLockouts(game::GameHandler& gameHandler);
    void renderAchievementWindow(game::GameHandler& gameHandler);
    void renderGmTicketWindow(game::GameHandler& gameHandler);
    void renderBookWindow(game::GameHandler& gameHandler);
    void renderTitlesWindow(game::GameHandler& gameHandler);
    void renderEquipSetWindow(game::GameHandler& gameHandler);
    void renderSkillsWindow(game::GameHandler& gameHandler);

    // ---- State owned by this manager ----

    // Instance lockouts
    bool showInstanceLockouts_ = false;

    // Achievements
    bool showAchievementWindow_ = false;
    char achievementSearchBuf_[128] = {};

    // Skills / Professions
    bool showSkillsWindow_ = false;

    // Titles
    bool showTitlesWindow_ = false;

    // Equipment Sets
    bool showEquipSetWindow_ = false;

    // GM Ticket
    bool showGmTicketWindow_     = false;
    bool gmTicketWindowWasOpen_  = false;
    char gmTicketBuf_[2048] = {};

    // Book / scroll reader
    bool showBookWindow_ = false;
    int  bookCurrentPage_ = 0;

    // Death screen
    float deathElapsed_ = 0.0f;
    bool deathTimerRunning_ = false;
    static constexpr float kForcedReleaseSec = 360.0f;

    // Escape menu
    bool showEscapeMenu = false;

    // Mail compose
    char mailRecipientBuffer_[256] = "";
    char mailSubjectBuffer_[256] = "";
    char mailBodyBuffer_[2048] = "";
    int mailComposeMoney_[3] = {0, 0, 0};

    // Vendor
    char vendorSearchFilter_[128] = "";
    bool vendorConfirmOpen_ = false;
    uint64_t vendorConfirmGuid_ = 0;
    uint32_t vendorConfirmItemId_ = 0;
    uint32_t vendorConfirmSlot_ = 0;
    uint32_t vendorConfirmQty_ = 1;
    uint32_t vendorConfirmPrice_ = 0;
    std::string vendorConfirmItemName_;
    bool vendorBagsOpened_ = false;

    // Barber shop
    int barberHairStyle_ = 0;
    int barberHairColor_ = 0;
    int barberFacialHair_ = 0;
    int barberOrigHairStyle_ = 0;
    int barberOrigHairColor_ = 0;
    int barberOrigFacialHair_ = 0;
    bool barberInitialized_ = false;

    // Trainer
    char trainerSearchFilter_[128] = "";

    // Auction house
    char auctionSearchName_[256] = "";
    int auctionLevelMin_ = 0;
    int auctionLevelMax_ = 0;
    int auctionQuality_ = 0;
    int auctionSellDuration_ = 2;
    int auctionSellBid_[3] = {0, 0, 0};
    int auctionSellBuyout_[3] = {0, 0, 0};
    int auctionSelectedItem_ = -1;
    int auctionSellSlotIndex_ = -1;
    uint32_t auctionBrowseOffset_ = 0;
    int auctionItemClass_ = -1;
    int auctionItemSubClass_ = -1;
    bool auctionUsableOnly_ = false;

    // Guild bank money input
    int guildBankMoneyInput_[3] = {0, 0, 0};

    // ItemExtendedCost.dbc cache
    struct ExtendedCostEntry {
        uint32_t honorPoints = 0;
        uint32_t arenaPoints = 0;
        uint32_t itemId[5] = {};
        uint32_t itemCount[5] = {};
    };
    std::unordered_map<uint32_t, ExtendedCostEntry> extendedCostCache_;
    bool extendedCostDbLoaded_ = false;

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

private:
    UIServices services_;
    void loadExtendedCostDBC();
    std::string formatExtendedCost(uint32_t extendedCostId, game::GameHandler& gameHandler);
};

} // namespace ui
} // namespace wowee
