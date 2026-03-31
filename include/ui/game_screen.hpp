#pragma once

#include "game/game_handler.hpp"
#include "game/inventory.hpp"
// WorldMap is now owned by Renderer, accessed via getWorldMap()
#include "rendering/character_preview.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/talent_screen.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/chat_panel.hpp"
#include "ui/toast_manager.hpp"
#include "ui/dialog_manager.hpp"
#include "ui/settings_panel.hpp"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; }
namespace ui {

/**
 * In-game screen UI
 *
 * Displays player info, entity list, chat, and game controls
 */
class GameScreen {
public:
    GameScreen();

    /**
     * Render the UI
     * @param gameHandler Reference to game handler
     */
    void render(game::GameHandler& gameHandler);

    /**
     * Check if chat input is active
     */
    bool isChatInputActive() const { return chatPanel_.isChatInputActive(); }

    void saveSettings();
    void loadSettings();

private:
    // Chat panel (extracted from GameScreen — owns all chat state and rendering)
    ChatPanel chatPanel_;

    // Toast manager (extracted from GameScreen — owns all toast/notification state and rendering)
    ToastManager toastManager_;

    // Dialog manager (extracted from GameScreen — owns all popup/dialog rendering)
    DialogManager dialogManager_;

    // Settings panel (extracted from GameScreen — owns all settings UI and config state)
    SettingsPanel settingsPanel_;

    // Action bar error-flash: spellId → wall-clock time (seconds) when the flash ends.
    // Populated by the SpellCastFailedCallback; queried during action bar button rendering.
    std::unordered_map<uint32_t, float> actionFlashEndTimes_;

    // UI state
    bool showEntityWindow = false;
    bool showChatWindow = true;
    bool showMinimap_ = true;  // M key toggles minimap
    bool showNameplates_ = true;  // V key toggles enemy/NPC nameplates
    uint64_t nameplateCtxGuid_ = 0; // GUID of nameplate right-clicked (0 = none)
    ImVec2 nameplateCtxPos_{};      // Screen position of nameplate right-click
    uint32_t lastPlayerHp_ = 0;   // Previous frame HP for damage flash detection
    float damageFlashAlpha_ = 0.0f; // Screen edge flash intensity (fades to 0)


    // Raid Warning / Boss Emote big-text overlay (center-screen, fades after 5s)
    struct RaidWarnEntry {
        std::string text;
        float age = 0.0f;
        bool isBossEmote = false;  // true = amber, false (raid warning) = red+yellow
        static constexpr float LIFETIME = 5.0f;
    };
    std::vector<RaidWarnEntry> raidWarnEntries_;
    bool raidWarnCallbackSet_ = false;
    size_t raidWarnChatSeenCount_ = 0;  // index into chat history for unread scan

    // UIErrorsFrame: WoW-style center-bottom error messages (spell fails, out of range, etc.)
    struct UIErrorEntry { std::string text; float age = 0.0f; };
    std::vector<UIErrorEntry> uiErrors_;
    bool uiErrorCallbackSet_ = false;
    static constexpr float kUIErrorLifetime = 2.5f;
    bool castFailedCallbackSet_ = false;
    static constexpr float kActionFlashDuration = 0.5f;  // seconds for error-red overlay to fade



    // Death screen: elapsed time since the death dialog first appeared
    float deathElapsed_ = 0.0f;
    bool deathTimerRunning_ = false;
    // WoW forces release after ~6 minutes; show countdown until then
    static constexpr float kForcedReleaseSec = 360.0f;

    bool showPlayerInfo = false;
    bool showSocialFrame_ = false;  // O key toggles social/friends list
    bool showGuildRoster_ = false;
    bool showRaidFrames_ = true;  // F key toggles raid/party frames
    bool showWorldMap_ = false;  // W key toggles world map
    std::string selectedGuildMember_;
    bool showGuildNoteEdit_ = false;
    bool editingOfficerNote_ = false;
    char guildNoteEditBuffer_[256] = {0};
    int guildRosterTab_ = 0;  // 0=Roster, 1=Guild Info
    char guildMotdEditBuffer_[256] = {0};
    bool showMotdEdit_ = false;
    char petitionNameBuffer_[64] = {0};
    char addRankNameBuffer_[64] = {0};
    bool showAddRankModal_ = false;
    bool vendorBagsOpened_ = false;  // Track if bags were auto-opened for current vendor session
    ImVec2 questTrackerPos_ = ImVec2(-1.0f, -1.0f);  // <0 = use default
    ImVec2 questTrackerSize_ = ImVec2(220.0f, 200.0f); // saved size
    float questTrackerRightOffset_ = -1.0f;            // pixels from right edge; <0 = use default
    bool questTrackerPosInit_ = false;
    bool showEscapeMenu = false;

    // Macro editor popup state
    uint32_t macroEditorId_   = 0;        // macro index being edited
    bool     macroEditorOpen_ = false;    // deferred OpenPopup flag
    char     macroEditorBuf_[256] = {};   // edit buffer

    /**
     * Render player info window
     */
    void renderPlayerInfo(game::GameHandler& gameHandler);

    /**
     * Render entity list window
     */
    void renderEntityList(game::GameHandler& gameHandler);

    /**
     * Render player unit frame (top-left)
     */
    void renderPlayerFrame(game::GameHandler& gameHandler);

    /**
     * Render target frame
     */
    void renderTargetFrame(game::GameHandler& gameHandler);
    void renderFocusFrame(game::GameHandler& gameHandler);

    /**
     * Render pet frame (below player frame when player has an active pet)
     */
    void renderPetFrame(game::GameHandler& gameHandler);
    void renderTotemFrame(game::GameHandler& gameHandler);

    /**
     * Process targeting input (Tab, Escape, click)
     */
    void processTargetInput(game::GameHandler& gameHandler);

    /**
     * Rebuild character geosets from current equipment state
     */
    void updateCharacterGeosets(game::Inventory& inventory);

    /**
     * Re-composite character skin texture from current equipment
     */
    void updateCharacterTextures(game::Inventory& inventory);

    // ---- New UI renders ----
    void renderActionBar(game::GameHandler& gameHandler);
    void renderStanceBar(game::GameHandler& gameHandler);
    void renderBagBar(game::GameHandler& gameHandler);
    void renderXpBar(game::GameHandler& gameHandler);
    void renderRepBar(game::GameHandler& gameHandler);
    void renderCastBar(game::GameHandler& gameHandler);
    void renderMirrorTimers(game::GameHandler& gameHandler);
    void renderCooldownTracker(game::GameHandler& gameHandler);
    void renderCombatText(game::GameHandler& gameHandler);
    void renderRaidWarningOverlay(game::GameHandler& gameHandler);
    void renderPartyFrames(game::GameHandler& gameHandler);
    void renderBossFrames(game::GameHandler& gameHandler);
    void renderUIErrors(game::GameHandler& gameHandler, float deltaTime);

    void renderBuffBar(game::GameHandler& gameHandler);
    void renderSocialFrame(game::GameHandler& gameHandler);
    void renderLootWindow(game::GameHandler& gameHandler);
    void renderGossipWindow(game::GameHandler& gameHandler);
    void renderQuestDetailsWindow(game::GameHandler& gameHandler);
    void renderQuestRequestItemsWindow(game::GameHandler& gameHandler);
    void renderQuestOfferRewardWindow(game::GameHandler& gameHandler);
    void renderVendorWindow(game::GameHandler& gameHandler);
    void renderTrainerWindow(game::GameHandler& gameHandler);
    void renderBarberShopWindow(game::GameHandler& gameHandler);
    void renderStableWindow(game::GameHandler& gameHandler);
    void renderTaxiWindow(game::GameHandler& gameHandler);
    void renderLogoutCountdown(game::GameHandler& gameHandler);
    void renderDeathScreen(game::GameHandler& gameHandler);
    void renderReclaimCorpseButton(game::GameHandler& gameHandler);
    void renderEscapeMenu();
    void renderQuestMarkers(game::GameHandler& gameHandler);
    void renderMinimapMarkers(game::GameHandler& gameHandler);
    void renderQuestObjectiveTracker(game::GameHandler& gameHandler);
    void renderGuildRoster(game::GameHandler& gameHandler);
    void renderMailWindow(game::GameHandler& gameHandler);
    void renderMailComposeWindow(game::GameHandler& gameHandler);
    void renderBankWindow(game::GameHandler& gameHandler);
    void renderGuildBankWindow(game::GameHandler& gameHandler);
    void renderAuctionHouseWindow(game::GameHandler& gameHandler);
    void renderDungeonFinderWindow(game::GameHandler& gameHandler);
    void renderInstanceLockouts(game::GameHandler& gameHandler);
    void renderNameplates(game::GameHandler& gameHandler);
    void renderBattlegroundScore(game::GameHandler& gameHandler);
    void renderDPSMeter(game::GameHandler& gameHandler);
    void renderDurabilityWarning(game::GameHandler& gameHandler);
    void takeScreenshot(game::GameHandler& gameHandler);

    /**
     * Inventory screen
     */
    void renderWorldMap(game::GameHandler& gameHandler);

    InventoryScreen inventoryScreen;
    uint64_t inventoryScreenCharGuid_ = 0;  // GUID of character inventory screen was initialized for
    QuestLogScreen questLogScreen;
    SpellbookScreen spellbookScreen;
    TalentScreen talentScreen;
    // WorldMap is now owned by Renderer (accessed via renderer->getWorldMap())

    // Spell icon cache: spellId -> GL texture ID
    std::unordered_map<uint32_t, VkDescriptorSet> spellIconCache_;
    // SpellIconID -> icon path (from SpellIcon.dbc)
    std::unordered_map<uint32_t, std::string> spellIconPaths_;
    // SpellID -> SpellIconID (from Spell.dbc field 133)
    std::unordered_map<uint32_t, uint32_t> spellIconIds_;
    bool spellIconDbLoaded_ = false;
    VkDescriptorSet getSpellIcon(uint32_t spellId, pipeline::AssetManager* am);

    // ItemExtendedCost.dbc cache: extendedCostId -> cost details
    struct ExtendedCostEntry {
        uint32_t honorPoints = 0;
        uint32_t arenaPoints = 0;
        uint32_t itemId[5] = {};
        uint32_t itemCount[5] = {};
    };
    std::unordered_map<uint32_t, ExtendedCostEntry> extendedCostCache_;
    bool extendedCostDbLoaded_ = false;
    void loadExtendedCostDBC();
    std::string formatExtendedCost(uint32_t extendedCostId, game::GameHandler& gameHandler);

    // Macro cooldown cache: maps macro ID → resolved primary spell ID (0 = no spell found)
    std::unordered_map<uint32_t, uint32_t> macroPrimarySpellCache_;
    size_t macroCacheSpellCount_ = 0;  // invalidates cache when spell list changes
    uint32_t resolveMacroPrimarySpellId(uint32_t macroId, game::GameHandler& gameHandler);

    // Death Knight rune bar: client-predicted fill (0.0=depleted, 1.0=ready) for smooth animation
    float runeClientFill_[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    // Action bar drag state (-1 = not dragging)
    int actionBarDragSlot_ = -1;
    VkDescriptorSet actionBarDragIcon_ = VK_NULL_HANDLE;

    // Bag bar state
    VkDescriptorSet backpackIconTexture_ = VK_NULL_HANDLE;
    VkDescriptorSet emptyBagSlotTexture_ = VK_NULL_HANDLE;
    int bagBarPickedSlot_ = -1;   // Visual drag in progress (-1 = none)
    int bagBarDragSource_ = -1;   // Mouse pressed on this slot, waiting for drag or click (-1 = none)

    // Who Results window
    bool  showWhoWindow_ = false;
    void  renderWhoWindow(game::GameHandler& gameHandler);

    // Combat Log window
    bool  showCombatLog_ = false;
    void  renderCombatLog(game::GameHandler& gameHandler);

    // Instance Lockouts window
    bool  showInstanceLockouts_ = false;

    // Dungeon Finder state
    bool  showDungeonFinder_ = false;

    // Achievements window
    bool  showAchievementWindow_ = false;
    char  achievementSearchBuf_[128] = {};
    void  renderAchievementWindow(game::GameHandler& gameHandler);

    // Skills / Professions window (K key)
    bool  showSkillsWindow_ = false;
    void  renderSkillsWindow(game::GameHandler& gameHandler);

    // Titles window
    bool  showTitlesWindow_ = false;
    void  renderTitlesWindow(game::GameHandler& gameHandler);

    // Equipment Set Manager window
    bool  showEquipSetWindow_ = false;
    void  renderEquipSetWindow(game::GameHandler& gameHandler);

    // GM Ticket window
    bool  showGmTicketWindow_     = false;
    bool  gmTicketWindowWasOpen_  = false; ///< Previous frame state; used to fire one-shot query
    char  gmTicketBuf_[2048] = {};
    void  renderGmTicketWindow(game::GameHandler& gameHandler);

    // Pet rename modal (triggered from pet frame context menu)
    bool petRenameOpen_ = false;
    char petRenameBuf_[16] = {};

    // Inspect window
    bool  showInspectWindow_ = false;
    void  renderInspectWindow(game::GameHandler& gameHandler);

    // Readable text window (books / scrolls / notes)
    bool  showBookWindow_ = false;
    int   bookCurrentPage_ = 0;
    void  renderBookWindow(game::GameHandler& gameHandler);

    // Threat window
    bool  showThreatWindow_ = false;
    void  renderThreatWindow(game::GameHandler& gameHandler);

    // BG scoreboard window
    bool  showBgScoreboard_ = false;
    void  renderBgScoreboard(game::GameHandler& gameHandler);
    uint8_t lfgRoles_ = 0x08;  // default: DPS (0x02=tank, 0x04=healer, 0x08=dps)
    uint32_t lfgSelectedDungeon_ = 861;  // default: random dungeon (entry 861 = Random Dungeon WotLK)

    // Mail compose state
    char mailRecipientBuffer_[256] = "";
    char mailSubjectBuffer_[256] = "";
    char mailBodyBuffer_[2048] = "";
    int mailComposeMoney_[3] = {0, 0, 0};  // gold, silver, copper

    // Vendor search filter
    char vendorSearchFilter_[128] = "";

    // Vendor purchase confirmation for expensive items
    bool vendorConfirmOpen_ = false;
    uint64_t vendorConfirmGuid_ = 0;
    uint32_t vendorConfirmItemId_ = 0;
    uint32_t vendorConfirmSlot_ = 0;
    uint32_t vendorConfirmQty_ = 1;
    uint32_t vendorConfirmPrice_ = 0;
    std::string vendorConfirmItemName_;

    // Barber shop UI state
    int barberHairStyle_ = 0;
    int barberHairColor_ = 0;
    int barberFacialHair_ = 0;
    int barberOrigHairStyle_ = 0;
    int barberOrigHairColor_ = 0;
    int barberOrigFacialHair_ = 0;
    bool barberInitialized_ = false;

    // Trainer search filter
    char trainerSearchFilter_[128] = "";

    // Auction house UI state
    char auctionSearchName_[256] = "";
    int auctionLevelMin_ = 0;
    int auctionLevelMax_ = 0;
    int auctionQuality_ = 0;
    int auctionSellDuration_ = 2;  // 0=12h, 1=24h, 2=48h
    int auctionSellBid_[3] = {0, 0, 0};     // gold, silver, copper
    int auctionSellBuyout_[3] = {0, 0, 0};  // gold, silver, copper
    int auctionSelectedItem_ = -1;
    int auctionSellSlotIndex_ = -1;          // Selected backpack slot for selling
    uint32_t auctionBrowseOffset_ = 0;       // Pagination offset for browse results
    int auctionItemClass_ = -1;              // Item class filter (-1 = All)
    int auctionItemSubClass_ = -1;           // Item subclass filter (-1 = All)
    bool auctionUsableOnly_ = false;         // Filter to items usable by current class/level

    // Guild bank money input
    int guildBankMoneyInput_[3] = {0, 0, 0};  // gold, silver, copper

    // Left-click targeting: distinguish click from camera drag
    glm::vec2 leftClickPressPos_ = glm::vec2(0.0f);
    bool leftClickWasPress_ = false;


    bool appearanceCallbackSet_ = false;
    bool ghostOpacityStateKnown_ = false;
    bool ghostOpacityLastState_ = false;
    uint32_t ghostOpacityLastInstanceId_ = 0;


    void renderWeatherOverlay(game::GameHandler& gameHandler);

    // DPS / HPS meter
    float dpsCombatAge_ = 0.0f;   // seconds in current combat (for accurate early-combat DPS)
    bool dpsWasInCombat_ = false;
    float dpsEncounterDamage_ = 0.0f;  // total player damage this combat
    float dpsEncounterHeal_   = 0.0f;  // total player healing this combat
    size_t dpsLogSeenCount_   = 0;     // log entries already scanned

public:
    void openDungeonFinder() { showDungeonFinder_ = true; }
    ToastManager& toastManager() { return toastManager_; }
};

} // namespace ui
} // namespace wowee
