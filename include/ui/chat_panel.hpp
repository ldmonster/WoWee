#pragma once

#include "game/game_handler.hpp"
#include "ui/ui_services.hpp"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <functional>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; }
namespace ui {

class InventoryScreen;
class SpellbookScreen;
class QuestLogScreen;

/**
 * Self-contained chat UI panel extracted from GameScreen.
 *
 * Owns all chat state: input buffer, sent-history, tab filtering,
 * slash-command parsing, chat bubbles, and chat-related settings.
 */
class ChatPanel {
public:
    ChatPanel();

    // ---- Main entry points (called by GameScreen) ----

    /**
     * Render the chat window (tabs, history, input, etc.)
     */
    void render(game::GameHandler& gameHandler,
               InventoryScreen& inventoryScreen,
               SpellbookScreen& spellbookScreen,
               QuestLogScreen& questLogScreen);

    /**
     * Render 3D-projected chat bubbles above entities.
     */
    void renderBubbles(game::GameHandler& gameHandler);

    /**
     * Register one-shot callbacks on GameHandler (call once per session).
     * Sets up the chat-bubble callback.
     */
    void setupCallbacks(game::GameHandler& gameHandler);

    // ---- Input helpers (called by GameScreen keybind handling) ----

    bool isChatInputActive() const { return chatInputActive_; }

    /** Insert a spell / item link into the chat input buffer (shift-click). */
    void insertChatLink(const std::string& link);

    /** Activate the input field with a leading '/' (slash key). */
    void activateSlashInput();

    /** Activate (focus) the input field (Enter key). */
    void activateInput();

    /** Request that the chat input be focused next frame. */
    void requestRefocus() { refocusChatInput_ = true; }

    /** Set up a whisper to the given player name and focus input. */
    void setWhisperTarget(const std::string& name);

    /** Execute a macro body (one line per 'click'). */
    void executeMacroText(game::GameHandler& gameHandler,
                          InventoryScreen& inventoryScreen,
                          SpellbookScreen& spellbookScreen,
                          QuestLogScreen& questLogScreen,
                          const std::string& macroText);

    // ---- Slash-command side-effects ----
    // GameScreen reads these each frame, then clears them.

    struct SlashCommands {
        bool showInspect   = false;
        bool toggleThreat  = false;
        bool showBgScore   = false;
        bool showGmTicket  = false;
        bool showWho       = false;
        bool toggleCombatLog = false;
        bool takeScreenshot = false;
    };

    /** Return accumulated slash-command flags and reset them. */
    SlashCommands consumeSlashCommands();

    // ---- Chat settings (read/written by GameScreen save/load & settings tab) ----

    bool  chatShowTimestamps  = false;
    int   chatFontSize        = 1;   // 0=small, 1=medium, 2=large
    bool  chatAutoJoinGeneral = true;
    bool  chatAutoJoinTrade   = true;
    bool  chatAutoJoinLocalDefense = true;
    bool  chatAutoJoinLFG     = true;
    bool  chatAutoJoinLocal   = true;
    int   activeChatTab       = 0;

    /** Spell icon lookup callback — set by GameScreen each frame before render(). */
    std::function<VkDescriptorSet(uint32_t, pipeline::AssetManager*)> getSpellIcon;

    /** Render the "Chat" tab inside the Settings window. */
    void renderSettingsTab(std::function<void()> saveSettingsFn);

    /** Reset all chat settings to defaults. */
    void restoreDefaults();

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services) { services_ = services; }

    /** Replace $g/$G and $n/$N gender/name placeholders in quest/chat text. */
    std::string replaceGenderPlaceholders(const std::string& text, game::GameHandler& gameHandler);

private:
    // Injected UI services (Phase B singleton breaking)
    UIServices services_;

    // ---- Chat input state ----
    char chatInputBuffer_[512] = "";
    char whisperTargetBuffer_[256] = "";
    bool chatInputActive_ = false;
    int  selectedChatType_ = 0;  // 0=SAY .. 10=CHANNEL
    int  lastChatType_     = 0;
    int  selectedChannelIdx_ = 0;
    bool chatInputMoveCursorToEnd_ = false;
    bool refocusChatInput_ = false;

    // Sent-message history (Up/Down arrow recall)
    std::vector<std::string> chatSentHistory_;
    int chatHistoryIdx_ = -1;

    // Macro stop flag
    bool macroStopped_ = false;

    // Tab-completion state
    std::string chatTabPrefix_;
    std::vector<std::string> chatTabMatches_;
    int chatTabMatchIdx_ = -1;

    // Mention notification
    size_t chatMentionSeenCount_ = 0;

    // ---- Chat tabs ----
    struct ChatTab {
        std::string name;
        uint64_t typeMask;
    };
    std::vector<ChatTab> chatTabs_;
    std::vector<int> chatTabUnread_;
    size_t chatTabSeenCount_ = 0;

    void initChatTabs();
    bool shouldShowMessage(const game::MessageChatData& msg, int tabIndex) const;

    // ---- Chat window visual state ----
    bool  chatScrolledUp_          = false;
    bool  chatForceScrollToBottom_ = false;
    bool  chatWindowLocked_        = true;
    ImVec2 chatWindowPos_          = ImVec2(0.0f, 0.0f);
    bool  chatWindowPosInit_       = false;

    // ---- Chat bubbles ----
    struct ChatBubble {
        uint64_t senderGuid   = 0;
        std::string message;
        float timeRemaining   = 0.0f;
        float totalDuration   = 0.0f;
        bool isYell           = false;
    };
    std::vector<ChatBubble> chatBubbles_;
    bool chatBubbleCallbackSet_ = false;

    // ---- Whisper toast state (populated in render, rendered by GameScreen/ToastManager) ----
    // Whisper scanning lives here because it's tightly coupled to chat history iteration.
    size_t whisperSeenCount_ = 0;

    // ---- Helpers ----
    void sendChatMessage(game::GameHandler& gameHandler,
                         InventoryScreen& inventoryScreen,
                         SpellbookScreen& spellbookScreen,
                         QuestLogScreen& questLogScreen);
    const char* getChatTypeName(game::ChatType type) const;
    ImVec4 getChatTypeColor(game::ChatType type) const;

    // Cached game handler for input callback (set each frame in render)
    game::GameHandler* cachedGameHandler_ = nullptr;

    // Join channel input buffer
    char joinChannelBuffer_[128] = "";

    // Slash command flags (accumulated, consumed by GameScreen)
    SlashCommands slashCmds_;
};

} // namespace ui
} // namespace wowee
