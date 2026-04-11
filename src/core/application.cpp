#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/profiler.hpp"
#include "core/npc_interaction_callback_handler.hpp"
#include "core/audio_callback_handler.hpp"
#include "core/entity_spawn_callback_handler.hpp"
#include "core/animation_callback_handler.hpp"
#include "core/transport_callback_handler.hpp"
#include "core/world_entry_callback_handler.hpp"
#include "core/ui_screen_callback_handler.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation_controller.hpp"
#include <unordered_set>
#include <cmath>
#include <chrono>
#include "core/spawn_presets.hpp"
#include "core/logger.hpp"
#include "core/memory_monitor.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "audio/npc_voice_manager.hpp"
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
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/minimap.hpp"
#include "rendering/quest_marker_renderer.hpp"
#include "rendering/loading_screen.hpp"
#include "audio/music_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/audio_engine.hpp"
#include "audio/audio_coordinator.hpp"
#include "addons/addon_manager.hpp"
#include <imgui.h>
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wdt_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "ui/ui_manager.hpp"
#include "ui/ui_services.hpp"
#include "auth/auth_handler.hpp"
#include "game/game_handler.hpp"
#include "game/transport_manager.hpp"
#include "game/world.hpp"
#include "game/expansion_profile.hpp"
#include "game/packet_parsers.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"

#include <SDL2/SDL.h>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <set>
#include <filesystem>
#include <fstream>

#include <thread>
#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace wowee {
namespace core {

namespace {
bool envFlagEnabled(const char* key, bool defaultValue = false) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    return !(raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F' ||
             raw[0] == 'n' || raw[0] == 'N');
}

} // namespace

Application* Application::instance = nullptr;

Application::Application() {
    instance = this;
}

Application::~Application() {
    shutdown();
    instance = nullptr;
}

bool Application::initialize() {
    LOG_INFO("Initializing Wowee Native Client");

    // Initialize memory monitoring for dynamic cache sizing
    core::MemoryMonitor::getInstance().initialize();

    // Create window
    WindowConfig windowConfig;
    windowConfig.title = "Wowee";
    windowConfig.width = 1280;
    windowConfig.height = 720;
    windowConfig.vsync = false;

    window = std::make_unique<Window>(windowConfig);
    if (!window->initialize()) {
        LOG_FATAL("Failed to initialize window");
        return false;
    }

    // Create renderer
    renderer = std::make_unique<rendering::Renderer>();
    if (!renderer->initialize(window.get())) {
        LOG_FATAL("Failed to initialize renderer");
        return false;
    }

    // Create and initialize audio coordinator (owns all audio managers)
    audioCoordinator_ = std::make_unique<audio::AudioCoordinator>();
    if (!audioCoordinator_->initialize())
        LOG_WARNING("Audio coordinator initialization failed — game will run without audio");
    renderer->setAudioCoordinator(audioCoordinator_.get());

    // Create UI manager
    uiManager = std::make_unique<ui::UIManager>();
    if (!uiManager->initialize(window.get())) {
        LOG_FATAL("Failed to initialize UI manager");
        return false;
    }

    // Create subsystems
    authHandler = std::make_unique<auth::AuthHandler>();
    world = std::make_unique<game::World>();

    // Create and initialize expansion registry
    expansionRegistry_ = std::make_unique<game::ExpansionRegistry>();

    // Create DBC layout
    dbcLayout_ = std::make_unique<pipeline::DBCLayout>();

    // Create asset manager
    assetManager = std::make_unique<pipeline::AssetManager>();

    // Populate game services — all subsystems now available
    gameServices_.renderer = renderer.get();
    gameServices_.audioCoordinator = audioCoordinator_.get();
    gameServices_.assetManager = assetManager.get();
    gameServices_.expansionRegistry = expansionRegistry_.get();

    // Create game handler with explicit service dependencies
    gameHandler = std::make_unique<game::GameHandler>(gameServices_);

    // Try to get WoW data path from environment variable
    const char* dataPathEnv = std::getenv("WOW_DATA_PATH");
    std::string dataPath = dataPathEnv ? dataPathEnv : "./Data";

    // Scan for available expansion profiles
    expansionRegistry_->initialize(dataPath);

    // Load expansion-specific opcode table
    if (gameHandler && expansionRegistry_) {
        auto* profile = expansionRegistry_->getActive();
        if (profile) {
            std::string opcodesPath = profile->dataPath + "/opcodes.json";
            if (!gameHandler->getOpcodeTable().loadFromJson(opcodesPath)) {
                LOG_ERROR("Failed to load opcodes from ", opcodesPath);
            }
            game::setActiveOpcodeTable(&gameHandler->getOpcodeTable());

            // Load expansion-specific update field table
            std::string updateFieldsPath = profile->dataPath + "/update_fields.json";
            if (!gameHandler->getUpdateFieldTable().loadFromJson(updateFieldsPath)) {
                LOG_ERROR("Failed to load update fields from ", updateFieldsPath);
            }
            game::setActiveUpdateFieldTable(&gameHandler->getUpdateFieldTable());

            // Create expansion-specific packet parsers
            gameHandler->setPacketParsers(game::createPacketParsers(profile->id));

            // Load expansion-specific DBC layouts
            if (dbcLayout_) {
                std::string dbcLayoutsPath = profile->dataPath + "/dbc_layouts.json";
                if (!dbcLayout_->loadFromJson(dbcLayoutsPath)) {
                    LOG_ERROR("Failed to load DBC layouts from ", dbcLayoutsPath);
                }
                pipeline::setActiveDBCLayout(dbcLayout_.get());
            }
        }
    }

    // Try expansion-specific asset path first, fall back to base Data/
    std::string assetPath = dataPath;
    if (expansionRegistry_) {
        auto* profile = expansionRegistry_->getActive();
        if (profile && !profile->dataPath.empty()) {
            // Enable expansion-specific CSV DBC lookup (Data/expansions/<id>/db/*.csv).
            assetManager->setExpansionDataPath(profile->dataPath);

            std::string expansionManifest = profile->dataPath + "/manifest.json";
            if (std::filesystem::exists(expansionManifest)) {
                assetPath = profile->dataPath;
                LOG_INFO("Using expansion-specific asset path: ", assetPath);
                // Register base Data/ as fallback so world terrain files are found
                // even when the expansion path only contains DBC overrides.
                if (assetPath != dataPath) {
                    assetManager->setBaseFallbackPath(dataPath);
                }
            }
        }
    }

    LOG_INFO("Attempting to load WoW assets from: ", assetPath);
    if (assetManager->initialize(assetPath)) {
        LOG_INFO("Asset manager initialized successfully");
        // Eagerly load creature display DBC lookups so first spawn doesn't stall
        entitySpawner_ = std::make_unique<EntitySpawner>(
            renderer.get(), assetManager.get(), gameHandler.get(),
            dbcLayout_.get(), &gameServices_);
        entitySpawner_->initialize();

        appearanceComposer_ = std::make_unique<AppearanceComposer>(
            renderer.get(), assetManager.get(), gameHandler.get(),
            dbcLayout_.get(), entitySpawner_.get());

        // Wire AppearanceComposer to UI components (Phase A singleton breaking)
        if (uiManager) {
            uiManager->setAppearanceComposer(appearanceComposer_.get());
            
            // Wire all services to UI components (Phase B singleton breaking)
            ui::UIServices uiServices;
            uiServices.window = window.get();
            uiServices.renderer = renderer.get();
            uiServices.assetManager = assetManager.get();
            uiServices.gameHandler = gameHandler.get();
            uiServices.expansionRegistry = expansionRegistry_.get();
            uiServices.addonManager = addonManager_.get();  // May be nullptr here, re-wire later
            uiServices.audioCoordinator = audioCoordinator_.get();
            uiServices.entitySpawner = entitySpawner_.get();
            uiServices.appearanceComposer = appearanceComposer_.get();
            uiServices.worldLoader = worldLoader_.get();
            uiManager->setServices(uiServices);
        }

        // Ensure the main in-world CharacterRenderer can load textures immediately.
        // Previously this was only wired during terrain initialization, which meant early spawns
        // (before terrain load) would render with white fallback textures (notably hair).
        if (renderer && renderer->getCharacterRenderer()) {
            renderer->getCharacterRenderer()->setAssetManager(assetManager.get());
        }

        // Load transport paths from TransportAnimation.dbc and TaxiPathNode.dbc
        if (gameHandler && gameHandler->getTransportManager()) {
            gameHandler->getTransportManager()->loadTransportAnimationDBC(assetManager.get());
            gameHandler->getTransportManager()->loadTaxiPathNodeDBC(assetManager.get());
        }

        // Initialize addon system
        addonManager_ = std::make_unique<addons::AddonManager>();
        addons::LuaServices luaSvc;
        luaSvc.window            = window.get();
        luaSvc.audioCoordinator  = audioCoordinator_.get();
        luaSvc.expansionRegistry = expansionRegistry_.get();
        if (addonManager_->initialize(gameHandler.get(), luaSvc)) {
            std::string addonsDir = assetPath + "/interface/AddOns";
            addonManager_->scanAddons(addonsDir);
            // Wire Lua errors to UI error display
            addonManager_->getLuaEngine()->setLuaErrorCallback([gh = gameHandler.get()](const std::string& err) {
                if (gh) gh->addUIError(err);
            });
            // Wire chat messages to addon event dispatch
            gameHandler->setAddonChatCallback([this](const game::MessageChatData& msg) {
                if (!addonManager_ || !addonsLoaded_) return;
                // Map ChatType to WoW event name
                const char* eventName = nullptr;
                switch (msg.type) {
                    case game::ChatType::SAY:          eventName = "CHAT_MSG_SAY"; break;
                    case game::ChatType::YELL:         eventName = "CHAT_MSG_YELL"; break;
                    case game::ChatType::WHISPER:       eventName = "CHAT_MSG_WHISPER"; break;
                    case game::ChatType::PARTY:         eventName = "CHAT_MSG_PARTY"; break;
                    case game::ChatType::GUILD:         eventName = "CHAT_MSG_GUILD"; break;
                    case game::ChatType::OFFICER:       eventName = "CHAT_MSG_OFFICER"; break;
                    case game::ChatType::RAID:          eventName = "CHAT_MSG_RAID"; break;
                    case game::ChatType::RAID_WARNING:  eventName = "CHAT_MSG_RAID_WARNING"; break;
                    case game::ChatType::BATTLEGROUND:  eventName = "CHAT_MSG_BATTLEGROUND"; break;
                    case game::ChatType::SYSTEM:        eventName = "CHAT_MSG_SYSTEM"; break;
                    case game::ChatType::CHANNEL:       eventName = "CHAT_MSG_CHANNEL"; break;
                    case game::ChatType::EMOTE:
                    case game::ChatType::TEXT_EMOTE:    eventName = "CHAT_MSG_EMOTE"; break;
                    case game::ChatType::ACHIEVEMENT:   eventName = "CHAT_MSG_ACHIEVEMENT"; break;
                    case game::ChatType::GUILD_ACHIEVEMENT: eventName = "CHAT_MSG_GUILD_ACHIEVEMENT"; break;
                    case game::ChatType::WHISPER_INFORM: eventName = "CHAT_MSG_WHISPER_INFORM"; break;
                    case game::ChatType::RAID_LEADER:   eventName = "CHAT_MSG_RAID_LEADER"; break;
                    case game::ChatType::BATTLEGROUND_LEADER: eventName = "CHAT_MSG_BATTLEGROUND_LEADER"; break;
                    case game::ChatType::MONSTER_SAY:    eventName = "CHAT_MSG_MONSTER_SAY"; break;
                    case game::ChatType::MONSTER_YELL:   eventName = "CHAT_MSG_MONSTER_YELL"; break;
                    case game::ChatType::MONSTER_EMOTE:  eventName = "CHAT_MSG_MONSTER_EMOTE"; break;
                    case game::ChatType::MONSTER_WHISPER: eventName = "CHAT_MSG_MONSTER_WHISPER"; break;
                    case game::ChatType::RAID_BOSS_EMOTE: eventName = "CHAT_MSG_RAID_BOSS_EMOTE"; break;
                    case game::ChatType::RAID_BOSS_WHISPER: eventName = "CHAT_MSG_RAID_BOSS_WHISPER"; break;
                    case game::ChatType::BG_SYSTEM_NEUTRAL:  eventName = "CHAT_MSG_BG_SYSTEM_NEUTRAL"; break;
                    case game::ChatType::BG_SYSTEM_ALLIANCE: eventName = "CHAT_MSG_BG_SYSTEM_ALLIANCE"; break;
                    case game::ChatType::BG_SYSTEM_HORDE:    eventName = "CHAT_MSG_BG_SYSTEM_HORDE"; break;
                    case game::ChatType::MONSTER_PARTY:  eventName = "CHAT_MSG_MONSTER_PARTY"; break;
                    case game::ChatType::AFK:            eventName = "CHAT_MSG_AFK"; break;
                    case game::ChatType::DND:            eventName = "CHAT_MSG_DND"; break;
                    case game::ChatType::LOOT:           eventName = "CHAT_MSG_LOOT"; break;
                    case game::ChatType::SKILL:          eventName = "CHAT_MSG_SKILL"; break;
                    default: break;
                }
                if (eventName) {
                    addonManager_->fireEvent(eventName, {msg.message, msg.senderName});
                }
            });
            // Wire generic game events to addon dispatch
            gameHandler->setAddonEventCallback([this](const std::string& event, const std::vector<std::string>& args) {
                if (addonManager_ && addonsLoaded_) {
                    addonManager_->fireEvent(event, args);
                }
            });
            // Wire spell icon path resolver for Lua API (GetSpellInfo, UnitBuff icon, etc.)
            {
                auto spellIconPaths  = std::make_shared<std::unordered_map<uint32_t, std::string>>();
                auto spellIconIds    = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
                auto loaded          = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                gameHandler->setSpellIconPathResolver([spellIconPaths, spellIconIds, loaded, am](uint32_t spellId) -> std::string {
                    if (!am) return {};
                    // Lazy-load SpellIcon.dbc + Spell.dbc icon IDs on first call
                    if (!*loaded) {
                        *loaded = true;
                        auto iconDbc = am->loadDBC("SpellIcon.dbc");
                        const auto* iconL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellIcon") : nullptr;
                        if (iconDbc && iconDbc->isLoaded()) {
                            for (uint32_t i = 0; i < iconDbc->getRecordCount(); i++) {
                                uint32_t id = iconDbc->getUInt32(i, iconL ? (*iconL)["ID"] : 0);
                                std::string path = iconDbc->getString(i, iconL ? (*iconL)["Path"] : 1);
                                if (!path.empty() && id > 0) (*spellIconPaths)[id] = path;
                            }
                        }
                        auto spellDbc = am->loadDBC("Spell.dbc");
                        const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
                        if (spellDbc && spellDbc->isLoaded()) {
                            uint32_t fieldCount = spellDbc->getFieldCount();
                            uint32_t iconField = 133; // WotLK default
                            uint32_t idField = 0;
                            if (spellL) {
                                uint32_t layoutIcon = (*spellL)["IconID"];
                                if (layoutIcon < fieldCount && fieldCount <= layoutIcon + 20) {
                                    iconField = layoutIcon;
                                    idField = (*spellL)["ID"];
                                }
                            }
                            for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                                uint32_t id = spellDbc->getUInt32(i, idField);
                                uint32_t iconId = spellDbc->getUInt32(i, iconField);
                                if (id > 0 && iconId > 0) (*spellIconIds)[id] = iconId;
                            }
                        }
                    }
                    auto iit = spellIconIds->find(spellId);
                    if (iit == spellIconIds->end()) return {};
                    auto pit = spellIconPaths->find(iit->second);
                    if (pit == spellIconPaths->end()) return {};
                    return pit->second;
                });
            }
            // Wire item icon path resolver: displayInfoId -> "Interface\\Icons\\INV_..."
            {
                auto iconNames = std::make_shared<std::unordered_map<uint32_t, std::string>>();
                auto loaded    = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                gameHandler->setItemIconPathResolver([iconNames, loaded, am](uint32_t displayInfoId) -> std::string {
                    if (!am || displayInfoId == 0) return {};
                    if (!*loaded) {
                        *loaded = true;
                        auto dbc = am->loadDBC("ItemDisplayInfo.dbc");
                        const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                        if (dbc && dbc->isLoaded()) {
                            uint32_t iconField = dispL ? (*dispL)["InventoryIcon"] : 5;
                            for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
                                uint32_t id = dbc->getUInt32(i, 0); // field 0 = ID
                                std::string name = dbc->getString(i, iconField);
                                if (id > 0 && !name.empty()) (*iconNames)[id] = name;
                            }
                            LOG_INFO("Loaded ", iconNames->size(), " item icon names from ItemDisplayInfo.dbc");
                        }
                    }
                    auto it = iconNames->find(displayInfoId);
                    if (it == iconNames->end()) return {};
                    return "Interface\\Icons\\" + it->second;
                });
            }
            // Wire spell data resolver: spellId -> {castTimeMs, minRange, maxRange}
            {
                auto castTimeMap = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
                auto rangeMap    = std::make_shared<std::unordered_map<uint32_t, std::pair<float,float>>>();
                auto spellCastIdx = std::make_shared<std::unordered_map<uint32_t, uint32_t>>(); // spellId→castTimeIdx
                auto spellRangeIdx = std::make_shared<std::unordered_map<uint32_t, uint32_t>>(); // spellId→rangeIdx
                struct SpellCostEntry { uint32_t manaCost = 0; uint8_t powerType = 0; };
                auto spellCostMap = std::make_shared<std::unordered_map<uint32_t, SpellCostEntry>>();
                auto loaded = std::make_shared<bool>(false);
                auto* am = assetManager.get();
                gameHandler->setSpellDataResolver([castTimeMap, rangeMap, spellCastIdx, spellRangeIdx, spellCostMap, loaded, am](uint32_t spellId) -> game::GameHandler::SpellDataInfo {
                    if (!am) return {};
                    if (!*loaded) {
                        *loaded = true;
                        // Load SpellCastTimes.dbc
                        auto ctDbc = am->loadDBC("SpellCastTimes.dbc");
                        if (ctDbc && ctDbc->isLoaded()) {
                            for (uint32_t i = 0; i < ctDbc->getRecordCount(); ++i) {
                                uint32_t id = ctDbc->getUInt32(i, 0);
                                int32_t base = static_cast<int32_t>(ctDbc->getUInt32(i, 1));
                                if (id > 0 && base > 0) (*castTimeMap)[id] = static_cast<uint32_t>(base);
                            }
                        }
                        // Load SpellRange.dbc
                        const auto* srL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellRange") : nullptr;
                        uint32_t minRField = srL ? (*srL)["MinRange"] : 1;
                        uint32_t maxRField = srL ? (*srL)["MaxRange"] : 4;
                        auto rDbc = am->loadDBC("SpellRange.dbc");
                        if (rDbc && rDbc->isLoaded()) {
                            for (uint32_t i = 0; i < rDbc->getRecordCount(); ++i) {
                                uint32_t id = rDbc->getUInt32(i, 0);
                                float minR = rDbc->getFloat(i, minRField);
                                float maxR = rDbc->getFloat(i, maxRField);
                                if (id > 0) (*rangeMap)[id] = {minR, maxR};
                            }
                        }
                        // Load Spell.dbc: extract castTimeIndex and rangeIndex per spell
                        auto sDbc = am->loadDBC("Spell.dbc");
                        const auto* spL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
                        if (sDbc && sDbc->isLoaded()) {
                            uint32_t idF = spL ? (*spL)["ID"] : 0;
                            uint32_t ctF = spL ? (*spL)["CastingTimeIndex"] : 134; // WotLK default
                            uint32_t rF  = spL ? (*spL)["RangeIndex"] : 132;
                            uint32_t ptF = UINT32_MAX, mcF = UINT32_MAX;
                            if (spL) {
                                try { ptF = (*spL)["PowerType"]; } catch (...) {}
                                try { mcF = (*spL)["ManaCost"]; } catch (...) {}
                            }
                            uint32_t fc = sDbc->getFieldCount();
                            for (uint32_t i = 0; i < sDbc->getRecordCount(); ++i) {
                                uint32_t id = sDbc->getUInt32(i, idF);
                                if (id == 0) continue;
                                uint32_t ct = sDbc->getUInt32(i, ctF);
                                uint32_t ri = sDbc->getUInt32(i, rF);
                                if (ct > 0) (*spellCastIdx)[id] = ct;
                                if (ri > 0) (*spellRangeIdx)[id] = ri;
                                // Extract power cost
                                uint32_t mc = (mcF < fc) ? sDbc->getUInt32(i, mcF) : 0;
                                uint8_t  pt = (ptF < fc) ? static_cast<uint8_t>(sDbc->getUInt32(i, ptF)) : 0;
                                if (mc > 0) (*spellCostMap)[id] = {mc, pt};
                            }
                        }
                        LOG_INFO("SpellDataResolver: loaded ", spellCastIdx->size(), " cast indices, ",
                                 spellRangeIdx->size(), " range indices");
                    }
                    game::GameHandler::SpellDataInfo info;
                    auto ciIt = spellCastIdx->find(spellId);
                    if (ciIt != spellCastIdx->end()) {
                        auto ctIt = castTimeMap->find(ciIt->second);
                        if (ctIt != castTimeMap->end()) info.castTimeMs = ctIt->second;
                    }
                    auto riIt = spellRangeIdx->find(spellId);
                    if (riIt != spellRangeIdx->end()) {
                        auto rIt = rangeMap->find(riIt->second);
                        if (rIt != rangeMap->end()) {
                            info.minRange = rIt->second.first;
                            info.maxRange = rIt->second.second;
                        }
                    }
                    auto mcIt = spellCostMap->find(spellId);
                    if (mcIt != spellCostMap->end()) {
                        info.manaCost = mcIt->second.manaCost;
                        info.powerType = mcIt->second.powerType;
                    }
                    return info;
                });
            }
            // Wire random property/suffix name resolver for item display
            {
                auto propNames   = std::make_shared<std::unordered_map<int32_t, std::string>>();
                auto propLoaded  = std::make_shared<bool>(false);
                auto* amPtr = assetManager.get();
                gameHandler->setRandomPropertyNameResolver([propNames, propLoaded, amPtr](int32_t id) -> std::string {
                    if (!amPtr || id == 0) return {};
                    if (!*propLoaded) {
                        *propLoaded = true;
                        // ItemRandomProperties.dbc: ID=0, Name=4 (string)
                        if (auto dbc = amPtr->loadDBC("ItemRandomProperties.dbc"); dbc && dbc->isLoaded()) {
                            uint32_t nameField = (dbc->getFieldCount() > 4) ? 4 : 1;
                            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                int32_t rid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                std::string name = dbc->getString(r, nameField);
                                if (!name.empty() && rid > 0) (*propNames)[rid] = name;
                            }
                        }
                        // ItemRandomSuffix.dbc: ID=0, Name=4 (string) — stored as negative IDs
                        if (auto dbc = amPtr->loadDBC("ItemRandomSuffix.dbc"); dbc && dbc->isLoaded()) {
                            uint32_t nameField = (dbc->getFieldCount() > 4) ? 4 : 1;
                            for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                                int32_t rid = static_cast<int32_t>(dbc->getUInt32(r, 0));
                                std::string name = dbc->getString(r, nameField);
                                if (!name.empty() && rid > 0) (*propNames)[-rid] = name;
                            }
                        }
                    }
                    auto it = propNames->find(id);
                    return (it != propNames->end()) ? it->second : std::string{};
                });
            }
            LOG_INFO("Addon system initialized, found ", addonManager_->getAddons().size(), " addon(s)");
        } else {
            LOG_WARNING("Failed to initialize addon system");
            addonManager_.reset();
        }

        // Initialize world loader (handles terrain streaming, world preload, map transitions)
        worldLoader_ = std::make_unique<WorldLoader>(
            *this, renderer.get(), assetManager.get(), gameHandler.get(),
            entitySpawner_.get(), appearanceComposer_.get(), window.get(),
            world.get(), addonManager_.get());

        // Re-wire UIServices now that all services (addonManager_, worldLoader_) are available
        if (uiManager) {
            ui::UIServices uiServices;
            uiServices.window = window.get();
            uiServices.renderer = renderer.get();
            uiServices.assetManager = assetManager.get();
            uiServices.gameHandler = gameHandler.get();
            uiServices.expansionRegistry = expansionRegistry_.get();
            uiServices.addonManager = addonManager_.get();
            uiServices.audioCoordinator = audioCoordinator_.get();
            uiServices.entitySpawner = entitySpawner_.get();
            uiServices.appearanceComposer = appearanceComposer_.get();
            uiServices.worldLoader = worldLoader_.get();
            uiManager->setServices(uiServices);
        }

        // Start background preload for last-played character's world.
        // Warms the file cache so terrain tile loading is faster at Enter World.
        {
            auto lastWorld = worldLoader_->loadLastWorldInfo();
            if (lastWorld.valid) {
                worldLoader_->startWorldPreload(lastWorld.mapId, lastWorld.mapName, lastWorld.x, lastWorld.y);
            }
        }

    } else {
        LOG_WARNING("Failed to initialize asset manager - asset loading will be unavailable");
        LOG_WARNING("Set WOW_DATA_PATH environment variable to your WoW Data directory");
    }

    // Set up UI callbacks
    setupUICallbacks();

    LOG_INFO("Application initialized successfully");
    running = true;
    return true;
}

void Application::run() {
    ZoneScopedN("Application::run");
    LOG_INFO("Starting main loop");

    // Pin main thread to a dedicated CPU core to reduce scheduling jitter
    {
        int numCores = static_cast<int>(std::thread::hardware_concurrency());
        if (numCores >= 2) {
#ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(0, &cpuset);
            int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
            if (rc == 0) {
                LOG_INFO("Main thread pinned to CPU core 0 (", numCores, " cores available)");
            } else {
                LOG_WARNING("Failed to pin main thread to CPU core 0 (error ", rc, ")");
            }
#elif defined(_WIN32)
            DWORD_PTR mask = 1; // Core 0
            DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
            if (prev != 0) {
                LOG_INFO("Main thread pinned to CPU core 0 (", numCores, " cores available)");
            } else {
                LOG_WARNING("Failed to pin main thread to CPU core 0 (error ", GetLastError(), ")");
            }
#elif defined(__APPLE__)
            // macOS doesn't support hard pinning — use affinity tags to hint
            // that the main thread should stay on its own core group
            thread_affinity_policy_data_t policy = { 1 }; // tag 1 = main thread group
            kern_return_t kr = thread_policy_set(
                pthread_mach_thread_np(pthread_self()),
                THREAD_AFFINITY_POLICY,
                reinterpret_cast<thread_policy_t>(&policy),
                THREAD_AFFINITY_POLICY_COUNT);
            if (kr == KERN_SUCCESS) {
                LOG_INFO("Main thread affinity tag set (", numCores, " cores available)");
            } else {
                LOG_WARNING("Failed to set main thread affinity tag (error ", kr, ")");
            }
#endif
        }
    }

    const bool frameProfileEnabled = envFlagEnabled("WOWEE_FRAME_PROFILE", false);
    if (frameProfileEnabled) {
        LOG_INFO("Frame timing profile enabled (WOWEE_FRAME_PROFILE=1)");
    }

    auto lastTime = std::chrono::high_resolution_clock::now();
    std::atomic<bool> watchdogRunning{true};
    std::atomic<int64_t> watchdogHeartbeatMs{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()
    };
    // Signal flag: watchdog sets this when a stall is detected, main loop
    // handles the actual SDL calls. SDL2 video functions must only be called
    // from the main thread (the one that called SDL_Init); calling them from
    // a background thread is UB on macOS (Cocoa) and unsafe on other platforms.
    std::atomic<bool> watchdogRequestRelease{false};
    std::thread watchdogThread([&watchdogRunning, &watchdogHeartbeatMs, &watchdogRequestRelease]() {
        bool signalledForCurrentStall = false;
        while (watchdogRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t lastBeatMs = watchdogHeartbeatMs.load(std::memory_order_acquire);
            const int64_t stallMs = nowMs - lastBeatMs;

            if (stallMs > 1500) {
                if (!signalledForCurrentStall) {
                    watchdogRequestRelease.store(true, std::memory_order_release);
                    LOG_WARNING("Main-loop stall detected (", stallMs,
                                "ms) — requesting mouse capture release");
                    signalledForCurrentStall = true;
                }
            } else {
                signalledForCurrentStall = false;
            }
        }
    });

    try {
        while (running && !window->shouldClose()) {
            watchdogHeartbeatMs.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_release);

            // Handle watchdog mouse-release request on the main thread where
            // SDL video calls are safe (required by SDL2 threading model).
            if (watchdogRequestRelease.exchange(false, std::memory_order_acq_rel)) {
                SDL_SetRelativeMouseMode(SDL_FALSE);
                SDL_ShowCursor(SDL_ENABLE);
                if (window && window->getSDLWindow()) {
                    SDL_SetWindowGrab(window->getSDLWindow(), SDL_FALSE);
                }
                LOG_WARNING("Watchdog: force-released mouse capture on main thread");
            }

            // Calculate delta time
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<float> deltaTimeDuration = currentTime - lastTime;
            float deltaTime = deltaTimeDuration.count();
            lastTime = currentTime;

            // Cap delta time to prevent large jumps
            if (deltaTime > 0.1f) {
                deltaTime = 0.1f;
            }

            // Poll events
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                // Pass event to UI manager first
                if (uiManager) {
                    uiManager->processEvent(event);
                }

                // Pass mouse events to camera controller (skip when UI has mouse focus)
                if (renderer && renderer->getCameraController() && !ImGui::GetIO().WantCaptureMouse) {
                    if (event.type == SDL_MOUSEMOTION) {
                        renderer->getCameraController()->processMouseMotion(event.motion);
                    }
                    else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                        renderer->getCameraController()->processMouseButton(event.button);
                    }
                    else if (event.type == SDL_MOUSEWHEEL) {
                        renderer->getCameraController()->processMouseWheel(static_cast<float>(event.wheel.y));
                    }
                }

                // Handle window events
                if (event.type == SDL_QUIT) {
                    window->setShouldClose(true);
                }
                else if (event.type == SDL_WINDOWEVENT) {
                    if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int newWidth = event.window.data1;
                        int newHeight = event.window.data2;
                        window->setSize(newWidth, newHeight);
                        // Mark swapchain dirty so it gets recreated at the correct size
                        if (window->getVkContext()) {
                            window->getVkContext()->markSwapchainDirty();
                        }
                        // Vulkan viewport set in command buffer, not globally
                        if (renderer && renderer->getCamera() && newHeight > 0) {
                            renderer->getCamera()->setAspectRatio(static_cast<float>(newWidth) / newHeight);
                        }
                        // Notify addons so UI layouts can adapt to the new size
                        if (addonManager_)
                            addonManager_->fireEvent("DISPLAY_SIZE_CHANGED");
                    }
                }
                // Debug controls
                else if (event.type == SDL_KEYDOWN) {
                    // Skip non-function-key input when UI (chat) has keyboard focus
                    bool uiHasKeyboard = ImGui::GetIO().WantCaptureKeyboard;
                    auto sc = event.key.keysym.scancode;
                    bool isFKey = (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12);
                    if (uiHasKeyboard && !isFKey) {
                        continue;  // Let ImGui handle the keystroke
                    }

                    // F1: Toggle performance HUD
                    if (event.key.keysym.scancode == SDL_SCANCODE_F1) {
                        if (renderer && renderer->getPerformanceHUD()) {
                            renderer->getPerformanceHUD()->toggle();
                            bool enabled = renderer->getPerformanceHUD()->isEnabled();
                            LOG_INFO("Performance HUD: ", enabled ? "ON" : "OFF");
                        }
                    }
                    // F4: Toggle shadows
                    else if (event.key.keysym.scancode == SDL_SCANCODE_F4) {
                        if (renderer) {
                            bool enabled = !renderer->areShadowsEnabled();
                            renderer->setShadowsEnabled(enabled);
                            LOG_INFO("Shadows: ", enabled ? "ON" : "OFF");
                        }
                    }
                    // F8: Debug WMO floor at current position
                    else if (event.key.keysym.scancode == SDL_SCANCODE_F8 && event.key.repeat == 0) {
                        if (renderer && renderer->getWMORenderer()) {
                            glm::vec3 pos = renderer->getCharacterPosition();
                            LOG_WARNING("F8: WMO floor debug at render pos (", pos.x, ", ", pos.y, ", ", pos.z, ")");
                            renderer->getWMORenderer()->debugDumpGroupsAtPosition(pos.x, pos.y, pos.z);
                        }
                    }
                }
            }

            // Update input
            Input::getInstance().update();

            // Update application state
            try {
                FrameMark;
                update(deltaTime);
            } catch (const std::bad_alloc& e) {
                LOG_ERROR("OOM during Application::update (state=", static_cast<int>(state),
                          ", dt=", deltaTime, "): ", e.what());
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("Exception during Application::update (state=", static_cast<int>(state),
                          ", dt=", deltaTime, "): ", e.what());
                throw;
            }
            // Render
            try {
                render();
            } catch (const std::bad_alloc& e) {
                LOG_ERROR("OOM during Application::render (state=", static_cast<int>(state), "): ", e.what());
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("Exception during Application::render (state=", static_cast<int>(state), "): ", e.what());
                throw;
            }
            // Swap buffers
            try {
                window->swapBuffers();
            } catch (const std::bad_alloc& e) {
                LOG_ERROR("OOM during swapBuffers: ", e.what());
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("Exception during swapBuffers: ", e.what());
                throw;
            }

            // Exit gracefully on GPU device lost (unrecoverable)
            if (renderer && renderer->getVkContext() && renderer->getVkContext()->isDeviceLost()) {
                LOG_ERROR("GPU device lost — exiting application");
                window->setShouldClose(true);
            }

            // Soft frame rate cap when vsync is off to prevent 100% CPU usage.
            // Target ~240 FPS max (~4.2ms per frame); vsync handles its own pacing.
            if (!window->isVsyncEnabled() && deltaTime < 0.004f) {
                float sleepMs = (0.004f - deltaTime) * 1000.0f;
                if (sleepMs > 0.5f)
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        static_cast<int64_t>(sleepMs * 900.0f)));  // 90% of target to account for sleep overshoot
            }
        }
    } catch (...) {
        watchdogRunning.store(false, std::memory_order_release);
        if (watchdogThread.joinable()) {
            watchdogThread.join();
        }
        throw;
    }

    watchdogRunning.store(false, std::memory_order_release);
    if (watchdogThread.joinable()) {
        watchdogThread.join();
    }

    LOG_INFO("Main loop ended");
}

void Application::shutdown() {
    LOG_DEBUG("Shutting down application...");

    // Hide the window immediately so the OS doesn't think the app is frozen
    // during the (potentially slow) resource cleanup below.
    if (window && window->getSDLWindow()) {
        SDL_HideWindow(window->getSDLWindow());
    }

    // Stop background world preloader before destroying AssetManager
    if (worldLoader_) {
        worldLoader_->cancelWorldPreload();
    };

    // Save floor cache before renderer is destroyed
    if (renderer && renderer->getWMORenderer()) {
        size_t cacheSize = renderer->getWMORenderer()->getFloorCacheSize();
        if (cacheSize > 0) {
            LOG_DEBUG("Saving WMO floor cache (", cacheSize, " entries)...");
            renderer->getWMORenderer()->saveFloorCache();
            LOG_DEBUG("Floor cache saved.");
        }
    }

    // Explicitly shut down the renderer before destroying it — this ensures
    // all sub-renderers free their VMA allocations in the correct order,
    // before VkContext::shutdown() calls vmaDestroyAllocator().
    LOG_DEBUG("Shutting down renderer...");
    if (renderer) {
        renderer->shutdown();
    }
    LOG_DEBUG("Renderer shutdown complete, resetting...");
    renderer.reset();

    // Shutdown audio coordinator after renderer (renderer may reference audio during shutdown)
    if (audioCoordinator_) {
        audioCoordinator_->shutdown();
    }
    audioCoordinator_.reset();

    LOG_DEBUG("Resetting world...");
    world.reset();
    LOG_DEBUG("Resetting gameHandler...");
    gameHandler.reset();
    gameServices_ = {};
    LOG_DEBUG("Resetting authHandler...");
    authHandler.reset();
    LOG_DEBUG("Resetting assetManager...");
    assetManager.reset();
    LOG_DEBUG("Resetting uiManager...");
    uiManager.reset();
    LOG_DEBUG("Resetting window...");
    window.reset();

    running = false;
    LOG_DEBUG("Application shutdown complete");
}

void Application::setState(AppState newState) {
    if (state == newState) {
        return;
    }

    LOG_INFO("State transition: ", static_cast<int>(state), " -> ", static_cast<int>(newState));
    state = newState;

    // Handle state transitions
    switch (newState) {
        case AppState::AUTHENTICATION:
            // Show auth screen
            break;
        case AppState::REALM_SELECTION:
            // Show realm screen
            break;
        case AppState::CHARACTER_CREATION:
            // Show character create screen
            break;
        case AppState::CHARACTER_SELECTION:
            // Show character screen
            if (uiManager && assetManager) {
                uiManager->getCharacterScreen().setAssetManager(assetManager.get());
            }
            // Ensure no stale in-world player model leaks into the next login attempt.
            // If we reuse a previously spawned instance without forcing a respawn, appearance (notably hair) can desync.
            if (addonManager_ && addonsLoaded_) {
                addonManager_->fireEvent("PLAYER_LEAVING_WORLD");
                addonManager_->saveAllSavedVariables();
            }
            npcsSpawned = false;
            playerCharacterSpawned = false;
            addonsLoaded_ = false;
            if (appearanceComposer_) appearanceComposer_->setWeaponsSheathed(false);
            wasAutoAttacking_ = false;
            if (worldLoader_) worldLoader_->resetLoadedMap();
            spawnedPlayerGuid_ = 0;
            spawnedAppearanceBytes_ = 0;
            spawnedFacialFeatures_ = 0;
            if (renderer && renderer->getCharacterRenderer()) {
                uint32_t oldInst = renderer->getCharacterInstanceId();
                if (oldInst > 0) {
                    renderer->setCharacterFollow(0);
                    if (auto* ac = renderer->getAnimationController()) ac->clearMount();
                    renderer->getCharacterRenderer()->removeInstance(oldInst);
                }
            }
            break;
        case AppState::IN_GAME: {
            // Wire up movement opcodes from camera controller
            if (renderer && renderer->getCameraController()) {
                auto* cc = renderer->getCameraController();
                cc->setMovementCallback([this](uint32_t opcode) {
                    if (gameHandler) {
                        gameHandler->sendMovement(static_cast<game::Opcode>(opcode));
                    }
                });
                cc->setStandUpCallback([this]() {
                    if (gameHandler) {
                        gameHandler->setStandState(rendering::AnimationController::STAND_STATE_STAND);
                    }
                });
                cc->setSitDownCallback([this]() {
                    if (gameHandler) {
                        gameHandler->setStandState(rendering::AnimationController::STAND_STATE_SIT);
                    }
                    if (renderer) {
                        if (auto* ac = renderer->getAnimationController()) {
                            ac->setStandState(rendering::AnimationController::STAND_STATE_SIT);
                        }
                    }
                });
                cc->setAutoFollowCancelCallback([this]() {
                    if (gameHandler) {
                        gameHandler->cancelFollow();
                    }
                });
                cc->setUseWoWSpeed(true);
            }
            if (gameHandler) {
                gameHandler->setMeleeSwingCallback([this](uint32_t spellId) {
                    if (renderer) {
                        // Ranged auto-attack spells: Auto Shot (75), Shoot (5019), Throw (2764)
                        if (spellId == 75 || spellId == 5019 || spellId == 2764) {
                            if (appearanceComposer_ && !appearanceComposer_->isShowingRanged())
                                appearanceComposer_->showRangedWeapon(true);
                            if (auto* ac = renderer->getAnimationController()) ac->triggerRangedShot();
                        } else if (spellId != 0) {
                            if (appearanceComposer_ && appearanceComposer_->isShowingRanged())
                                appearanceComposer_->showRangedWeapon(false);
                            if (auto* ac = renderer->getAnimationController()) ac->triggerSpecialAttack(spellId);
                        } else {
                            if (appearanceComposer_ && appearanceComposer_->isShowingRanged())
                                appearanceComposer_->showRangedWeapon(false);
                            if (auto* ac = renderer->getAnimationController()) ac->triggerMeleeSwing();
                        }
                    }
                });
                gameHandler->setRangedWeaponSwapCallback([this](bool show) {
                    if (appearanceComposer_) appearanceComposer_->showRangedWeapon(show);
                });
                gameHandler->setKnockBackCallback([this](float vcos, float vsin, float hspeed, float vspeed) {
                    if (renderer && renderer->getCameraController()) {
                        renderer->getCameraController()->applyKnockBack(vcos, vsin, hspeed, vspeed);
                    }
                });
                gameHandler->setCameraShakeCallback([this](float magnitude, float frequency, float duration) {
                    if (renderer && renderer->getCameraController()) {
                        renderer->getCameraController()->triggerShake(magnitude, frequency, duration);
                    }
                });
                gameHandler->setAutoFollowCallback([this](const glm::vec3* renderPos) {
                    if (renderer && renderer->getCameraController()) {
                        if (renderPos) {
                            renderer->getCameraController()->setAutoFollow(renderPos);
                        } else {
                            renderer->getCameraController()->cancelAutoFollow();
                        }
                    }
                });
            }
            // Load quest marker models
            loadQuestMarkerModels();
            break;
        }
        case AppState::DISCONNECTED:
            // Back to auth
            break;
    }
}

void Application::reloadExpansionData() {
    if (!expansionRegistry_ || !gameHandler) return;
    auto* profile = expansionRegistry_->getActive();
    if (!profile) return;

    LOG_INFO("Reloading expansion data for: ", profile->name);

    std::string opcodesPath = profile->dataPath + "/opcodes.json";
    if (!gameHandler->getOpcodeTable().loadFromJson(opcodesPath)) {
        LOG_ERROR("Failed to load opcodes from ", opcodesPath);
    }
    game::setActiveOpcodeTable(&gameHandler->getOpcodeTable());

    std::string updateFieldsPath = profile->dataPath + "/update_fields.json";
    if (!gameHandler->getUpdateFieldTable().loadFromJson(updateFieldsPath)) {
        LOG_ERROR("Failed to load update fields from ", updateFieldsPath);
    }
    game::setActiveUpdateFieldTable(&gameHandler->getUpdateFieldTable());

    gameHandler->setPacketParsers(game::createPacketParsers(profile->id));

    if (dbcLayout_) {
        std::string dbcLayoutsPath = profile->dataPath + "/dbc_layouts.json";
        if (!dbcLayout_->loadFromJson(dbcLayoutsPath)) {
            LOG_ERROR("Failed to load DBC layouts from ", dbcLayoutsPath);
        }
        pipeline::setActiveDBCLayout(dbcLayout_.get());
    }

    // Update expansion data path for CSV DBC lookups and clear DBC cache
    if (assetManager && !profile->dataPath.empty()) {
        assetManager->setExpansionDataPath(profile->dataPath);
        assetManager->clearDBCCache();
    }

    // Reset map name cache so it reloads from new expansion's Map.dbc
    if (worldLoader_) worldLoader_->resetMapNameCache();

    // Reset game handler DBC caches so they reload from new expansion data
    if (gameHandler) {
        gameHandler->resetDbcCaches();
    }

    // Rebuild creature display lookups with the new expansion's DBC layout
    if (entitySpawner_) entitySpawner_->rebuildLookups();
}

void Application::logoutToLogin() {
    LOG_INFO("Logout requested");

    // Disconnect TransportManager from WMORenderer before tearing down
    if (gameHandler && gameHandler->getTransportManager()) {
        gameHandler->getTransportManager()->setWMORenderer(nullptr);
    }

    if (gameHandler) {
        gameHandler->disconnect();
    }

    // --- Per-session flags ---
    npcsSpawned = false;
    playerCharacterSpawned = false;
    if (appearanceComposer_) appearanceComposer_->setWeaponsSheathed(false);
    wasAutoAttacking_ = false;
    if (worldLoader_) worldLoader_->resetLoadedMap();
    if (worldEntryCallbacks_) worldEntryCallbacks_->resetState();
    facingSendCooldown_ = 0.0f;
    lastSentCanonicalYaw_ = 1000.0f;
    taxiStreamCooldown_ = 0.0f;
    idleYawned_ = false;

    // --- Charge state ---
    if (animationCallbacks_) animationCallbacks_->resetChargeState();

    // --- Player identity ---
    spawnedPlayerGuid_ = 0;
    spawnedAppearanceBytes_ = 0;
    spawnedFacialFeatures_ = 0;

    // --- Reset all EntitySpawner state (mount, creatures, players, GOs, queues, caches) ---
    if (entitySpawner_) entitySpawner_->resetAllState();

    world.reset();

    if (renderer) {
        renderer->resetCombatVisualState();
        // Remove old player model so it doesn't persist into next session
        if (auto* charRenderer = renderer->getCharacterRenderer()) {
            charRenderer->removeInstance(1);
        }
        // Clear all world geometry renderers
        if (auto* wmo = renderer->getWMORenderer()) {
            wmo->clearInstances();
        }
        if (auto* m2 = renderer->getM2Renderer()) {
            m2->clear();
        }
        // Clear terrain tile tracking + water surfaces so next world entry starts fresh.
        // Use softReset() instead of unloadAll() to avoid blocking on worker thread joins.
        if (auto* terrain = renderer->getTerrainManager()) {
            terrain->softReset();
        }
        if (auto* questMarkers = renderer->getQuestMarkerRenderer()) {
            questMarkers->clear();
        }
        if (auto* ac = renderer->getAnimationController()) ac->clearMount();
        renderer->setCharacterFollow(0);
        if (auto* music = audioCoordinator_ ? audioCoordinator_->getMusicManager() : nullptr) {
            music->stopMusic(0.0f);
        }
    }

    // Clear stale realm/character selection so switching servers starts fresh
    if (uiManager) {
        uiManager->getRealmScreen().reset();
        uiManager->getCharacterScreen().reset();
    }
    setState(AppState::AUTHENTICATION);
}

void Application::update(float deltaTime) {
    ZoneScopedN("Application::update");
    const char* updateCheckpoint = "enter";
    try {
    // Update based on current state
    updateCheckpoint = "state switch";
    switch (state) {
        case AppState::AUTHENTICATION:
            updateCheckpoint = "auth: enter";
            if (authHandler) {
                authHandler->update(deltaTime);
            }
            break;

        case AppState::REALM_SELECTION:
            updateCheckpoint = "realm_selection: enter";
            if (authHandler) {
                authHandler->update(deltaTime);
            }
            break;

        case AppState::CHARACTER_CREATION:
            updateCheckpoint = "char_creation: enter";
            if (gameHandler) {
                gameHandler->update(deltaTime);
            }
            if (uiManager) {
                uiManager->getCharacterCreateScreen().update(deltaTime);
            }
            break;

        case AppState::CHARACTER_SELECTION:
            updateCheckpoint = "char_selection: enter";
            if (gameHandler) {
                gameHandler->update(deltaTime);
            }
            break;

        case AppState::IN_GAME: {
            updateCheckpoint = "in_game: enter";
            const char* inGameStep = "begin";
            try {
            auto runInGameStage = [&](const char* stageName, auto&& fn) {
                auto stageStart = std::chrono::steady_clock::now();
                try {
                    fn();
                } catch (const std::bad_alloc& e) {
                    LOG_ERROR("OOM during IN_GAME update stage '", stageName, "': ", e.what());
                    throw;
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception during IN_GAME update stage '", stageName, "': ", e.what());
                    throw;
                }
                auto stageEnd = std::chrono::steady_clock::now();
                float stageMs = std::chrono::duration<float, std::milli>(stageEnd - stageStart).count();
                if (stageMs > 50.0f) {
                    LOG_WARNING("SLOW update stage '", stageName, "': ", stageMs, "ms");
                }
            };
            inGameStep = "gameHandler update";
            updateCheckpoint = "in_game: gameHandler update";
            runInGameStage("gameHandler->update", [&] {
                if (gameHandler) {
                    gameHandler->update(deltaTime);
                }
            });
            if (addonManager_ && addonsLoaded_) {
                addonManager_->update(deltaTime);
            }
            // Always unsheath on combat engage.
            inGameStep = "auto-unsheathe";
            updateCheckpoint = "in_game: auto-unsheathe";
            if (gameHandler) {
                const bool autoAttacking = gameHandler->isAutoAttacking();
                if (autoAttacking && !wasAutoAttacking_ && appearanceComposer_ && appearanceComposer_->isWeaponsSheathed()) {
                    appearanceComposer_->setWeaponsSheathed(false);
                    appearanceComposer_->loadEquippedWeapons();
                }
                // Swap back to melee weapon when auto-attack stops
                if (!autoAttacking && wasAutoAttacking_ && appearanceComposer_ && appearanceComposer_->isShowingRanged()) {
                    appearanceComposer_->showRangedWeapon(false);
                }
                wasAutoAttacking_ = autoAttacking;
            }

            // Toggle weapon sheathe state with Z (ignored while UI captures keyboard).
            inGameStep = "weapon-toggle input";
            updateCheckpoint = "in_game: weapon-toggle input";
            {
                const bool uiWantsKeyboard = ImGui::GetIO().WantCaptureKeyboard;
                auto& input = Input::getInstance();
                if (!uiWantsKeyboard && input.isKeyJustPressed(SDL_SCANCODE_Z) && appearanceComposer_) {
                    appearanceComposer_->toggleWeaponsSheathed();
                    appearanceComposer_->loadEquippedWeapons();
                }
            }

            inGameStep = "world update";
            updateCheckpoint = "in_game: world update";
            runInGameStage("world->update", [&] {
                if (world) {
                    world->update(deltaTime);
                }
            });
            inGameStep = "spawn/equipment queues";
            updateCheckpoint = "in_game: spawn/equipment queues";
            runInGameStage("spawn/equipment queues", [&] {
                if (entitySpawner_) entitySpawner_->update();
                if (auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr) {
                    cr->processPendingNormalMaps(4);
                }
            });
            // Self-heal missing creature visuals: if a nearby UNIT exists in
            // entity state but has no render instance, queue a spawn retry.
            inGameStep = "creature resync scan";
            updateCheckpoint = "in_game: creature resync scan";
            if (gameHandler) {
                static float creatureResyncTimer = 0.0f;
                creatureResyncTimer += deltaTime;
                if (creatureResyncTimer >= 3.0f) {
                    creatureResyncTimer = 0.0f;

                    glm::vec3 playerPos(0.0f);
                    bool havePlayerPos = false;
                    uint64_t playerGuid = gameHandler->getPlayerGuid();
                    if (auto playerEntity = gameHandler->getEntityManager().getEntity(playerGuid)) {
                        playerPos = glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ());
                        havePlayerPos = true;
                    }

                    const float kResyncRadiusSq = 260.0f * 260.0f;
                    for (const auto& pair : gameHandler->getEntityManager().getEntities()) {
                        uint64_t guid = pair.first;
                        const auto& entity = pair.second;
                        if (!entity || guid == playerGuid) continue;
                        if (entity->getType() != game::ObjectType::UNIT) continue;
                        auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
                        if (!unit || unit->getDisplayId() == 0) continue;
                        if (entitySpawner_->isCreatureSpawned(guid) || entitySpawner_->isCreaturePending(guid)) continue;

                        if (havePlayerPos) {
                            glm::vec3 pos(unit->getX(), unit->getY(), unit->getZ());
                            glm::vec3 delta = pos - playerPos;
                            float distSq = glm::dot(delta, delta);
                            if (distSq > kResyncRadiusSq) continue;
                        }

                        float retryScale = 1.0f;
                        {
                            using game::fieldIndex; using game::UF;
                            uint16_t si = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
                            if (si != 0xFFFF) {
                                uint32_t raw = unit->getField(si);
                                if (raw != 0) {
                                    float s2 = 1.0f;
                                    std::memcpy(&s2, &raw, sizeof(float));
                                    if (s2 > 0.01f && s2 < 100.0f) retryScale = s2;
                                }
                            }
                        }
                        entitySpawner_->queueCreatureSpawn(guid, unit->getDisplayId(),
                            unit->getX(), unit->getY(), unit->getZ(),
                            unit->getOrientation(), retryScale);
                    }
                }
            }

            inGameStep = "gameobject/transport queues";
            updateCheckpoint = "in_game: gameobject/transport queues";
            runInGameStage("gameobject/transport queues", [&] {
                // GO/transport queues handled by entitySpawner_->update() above
            });
            inGameStep = "pending mount";
            updateCheckpoint = "in_game: pending mount";
            runInGameStage("processPendingMount", [&] {
                // Mount processing handled by entitySpawner_->update() above
            });
            // Update 3D quest markers above NPCs
            inGameStep = "quest markers";
            updateCheckpoint = "in_game: quest markers";
            runInGameStage("updateQuestMarkers", [&] {
                updateQuestMarkers();
            });
            // Sync server run speed to camera controller
            inGameStep = "post-update sync";
            updateCheckpoint = "in_game: post-update sync";
            runInGameStage("post-update sync", [&] {
                if (renderer && gameHandler && renderer->getCameraController()) {
                    renderer->getCameraController()->setRunSpeedOverride(gameHandler->getServerRunSpeed());
                    renderer->getCameraController()->setWalkSpeedOverride(gameHandler->getServerWalkSpeed());
                    renderer->getCameraController()->setSwimSpeedOverride(gameHandler->getServerSwimSpeed());
                    renderer->getCameraController()->setSwimBackSpeedOverride(gameHandler->getServerSwimBackSpeed());
                    renderer->getCameraController()->setFlightSpeedOverride(gameHandler->getServerFlightSpeed());
                    renderer->getCameraController()->setFlightBackSpeedOverride(gameHandler->getServerFlightBackSpeed());
                    renderer->getCameraController()->setRunBackSpeedOverride(gameHandler->getServerRunBackSpeed());
                    renderer->getCameraController()->setTurnRateOverride(gameHandler->getServerTurnRate());
                    renderer->getCameraController()->setMovementRooted(gameHandler->isPlayerRooted());
                    renderer->getCameraController()->setGravityDisabled(gameHandler->isGravityDisabled());
                    renderer->getCameraController()->setFeatherFallActive(gameHandler->isFeatherFalling());
                    renderer->getCameraController()->setWaterWalkActive(gameHandler->isWaterWalking());
                    renderer->getCameraController()->setFlyingActive(gameHandler->isPlayerFlying());
                    renderer->getCameraController()->setHoverActive(gameHandler->isHovering());

                    // Sync camera forward pitch to movement packets during flight / swimming.
                    // The server writes the pitch field when FLYING or SWIMMING flags are set;
                    // without this sync it would always be 0 (horizontal), causing other
                    // players to see the character flying flat even when pitching up/down.
                    if (gameHandler->isPlayerFlying() || gameHandler->isSwimming()) {
                        if (auto* cam = renderer->getCamera()) {
                            glm::vec3 fwd = cam->getForward();
                            float len = glm::length(fwd);
                            if (len > 1e-4f) {
                                float pitchRad = std::asin(std::clamp(fwd.z / len, -1.0f, 1.0f));
                                gameHandler->setMovementPitch(pitchRad);
                                // Tilt the mount/character model to match flight direction
                                // (taxi flight uses setTaxiOrientationCallback for this instead)
                                if (gameHandler->isPlayerFlying() && gameHandler->isMounted()) {
                                    if (auto* ac = renderer->getAnimationController()) ac->setMountPitchRoll(pitchRad, 0.0f);
                                }
                            }
                        }
                    } else if (gameHandler->isMounted()) {
                        // Reset mount pitch when not flying
                        if (auto* ac = renderer->getAnimationController()) ac->setMountPitchRoll(0.0f, 0.0f);
                    }
                }

                bool onTaxi = gameHandler &&
                              (gameHandler->isOnTaxiFlight() ||
                               gameHandler->isTaxiMountActive() ||
                               gameHandler->isTaxiActivationPending());
                bool onTransportNow = gameHandler && gameHandler->isOnTransport();
                // Clear stale client-side transport state when the tracked transport no longer exists.
                if (onTransportNow && gameHandler->getTransportManager()) {
                    auto* currentTracked = gameHandler->getTransportManager()->getTransport(
                        gameHandler->getPlayerTransportGuid());
                    if (!currentTracked) {
                        gameHandler->clearPlayerTransport();
                        onTransportNow = false;
                    }
                }
                // M2 transports (trams) use position-delta approach: player keeps normal
                // movement and the transport's frame-to-frame delta is applied on top.
                // Only WMO transports (ships) use full external-driven mode.
                bool isM2Transport = false;
                if (onTransportNow && gameHandler->getTransportManager()) {
                    auto* tr = gameHandler->getTransportManager()->getTransport(gameHandler->getPlayerTransportGuid());
                    isM2Transport = (tr && tr->isM2);
                }
                bool onWMOTransport = onTransportNow && !isM2Transport;
                if (worldEntryCallbacks_ && worldEntryCallbacks_->getWorldEntryMovementGraceTimer() > 0.0f) {
                    worldEntryCallbacks_->setWorldEntryMovementGraceTimer(
                        worldEntryCallbacks_->getWorldEntryMovementGraceTimer() - deltaTime);
                    // Clear stale movement from before teleport each frame
                    // until grace period expires (keys may still be held)
                    if (renderer && renderer->getCameraController())
                        renderer->getCameraController()->clearMovementInputs();
                }
                // Hearth teleport: delegated to WorldEntryCallbackHandler
                if (worldEntryCallbacks_) {
                    worldEntryCallbacks_->update(deltaTime);
                }
                if (renderer && renderer->getCameraController()) {
                const bool externallyDrivenMotion = onTaxi || onWMOTransport || (animationCallbacks_ && animationCallbacks_->isCharging());
                // Keep physics frozen (externalFollow) during landing clamp when terrain
                // hasn't loaded yet — prevents gravity from pulling player through void.
                bool hearthFreeze = worldEntryCallbacks_ && worldEntryCallbacks_->isHearthTeleportPending();
                bool landingClampActive = !onTaxi && worldEntryCallbacks_ && worldEntryCallbacks_->getTaxiLandingClampTimer() > 0.0f &&
                                          worldEntryCallbacks_->getWorldEntryMovementGraceTimer() <= 0.0f &&
                                          !gameHandler->isMounted();
                renderer->getCameraController()->setExternalFollow(externallyDrivenMotion || landingClampActive || hearthFreeze);
                renderer->getCameraController()->setExternalMoving(externallyDrivenMotion);
                if (externallyDrivenMotion) {
                    // Drop any stale local movement toggles while server drives taxi motion.
                    renderer->getCameraController()->clearMovementInputs();
                    if (worldEntryCallbacks_) worldEntryCallbacks_->setTaxiLandingClampTimer(0.0f);
                }
                if (worldEntryCallbacks_ && worldEntryCallbacks_->getLastTaxiFlight() && !onTaxi) {
                    renderer->getCameraController()->clearMovementInputs();
                    // Keep clamping until terrain loads at landing position.
                    // Timer only counts down once a valid floor is found.
                    if (worldEntryCallbacks_) worldEntryCallbacks_->setTaxiLandingClampTimer(2.0f);
                }
                if (landingClampActive) {
                    if (renderer && gameHandler) {
                        glm::vec3 p = renderer->getCharacterPosition();
                        std::optional<float> terrainFloor;
                        std::optional<float> wmoFloor;
                        std::optional<float> m2Floor;
                        if (renderer->getTerrainManager()) {
                            terrainFloor = renderer->getTerrainManager()->getHeightAt(p.x, p.y);
                        }
                        if (renderer->getWMORenderer()) {
                            // Probe from above so we can recover when current Z is already below floor.
                            wmoFloor = renderer->getWMORenderer()->getFloorHeight(p.x, p.y, p.z + 40.0f);
                        }
                        if (renderer->getM2Renderer()) {
                            // Include M2 floors (bridges/platforms) in landing recovery.
                            m2Floor = renderer->getM2Renderer()->getFloorHeight(p.x, p.y, p.z + 40.0f);
                        }

                        std::optional<float> targetFloor;
                        if (terrainFloor) targetFloor = terrainFloor;
                        if (wmoFloor && (!targetFloor || *wmoFloor > *targetFloor)) targetFloor = wmoFloor;
                        if (m2Floor && (!targetFloor || *m2Floor > *targetFloor)) targetFloor = m2Floor;

                        if (targetFloor) {
                            // Floor found — snap player to it and start countdown to release
                            float targetZ = *targetFloor + 0.10f;
                            if (std::abs(p.z - targetZ) > 0.05f) {
                                p.z = targetZ;
                                renderer->getCharacterPosition() = p;
                                glm::vec3 canonical = core::coords::renderToCanonical(p);
                                gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
                                gameHandler->sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
                            }
                            float clampTimer = worldEntryCallbacks_ ? worldEntryCallbacks_->getTaxiLandingClampTimer() : 0.0f;
                            clampTimer -= deltaTime;
                            if (worldEntryCallbacks_) worldEntryCallbacks_->setTaxiLandingClampTimer(clampTimer);
                        }
                        // No floor found: don't decrement timer, keep player frozen until terrain loads
                    }
                }
                bool idleOrbit = renderer->getCameraController()->isIdleOrbit();
                if (idleOrbit && !idleYawned_ && renderer) {
                    if (auto* ac = renderer->getAnimationController()) ac->playEmote("yawn");
                    idleYawned_ = true;
                } else if (!idleOrbit) {
                    idleYawned_ = false;
                }
                }
                if (renderer) {
                    if (auto* ac = renderer->getAnimationController()) ac->setTaxiFlight(onTaxi);
                }
                if (renderer && renderer->getTerrainManager()) {
                renderer->getTerrainManager()->setStreamingEnabled(true);
                // Taxi flights move fast (32 u/s) — load further ahead so terrain is ready
                // before the camera arrives.  Keep updates frequent to spot new tiles early.
                renderer->getTerrainManager()->setUpdateInterval(onTaxi ? 0.033f : 0.033f);
                renderer->getTerrainManager()->setLoadRadius(onTaxi ? 8 : 4);
                renderer->getTerrainManager()->setUnloadRadius(onTaxi ? 12 : 7);
                renderer->getTerrainManager()->setTaxiStreamingMode(onTaxi);
                }
                if (worldEntryCallbacks_) worldEntryCallbacks_->setLastTaxiFlight(onTaxi);

                // Sync character render position ↔ canonical WoW coords each frame
                if (renderer && gameHandler) {
                // For position sync branching, only WMO transports use the dedicated
                // onTransport branch. M2 transports use the normal movement else branch
                // with a position-delta correction applied on top.
                bool onTransport = onWMOTransport;

                static bool wasOnTransport = false;
                bool onTransportNowDbg = gameHandler->isOnTransport();
                if (onTransportNowDbg != wasOnTransport) {
                    LOG_DEBUG("Transport state changed: onTransport=", onTransportNowDbg,
                             " isM2=", isM2Transport,
                             " guid=0x", std::hex, gameHandler->getPlayerTransportGuid(), std::dec);
                    wasOnTransport = onTransportNowDbg;
                }

                if (onTaxi) {
                    auto playerEntity = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid());
                    glm::vec3 canonical(0.0f);
                    bool haveCanonical = false;
                    if (playerEntity) {
                        canonical = glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ());
                        haveCanonical = true;
                    } else {
                        // Fallback for brief entity gaps during taxi start/updates:
                        // movementInfo is still updated by client taxi simulation.
                        const auto& move = gameHandler->getMovementInfo();
                        canonical = glm::vec3(move.x, move.y, move.z);
                        haveCanonical = true;
                    }
                    if (haveCanonical) {
                        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
                        renderer->getCharacterPosition() = renderPos;
                        if (renderer->getCameraController()) {
                            glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable();
                            if (followTarget) {
                                *followTarget = renderPos;
                            }
                        }
                    }
                } else if (onTransport) {
                    // WMO transport mode (ships): compose world position from transform + local offset
                    glm::vec3 canonical = gameHandler->getComposedWorldPosition();
                    glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
                    renderer->getCharacterPosition() = renderPos;
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
                    if (renderer->getCameraController()) {
                        glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable();
                        if (followTarget) {
                            *followTarget = renderPos;
                        }
                    }
                } else if (animationCallbacks_ && animationCallbacks_->isCharging()) {
                    // Warrior Charge: interpolation delegated to AnimationCallbackHandler
                    animationCallbacks_->updateCharge(deltaTime);
                } else {
                    glm::vec3 renderPos = renderer->getCharacterPosition();

                    // M2 transport riding: resolve in canonical space and lock once per frame.
                    // This avoids visible jitter from mixed render/canonical delta application.
                    if (isM2Transport && gameHandler->getTransportManager()) {
                        auto* tr = gameHandler->getTransportManager()->getTransport(
                            gameHandler->getPlayerTransportGuid());
                        if (tr) {
                            // Keep passenger locked to elevator vertical motion while grounded.
                            // Without this, floor clamping can hold world-Z static unless the
                            // player is jumping, which makes lifts appear to not move vertically.
                            glm::vec3 tentativeCanonical = core::coords::renderToCanonical(renderPos);
                            glm::vec3 localOffset = gameHandler->getPlayerTransportOffset();
                            localOffset.x = tentativeCanonical.x - tr->position.x;
                            localOffset.y = tentativeCanonical.y - tr->position.y;
                            if (renderer->getCameraController() &&
                                !renderer->getCameraController()->isGrounded()) {
                                // While airborne (jump/fall), allow local Z offset to change.
                                localOffset.z = tentativeCanonical.z - tr->position.z;
                            }
                            gameHandler->setPlayerTransportOffset(localOffset);

                            glm::vec3 lockedCanonical = tr->position + localOffset;
                            renderPos = core::coords::canonicalToRender(lockedCanonical);
                            renderer->getCharacterPosition() = renderPos;
                        }
                    }

                    glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);

                    // Sync orientation: camera yaw (degrees) → WoW orientation (radians)
                    float yawDeg = renderer->getCharacterYaw();
                    // Keep all game-side orientation in canonical space.
                    // We historically sent serverYaw = radians(yawDeg - 90). With the new
                    // canonical<->server mapping (serverYaw = PI/2 - canonicalYaw), the
                    // equivalent canonical yaw is radians(180 - yawDeg).
                    float canonicalYaw = core::coords::normalizeAngleRad(glm::radians(180.0f - yawDeg));
                    gameHandler->setOrientation(canonicalYaw);

                    // Send MSG_MOVE_SET_FACING when the player changes facing direction
                    // (e.g. via mouse-look). Without this, the server predicts movement in
                    // the old facing and position-corrects on the next heartbeat — the
                    // micro-teleporting the GM observed.
                    // Skip while keyboard-turning: the server tracks that via TURN_LEFT/RIGHT flags.
                    facingSendCooldown_ -= deltaTime;
                    const auto& mi = gameHandler->getMovementInfo();
                    constexpr uint32_t kTurnFlags =
                        static_cast<uint32_t>(game::MovementFlags::TURN_LEFT) |
                        static_cast<uint32_t>(game::MovementFlags::TURN_RIGHT);
                    bool keyboardTurning = (mi.flags & kTurnFlags) != 0;
                    if (!keyboardTurning && facingSendCooldown_ <= 0.0f) {
                        float yawDiff = core::coords::normalizeAngleRad(canonicalYaw - lastSentCanonicalYaw_);
                        if (std::abs(yawDiff) > glm::radians(3.0f)) {
                            gameHandler->sendMovement(game::Opcode::MSG_MOVE_SET_FACING);
                            lastSentCanonicalYaw_ = canonicalYaw;
                            facingSendCooldown_ = 0.1f;  // max 10 Hz
                        }
                    }

                    // Client-side transport boarding detection (for M2 transports like trams
                    // and lifts where the server doesn't send transport attachment data).
                    // Thunder Bluff elevators use model origins that can be far from the deck
                    // the player stands on, so they need wider attachment bounds.
                    if (gameHandler->getTransportManager() && !gameHandler->isOnTransport()) {
                        auto* tm = gameHandler->getTransportManager();
                        glm::vec3 playerCanonical = core::coords::renderToCanonical(renderPos);
                        constexpr float kM2BoardHorizDistSq = 12.0f * 12.0f;
                        constexpr float kM2BoardVertDist = 15.0f;
                        constexpr float kTbLiftBoardHorizDistSq = 22.0f * 22.0f;
                        constexpr float kTbLiftBoardVertDist = 14.0f;

                        uint64_t bestGuid = 0;
                        float bestScore = 1e30f;
                        for (auto& [guid, transport] : tm->getTransports()) {
                            if (!transport.isM2) continue;
                            const bool isThunderBluffLift =
                                (transport.entry >= 20649u && transport.entry <= 20657u);
                            const float maxHorizDistSq = isThunderBluffLift
                                ? kTbLiftBoardHorizDistSq
                                : kM2BoardHorizDistSq;
                            const float maxVertDist = isThunderBluffLift
                                ? kTbLiftBoardVertDist
                                : kM2BoardVertDist;
                            glm::vec3 diff = playerCanonical - transport.position;
                            float horizDistSq = diff.x * diff.x + diff.y * diff.y;
                            float vertDist = std::abs(diff.z);
                            if (horizDistSq < maxHorizDistSq && vertDist < maxVertDist) {
                                float score = horizDistSq + vertDist * vertDist;
                                if (score < bestScore) {
                                    bestScore = score;
                                    bestGuid = guid;
                                }
                            }
                        }
                        if (bestGuid != 0) {
                            auto* tr = tm->getTransport(bestGuid);
                            if (tr) {
                                gameHandler->setPlayerOnTransport(bestGuid, playerCanonical - tr->position);
                                LOG_DEBUG("M2 transport boarding: guid=0x", std::hex, bestGuid, std::dec);
                            }
                        }
                    }

                    // M2 transport disembark: player walked far enough from transport center
                    if (isM2Transport && gameHandler->getTransportManager()) {
                        auto* tm = gameHandler->getTransportManager();
                        auto* tr = tm->getTransport(gameHandler->getPlayerTransportGuid());
                        if (tr) {
                            glm::vec3 playerCanonical = core::coords::renderToCanonical(renderPos);
                            glm::vec3 diff = playerCanonical - tr->position;
                            float horizDistSq = diff.x * diff.x + diff.y * diff.y;
                            const bool isThunderBluffLift =
                                (tr->entry >= 20649u && tr->entry <= 20657u);
                            constexpr float kM2DisembarkHorizDistSq = 15.0f * 15.0f;
                            constexpr float kTbLiftDisembarkHorizDistSq = 28.0f * 28.0f;
                            constexpr float kM2DisembarkVertDist = 18.0f;
                            constexpr float kTbLiftDisembarkVertDist = 16.0f;
                            const float disembarkHorizDistSq = isThunderBluffLift
                                ? kTbLiftDisembarkHorizDistSq
                                : kM2DisembarkHorizDistSq;
                            const float disembarkVertDist = isThunderBluffLift
                                ? kTbLiftDisembarkVertDist
                                : kM2DisembarkVertDist;
                            if (horizDistSq > disembarkHorizDistSq || std::abs(diff.z) > disembarkVertDist) {
                                gameHandler->clearPlayerTransport();
                                LOG_DEBUG("M2 transport disembark");
                            }
                        }
                    }
                }
                }
            });

            // Keep creature render instances aligned with authoritative entity positions.
            // This prevents desync where target circles move with server entities but
            // creature models remain at stale spawn positions.
            inGameStep = "creature render sync";
            updateCheckpoint = "in_game: creature render sync";
            auto creatureSyncStart = std::chrono::steady_clock::now();
            if (renderer && gameHandler && renderer->getCharacterRenderer()) {
                auto* charRenderer = renderer->getCharacterRenderer();
                static float npcWeaponRetryTimer = 0.0f;
                npcWeaponRetryTimer += deltaTime;
                const bool npcWeaponRetryTick = (npcWeaponRetryTimer >= 1.0f);
                if (npcWeaponRetryTick) npcWeaponRetryTimer = 0.0f;
                int weaponAttachesThisTick = 0;
                glm::vec3 playerPos(0.0f);
                glm::vec3 playerRenderPos(0.0f);
                bool havePlayerPos = false;
                float playerCollisionRadius = 0.65f;
                if (auto playerEntity = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid())) {
                    playerPos = glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ());
                    playerRenderPos = core::coords::canonicalToRender(playerPos);
                    havePlayerPos = true;
                    glm::vec3 pc;
                    float pr = 0.0f;
                    if (getRenderBoundsForGuid(gameHandler->getPlayerGuid(), pc, pr)) {
                        playerCollisionRadius = std::clamp(pr * 0.35f, 0.45f, 1.1f);
                    }
                }
                const float syncRadiusSq = 320.0f * 320.0f;
                auto& _creatureInstances = entitySpawner_->getCreatureInstances();
                auto& _creatureWeaponsAttached = entitySpawner_->getCreatureWeaponsAttached();
                auto& _creatureWeaponAttachAttempts = entitySpawner_->getCreatureWeaponAttachAttempts();
                auto& _creatureModelIds = entitySpawner_->getCreatureModelIds();
                auto& _modelIdIsWolfLike = entitySpawner_->getModelIdIsWolfLike();
                auto& _creatureRenderPosCache = entitySpawner_->getCreatureRenderPosCache();
                auto& _creatureSwimmingState = entitySpawner_->getCreatureSwimmingState();
                auto& _creatureWalkingState = entitySpawner_->getCreatureWalkingState();
                auto& _creatureFlyingState = entitySpawner_->getCreatureFlyingState();
                auto& _creatureWasMoving = entitySpawner_->getCreatureWasMoving();
                auto& _creatureWasSwimming = entitySpawner_->getCreatureWasSwimming();
                auto& _creatureWasFlying = entitySpawner_->getCreatureWasFlying();
                auto& _creatureWasWalking = entitySpawner_->getCreatureWasWalking();
                for (const auto& [guid, instanceId] : _creatureInstances) {
                    auto entity = gameHandler->getEntityManager().getEntity(guid);
                    if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

                    if (npcWeaponRetryTick &&
                        weaponAttachesThisTick < EntitySpawner::MAX_WEAPON_ATTACHES_PER_TICK &&
                        !_creatureWeaponsAttached.count(guid)) {
                        uint8_t attempts = 0;
                        auto itAttempts = _creatureWeaponAttachAttempts.find(guid);
                        if (itAttempts != _creatureWeaponAttachAttempts.end()) attempts = itAttempts->second;
                        if (attempts < 30) {
                            weaponAttachesThisTick++;
                            if (entitySpawner_->tryAttachCreatureVirtualWeapons(guid, instanceId)) {
                                _creatureWeaponsAttached.insert(guid);
                                _creatureWeaponAttachAttempts.erase(guid);
                            } else {
                                _creatureWeaponAttachAttempts[guid] = static_cast<uint8_t>(attempts + 1);
                            }
                        }
                    }

                    // Distance check uses getLatestX/Y/Z (server-authoritative destination) to
                    // avoid false-culling entities that moved while getX/Y/Z was stale.
                    // Position sync still uses getX/Y/Z to preserve smooth interpolation for
                    // nearby entities; distant entities (> 150u) have planarDist≈0 anyway
                    // so the renderer remains driven correctly by creatureMoveCallback_.
                    glm::vec3 latestCanonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
                    float canonDistSq = 0.0f;
                    if (havePlayerPos) {
                        glm::vec3 d = latestCanonical - playerPos;
                        canonDistSq = glm::dot(d, d);
                        if (canonDistSq > syncRadiusSq) continue;
                    }

                    // Use the destination position once the entity has reached its
                    // target.  During the dead-reckoning overrun window getX/Y/Z
                    // drifts past the destination at the last known velocity;
                    // using getLatest (== moveEnd while isMoving_) avoids the
                    // visible forward-drift followed by a backward snap.
                    const bool inOverrun = entity->isEntityMoving() && !entity->isActivelyMoving();
                    glm::vec3 canonical(
                        inOverrun ? entity->getLatestX() : entity->getX(),
                        inOverrun ? entity->getLatestY() : entity->getY(),
                        inOverrun ? entity->getLatestZ() : entity->getZ());
                    glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

                    // Clamp creature Z to terrain surface during movement interpolation.
                    // The server sends single-segment moves and expects the client to place
                    // creatures on the ground.  Only clamp while actively moving — idle
                    // creatures keep their server-authoritative Z (flight masters, etc.).
                    if (entity->isActivelyMoving() && renderer->getTerrainManager()) {
                        auto terrainZ = renderer->getTerrainManager()->getHeightAt(renderPos.x, renderPos.y);
                        if (terrainZ.has_value()) {
                            renderPos.z = terrainZ.value();
                        }
                    }

                    // Visual collision guard: keep hostile melee units from rendering inside the
                    // player's model while attacking. This is client-side only (no server position change).
                    // Only check for creatures within 8 units (melee range) — saves expensive
                    // getRenderBoundsForGuid/getModelData calls for distant creatures.
                    bool clipGuardEligible = false;
                    bool isCombatTarget = false;
                    if (havePlayerPos && canonDistSq < 64.0f) { // 8² = melee range
                        auto unit = std::static_pointer_cast<game::Unit>(entity);
                        const uint64_t currentTargetGuid = gameHandler->hasTarget() ? gameHandler->getTargetGuid() : 0;
                        const uint64_t autoAttackGuid = gameHandler->getAutoAttackTargetGuid();
                        isCombatTarget = (guid == currentTargetGuid || guid == autoAttackGuid);
                        clipGuardEligible = unit->getHealth() > 0 &&
                                            (unit->isHostile() ||
                                             gameHandler->isAggressiveTowardPlayer(guid) ||
                                             isCombatTarget);
                    }
                    if (clipGuardEligible) {
                        float creatureCollisionRadius = 0.8f;
                        glm::vec3 cc;
                        float cr = 0.0f;
                        if (getRenderBoundsForGuid(guid, cc, cr)) {
                            creatureCollisionRadius = std::clamp(cr * 0.45f, 0.65f, 1.9f);
                        }

                        float minSep = std::max(playerCollisionRadius + creatureCollisionRadius, 1.9f);
                        if (isCombatTarget) {
                            // Stronger spacing for the actively engaged attacker to avoid bite-overlap.
                            minSep = std::max(minSep, 2.2f);
                        }

                        // Species/model-specific spacing for wolf-like creatures (their lunge anims
                        // often put head/torso inside the player capsule).
                        auto mit = _creatureModelIds.find(guid);
                        if (mit != _creatureModelIds.end()) {
                            uint32_t mid = mit->second;
                            auto wolfIt = _modelIdIsWolfLike.find(mid);
                            if (wolfIt == _modelIdIsWolfLike.end()) {
                                bool isWolf = false;
                                if (const auto* md = charRenderer->getModelData(mid)) {
                                    std::string modelName = md->name;
                                    std::transform(modelName.begin(), modelName.end(), modelName.begin(),
                                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    isWolf = (modelName.find("wolf") != std::string::npos ||
                                              modelName.find("worg") != std::string::npos);
                                }
                                wolfIt = _modelIdIsWolfLike.emplace(mid, isWolf).first;
                            }
                            if (wolfIt->second) {
                                minSep = std::max(minSep, 2.45f);
                            }
                        }

                        glm::vec2 d2(renderPos.x - playerRenderPos.x, renderPos.y - playerRenderPos.y);
                        float distSq2 = glm::dot(d2, d2);
                        if (distSq2 < (minSep * minSep)) {
                            glm::vec2 dir2(1.0f, 0.0f);
                            if (distSq2 > 1e-6f) {
                                dir2 = d2 * (1.0f / std::sqrt(distSq2));
                            }
                            glm::vec2 clamped2 = glm::vec2(playerRenderPos.x, playerRenderPos.y) + dir2 * minSep;
                            renderPos.x = clamped2.x;
                            renderPos.y = clamped2.y;
                        }
                    }

                    auto posIt = _creatureRenderPosCache.find(guid);
                    if (posIt == _creatureRenderPosCache.end()) {
                        charRenderer->setInstancePosition(instanceId, renderPos);
                        _creatureRenderPosCache[guid] = renderPos;
                    } else {
                        const glm::vec3 prevPos = posIt->second;
                        float ddx2 = renderPos.x - prevPos.x;
                        float ddy2 = renderPos.y - prevPos.y;
                        float planarDistSq = ddx2 * ddx2 + ddy2 * ddy2;
                        float dz = std::abs(renderPos.z - prevPos.z);

                        auto unitPtr = std::static_pointer_cast<game::Unit>(entity);
                        const bool deadOrCorpse = unitPtr->getHealth() == 0;
                        const bool largeCorrection = (planarDistSq > 36.0f) || (dz > 3.0f);
                        // Use isActivelyMoving() so Run/Walk animation stops when the
                        // creature reaches its destination. Don't use position-change
                        // (planarDistSq) as a movement indicator when the entity is in
                        // the dead-reckoning overrun window — the residual velocity
                        // drift would keep the walk/run animation playing long after
                        // the creature has actually arrived. Only fall back to position-
                        // change detection for entities with no active movement tracking
                        // (e.g. teleports or position-only updates from the server).
                        const bool entityIsMoving = entity->isActivelyMoving();
                        constexpr float kMoveThreshSq = 0.03f * 0.03f;
                        const bool posChanging = planarDistSq > kMoveThreshSq || dz > 0.08f;
                        const bool isMovingNow = !deadOrCorpse && (entityIsMoving || (posChanging && !entity->isEntityMoving()));
                        if (deadOrCorpse || largeCorrection) {
                            charRenderer->setInstancePosition(instanceId, renderPos);
                        } else if (planarDistSq > kMoveThreshSq || dz > 0.08f) {
                            // Position changed in entity coords → drive renderer toward it.
                            float planarDist = std::sqrt(planarDistSq);
                            float duration = std::clamp(planarDist / 5.5f, 0.05f, 0.22f);
                            charRenderer->moveInstanceTo(instanceId, renderPos, duration);
                        }
                        // When entity is moving but getX/Y/Z is stale (distance-culled),
                        // don't call moveInstanceTo — creatureMoveCallback_ already drove
                        // the renderer to the correct destination via the spline packet.
                        posIt->second = renderPos;

                        // Drive movement animation: Walk/Run/Swim (4/5/42) when moving,
                        // Stand/SwimIdle (0/41) when idle. Walk(4) selected when WALKING flag is set.
                        // WoW M2 animation IDs: 4=Walk, 5=Run, 41=SwimIdle, 42=Swim.
                        // Only switch on transitions to avoid resetting animation time.
                        // Don't override Death (1) animation.
                        const bool isSwimmingNow = _creatureSwimmingState.count(guid) > 0;
                        const bool isWalkingNow  = _creatureWalkingState.count(guid) > 0;
                        const bool isFlyingNow   = _creatureFlyingState.count(guid) > 0;
                        bool prevMoving   = _creatureWasMoving[guid];
                        bool prevSwimming = _creatureWasSwimming[guid];
                        bool prevFlying   = _creatureWasFlying[guid];
                        bool prevWalking  = _creatureWasWalking[guid];
                        // Trigger animation update on any locomotion-state transition, not just
                        // moving/idle — e.g. creature lands while still moving → FlyForward→Run,
                        // or server changes WALKING flag while creature is already running → Walk.
                        const bool stateChanged = (isMovingNow  != prevMoving)   ||
                                                  (isSwimmingNow != prevSwimming) ||
                                                  (isFlyingNow   != prevFlying)   ||
                                                  (isWalkingNow  != prevWalking && isMovingNow);
                        if (stateChanged) {
                            _creatureWasMoving[guid]   = isMovingNow;
                            _creatureWasSwimming[guid] = isSwimmingNow;
                            _creatureWasFlying[guid]   = isFlyingNow;
                            _creatureWasWalking[guid]  = isWalkingNow;
                            uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                            bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                            if (!gotState || curAnimId != rendering::anim::DEATH) {
                                uint32_t targetAnim;
                                if (isMovingNow) {
                                    if (isFlyingNow)        targetAnim = rendering::anim::FLY_FORWARD;
                                    else if (isSwimmingNow) targetAnim = rendering::anim::SWIM;
                                    else if (isWalkingNow)  targetAnim = rendering::anim::WALK;
                                    else                    targetAnim = rendering::anim::RUN;
                                } else {
                                    if (isFlyingNow)        targetAnim = rendering::anim::FLY_IDLE;
                                    else if (isSwimmingNow) targetAnim = rendering::anim::SWIM_IDLE;
                                    else                    targetAnim = rendering::anim::STAND;
                                }
                                charRenderer->playAnimation(instanceId, targetAnim, /*loop=*/true);
                            }
                        }
                    }
                    float renderYaw = entity->getOrientation() + glm::radians(90.0f);
                    charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, 0.0f, renderYaw));
                }
            }
            {
                float csMs = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - creatureSyncStart).count();
                if (csMs > 5.0f) {
                    LOG_WARNING("SLOW update stage 'creature render sync': ", csMs, "ms (",
                                entitySpawner_->getCreatureInstances().size(), " creatures)");
                }
            }

            // --- Online player render sync (position, orientation, animation) ---
            // Mirrors the creature sync loop above but without collision guard or
            // weapon-attach logic.  Without this, online players never transition
            // back to Stand after movement stops ("run in place" bug).
            auto playerSyncStart = std::chrono::steady_clock::now();
            if (renderer && gameHandler && renderer->getCharacterRenderer()) {
                auto* charRenderer = renderer->getCharacterRenderer();
                glm::vec3 pPos(0.0f);
                bool havePPos = false;
                if (auto pe = gameHandler->getEntityManager().getEntity(gameHandler->getPlayerGuid())) {
                    pPos = glm::vec3(pe->getX(), pe->getY(), pe->getZ());
                    havePPos = true;
                }
                const float pSyncRadiusSq = 320.0f * 320.0f;

                auto& _playerInstances = entitySpawner_->getPlayerInstances();
                auto& _pCreatureWasMoving = entitySpawner_->getCreatureWasMoving();
                auto& _pCreatureWasSwimming = entitySpawner_->getCreatureWasSwimming();
                auto& _pCreatureWasFlying = entitySpawner_->getCreatureWasFlying();
                auto& _pCreatureWasWalking = entitySpawner_->getCreatureWasWalking();
                auto& _pCreatureSwimmingState = entitySpawner_->getCreatureSwimmingState();
                auto& _pCreatureWalkingState = entitySpawner_->getCreatureWalkingState();
                auto& _pCreatureFlyingState = entitySpawner_->getCreatureFlyingState();
                auto& _pCreatureRenderPosCache = entitySpawner_->getCreatureRenderPosCache();
                for (const auto& [guid, instanceId] : _playerInstances) {
                    auto entity = gameHandler->getEntityManager().getEntity(guid);
                    if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;

                    // Distance cull
                    if (havePPos) {
                        glm::vec3 latestCanonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
                        glm::vec3 d = latestCanonical - pPos;
                        if (glm::dot(d, d) > pSyncRadiusSq) continue;
                    }

                    // Position sync — clamp to destination during dead-reckoning
                    // overrun to avoid drift + backward snap (same as creature loop).
                    const bool inOverrun = entity->isEntityMoving() && !entity->isActivelyMoving();
                    glm::vec3 canonical(
                        inOverrun ? entity->getLatestX() : entity->getX(),
                        inOverrun ? entity->getLatestY() : entity->getY(),
                        inOverrun ? entity->getLatestZ() : entity->getZ());
                    glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

                    // Clamp other players' Z to terrain surface during movement
                    if (entity->isActivelyMoving() && renderer->getTerrainManager()) {
                        auto terrainZ = renderer->getTerrainManager()->getHeightAt(renderPos.x, renderPos.y);
                        if (terrainZ.has_value()) {
                            renderPos.z = terrainZ.value();
                        }
                    }

                    auto posIt = _pCreatureRenderPosCache.find(guid);
                    if (posIt == _pCreatureRenderPosCache.end()) {
                        charRenderer->setInstancePosition(instanceId, renderPos);
                        _pCreatureRenderPosCache[guid] = renderPos;
                    } else {
                        const glm::vec3 prevPos = posIt->second;
                        float ddx2 = renderPos.x - prevPos.x;
                        float ddy2 = renderPos.y - prevPos.y;
                        float planarDistSq = ddx2 * ddx2 + ddy2 * ddy2;
                        float dz = std::abs(renderPos.z - prevPos.z);

                        auto unitPtr = std::static_pointer_cast<game::Unit>(entity);
                        const bool deadOrCorpse = unitPtr->getHealth() == 0;
                        const bool largeCorrection = (planarDistSq > 36.0f) || (dz > 3.0f);
                        const bool entityIsMoving = entity->isActivelyMoving();
                        constexpr float kMoveThreshSq2 = 0.03f * 0.03f;
                        const bool posChanging2 = planarDistSq > kMoveThreshSq2 || dz > 0.08f;
                        const bool isMovingNow = !deadOrCorpse && (entityIsMoving || (posChanging2 && !entity->isEntityMoving()));

                        if (deadOrCorpse || largeCorrection) {
                            charRenderer->setInstancePosition(instanceId, renderPos);
                        } else if (planarDistSq > kMoveThreshSq2 || dz > 0.08f) {
                            float planarDist = std::sqrt(planarDistSq);
                            float duration = std::clamp(planarDist / 5.5f, 0.05f, 0.22f);
                            charRenderer->moveInstanceTo(instanceId, renderPos, duration);
                        }
                        posIt->second = renderPos;

                        // Drive movement animation (same logic as creatures)
                        const bool isSwimmingNow = _pCreatureSwimmingState.count(guid) > 0;
                        const bool isWalkingNow  = _pCreatureWalkingState.count(guid) > 0;
                        const bool isFlyingNow   = _pCreatureFlyingState.count(guid) > 0;
                        bool prevMoving   = _pCreatureWasMoving[guid];
                        bool prevSwimming = _pCreatureWasSwimming[guid];
                        bool prevFlying   = _pCreatureWasFlying[guid];
                        bool prevWalking  = _pCreatureWasWalking[guid];
                        const bool stateChanged = (isMovingNow  != prevMoving)   ||
                                                  (isSwimmingNow != prevSwimming) ||
                                                  (isFlyingNow   != prevFlying)   ||
                                                  (isWalkingNow  != prevWalking && isMovingNow);
                        if (stateChanged) {
                            _pCreatureWasMoving[guid]   = isMovingNow;
                            _pCreatureWasSwimming[guid] = isSwimmingNow;
                            _pCreatureWasFlying[guid]   = isFlyingNow;
                            _pCreatureWasWalking[guid]  = isWalkingNow;
                            uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                            bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                            if (!gotState || curAnimId != rendering::anim::DEATH) {
                                uint32_t targetAnim;
                                if (isMovingNow) {
                                    if (isFlyingNow)        targetAnim = rendering::anim::FLY_FORWARD;
                                    else if (isSwimmingNow) targetAnim = rendering::anim::SWIM;
                                    else if (isWalkingNow)  targetAnim = rendering::anim::WALK;
                                    else                    targetAnim = rendering::anim::RUN;
                                } else {
                                    if (isFlyingNow)        targetAnim = rendering::anim::FLY_IDLE;
                                    else if (isSwimmingNow) targetAnim = rendering::anim::SWIM_IDLE;
                                    else                    targetAnim = rendering::anim::STAND;
                                }
                                charRenderer->playAnimation(instanceId, targetAnim, /*loop=*/true);
                            }
                        }
                    }

                    // Orientation sync
                    float renderYaw = entity->getOrientation() + glm::radians(90.0f);
                    charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, 0.0f, renderYaw));
                }
            }
            {
                float psMs = std::chrono::duration<float, std::milli>(
                    std::chrono::steady_clock::now() - playerSyncStart).count();
                if (psMs > 5.0f) {
                    LOG_WARNING("SLOW update stage 'player render sync': ", psMs, "ms (",
                                entitySpawner_->getPlayerInstances().size(), " players)");
                }
            }

            // Movement heartbeat is sent from GameHandler::update() to avoid
            // duplicate packets from multiple update loops.

            } catch (const std::bad_alloc& e) {
                LOG_ERROR("OOM inside AppState::IN_GAME at step '", inGameStep, "': ", e.what());
                throw;
            } catch (const std::exception& e) {
                LOG_ERROR("Exception inside AppState::IN_GAME at step '", inGameStep, "': ", e.what());
                throw;
            }
            break;
        }

        case AppState::DISCONNECTED:
            // Handle disconnection
            break;
    }

    // Process any pending world entry request via WorldLoader
    if (worldLoader_ && state != AppState::DISCONNECTED) {
        worldLoader_->processPendingEntry();
    }

    // Update renderer (camera, etc.) only when in-game
    updateCheckpoint = "renderer update";
    if (renderer && state == AppState::IN_GAME) {
        auto rendererUpdateStart = std::chrono::steady_clock::now();
        try {
            renderer->update(deltaTime);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::update stage 'renderer->update': ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::update stage 'renderer->update': ", e.what());
            throw;
        }
        float ruMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - rendererUpdateStart).count();
        if (ruMs > 50.0f) {
            LOG_WARNING("SLOW update stage 'renderer->update': ", ruMs, "ms");
        }
    }
    // Update UI
    updateCheckpoint = "ui update";
    if (uiManager) {
        try {
            uiManager->update(deltaTime);
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("OOM during Application::update stage 'uiManager->update': ", e.what());
            throw;
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during Application::update stage 'uiManager->update': ", e.what());
            throw;
        }
    }
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("OOM in Application::update checkpoint '", updateCheckpoint, "': ", e.what());
        throw;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception in Application::update checkpoint '", updateCheckpoint, "': ", e.what());
        throw;
    }
}

void Application::render() {
    if (!renderer) {
        return;
    }

    renderer->beginFrame();

    // Only render 3D world when in-game
    if (state == AppState::IN_GAME) {
        if (world) {
            renderer->renderWorld(world.get(), gameHandler.get());
        } else {
            renderer->renderWorld(nullptr, gameHandler.get());
        }
    }

    // Render performance HUD (within ImGui frame, before UI ends the frame)
    if (renderer) {
        renderer->renderHUD();
    }

    // Render UI on top (ends ImGui frame with ImGui::Render())
    if (uiManager) {
        uiManager->render(state, authHandler.get(), gameHandler.get());
    }

    renderer->endFrame();
}

void Application::setupUICallbacks() {
    // ── UI screen callbacks (auth, realm, character selection/creation) ──
    uiScreenCallbacks_ = std::make_unique<UIScreenCallbackHandler>(
        *uiManager, *gameHandler, *authHandler, expansionRegistry_.get(),
        assetManager.get(),
        [this](AppState s) { setState(s); });
    uiScreenCallbacks_->setupCallbacks();

    // ── World entry, unstuck, hearthstone, bind point ──
    worldEntryCallbacks_ = std::make_unique<WorldEntryCallbackHandler>(
        *renderer, *gameHandler, worldLoader_.get(), entitySpawner_.get(),
        audioCoordinator_.get(), assetManager.get());
    worldEntryCallbacks_->setupCallbacks();

    // ── Entity spawn/despawn (creatures, players, game objects) ──
    entitySpawnCallbacks_ = std::make_unique<EntitySpawnCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler,
        [this](uint64_t guid) {
            uint64_t localGuid = gameHandler ? gameHandler->getPlayerGuid() : 0;
            uint64_t activeGuid = gameHandler ? gameHandler->getActiveCharacterGuid() : 0;
            return (localGuid != 0 && guid == localGuid) ||
                   (activeGuid != 0 && guid == activeGuid) ||
                   (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_);
        });
    entitySpawnCallbacks_->setupCallbacks();

    // ── Animation: death, respawn, swing, hit, spell, emote, charge, etc. ──
    animationCallbacks_ = std::make_unique<AnimationCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler);
    animationCallbacks_->setupCallbacks();

    // ── NPC interaction: greeting, farewell, vendor, aggro voice ──
    npcInteractionCallbacks_ = std::make_unique<NPCInteractionCallbackHandler>(
        *entitySpawner_, renderer.get(), *gameHandler, audioCoordinator_.get());
    npcInteractionCallbacks_->setupCallbacks();

    // ── Audio: music, sound effects, level-up, achievement, LFG ──
    audioCallbacks_ = std::make_unique<AudioCallbackHandler>(
        *assetManager, audioCoordinator_.get(), renderer.get(),
        uiManager.get(), *gameHandler);
    audioCallbacks_->setupCallbacks();

    // ── Transport: mount, taxi, transport spawn/move ──
    transportCallbacks_ = std::make_unique<TransportCallbackHandler>(
        *entitySpawner_, *renderer, *gameHandler, worldLoader_.get());
    transportCallbacks_->setupCallbacks();
}

void Application::spawnPlayerCharacter() {
    if (playerCharacterSpawned) return;
    if (!renderer || !renderer->getCharacterRenderer() || !renderer->getCamera()) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    auto* camera = renderer->getCamera();
    bool loaded = false;
    std::string m2Path = appearanceComposer_->getPlayerModelPath(playerRace_, playerGender_);
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

    // Try loading selected character model from MPQ
    if (assetManager && assetManager->isInitialized()) {
        auto m2Data = assetManager->readFile(m2Path);
        if (!m2Data.empty()) {
            auto model = pipeline::M2Loader::load(m2Data);

            // Load skin file for submesh/batch data
            std::string skinPath = modelDir + baseName + "00.skin";
            auto skinData = assetManager->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            }

            if (model.isValid()) {
                // Log texture slots
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    auto& tex = model.textures[ti];
                    LOG_INFO("  Texture ", ti, ": type=", tex.type, " name='", tex.filename, "'");
                }

                // Resolve textures from CharSections.dbc via AppearanceComposer
                PlayerTextureInfo texInfo;
                bool useCharSections = true;
                if (appearanceComposer_) {
                    uint32_t appearanceBytes = 0;
                    if (gameHandler) {
                        const game::Character* activeChar = gameHandler->getActiveCharacter();
                        if (activeChar) {
                            appearanceBytes = activeChar->appearanceBytes;
                        }
                    }
                    texInfo = appearanceComposer_->resolvePlayerTextures(model, playerRace_, playerGender_, appearanceBytes);
                }

                // Load external .anim files for sequences with external data.
                // Sequences WITH flag 0x20 have their animation data inline in the M2 file.
                // Sequences WITHOUT flag 0x20 store data in external .anim files.
                for (uint32_t si = 0; si < model.sequences.size(); si++) {
                    if (!(model.sequences[si].flags & 0x20)) {
                        // File naming: <ModelPath><AnimId>-<VariationIndex>.anim
                        // e.g. Character\Human\Male\HumanMale0097-00.anim
                        char animFileName[256];
                        snprintf(animFileName, sizeof(animFileName),
                            "%s%s%04u-%02u.anim",
                            modelDir.c_str(),
                            baseName.c_str(),
                            model.sequences[si].id,
                            model.sequences[si].variationIndex);
                        auto animFileData = assetManager->readFileOptional(animFileName);
                        if (!animFileData.empty()) {
                            pipeline::M2Loader::loadAnimFile(m2Data, animFileData, si, model);
                        }
                    }
                }

                charRenderer->loadModel(model, 1);

                // Apply composited textures via AppearanceComposer (saves skin state for re-compositing)
                if (useCharSections && appearanceComposer_) {
                    appearanceComposer_->compositePlayerSkin(1, texInfo);
                }

                loaded = true;
                LOG_INFO("Loaded character model: ", m2Path, " (", model.vertices.size(), " verts, ",
                         model.bones.size(), " bones, ", model.sequences.size(), " anims, ",
                         model.indices.size(), " indices, ", model.batches.size(), " batches");
                // Log all animation sequence IDs
                for (size_t i = 0; i < model.sequences.size(); i++) {
                }
            }
        }
    }

    // Fallback: create a simple cube if MPQ not available
    if (!loaded) {
        pipeline::M2Model testModel;
        float size = 2.0f;
        glm::vec3 cubePos[] = {
            {-size, -size, -size}, { size, -size, -size},
            { size,  size, -size}, {-size,  size, -size},
            {-size, -size,  size}, { size, -size,  size},
            { size,  size,  size}, {-size,  size,  size}
        };
        for (const auto& pos : cubePos) {
            pipeline::M2Vertex v;
            v.position = pos;
            v.normal = glm::normalize(pos);
            v.texCoords[0] = glm::vec2(0.0f);
            v.boneWeights[0] = 255;
            v.boneWeights[1] = v.boneWeights[2] = v.boneWeights[3] = 0;
            v.boneIndices[0] = 0;
            v.boneIndices[1] = v.boneIndices[2] = v.boneIndices[3] = 0;
            testModel.vertices.push_back(v);
        }
        uint16_t cubeIndices[] = {
            0,1,2, 0,2,3, 4,6,5, 4,7,6,
            0,4,5, 0,5,1, 2,6,7, 2,7,3,
            0,3,7, 0,7,4, 1,5,6, 1,6,2
        };
        for (uint16_t idx : cubeIndices)
            testModel.indices.push_back(idx);

        pipeline::M2Bone bone;
        bone.keyBoneId = -1;
        bone.flags = 0;
        bone.parentBone = -1;
        bone.submeshId = 0;
        bone.pivot = glm::vec3(0.0f);
        testModel.bones.push_back(bone);

        pipeline::M2Sequence seq{};
        seq.id = 0;
        seq.duration = 1000;
        testModel.sequences.push_back(seq);

        testModel.name = "TestCube";
        testModel.globalFlags = 0;
        charRenderer->loadModel(testModel, 1);
        LOG_INFO("Loaded fallback cube model (no MPQ data)");
    }

    // Spawn character at the camera controller's default position (matches hearthstone).
    // Most presets snap to floor; explicit WMO-floor presets keep their authored Z.
    auto* camCtrl = renderer->getCameraController();
    glm::vec3 spawnPos = camCtrl ? camCtrl->getDefaultPosition()
                                 : (camera->getPosition() - glm::vec3(0.0f, 0.0f, 5.0f));
    if (spawnSnapToGround && renderer->getTerrainManager()) {
        auto terrainH = renderer->getTerrainManager()->getHeightAt(spawnPos.x, spawnPos.y);
        if (terrainH) {
            spawnPos.z = *terrainH + 0.1f;
        }
    }
    uint32_t instanceId = charRenderer->createInstance(1, spawnPos,
        glm::vec3(0.0f), 1.0f);  // Scale 1.0 = normal WoW character size

    if (instanceId > 0) {
	        // Set up third-person follow
	        renderer->getCharacterPosition() = spawnPos;
	        renderer->setCharacterFollow(instanceId);

	        // Build default geosets for the active character via AppearanceComposer
	        uint8_t hairStyleId = 0;
	        uint8_t facialId = 0;
	        uint8_t raceId = 0;
	        uint8_t sexId = 0;
	        if (gameHandler) {
	            if (const game::Character* ch = gameHandler->getActiveCharacter()) {
	                hairStyleId = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
	                facialId = ch->facialFeatures;
	                raceId = static_cast<uint8_t>(ch->race);
	                sexId = static_cast<uint8_t>(ch->gender);
	            }
	        }
	        auto activeGeosets = appearanceComposer_
	            ? appearanceComposer_->buildDefaultPlayerGeosets(raceId, sexId, hairStyleId, facialId)
	            : std::unordered_set<uint16_t>{};
	        charRenderer->setActiveGeosets(instanceId, activeGeosets);

        // Play idle animation
        charRenderer->playAnimation(instanceId, rendering::anim::STAND, true);
        LOG_INFO("Spawned player character at (",
                static_cast<int>(spawnPos.x), ", ",
                static_cast<int>(spawnPos.y), ", ",
                static_cast<int>(spawnPos.z), ")");
        playerCharacterSpawned = true;

        // Set voice profile to match character race/gender
        if (auto* asm_ = audioCoordinator_ ? audioCoordinator_->getActivitySoundManager() : nullptr) {
            const char* raceFolder = "Human";
            const char* raceBase = "Human";
            switch (playerRace_) {
                case game::Race::HUMAN:    raceFolder = "Human"; raceBase = "Human"; break;
                case game::Race::ORC:      raceFolder = "Orc"; raceBase = "Orc"; break;
                case game::Race::DWARF:    raceFolder = "Dwarf"; raceBase = "Dwarf"; break;
                case game::Race::NIGHT_ELF: raceFolder = "NightElf"; raceBase = "NightElf"; break;
                case game::Race::UNDEAD:    raceFolder = "Scourge"; raceBase = "Scourge"; break;
                case game::Race::TAUREN:    raceFolder = "Tauren"; raceBase = "Tauren"; break;
                case game::Race::GNOME:     raceFolder = "Gnome"; raceBase = "Gnome"; break;
                case game::Race::TROLL:     raceFolder = "Troll"; raceBase = "Troll"; break;
                case game::Race::BLOOD_ELF: raceFolder = "BloodElf"; raceBase = "BloodElf"; break;
                case game::Race::DRAENEI:   raceFolder = "Draenei"; raceBase = "Draenei"; break;
                default: break;
            }
            bool useFemaleVoice = (playerGender_ == game::Gender::FEMALE);
            if (playerGender_ == game::Gender::NONBINARY && gameHandler) {
                if (const game::Character* ch = gameHandler->getActiveCharacter()) {
                    useFemaleVoice = ch->useFemaleModel;
                }
            }
            asm_->setCharacterVoiceProfile(std::string(raceFolder), std::string(raceBase), !useFemaleVoice);
        }

        // Track which character's appearance this instance represents so we can
        // respawn if the user logs into a different character without restarting.
        spawnedPlayerGuid_ = gameHandler ? gameHandler->getActiveCharacterGuid() : 0;
        spawnedAppearanceBytes_ = 0;
        spawnedFacialFeatures_ = 0;
        if (gameHandler) {
            if (const game::Character* ch = gameHandler->getActiveCharacter()) {
                spawnedAppearanceBytes_ = ch->appearanceBytes;
                spawnedFacialFeatures_ = ch->facialFeatures;
            }
        }

        // Set up camera controller for first-person player hiding
        if (renderer->getCameraController()) {
            renderer->getCameraController()->setCharacterRenderer(charRenderer, instanceId);
        }

        // Load equipped weapons (sword + shield)
        if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons();
    }
}

void Application::buildFactionHostilityMap(uint8_t playerRace) {
    if (!assetManager || !assetManager->isInitialized() || !gameHandler) return;

    auto ftDbc = assetManager->loadDBC("FactionTemplate.dbc");
    auto fDbc = assetManager->loadDBC("Faction.dbc");
    if (!ftDbc || !ftDbc->isLoaded()) return;

    // Race enum → race mask bit: race 1=0x1, 2=0x2, 3=0x4, 4=0x8, 5=0x10, 6=0x20, 7=0x40, 8=0x80, 10=0x200, 11=0x400
    uint32_t playerRaceMask = 0;
    if (playerRace >= 1 && playerRace <= 8) {
        playerRaceMask = 1u << (playerRace - 1);
    } else if (playerRace == 10) {
        playerRaceMask = 0x200;  // Blood Elf
    } else if (playerRace == 11) {
        playerRaceMask = 0x400;  // Draenei
    }

    // Race → player faction template ID
    // Human=1, Orc=2, Dwarf=3, NightElf=4, Undead=5, Tauren=6, Gnome=115, Troll=116, BloodElf=1610, Draenei=1629
    uint32_t playerFtId = 0;
    switch (playerRace) {
        case 1: playerFtId = 1; break;     // Human
        case 2: playerFtId = 2; break;     // Orc
        case 3: playerFtId = 3; break;     // Dwarf
        case 4: playerFtId = 4; break;     // Night Elf
        case 5: playerFtId = 5; break;     // Undead
        case 6: playerFtId = 6; break;     // Tauren
        case 7: playerFtId = 115; break;   // Gnome
        case 8: playerFtId = 116; break;   // Troll
        case 10: playerFtId = 1610; break; // Blood Elf
        case 11: playerFtId = 1629; break; // Draenei
        default: playerFtId = 1; break;
    }

    // Build set of hostile parent faction IDs from Faction.dbc base reputation
    const auto* facL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Faction") : nullptr;
    const auto* ftL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("FactionTemplate") : nullptr;
    std::unordered_set<uint32_t> hostileParentFactions;
    if (fDbc && fDbc->isLoaded()) {
        const uint32_t facID = facL ? (*facL)["ID"] : 0;
        const uint32_t facRaceMask0 = facL ? (*facL)["ReputationRaceMask0"] : 2;
        const uint32_t facBase0 = facL ? (*facL)["ReputationBase0"] : 10;
        for (uint32_t i = 0; i < fDbc->getRecordCount(); i++) {
            uint32_t factionId = fDbc->getUInt32(i, facID);
            for (int slot = 0; slot < 4; slot++) {
                uint32_t raceMask = fDbc->getUInt32(i, facRaceMask0 + slot);
                if (raceMask & playerRaceMask) {
                    int32_t baseRep = fDbc->getInt32(i, facBase0 + slot);
                    if (baseRep < 0) {
                        hostileParentFactions.insert(factionId);
                    }
                    break;
                }
            }
        }
        LOG_INFO("Faction.dbc: ", hostileParentFactions.size(), " factions hostile to race ", static_cast<int>(playerRace));
    }

    // Get player faction template data
    const uint32_t ftID = ftL ? (*ftL)["ID"] : 0;
    const uint32_t ftFaction = ftL ? (*ftL)["Faction"] : 1;
    const uint32_t ftFG = ftL ? (*ftL)["FactionGroup"] : 3;
    const uint32_t ftFriend = ftL ? (*ftL)["FriendGroup"] : 4;
    const uint32_t ftEnemy = ftL ? (*ftL)["EnemyGroup"] : 5;
    const uint32_t ftEnemy0 = ftL ? (*ftL)["Enemy0"] : 6;
    uint32_t playerFriendGroup = 0;
    uint32_t playerEnemyGroup = 0;
    uint32_t playerFactionId = 0;
    for (uint32_t i = 0; i < ftDbc->getRecordCount(); i++) {
        if (ftDbc->getUInt32(i, ftID) == playerFtId) {
            playerFriendGroup = ftDbc->getUInt32(i, ftFriend) | ftDbc->getUInt32(i, ftFG);
            playerEnemyGroup = ftDbc->getUInt32(i, ftEnemy);
            playerFactionId = ftDbc->getUInt32(i, ftFaction);
            break;
        }
    }

    // Build hostility map for each faction template
    std::unordered_map<uint32_t, bool> factionMap;
    for (uint32_t i = 0; i < ftDbc->getRecordCount(); i++) {
        uint32_t id = ftDbc->getUInt32(i, ftID);
        uint32_t parentFaction = ftDbc->getUInt32(i, ftFaction);
        uint32_t factionGroup = ftDbc->getUInt32(i, ftFG);
        uint32_t friendGroup = ftDbc->getUInt32(i, ftFriend);
        uint32_t enemyGroup = ftDbc->getUInt32(i, ftEnemy);

        // 1. Symmetric group check
        bool hostile = (enemyGroup & playerFriendGroup) != 0
                    || (factionGroup & playerEnemyGroup) != 0;

        // 2. Monster factionGroup bit (8)
        if (!hostile && (factionGroup & 8) != 0) {
            hostile = true;
        }

        // 3. Individual enemy faction IDs
        if (!hostile && playerFactionId > 0) {
            for (uint32_t e = ftEnemy0; e <= ftEnemy0 + 3; e++) {
                if (ftDbc->getUInt32(i, e) == playerFactionId) {
                    hostile = true;
                    break;
                }
            }
        }

        // 4. Parent faction base reputation check (Faction.dbc)
        if (!hostile && parentFaction > 0) {
            if (hostileParentFactions.count(parentFaction)) {
                hostile = true;
            }
        }

        // 5. If explicitly friendly (friendGroup includes player), override to non-hostile
        if (hostile && (friendGroup & playerFriendGroup) != 0) {
            hostile = false;
        }

        factionMap[id] = hostile;
    }

    uint32_t hostileCount = 0;
    for (const auto& [fid, h] : factionMap) { if (h) hostileCount++; }
    gameHandler->setFactionHostileMap(std::move(factionMap));
    LOG_INFO("Faction hostility for race ", static_cast<int>(playerRace), " (FT ", playerFtId, "): ",
        hostileCount, "/", ftDbc->getRecordCount(),
        " hostile (friendGroup=0x", std::hex, playerFriendGroup, ", enemyGroup=0x", playerEnemyGroup, std::dec, ")");
}

// Render bounds/position queries — delegates to EntitySpawner
bool Application::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (entitySpawner_) return entitySpawner_->getRenderBoundsForGuid(guid, outCenter, outRadius);
    return false;
}

bool Application::getRenderFootZForGuid(uint64_t guid, float& outFootZ) const {
    if (entitySpawner_) return entitySpawner_->getRenderFootZForGuid(guid, outFootZ);
    return false;
}

bool Application::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    if (entitySpawner_) return entitySpawner_->getRenderPositionForGuid(guid, outPos);
    return false;
}

void Application::loadQuestMarkerModels() {
    if (!assetManager || !renderer) return;

    // Quest markers are billboard sprites; the renderer's QuestMarkerRenderer handles
    // texture loading and pipeline setup during world initialization.
    // Calling initialize() here is a no-op if already done; harmless if called early.
    if (auto* qmr = renderer->getQuestMarkerRenderer()) {
        if (auto* vkCtx = renderer->getVkContext()) {
            VkDescriptorSetLayout pfl = renderer->getPerFrameSetLayout();
            if (pfl != VK_NULL_HANDLE) {
                if (!qmr->initialize(vkCtx, pfl, assetManager.get()))
                    LOG_WARNING("Quest marker renderer re-init failed (non-fatal)");
            }
        }
    }
}

void Application::updateQuestMarkers() {
    if (!gameHandler || !renderer) {
        return;
    }

    auto* questMarkerRenderer = renderer->getQuestMarkerRenderer();
    if (!questMarkerRenderer) {
        static bool logged = false;
        if (!logged) {
            LOG_WARNING("QuestMarkerRenderer not available!");
            logged = true;
        }
        return;
    }

    const auto& questStatuses = gameHandler->getNpcQuestStatuses();

    // Clear all markers (we'll re-add active ones)
    questMarkerRenderer->clear();

    static bool firstRun = true;
    int markersAdded = 0;

    // Add markers for NPCs with quest status
    for (const auto& [guid, status] : questStatuses) {
        // Determine marker type
        int markerType = -1;  // -1 = no marker

        using game::QuestGiverStatus;
        float markerGrayscale = 0.0f;  // 0 = colour, 1 = grey (trivial quests)
        switch (status) {
            case QuestGiverStatus::AVAILABLE:
                markerType = 0;  // Yellow !
                break;
            case QuestGiverStatus::AVAILABLE_LOW:
                markerType = 0;  // Grey ! (same texture, desaturated in shader)
                markerGrayscale = 1.0f;
                break;
            case QuestGiverStatus::REWARD:
            case QuestGiverStatus::REWARD_REP:
                markerType = 1;  // Yellow ?
                break;
            case QuestGiverStatus::INCOMPLETE:
                markerType = 2;  // Grey ?
                break;
            default:
                break;
        }

        if (markerType < 0) continue;

        // Get NPC entity position
        auto entity = gameHandler->getEntityManager().getEntity(guid);
        if (!entity) continue;
        if (entity->getType() == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            std::string name = unit->getName();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (name.find("spirit healer") != std::string::npos ||
                name.find("spirit guide") != std::string::npos) {
                continue; // Spirit healers/guides use their own white visual cue.
            }
        }

        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
        glm::vec3 renderPos = coords::canonicalToRender(canonical);

        // Get NPC bounding height for proper marker positioning
        glm::vec3 boundsCenter;
        float boundsRadius = 0.0f;
        float boundingHeight = 2.0f;  // Default
        if (getRenderBoundsForGuid(guid, boundsCenter, boundsRadius)) {
            boundingHeight = boundsRadius * 2.0f;
        }

        // Set the marker (renderer will handle positioning, bob, glow, etc.)
        questMarkerRenderer->setMarker(guid, renderPos, markerType, boundingHeight, markerGrayscale);
        markersAdded++;
    }

    if (firstRun && markersAdded > 0) {
        LOG_DEBUG("Quest markers: Added ", markersAdded, " markers on first run");
        firstRun = false;
    }
}

void Application::setupTestTransport() {
    if (!entitySpawner_) return;
    if (entitySpawner_->isTestTransportSetup()) return;
    if (!gameHandler || !renderer || !assetManager) return;

    auto* transportManager = gameHandler->getTransportManager();
    auto* wmoRenderer = renderer->getWMORenderer();
    if (!transportManager || !wmoRenderer) return;

    LOG_INFO("========================================");
    LOG_INFO("   SETTING UP TEST TRANSPORT");
    LOG_INFO("========================================");

    // Connect transport manager to WMO renderer
    transportManager->setWMORenderer(wmoRenderer);

    // Connect WMORenderer to M2Renderer (for hierarchical transforms: doodads following WMO parents)
    if (renderer->getM2Renderer()) {
        wmoRenderer->setM2Renderer(renderer->getM2Renderer());
        LOG_INFO("WMORenderer connected to M2Renderer for test transport doodad transforms");
    }

    // Define a simple circular path around Stormwind harbor (canonical coordinates)
    // These coordinates are approximate - adjust based on actual harbor layout
    std::vector<glm::vec3> harborPath = {
        {-8833.0f, 628.0f, 94.0f},   // Start point (Stormwind harbor)
        {-8900.0f, 650.0f, 94.0f},   // Move west
        {-8950.0f, 700.0f, 94.0f},   // Northwest
        {-8950.0f, 780.0f, 94.0f},   // North
        {-8900.0f, 830.0f, 94.0f},   // Northeast
        {-8833.0f, 850.0f, 94.0f},   // East
        {-8766.0f, 830.0f, 94.0f},   // Southeast
        {-8716.0f, 780.0f, 94.0f},   // South
        {-8716.0f, 700.0f, 94.0f},   // Southwest
        {-8766.0f, 650.0f, 94.0f},   // Back to start direction
    };

    // Register the path with transport manager
    uint32_t pathId = 1;
    float speed = 12.0f;  // 12 units/sec (slower than taxi for a leisurely boat ride)
    transportManager->loadPathFromNodes(pathId, harborPath, true, speed);
    LOG_INFO("Registered transport path ", pathId, " with ", harborPath.size(), " waypoints, speed=", speed);

    // Try transport WMOs in manifest-backed paths first.
    std::vector<std::string> transportCandidates = {
        "World\\wmo\\transports\\transport_ship\\transportship.wmo",
        "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo",
        "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo",
        "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo",
        // Legacy fallbacks
        "Transports\\Transportship\\Transportship.wmo",
        "Transports\\Boat\\Boat.wmo",
    };

    std::string transportWmoPath;
    std::vector<uint8_t> wmoData;
    for (const auto& candidate : transportCandidates) {
        wmoData = assetManager->readFile(candidate);
        if (!wmoData.empty()) {
            transportWmoPath = candidate;
            break;
        }
    }

    if (wmoData.empty()) {
        LOG_WARNING("No transport WMO found - test transport disabled");
        LOG_INFO("Expected under World\\wmo\\transports\\...");
        return;
    }

    LOG_INFO("Using transport WMO: ", transportWmoPath);

    // Load WMO model
    pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
    LOG_INFO("Transport WMO root loaded: ", transportWmoPath, " nGroups=", wmoModel.nGroups);

    // Load WMO groups
    int loadedGroups = 0;
    if (wmoModel.nGroups > 0) {
        std::string basePath = transportWmoPath.substr(0, transportWmoPath.size() - 4);

        for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
            char groupSuffix[16];
            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
            std::string groupPath = basePath + groupSuffix;
            std::vector<uint8_t> groupData = assetManager->readFile(groupPath);

            if (!groupData.empty()) {
                pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                loadedGroups++;
            } else {
                LOG_WARNING("  Failed to load WMO group ", gi, " for: ", basePath);
            }
        }
    }

    if (loadedGroups == 0 && wmoModel.nGroups > 0) {
        LOG_WARNING("Failed to load any WMO groups for transport");
        return;
    }

    // Load WMO into renderer
    uint32_t wmoModelId = 99999;  // Use high ID to avoid conflicts
    if (!wmoRenderer->loadModel(wmoModel, wmoModelId)) {
        LOG_WARNING("Failed to load transport WMO model into renderer");
        return;
    }

    // Create WMO instance at first waypoint (convert canonical to render coords)
    glm::vec3 startCanonical = harborPath[0];
    glm::vec3 startRender = core::coords::canonicalToRender(startCanonical);

    uint32_t wmoInstanceId = wmoRenderer->createInstance(wmoModelId, startRender,
                                                          glm::vec3(0.0f, 0.0f, 0.0f), 1.0f);

    if (wmoInstanceId == 0) {
        LOG_WARNING("Failed to create transport WMO instance");
        return;
    }

    // Register transport with transport manager
    uint64_t transportGuid = 0x1000000000000001ULL;  // Fake GUID for test
    transportManager->registerTransport(transportGuid, wmoInstanceId, pathId, startCanonical);

    // Optional: Set deck bounds (rough estimate for a ship deck)
    transportManager->setDeckBounds(transportGuid,
                                    glm::vec3(-15.0f, -30.0f, 0.0f),
                                    glm::vec3(15.0f, 30.0f, 10.0f));

    entitySpawner_->setTestTransportSetup(true);
    LOG_INFO("========================================");
    LOG_INFO("Test transport registered:");
    LOG_INFO("  GUID: 0x", std::hex, transportGuid, std::dec);
    LOG_INFO("  WMO Instance: ", wmoInstanceId);
    LOG_INFO("  Path: ", pathId, " (", harborPath.size(), " waypoints)");
    LOG_INFO("  Speed: ", speed, " units/sec");
    LOG_INFO("========================================");
    LOG_INFO("");
    LOG_INFO("To board the transport, use console command:");
    LOG_INFO("  /transport board");
    LOG_INFO("To disembark:");
    LOG_INFO("  /transport leave");
    LOG_INFO("========================================");
}

} // namespace core
} // namespace wowee
