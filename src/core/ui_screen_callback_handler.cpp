#include "core/ui_screen_callback_handler.hpp"
#include "core/application.hpp"  // AppState
#include "core/logger.hpp"
#include "ui/ui_manager.hpp"
#include "auth/auth_handler.hpp"
#include "game/game_handler.hpp"
#include "game/expansion_profile.hpp"
#include "game/world_packets.hpp"
#include "pipeline/asset_manager.hpp"

namespace wowee { namespace core {

UIScreenCallbackHandler::UIScreenCallbackHandler(
    ui::UIManager& uiManager,
    game::GameHandler& gameHandler,
    auth::AuthHandler& authHandler,
    game::ExpansionRegistry* expansionRegistry,
    pipeline::AssetManager* assetManager,
    SetStateFn setState)
    : uiManager_(uiManager)
    , gameHandler_(gameHandler)
    , authHandler_(authHandler)
    , expansionRegistry_(expansionRegistry)
    , assetManager_(assetManager)
    , setState_(std::move(setState))
{
}

void UIScreenCallbackHandler::setupCallbacks() {
    // Authentication screen callback
    uiManager_.getAuthScreen().setOnSuccess([this]() {
        LOG_INFO("Authentication successful, transitioning to realm selection");
        setState_(AppState::REALM_SELECTION);
    });

    // Realm selection callback
    uiManager_.getRealmScreen().setOnRealmSelected([this](const std::string& realmName, const std::string& realmAddress) {
        LOG_INFO("Realm selected: ", realmName, " (", realmAddress, ")");

        // Parse realm address (format: "hostname:port")
        std::string host = realmAddress;
        uint16_t port = 8085;  // Default world server port

        size_t colonPos = realmAddress.find(':');
        if (colonPos != std::string::npos) {
            host = realmAddress.substr(0, colonPos);
            try { port = static_cast<uint16_t>(std::stoi(realmAddress.substr(colonPos + 1))); }
            catch (...) { LOG_WARNING("Invalid port in realm address: ", realmAddress); }
        }

        // Connect to world server
        const auto& sessionKey = authHandler_.getSessionKey();
        std::string accountName = authHandler_.getUsername();
        if (accountName.empty()) {
            LOG_WARNING("Auth username missing; falling back to TESTACCOUNT");
            accountName = "TESTACCOUNT";
        }

        uint32_t realmId = 0;
        uint16_t realmBuild = 0;
        {
            // WotLK AUTH_SESSION includes a RealmID field; some servers reject if it's wrong/zero.
            const auto& realms = authHandler_.getRealms();
            for (const auto& r : realms) {
                if (r.name == realmName && r.address == realmAddress) {
                    realmId = r.id;
                    realmBuild = r.build;
                    break;
                }
            }
            LOG_INFO("Selected realmId=", realmId, " realmBuild=", realmBuild);
        }

        uint32_t clientBuild = 12340; // default WotLK
        if (expansionRegistry_) {
            auto* profile = expansionRegistry_->getActive();
            if (profile) clientBuild = profile->worldBuild;
        }
        // Prefer realm-reported build when available (e.g. vanilla servers
        // that report build 5875 in the realm list)
        if (realmBuild != 0) {
            clientBuild = realmBuild;
            LOG_INFO("Using realm-reported build: ", clientBuild);
        }
        if (gameHandler_.connect(host, port, sessionKey, accountName, clientBuild, realmId)) {
            LOG_INFO("Connected to world server, transitioning to character selection");
            setState_(AppState::CHARACTER_SELECTION);
        } else {
            LOG_ERROR("Failed to connect to world server");
        }
    });

    // Realm screen back button - return to login
    uiManager_.getRealmScreen().setOnBack([this]() {
        authHandler_.disconnect();
        uiManager_.getRealmScreen().reset();
        setState_(AppState::AUTHENTICATION);
    });

    // Character selection callback
    uiManager_.getCharacterScreen().setOnCharacterSelected([this](uint64_t characterGuid) {
        LOG_INFO("Character selected: GUID=0x", std::hex, characterGuid, std::dec);
        // Always set the active character GUID
        gameHandler_.setActiveCharacterGuid(characterGuid);
        // Keep CHARACTER_SELECTION active until world entry is fully loaded.
        // This avoids exposing pre-load hitching before the loading screen/intro.
    });

    // Character create screen callbacks
    uiManager_.getCharacterCreateScreen().setOnCreate([this](const game::CharCreateData& data) {
        pendingCreatedCharacterName_ = data.name;  // Store name for auto-selection
        gameHandler_.createCharacter(data);
    });

    uiManager_.getCharacterCreateScreen().setOnCancel([this]() {
        setState_(AppState::CHARACTER_SELECTION);
    });

    // Character create result callback
    gameHandler_.setCharCreateCallback([this](bool success, const std::string& msg) {
        if (success) {
            // Auto-select the newly created character
            if (!pendingCreatedCharacterName_.empty()) {
                uiManager_.getCharacterScreen().selectCharacterByName(pendingCreatedCharacterName_);
                pendingCreatedCharacterName_.clear();
            }
            setState_(AppState::CHARACTER_SELECTION);
        } else {
            uiManager_.getCharacterCreateScreen().setStatus(msg, true);
            pendingCreatedCharacterName_.clear();
        }
    });

    // Character login failure callback
    gameHandler_.setCharLoginFailCallback([this](const std::string& reason) {
        LOG_WARNING("Character login failed: ", reason);
        setState_(AppState::CHARACTER_SELECTION);
        uiManager_.getCharacterScreen().setStatus("Login failed: " + reason, true);
    });

    // "Create Character" button on character screen
    uiManager_.getCharacterScreen().setOnCreateCharacter([this]() {
        uiManager_.getCharacterCreateScreen().reset();
        // Apply expansion race/class constraints before showing the screen
        if (expansionRegistry_ && expansionRegistry_->getActive()) {
            auto* profile = expansionRegistry_->getActive();
            uiManager_.getCharacterCreateScreen().setExpansionConstraints(
                profile->races, profile->classes);
        }
        uiManager_.getCharacterCreateScreen().initializePreview(assetManager_);
        setState_(AppState::CHARACTER_CREATION);
    });

    // "Back" button on character screen
    uiManager_.getCharacterScreen().setOnBack([this]() {
        // Disconnect from world server and reset UI state for fresh realm selection
        gameHandler_.disconnect();
        uiManager_.getRealmScreen().reset();
        uiManager_.getCharacterScreen().reset();
        setState_(AppState::REALM_SELECTION);
    });

    // "Delete Character" button on character screen
    uiManager_.getCharacterScreen().setOnDeleteCharacter([this](uint64_t guid) {
        gameHandler_.deleteCharacter(guid);
    });

    // Character delete result callback
    gameHandler_.setCharDeleteCallback([this](bool success) {
        if (success) {
            uiManager_.getCharacterScreen().setStatus("Character deleted.");
            // Refresh character list
            gameHandler_.requestCharacterList();
        } else {
            uint8_t code = gameHandler_.getLastCharDeleteResult();
            uiManager_.getCharacterScreen().setStatus(
                "Delete failed (code " + std::to_string(static_cast<int>(code)) + ").", true);
        }
    });
}

}} // namespace wowee::core
