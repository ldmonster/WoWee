#include "core/application.hpp"
#include "core/coordinates.hpp"
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
#include "addons/addon_manager.hpp"
#include <imgui.h>
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/wdt_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "ui/ui_manager.hpp"
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

// Default (bare) geoset IDs per equipment group.
// Each group's base is groupNumber * 100; variant 01 is typically bare/default.
constexpr uint16_t kGeosetDefaultConnector = 101;   // Group  1: default hair connector
constexpr uint16_t kGeosetBareForearms     = 401;   // Group  4: no gloves
constexpr uint16_t kGeosetBareShins        = 503;   // Group  5: no boots
constexpr uint16_t kGeosetDefaultEars      = 702;   // Group  7: ears
constexpr uint16_t kGeosetBareSleeves      = 801;   // Group  8: no chest armor sleeves
constexpr uint16_t kGeosetDefaultKneepads  = 902;   // Group  9: kneepads
constexpr uint16_t kGeosetDefaultTabard    = 1201;  // Group 12: tabard base
constexpr uint16_t kGeosetBarePants        = 1301;  // Group 13: no leggings
constexpr uint16_t kGeosetNoCape           = 1501;  // Group 15: no cape
constexpr uint16_t kGeosetWithCape         = 1502;  // Group 15: with cape
constexpr uint16_t kGeosetBareFeet         = 2002;  // Group 20: bare feet
} // namespace


const char* Application::mapDisplayName(uint32_t mapId) {
    // Friendly display names for the loading screen
    switch (mapId) {
        case 0: return "Eastern Kingdoms";
        case 1: return "Kalimdor";
        case 530: return "Outland";
        case 571: return "Northrend";
        default: return nullptr;
    }
}

const char* Application::mapIdToName(uint32_t mapId) {
    // Fallback when Map.dbc is unavailable. Names must match WDT directory names
    // (case-insensitive — AssetManager lowercases all paths).
    switch (mapId) {
        // Continents
        case 0: return "Azeroth";
        case 1: return "Kalimdor";
        case 530: return "Expansion01";
        case 571: return "Northrend";
        // Classic dungeons/raids
        case 30: return "PVPZone01";
        case 33: return "Shadowfang";
        case 34: return "StormwindJail";
        case 36: return "DeadminesInstance";
        case 43: return "WailingCaverns";
        case 47: return "RazserfenKraulInstance";
        case 48: return "Blackfathom";
        case 70: return "Uldaman";
        case 90: return "GnomeragonInstance";
        case 109: return "SunkenTemple";
        case 129: return "RazorfenDowns";
        case 189: return "MonasteryInstances";
        case 209: return "TanarisInstance";
        case 229: return "BlackRockSpire";
        case 230: return "BlackrockDepths";
        case 249: return "OnyxiaLairInstance";
        case 289: return "ScholomanceInstance";
        case 309: return "Zul'Gurub";
        case 329: return "Stratholme";
        case 349: return "Mauradon";
        case 369: return "DeeprunTram";
        case 389: return "OrgrimmarInstance";
        case 409: return "MoltenCore";
        case 429: return "DireMaul";
        case 469: return "BlackwingLair";
        case 489: return "PVPZone03";
        case 509: return "AhnQiraj";
        case 529: return "PVPZone04";
        case 531: return "AhnQirajTemple";
        case 533: return "Stratholme Raid";
        // TBC
        case 532: return "Karazahn";
        case 534: return "HyjalPast";
        case 540: return "HellfireMilitary";
        case 542: return "HellfireDemon";
        case 543: return "HellfireRampart";
        case 544: return "HellfireRaid";
        case 545: return "CoilfangPumping";
        case 546: return "CoilfangMarsh";
        case 547: return "CoilfangDraenei";
        case 548: return "CoilfangRaid";
        case 550: return "TempestKeepRaid";
        case 552: return "TempestKeepArcane";
        case 553: return "TempestKeepAtrium";
        case 554: return "TempestKeepFactory";
        case 555: return "AuchindounShadow";
        case 556: return "AuchindounDraenei";
        case 557: return "AuchindounEthereal";
        case 558: return "AuchindounDemon";
        case 560: return "HillsbradPast";
        case 564: return "BlackTemple";
        case 565: return "GruulsLair";
        case 566: return "PVPZone05";
        case 568: return "ZulAman";
        case 580: return "SunwellPlateau";
        case 585: return "Sunwell5ManFix";
        // WotLK
        case 574: return "Valgarde70";
        case 575: return "UtgardePinnacle";
        case 576: return "Nexus70";
        case 578: return "Nexus80";
        case 595: return "StratholmeCOT";
        case 599: return "Ulduar70";
        case 600: return "Ulduar80";
        case 601: return "DrakTheronKeep";
        case 602: return "GunDrak";
        case 603: return "UlduarRaid";
        case 608: return "DalaranPrison";
        case 615: return "ChamberOfAspectsBlack";
        case 617: return "DeathKnightStart";
        case 619: return "Azjol_Uppercity";
        case 624: return "WintergraspRaid";
        case 631: return "IcecrownCitadel";
        case 632: return "IcecrownCitadel5Man";
        case 649: return "ArgentTournamentRaid";
        case 650: return "ArgentTournamentDungeon";
        case 658: return "QuarryOfTears";
        case 668: return "HallsOfReflection";
        case 724: return "ChamberOfAspectsRed";
        default: return "";
    }
}

std::string Application::getPlayerModelPath() const {
    return game::getPlayerModelPath(playerRace_, playerGender_);
}


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
        buildCreatureDisplayLookups();

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

        // Start background preload for last-played character's world.
        // Warms the file cache so terrain tile loading is faster at Enter World.
        {
            auto lastWorld = loadLastWorldInfo();
            if (lastWorld.valid) {
                startWorldPreload(lastWorld.mapId, lastWorld.mapName, lastWorld.x, lastWorld.y);
            }
        }

        // Initialize addon system
        addonManager_ = std::make_unique<addons::AddonManager>();
        if (addonManager_->initialize(gameHandler.get())) {
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
                        // Vulkan viewport set in command buffer, not globally
                        if (renderer && renderer->getCamera()) {
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
    LOG_WARNING("Shutting down application...");

    // Hide the window immediately so the OS doesn't think the app is frozen
    // during the (potentially slow) resource cleanup below.
    if (window && window->getSDLWindow()) {
        SDL_HideWindow(window->getSDLWindow());
    }

    // Stop background world preloader before destroying AssetManager
    cancelWorldPreload();

    // Save floor cache before renderer is destroyed
    if (renderer && renderer->getWMORenderer()) {
        size_t cacheSize = renderer->getWMORenderer()->getFloorCacheSize();
        if (cacheSize > 0) {
            LOG_WARNING("Saving WMO floor cache (", cacheSize, " entries)...");
            renderer->getWMORenderer()->saveFloorCache();
            LOG_WARNING("Floor cache saved.");
        }
    }

    // Explicitly shut down the renderer before destroying it — this ensures
    // all sub-renderers free their VMA allocations in the correct order,
    // before VkContext::shutdown() calls vmaDestroyAllocator().
    LOG_WARNING("Shutting down renderer...");
    if (renderer) {
        renderer->shutdown();
    }
    LOG_WARNING("Renderer shutdown complete, resetting...");
    renderer.reset();

    LOG_WARNING("Resetting world...");
    world.reset();
    LOG_WARNING("Resetting gameHandler...");
    gameHandler.reset();
    gameServices_ = {};
    LOG_WARNING("Resetting authHandler...");
    authHandler.reset();
    LOG_WARNING("Resetting assetManager...");
    assetManager.reset();
    LOG_WARNING("Resetting uiManager...");
    uiManager.reset();
    LOG_WARNING("Resetting window...");
    window.reset();

    running = false;
    LOG_WARNING("Application shutdown complete");
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
            weaponsSheathed_ = false;
            wasAutoAttacking_ = false;
            loadedMapId_ = 0xFFFFFFFF;
            spawnedPlayerGuid_ = 0;
            spawnedAppearanceBytes_ = 0;
            spawnedFacialFeatures_ = 0;
            if (renderer && renderer->getCharacterRenderer()) {
                uint32_t oldInst = renderer->getCharacterInstanceId();
                if (oldInst > 0) {
                    renderer->setCharacterFollow(0);
                    renderer->clearMount();
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
                        gameHandler->setStandState(0); // CMSG_STAND_STATE_CHANGE(STAND)
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
                gameHandler->setMeleeSwingCallback([this]() {
                    if (renderer) {
                        renderer->triggerMeleeSwing();
                    }
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
    mapNameCacheLoaded_ = false;
    mapNameById_.clear();

    // Reset game handler DBC caches so they reload from new expansion data
    if (gameHandler) {
        gameHandler->resetDbcCaches();
    }

    // Rebuild creature display lookups with the new expansion's DBC layout
    creatureLookupsBuilt_ = false;
    displayDataMap_.clear();
    humanoidExtraMap_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    nonRenderableCreatureDisplayIds_.clear();
    buildCreatureDisplayLookups();
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
    weaponsSheathed_ = false;
    wasAutoAttacking_ = false;
    loadedMapId_ = 0xFFFFFFFF;
    lastTaxiFlight_ = false;
    taxiLandingClampTimer_ = 0.0f;
    worldEntryMovementGraceTimer_ = 0.0f;
    facingSendCooldown_ = 0.0f;
    lastSentCanonicalYaw_ = 1000.0f;
    taxiStreamCooldown_ = 0.0f;
    idleYawned_ = false;

    // --- Charge state ---
    chargeActive_ = false;
    chargeTimer_ = 0.0f;
    chargeDuration_ = 0.0f;
    chargeTargetGuid_ = 0;

    // --- Player identity ---
    spawnedPlayerGuid_ = 0;
    spawnedAppearanceBytes_ = 0;
    spawnedFacialFeatures_ = 0;

    // --- Mount state ---
    mountInstanceId_ = 0;
    mountModelId_ = 0;
    pendingMountDisplayId_ = 0;

    // --- Creature instance tracking ---
    creatureInstances_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    creatureWeaponsAttached_.clear();
    creatureWeaponAttachAttempts_.clear();
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    deadCreatureGuids_.clear();
    nonRenderableCreatureDisplayIds_.clear();
    creaturePermanentFailureGuids_.clear();
    modelIdIsWolfLike_.clear();
    displayIdTexturesApplied_.clear();
    charSectionsCache_.clear();
    charSectionsCacheBuilt_ = false;

    // Wait for any in-flight async creature loads before clearing state
    for (auto& load : asyncCreatureLoads_) {
        if (load.future.valid()) load.future.wait();
    }
    asyncCreatureLoads_.clear();
    asyncCreatureDisplayLoads_.clear();

    // --- Creature spawn queues ---
    pendingCreatureSpawns_.clear();
    pendingCreatureSpawnGuids_.clear();
    creatureSpawnRetryCounts_.clear();

    // --- Player instance tracking ---
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    pendingOnlinePlayerEquipment_.clear();
    deferredEquipmentQueue_.clear();
    pendingPlayerSpawns_.clear();
    pendingPlayerSpawnGuids_.clear();

    // --- GameObject instance tracking ---
    gameObjectInstances_.clear();
    pendingGameObjectSpawns_.clear();
    pendingTransportMoves_.clear();
    pendingTransportRegistrations_.clear();
    pendingTransportDoodadBatches_.clear();

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
        renderer->clearMount();
        renderer->setCharacterFollow(0);
        if (auto* music = renderer->getMusicManager()) {
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
                if (autoAttacking && !wasAutoAttacking_ && weaponsSheathed_) {
                    weaponsSheathed_ = false;
                    loadEquippedWeapons();
                }
                wasAutoAttacking_ = autoAttacking;
            }

            // Toggle weapon sheathe state with Z (ignored while UI captures keyboard).
            inGameStep = "weapon-toggle input";
            updateCheckpoint = "in_game: weapon-toggle input";
            {
                const bool uiWantsKeyboard = ImGui::GetIO().WantCaptureKeyboard;
                auto& input = Input::getInstance();
                if (!uiWantsKeyboard && input.isKeyJustPressed(SDL_SCANCODE_Z)) {
                    weaponsSheathed_ = !weaponsSheathed_;
                    loadEquippedWeapons();
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
                processPlayerSpawnQueue();
                processCreatureSpawnQueue();
                processAsyncNpcCompositeResults();
                processDeferredEquipmentQueue();
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
                        if (creatureInstances_.count(guid) || pendingCreatureSpawnGuids_.count(guid)) continue;

                        if (havePlayerPos) {
                            glm::vec3 pos(unit->getX(), unit->getY(), unit->getZ());
                            glm::vec3 delta = pos - playerPos;
                            float distSq = glm::dot(delta, delta);
                            if (distSq > kResyncRadiusSq) continue;
                        }

                        PendingCreatureSpawn retrySpawn{};
                        retrySpawn.guid = guid;
                        retrySpawn.displayId = unit->getDisplayId();
                        retrySpawn.x = unit->getX();
                        retrySpawn.y = unit->getY();
                        retrySpawn.z = unit->getZ();
                        retrySpawn.orientation = unit->getOrientation();
                        {
                            using game::fieldIndex; using game::UF;
                            uint16_t si = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
                            if (si != 0xFFFF) {
                                uint32_t raw = unit->getField(si);
                                if (raw != 0) {
                                    float s2 = 1.0f;
                                    std::memcpy(&s2, &raw, sizeof(float));
                                    if (s2 > 0.01f && s2 < 100.0f) retrySpawn.scale = s2;
                                }
                            }
                        }
                        pendingCreatureSpawns_.push_back(retrySpawn);
                        pendingCreatureSpawnGuids_.insert(guid);
                    }
                }
            }

            inGameStep = "gameobject/transport queues";
            updateCheckpoint = "in_game: gameobject/transport queues";
            runInGameStage("gameobject/transport queues", [&] {
                processGameObjectSpawnQueue();
                processPendingTransportRegistrations();
                processPendingTransportDoodads();
            });
            inGameStep = "pending mount";
            updateCheckpoint = "in_game: pending mount";
            runInGameStage("processPendingMount", [&] {
                processPendingMount();
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
                                    renderer->setMountPitchRoll(pitchRad, 0.0f);
                                }
                            }
                        }
                    } else if (gameHandler->isMounted()) {
                        // Reset mount pitch when not flying
                        renderer->setMountPitchRoll(0.0f, 0.0f);
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
                if (worldEntryMovementGraceTimer_ > 0.0f) {
                    worldEntryMovementGraceTimer_ -= deltaTime;
                    // Clear stale movement from before teleport each frame
                    // until grace period expires (keys may still be held)
                    if (renderer && renderer->getCameraController())
                        renderer->getCameraController()->clearMovementInputs();
                }
                // Hearth teleport: keep player frozen until terrain loads at destination
                if (hearthTeleportPending_ && renderer && renderer->getTerrainManager()) {
                    hearthTeleportTimer_ -= deltaTime;
                    auto terrainH = renderer->getTerrainManager()->getHeightAt(
                        hearthTeleportPos_.x, hearthTeleportPos_.y);
                    if (terrainH || hearthTeleportTimer_ <= 0.0f) {
                        // Terrain loaded (or timeout) — snap to floor and release
                        if (terrainH) {
                            hearthTeleportPos_.z = *terrainH + 0.5f;
                            renderer->getCameraController()->teleportTo(hearthTeleportPos_);
                        }
                        renderer->getCameraController()->setExternalFollow(false);
                        worldEntryMovementGraceTimer_ = 1.0f;
                        hearthTeleportPending_ = false;
                        LOG_INFO("Unstuck hearth: terrain loaded, player released",
                                 terrainH ? "" : " (timeout)");
                    }
                }
                if (renderer && renderer->getCameraController()) {
                const bool externallyDrivenMotion = onTaxi || onWMOTransport || chargeActive_;
                // Keep physics frozen (externalFollow) during landing clamp when terrain
                // hasn't loaded yet — prevents gravity from pulling player through void.
                bool hearthFreeze = hearthTeleportPending_;
                bool landingClampActive = !onTaxi && taxiLandingClampTimer_ > 0.0f &&
                                          worldEntryMovementGraceTimer_ <= 0.0f &&
                                          !gameHandler->isMounted();
                renderer->getCameraController()->setExternalFollow(externallyDrivenMotion || landingClampActive || hearthFreeze);
                renderer->getCameraController()->setExternalMoving(externallyDrivenMotion);
                if (externallyDrivenMotion) {
                    // Drop any stale local movement toggles while server drives taxi motion.
                    renderer->getCameraController()->clearMovementInputs();
                    taxiLandingClampTimer_ = 0.0f;
                }
                if (lastTaxiFlight_ && !onTaxi) {
                    renderer->getCameraController()->clearMovementInputs();
                    // Keep clamping until terrain loads at landing position.
                    // Timer only counts down once a valid floor is found.
                    taxiLandingClampTimer_ = 2.0f;
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
                            taxiLandingClampTimer_ -= deltaTime;
                        }
                        // No floor found: don't decrement timer, keep player frozen until terrain loads
                    }
                }
                bool idleOrbit = renderer->getCameraController()->isIdleOrbit();
                if (idleOrbit && !idleYawned_ && renderer) {
                    renderer->playEmote("yawn");
                    idleYawned_ = true;
                } else if (!idleOrbit) {
                    idleYawned_ = false;
                }
                }
                if (renderer) {
                    renderer->setTaxiFlight(onTaxi);
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
                lastTaxiFlight_ = onTaxi;

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
                } else if (chargeActive_) {
                    // Warrior Charge: lerp position from start to end using smoothstep
                    chargeTimer_ += deltaTime;
                    float t = std::min(chargeTimer_ / chargeDuration_, 1.0f);
                    // smoothstep for natural acceleration/deceleration
                    float s = t * t * (3.0f - 2.0f * t);
                    glm::vec3 renderPos = chargeStartPos_ + (chargeEndPos_ - chargeStartPos_) * s;
                    renderer->getCharacterPosition() = renderPos;

                    // Keep facing toward target and emit charge effect
                    glm::vec3 dir = chargeEndPos_ - chargeStartPos_;
                    float dirLenSq = glm::dot(dir, dir);
                    if (dirLenSq > 1e-4f) {
                        dir *= glm::inversesqrt(dirLenSq);
                        float yawDeg = glm::degrees(std::atan2(dir.x, dir.y));
                        renderer->setCharacterYaw(yawDeg);
                        renderer->emitChargeEffect(renderPos, dir);
                    }

                    // Sync to game handler
                    glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);

                    // Update camera follow target
                    if (renderer->getCameraController()) {
                        glm::vec3* followTarget = renderer->getCameraController()->getFollowTargetMutable();
                        if (followTarget) {
                            *followTarget = renderPos;
                        }
                    }

                    // Charge complete
                    if (t >= 1.0f) {
                        chargeActive_ = false;
                        renderer->setCharging(false);
                        renderer->stopChargeEffect();
                        renderer->getCameraController()->setExternalFollow(false);
                        renderer->getCameraController()->setExternalMoving(false);

                        // Snap to melee range of target's CURRENT position (it may have moved)
                        if (chargeTargetGuid_ != 0) {
                            auto targetEntity = gameHandler->getEntityManager().getEntity(chargeTargetGuid_);
                            if (targetEntity) {
                                glm::vec3 targetCanonical(targetEntity->getX(), targetEntity->getY(), targetEntity->getZ());
                                glm::vec3 targetRender = core::coords::canonicalToRender(targetCanonical);
                                glm::vec3 toTarget = targetRender - renderPos;
                                float dSq = glm::dot(toTarget, toTarget);
                                if (dSq > 2.25f) {
                                    // Place us 1.5 units from target (well within 8-unit melee range)
                                    glm::vec3 snapPos = targetRender - toTarget * (1.5f * glm::inversesqrt(dSq));
                                    renderer->getCharacterPosition() = snapPos;
                                    glm::vec3 snapCanonical = core::coords::renderToCanonical(snapPos);
                                    gameHandler->setPosition(snapCanonical.x, snapCanonical.y, snapCanonical.z);
                                    if (renderer->getCameraController()) {
                                        glm::vec3* ft = renderer->getCameraController()->getFollowTargetMutable();
                                        if (ft) *ft = snapPos;
                                    }
                                }
                            }
                            gameHandler->startAutoAttack(chargeTargetGuid_);
                            renderer->triggerMeleeSwing();
                        }

                        // Send movement heartbeat so server knows our new position
                        gameHandler->sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
                    }
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
                for (const auto& [guid, instanceId] : creatureInstances_) {
                    auto entity = gameHandler->getEntityManager().getEntity(guid);
                    if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

                    if (npcWeaponRetryTick &&
                        weaponAttachesThisTick < MAX_WEAPON_ATTACHES_PER_TICK &&
                        !creatureWeaponsAttached_.count(guid)) {
                        uint8_t attempts = 0;
                        auto itAttempts = creatureWeaponAttachAttempts_.find(guid);
                        if (itAttempts != creatureWeaponAttachAttempts_.end()) attempts = itAttempts->second;
                        if (attempts < 30) {
                            weaponAttachesThisTick++;
                            if (tryAttachCreatureVirtualWeapons(guid, instanceId)) {
                                creatureWeaponsAttached_.insert(guid);
                                creatureWeaponAttachAttempts_.erase(guid);
                            } else {
                                creatureWeaponAttachAttempts_[guid] = static_cast<uint8_t>(attempts + 1);
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

                    glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
                    glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

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
                        auto mit = creatureModelIds_.find(guid);
                        if (mit != creatureModelIds_.end()) {
                            uint32_t mid = mit->second;
                            auto wolfIt = modelIdIsWolfLike_.find(mid);
                            if (wolfIt == modelIdIsWolfLike_.end()) {
                                bool isWolf = false;
                                if (const auto* md = charRenderer->getModelData(mid)) {
                                    std::string modelName = md->name;
                                    std::transform(modelName.begin(), modelName.end(), modelName.begin(),
                                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    isWolf = (modelName.find("wolf") != std::string::npos ||
                                              modelName.find("worg") != std::string::npos);
                                }
                                wolfIt = modelIdIsWolfLike_.emplace(mid, isWolf).first;
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

                    auto posIt = creatureRenderPosCache_.find(guid);
                    if (posIt == creatureRenderPosCache_.end()) {
                        charRenderer->setInstancePosition(instanceId, renderPos);
                        creatureRenderPosCache_[guid] = renderPos;
                    } else {
                        const glm::vec3 prevPos = posIt->second;
                        float ddx2 = renderPos.x - prevPos.x;
                        float ddy2 = renderPos.y - prevPos.y;
                        float planarDistSq = ddx2 * ddx2 + ddy2 * ddy2;
                        float dz = std::abs(renderPos.z - prevPos.z);

                        auto unitPtr = std::static_pointer_cast<game::Unit>(entity);
                        const bool deadOrCorpse = unitPtr->getHealth() == 0;
                        const bool largeCorrection = (planarDistSq > 36.0f) || (dz > 3.0f);
                        // isEntityMoving() reflects server-authoritative move state set by
                        // startMoveTo() in handleMonsterMove, regardless of distance-cull.
                        // This correctly detects movement for distant creatures (> 150u)
                        // where updateMovement() is not called and getX/Y/Z() stays stale.
                        // Use isActivelyMoving() (not isEntityMoving()) so the
                        // Run/Walk animation stops when the creature reaches its
                        // destination, rather than persisting through the dead-
                        // reckoning overrun window.
                        const bool entityIsMoving = entity->isActivelyMoving();
                        constexpr float kMoveThreshSq = 0.03f * 0.03f;
                        const bool isMovingNow = !deadOrCorpse && (entityIsMoving || planarDistSq > kMoveThreshSq || dz > 0.08f);
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
                        const bool isSwimmingNow = creatureSwimmingState_.count(guid) > 0;
                        const bool isWalkingNow  = creatureWalkingState_.count(guid) > 0;
                        const bool isFlyingNow   = creatureFlyingState_.count(guid) > 0;
                        bool prevMoving   = creatureWasMoving_[guid];
                        bool prevSwimming = creatureWasSwimming_[guid];
                        bool prevFlying   = creatureWasFlying_[guid];
                        bool prevWalking  = creatureWasWalking_[guid];
                        // Trigger animation update on any locomotion-state transition, not just
                        // moving/idle — e.g. creature lands while still moving → FlyForward→Run,
                        // or server changes WALKING flag while creature is already running → Walk.
                        const bool stateChanged = (isMovingNow  != prevMoving)   ||
                                                  (isSwimmingNow != prevSwimming) ||
                                                  (isFlyingNow   != prevFlying)   ||
                                                  (isWalkingNow  != prevWalking && isMovingNow);
                        if (stateChanged) {
                            creatureWasMoving_[guid]   = isMovingNow;
                            creatureWasSwimming_[guid] = isSwimmingNow;
                            creatureWasFlying_[guid]   = isFlyingNow;
                            creatureWasWalking_[guid]  = isWalkingNow;
                            uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                            bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                            if (!gotState || curAnimId != 1 /*Death*/) {
                                uint32_t targetAnim;
                                if (isMovingNow) {
                                    if (isFlyingNow)        targetAnim = 159u; // FlyForward
                                    else if (isSwimmingNow) targetAnim = 42u;  // Swim
                                    else if (isWalkingNow)  targetAnim = 4u;   // Walk
                                    else                    targetAnim = 5u;   // Run
                                } else {
                                    if (isFlyingNow)        targetAnim = 158u; // FlyIdle (hover)
                                    else if (isSwimmingNow) targetAnim = 41u;  // SwimIdle
                                    else                    targetAnim = 0u;   // Stand
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
                                creatureInstances_.size(), " creatures)");
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

                for (const auto& [guid, instanceId] : playerInstances_) {
                    auto entity = gameHandler->getEntityManager().getEntity(guid);
                    if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;

                    // Distance cull
                    if (havePPos) {
                        glm::vec3 latestCanonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
                        glm::vec3 d = latestCanonical - pPos;
                        if (glm::dot(d, d) > pSyncRadiusSq) continue;
                    }

                    // Position sync
                    glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
                    glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

                    auto posIt = creatureRenderPosCache_.find(guid);
                    if (posIt == creatureRenderPosCache_.end()) {
                        charRenderer->setInstancePosition(instanceId, renderPos);
                        creatureRenderPosCache_[guid] = renderPos;
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
                        const bool isMovingNow = !deadOrCorpse && (entityIsMoving || planarDistSq > kMoveThreshSq2 || dz > 0.08f);

                        if (deadOrCorpse || largeCorrection) {
                            charRenderer->setInstancePosition(instanceId, renderPos);
                        } else if (planarDistSq > kMoveThreshSq2 || dz > 0.08f) {
                            float planarDist = std::sqrt(planarDistSq);
                            float duration = std::clamp(planarDist / 5.5f, 0.05f, 0.22f);
                            charRenderer->moveInstanceTo(instanceId, renderPos, duration);
                        }
                        posIt->second = renderPos;

                        // Drive movement animation (same logic as creatures)
                        const bool isSwimmingNow = creatureSwimmingState_.count(guid) > 0;
                        const bool isWalkingNow  = creatureWalkingState_.count(guid) > 0;
                        const bool isFlyingNow   = creatureFlyingState_.count(guid) > 0;
                        bool prevMoving   = creatureWasMoving_[guid];
                        bool prevSwimming = creatureWasSwimming_[guid];
                        bool prevFlying   = creatureWasFlying_[guid];
                        bool prevWalking  = creatureWasWalking_[guid];
                        const bool stateChanged = (isMovingNow  != prevMoving)   ||
                                                  (isSwimmingNow != prevSwimming) ||
                                                  (isFlyingNow   != prevFlying)   ||
                                                  (isWalkingNow  != prevWalking && isMovingNow);
                        if (stateChanged) {
                            creatureWasMoving_[guid]   = isMovingNow;
                            creatureWasSwimming_[guid] = isSwimmingNow;
                            creatureWasFlying_[guid]   = isFlyingNow;
                            creatureWasWalking_[guid]  = isWalkingNow;
                            uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                            bool gotState = charRenderer->getAnimationState(instanceId, curAnimId, curT, curDur);
                            if (!gotState || curAnimId != 1 /*Death*/) {
                                uint32_t targetAnim;
                                if (isMovingNow) {
                                    if (isFlyingNow)        targetAnim = 159u; // FlyForward
                                    else if (isSwimmingNow) targetAnim = 42u;  // Swim
                                    else if (isWalkingNow)  targetAnim = 4u;   // Walk
                                    else                    targetAnim = 5u;   // Run
                                } else {
                                    if (isFlyingNow)        targetAnim = 158u; // FlyIdle (hover)
                                    else if (isSwimmingNow) targetAnim = 41u;  // SwimIdle
                                    else                    targetAnim = 0u;   // Stand
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
                                playerInstances_.size(), " players)");
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

    if (pendingWorldEntry_ && !loadingWorld_ && state != AppState::DISCONNECTED) {
        auto entry = *pendingWorldEntry_;
        pendingWorldEntry_.reset();
        worldEntryMovementGraceTimer_ = 2.0f;
        taxiLandingClampTimer_ = 0.0f;
        lastTaxiFlight_ = false;
        if (renderer && renderer->getCameraController()) {
            renderer->getCameraController()->clearMovementInputs();
            renderer->getCameraController()->suppressMovementFor(1.0f);
                renderer->getCameraController()->suspendGravityFor(10.0f);
        }
        loadOnlineWorldTerrain(entry.mapId, entry.x, entry.y, entry.z);
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
    // Authentication screen callback
    uiManager->getAuthScreen().setOnSuccess([this]() {
        LOG_INFO("Authentication successful, transitioning to realm selection");
        setState(AppState::REALM_SELECTION);
    });

    // Realm selection callback
    uiManager->getRealmScreen().setOnRealmSelected([this](const std::string& realmName, const std::string& realmAddress) {
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
        const auto& sessionKey = authHandler->getSessionKey();
        std::string accountName = authHandler->getUsername();
        if (accountName.empty()) {
            LOG_WARNING("Auth username missing; falling back to TESTACCOUNT");
            accountName = "TESTACCOUNT";
        }

        uint32_t realmId = 0;
        uint16_t realmBuild = 0;
        {
            // WotLK AUTH_SESSION includes a RealmID field; some servers reject if it's wrong/zero.
            const auto& realms = authHandler->getRealms();
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
        if (gameHandler->connect(host, port, sessionKey, accountName, clientBuild, realmId)) {
            LOG_INFO("Connected to world server, transitioning to character selection");
            setState(AppState::CHARACTER_SELECTION);
        } else {
            LOG_ERROR("Failed to connect to world server");
        }
    });

    // Realm screen back button - return to login
    uiManager->getRealmScreen().setOnBack([this]() {
        if (authHandler) {
            authHandler->disconnect();
        }
        uiManager->getRealmScreen().reset();
        setState(AppState::AUTHENTICATION);
    });

    // Character selection callback
    uiManager->getCharacterScreen().setOnCharacterSelected([this](uint64_t characterGuid) {
        LOG_INFO("Character selected: GUID=0x", std::hex, characterGuid, std::dec);
        // Always set the active character GUID
        if (gameHandler) {
            gameHandler->setActiveCharacterGuid(characterGuid);
        }
        // Keep CHARACTER_SELECTION active until world entry is fully loaded.
        // This avoids exposing pre-load hitching before the loading screen/intro.
    });

    // Character create screen callbacks
    uiManager->getCharacterCreateScreen().setOnCreate([this](const game::CharCreateData& data) {
        pendingCreatedCharacterName_ = data.name;  // Store name for auto-selection
        gameHandler->createCharacter(data);
    });

    uiManager->getCharacterCreateScreen().setOnCancel([this]() {
        setState(AppState::CHARACTER_SELECTION);
    });

    // Character create result callback
    gameHandler->setCharCreateCallback([this](bool success, const std::string& msg) {
        if (success) {
            // Auto-select the newly created character
            if (!pendingCreatedCharacterName_.empty()) {
                uiManager->getCharacterScreen().selectCharacterByName(pendingCreatedCharacterName_);
                pendingCreatedCharacterName_.clear();
            }
            setState(AppState::CHARACTER_SELECTION);
        } else {
            uiManager->getCharacterCreateScreen().setStatus(msg, true);
            pendingCreatedCharacterName_.clear();
        }
    });

    // Character login failure callback
    gameHandler->setCharLoginFailCallback([this](const std::string& reason) {
        LOG_WARNING("Character login failed: ", reason);
        setState(AppState::CHARACTER_SELECTION);
        uiManager->getCharacterScreen().setStatus("Login failed: " + reason, true);
    });

    // World entry callback (online mode) - load terrain when entering world
    gameHandler->setWorldEntryCallback([this](uint32_t mapId, float x, float y, float z, bool isInitialEntry) {
        LOG_INFO("Online world entry: mapId=", mapId, " pos=(", x, ", ", y, ", ", z, ")"
                 " initial=", isInitialEntry);
        if (renderer) {
            renderer->resetCombatVisualState();
        }

        // Reconnect to the same map: terrain stays loaded but all online entities are stale.
        // Despawn them properly so the server's fresh CREATE_OBJECTs will re-populate the world.
        if (mapId == loadedMapId_ && renderer && renderer->getTerrainManager() && isInitialEntry) {
            LOG_INFO("Reconnect to same map ", mapId, ": clearing stale online entities (terrain preserved)");

            // Pending spawn queues and failure caches
            pendingCreatureSpawns_.clear();
            pendingCreatureSpawnGuids_.clear();
            creatureSpawnRetryCounts_.clear();
            creaturePermanentFailureGuids_.clear();  // Clear so previously-failed GUIDs can retry
            deadCreatureGuids_.clear();              // Will be re-populated from fresh server state
            pendingPlayerSpawns_.clear();
            pendingPlayerSpawnGuids_.clear();
            pendingOnlinePlayerEquipment_.clear();
            deferredEquipmentQueue_.clear();
            pendingGameObjectSpawns_.clear();

            // Properly despawn all tracked instances from the renderer
            {
                std::vector<uint64_t> guids;
                guids.reserve(creatureInstances_.size());
                for (const auto& [g, _] : creatureInstances_) guids.push_back(g);
                for (auto g : guids) despawnOnlineCreature(g);
            }
            {
                std::vector<uint64_t> guids;
                guids.reserve(playerInstances_.size());
                for (const auto& [g, _] : playerInstances_) guids.push_back(g);
                for (auto g : guids) despawnOnlinePlayer(g);
            }
            {
                std::vector<uint64_t> guids;
                guids.reserve(gameObjectInstances_.size());
                for (const auto& [g, _] : gameObjectInstances_) guids.push_back(g);
                for (auto g : guids) despawnOnlineGameObject(g);
            }

            // Update player position and re-queue nearby tiles (same logic as teleport)
            glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(x, y, z));
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            renderer->getCharacterPosition() = renderPos;
            if (renderer->getCameraController()) {
                auto* ft = renderer->getCameraController()->getFollowTargetMutable();
                if (ft) *ft = renderPos;
                renderer->getCameraController()->clearMovementInputs();
                renderer->getCameraController()->suppressMovementFor(1.0f);
                renderer->getCameraController()->suspendGravityFor(10.0f);
            }
            worldEntryMovementGraceTimer_ = 2.0f;
            taxiLandingClampTimer_ = 0.0f;
            lastTaxiFlight_ = false;
            renderer->getTerrainManager()->processReadyTiles();
            {
                auto [tileX, tileY] = core::coords::worldToTile(renderPos.x, renderPos.y);
                std::vector<std::pair<int,int>> nearbyTiles;
                nearbyTiles.reserve(289);
                for (int dy = -8; dy <= 8; dy++)
                    for (int dx = -8; dx <= 8; dx++)
                        nearbyTiles.push_back({tileX + dx, tileY + dy});
                renderer->getTerrainManager()->precacheTiles(nearbyTiles);
            }
            return;
        }

        // Same-map teleport (taxi landing, GM teleport, hearthstone on same continent):
        if (mapId == loadedMapId_ && renderer && renderer->getTerrainManager()) {
            // Check if teleport is far enough to need terrain loading (>500 render units)
            glm::vec3 oldPos = renderer->getCharacterPosition();
            glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(x, y, z));
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            float teleportDistSq = glm::dot(renderPos - oldPos, renderPos - oldPos);
            bool farTeleport = (teleportDistSq > 500.0f * 500.0f);

            if (farTeleport) {
                // Far same-map teleport (hearthstone, etc.): defer full world reload
                // to next frame to avoid blocking the packet handler for 20+ seconds.
                LOG_WARNING("Far same-map teleport (dist=", std::sqrt(teleportDistSq),
                            "), deferring world reload to next frame");
                // Update position immediately so the player doesn't keep moving at old location
                renderer->getCharacterPosition() = renderPos;
                if (renderer->getCameraController()) {
                    auto* ft = renderer->getCameraController()->getFollowTargetMutable();
                    if (ft) *ft = renderPos;
                    renderer->getCameraController()->clearMovementInputs();
                    renderer->getCameraController()->suppressMovementFor(1.0f);
                renderer->getCameraController()->suspendGravityFor(10.0f);
                }
                pendingWorldEntry_ = PendingWorldEntry{mapId, x, y, z};
                return;
            }
            LOG_INFO("Same-map teleport (map ", mapId, "), skipping full world reload");
            // canonical and renderPos already computed above for distance check
            renderer->getCharacterPosition() = renderPos;
            if (renderer->getCameraController()) {
                auto* ft = renderer->getCameraController()->getFollowTargetMutable();
                if (ft) *ft = renderPos;
            }
            worldEntryMovementGraceTimer_ = 2.0f;
            taxiLandingClampTimer_ = 0.0f;
            lastTaxiFlight_ = false;
            // Stop any movement that was active before the teleport
            if (renderer->getCameraController()) {
                renderer->getCameraController()->clearMovementInputs();
                renderer->getCameraController()->suppressMovementFor(0.5f);
            }
            // Kick off async upload for any tiles that finished background
            // parsing.  Use the bounded processReadyTiles() instead of
            // processAllReadyTiles() to avoid multi-second main-thread stalls
            // when many tiles are ready (the rest will finalize over subsequent
            // frames via the normal terrain update loop).
            renderer->getTerrainManager()->processReadyTiles();

            // Queue all remaining tiles within the load radius (8 tiles = 17x17)
            // at the new position. precacheTiles skips already-loaded/pending tiles,
            // so this only enqueues tiles that aren't yet in the pipeline.
            // This ensures background workers immediately start loading everything
            // visible from the new position (hearthstone may land far from old location).
            {
                auto [tileX, tileY] = core::coords::worldToTile(renderPos.x, renderPos.y);
                std::vector<std::pair<int,int>> nearbyTiles;
                nearbyTiles.reserve(289);
                for (int dy = -8; dy <= 8; dy++)
                    for (int dx = -8; dx <= 8; dx++)
                        nearbyTiles.push_back({tileX + dx, tileY + dy});
                renderer->getTerrainManager()->precacheTiles(nearbyTiles);
            }
            return;
        }

        // If a world load is already in progress (re-entrant call from
        // gameHandler->update() processing SMSG_NEW_WORLD during warmup),
        // defer this entry. The current load will pick it up when it finishes.
        if (loadingWorld_) {
            LOG_WARNING("World entry deferred: map ", mapId, " while loading (will process after current load)");
            pendingWorldEntry_ = {mapId, x, y, z};
            return;
        }

        // Full world loads are expensive and `loadOnlineWorldTerrain()` itself
        // drives `gameHandler->update()` during warmup. Queue the load here so
        // it runs after the current packet handler returns instead of recursing
        // from `SMSG_LOGIN_VERIFY_WORLD` / `SMSG_NEW_WORLD`.
        LOG_WARNING("Queued world entry: map ", mapId, " pos=(", x, ", ", y, ", ", z, ")");
        pendingWorldEntry_ = {mapId, x, y, z};
    });

    auto sampleBestFloorAt = [this](float x, float y, float probeZ) -> std::optional<float> {
        std::optional<float> terrainFloor;
        std::optional<float> wmoFloor;
        std::optional<float> m2Floor;

        if (renderer && renderer->getTerrainManager()) {
            terrainFloor = renderer->getTerrainManager()->getHeightAt(x, y);
        }
        if (renderer && renderer->getWMORenderer()) {
            wmoFloor = renderer->getWMORenderer()->getFloorHeight(x, y, probeZ);
        }
        if (renderer && renderer->getM2Renderer()) {
            m2Floor = renderer->getM2Renderer()->getFloorHeight(x, y, probeZ);
        }

        std::optional<float> best;
        if (terrainFloor) best = terrainFloor;
        if (wmoFloor && (!best || *wmoFloor > *best)) best = wmoFloor;
        if (m2Floor && (!best || *m2Floor > *best)) best = m2Floor;
        return best;
    };

    auto clearStuckMovement = [this]() {
        if (renderer && renderer->getCameraController()) {
            renderer->getCameraController()->clearMovementInputs();
        }
        if (gameHandler) {
            gameHandler->forceClearTaxiAndMovementState();
            gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP);
            gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP_STRAFE);
            gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP_TURN);
            gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP_SWIM);
            gameHandler->sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
        }
    };

    auto syncTeleportedPositionToServer = [this](const glm::vec3& renderPos) {
        if (!gameHandler) return;
        glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
        gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
        gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP);
        gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP_STRAFE);
        gameHandler->sendMovement(game::Opcode::MSG_MOVE_STOP_TURN);
        gameHandler->sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
    };

    auto forceServerTeleportCommand = [this](const glm::vec3& renderPos) {
        if (!gameHandler) return;
        // Server-authoritative reset first, then teleport.
        gameHandler->sendChatMessage(game::ChatType::SAY, ".revive", "");
        gameHandler->sendChatMessage(game::ChatType::SAY, ".dismount", "");

        glm::vec3 canonical = core::coords::renderToCanonical(renderPos);
        glm::vec3 serverPos = core::coords::canonicalToServer(canonical);
        std::ostringstream cmd;
        cmd.setf(std::ios::fixed);
        cmd.precision(3);
        cmd << ".go xyz "
            << serverPos.x << " "
            << serverPos.y << " "
            << serverPos.z << " "
            << gameHandler->getCurrentMapId() << " "
            << gameHandler->getMovementInfo().orientation;
        gameHandler->sendChatMessage(game::ChatType::SAY, cmd.str(), "");
    };

    // /unstuck — nudge player forward and snap to floor at destination.
    gameHandler->setUnstuckCallback([this, sampleBestFloorAt, clearStuckMovement, syncTeleportedPositionToServer, forceServerTeleportCommand]() {
        if (!renderer || !renderer->getCameraController()) return;
        worldEntryMovementGraceTimer_ = std::max(worldEntryMovementGraceTimer_, 1.5f);
        taxiLandingClampTimer_ = 0.0f;
        lastTaxiFlight_ = false;
        clearStuckMovement();
        auto* cc = renderer->getCameraController();
        auto* ft = cc->getFollowTargetMutable();
        if (!ft) return;

        glm::vec3 pos = *ft;

        // Always nudge forward first to escape stuck geometry (M2 models, collision seams).
        if (gameHandler) {
            float renderYaw = gameHandler->getMovementInfo().orientation + glm::radians(90.0f);
            pos.x += std::cos(renderYaw) * 5.0f;
            pos.y += std::sin(renderYaw) * 5.0f;
        }

        // Sample floor at the DESTINATION position (after nudge).
        // Pick the highest floor so we snap up to WMO floors when fallen below.
        bool foundFloor = false;
        if (auto floor = sampleBestFloorAt(pos.x, pos.y, pos.z + 60.0f)) {
            pos.z = *floor + 0.2f;
            foundFloor = true;
        }

        cc->teleportTo(pos);
        if (!foundFloor) {
            cc->setGrounded(false);  // Let gravity pull player down to a surface
        }
        syncTeleportedPositionToServer(pos);
        forceServerTeleportCommand(pos);
        clearStuckMovement();
        LOG_INFO("Unstuck: nudged forward and snapped to floor");
    });

    // /unstuckgy — stronger recovery: safe/home position, then sampled floor fallback.
    gameHandler->setUnstuckGyCallback([this, sampleBestFloorAt, clearStuckMovement, syncTeleportedPositionToServer, forceServerTeleportCommand]() {
        if (!renderer || !renderer->getCameraController()) return;
        worldEntryMovementGraceTimer_ = std::max(worldEntryMovementGraceTimer_, 1.5f);
        taxiLandingClampTimer_ = 0.0f;
        lastTaxiFlight_ = false;
        clearStuckMovement();
        auto* cc = renderer->getCameraController();
        auto* ft = cc->getFollowTargetMutable();
        if (!ft) return;

        // Try last safe position first (nearby, terrain already loaded)
        if (cc->hasLastSafePosition()) {
            glm::vec3 safePos = cc->getLastSafePosition();
            safePos.z += 5.0f;
            cc->teleportTo(safePos);
            syncTeleportedPositionToServer(safePos);
            forceServerTeleportCommand(safePos);
            clearStuckMovement();
            LOG_INFO("Unstuck: teleported to last safe position");
            return;
        }

        uint32_t bindMap = 0;
        glm::vec3 bindPos(0.0f);
        if (gameHandler && gameHandler->getHomeBind(bindMap, bindPos) &&
            bindMap == gameHandler->getCurrentMapId()) {
            bindPos.z += 2.0f;
            cc->teleportTo(bindPos);
            syncTeleportedPositionToServer(bindPos);
            forceServerTeleportCommand(bindPos);
            clearStuckMovement();
            LOG_INFO("Unstuck: teleported to home bind position");
            return;
        }

        // No safe/bind position — try current XY with a high floor probe.
        glm::vec3 pos = *ft;
        if (auto floor = sampleBestFloorAt(pos.x, pos.y, pos.z + 120.0f)) {
            pos.z = *floor + 0.5f;
            cc->teleportTo(pos);
            syncTeleportedPositionToServer(pos);
            forceServerTeleportCommand(pos);
            clearStuckMovement();
            LOG_INFO("Unstuck: teleported to sampled floor");
            return;
        }

        // Last fallback: high snap to clear deeply bad geometry.
        pos.z += 60.0f;
        cc->teleportTo(pos);
        syncTeleportedPositionToServer(pos);
        forceServerTeleportCommand(pos);
        clearStuckMovement();
        LOG_INFO("Unstuck: high fallback snap");
    });

    // /unstuckhearth — teleport to hearthstone bind point (server-synced).
    // Freezes player until terrain loads at destination to prevent falling through world.
    gameHandler->setUnstuckHearthCallback([this, clearStuckMovement, forceServerTeleportCommand]() {
        if (!renderer || !renderer->getCameraController() || !gameHandler) return;

        uint32_t bindMap = 0;
        glm::vec3 bindPos(0.0f);
        if (!gameHandler->getHomeBind(bindMap, bindPos)) {
            LOG_WARNING("Unstuck hearth: no bind point available");
            return;
        }

        worldEntryMovementGraceTimer_ = 10.0f;  // long grace — terrain load check will clear it
        taxiLandingClampTimer_ = 0.0f;
        lastTaxiFlight_ = false;
        clearStuckMovement();

        auto* cc = renderer->getCameraController();
        glm::vec3 renderPos = core::coords::canonicalToRender(bindPos);
        renderPos.z += 2.0f;

        // Freeze player in place (no gravity/movement) until terrain loads
        cc->teleportTo(renderPos);
        cc->setExternalFollow(true);
        forceServerTeleportCommand(renderPos);
        clearStuckMovement();

        // Set pending state — update loop will unfreeze once terrain is loaded
        hearthTeleportPending_ = true;
        hearthTeleportPos_ = renderPos;
        hearthTeleportTimer_ = 15.0f;  // 15s safety timeout
        LOG_INFO("Unstuck hearth: teleporting to bind point, waiting for terrain...");
    });

    // Auto-unstuck: falling for > 5 seconds = void fall, teleport to map entry
    if (renderer->getCameraController()) {
        renderer->getCameraController()->setAutoUnstuckCallback([this, forceServerTeleportCommand]() {
            if (!renderer || !renderer->getCameraController()) return;
            auto* cc = renderer->getCameraController();

            // Last resort: teleport to map entry point (terrain guaranteed loaded here)
            glm::vec3 spawnPos = cc->getDefaultPosition();
            spawnPos.z += 5.0f;
            cc->teleportTo(spawnPos);
            forceServerTeleportCommand(spawnPos);
            LOG_INFO("Auto-unstuck: teleported to map entry point (server synced)");
        });
    }

    // Bind point update (innkeeper) — position stored in gameHandler->getHomeBind()
    gameHandler->setBindPointCallback([this](uint32_t mapId, float x, float y, float z) {
        LOG_INFO("Bindpoint set: mapId=", mapId, " pos=(", x, ", ", y, ", ", z, ")");
    });

    // Hearthstone preload callback: begin loading terrain at the bind point as soon as
    // the player starts casting Hearthstone.  The ~10 s cast gives enough time for
    // the background streaming workers to bring tiles into the cache so the player
    // lands on solid ground instead of falling through un-loaded terrain.
    gameHandler->setHearthstonePreloadCallback([this](uint32_t mapId, float x, float y, float z) {
        if (!renderer || !assetManager) return;

        auto* terrainMgr = renderer->getTerrainManager();
        if (!terrainMgr) return;

        // Resolve map name from the cached Map.dbc table
        std::string mapName;
        if (auto it = mapNameById_.find(mapId); it != mapNameById_.end()) {
            mapName = it->second;
        } else {
            mapName = mapIdToName(mapId);
        }
        if (mapName.empty()) mapName = "Azeroth";

        if (mapId == loadedMapId_) {
            // Same map: pre-enqueue tiles around the bind point so workers start
            // loading them now. Uses render-space coords (canonicalToRender).
            // Use radius 4 (9x9=81 tiles) — hearthstone cast is ~10s, enough time
            // for workers to parse most of these before the player arrives.
            glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
            auto [tileX, tileY] = core::coords::worldToTile(renderPos.x, renderPos.y);

            std::vector<std::pair<int,int>> tiles;
            tiles.reserve(81);
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++)
                    tiles.push_back({tileX + dx, tileY + dy});

            terrainMgr->precacheTiles(tiles);
            LOG_INFO("Hearthstone preload: enqueued ", tiles.size(),
                     " tiles around bind point (same map) tile=[", tileX, ",", tileY, "]");
        } else {
            // Different map: warm the file cache so ADT parsing is fast when
            // loadOnlineWorldTerrain runs its blocking load loop.
            // homeBindPos_ is canonical; startWorldPreload expects server coords.
            glm::vec3 server = core::coords::canonicalToServer(glm::vec3(x, y, z));
            startWorldPreload(mapId, mapName, server.x, server.y);
            LOG_INFO("Hearthstone preload: started file cache warm for map '", mapName,
                     "' (id=", mapId, ")");
        }
    });

    // Faction hostility map is built in buildFactionHostilityMap() when character enters world

    // Creature spawn callback (online mode) - spawn creature models
    gameHandler->setCreatureSpawnCallback([this](uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        // Queue spawns to avoid hanging when many creatures appear at once.
        // Deduplicate so repeated updates don't flood pending queue.
        if (creatureInstances_.count(guid)) return;
        if (pendingCreatureSpawnGuids_.count(guid)) return;
        pendingCreatureSpawns_.push_back({guid, displayId, x, y, z, orientation, scale});
        pendingCreatureSpawnGuids_.insert(guid);
    });

    // Player spawn callback (online mode) - spawn player models with correct textures
    gameHandler->setPlayerSpawnCallback([this](uint64_t guid,
                                              uint32_t /*displayId*/,
                                              uint8_t raceId,
                                              uint8_t genderId,
                                              uint32_t appearanceBytes,
                                              uint8_t facialFeatures,
                                              float x, float y, float z, float orientation) {
        LOG_WARNING("playerSpawnCallback: guid=0x", std::hex, guid, std::dec,
                    " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId),
                    " pos=(", x, ",", y, ",", z, ")");
        // Skip local player — already spawned as the main character
        uint64_t localGuid = gameHandler ? gameHandler->getPlayerGuid() : 0;
        uint64_t activeGuid = gameHandler ? gameHandler->getActiveCharacterGuid() : 0;
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_)) {
            return;
        }
        if (playerInstances_.count(guid)) return;
        if (pendingPlayerSpawnGuids_.count(guid)) return;
        pendingPlayerSpawns_.push_back({guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation});
        pendingPlayerSpawnGuids_.insert(guid);
    });

    // Online player equipment callback - apply armor geosets/skin overlays per player instance.
    gameHandler->setPlayerEquipmentCallback([this](uint64_t guid,
                                                  const std::array<uint32_t, 19>& displayInfoIds,
                                                  const std::array<uint8_t, 19>& inventoryTypes) {
        // Queue equipment compositing instead of doing it immediately —
        // compositeWithRegions is expensive (file I/O + CPU blit + GPU upload)
        // and causes frame stutters if multiple players update at once.
        deferredEquipmentQueue_.push_back({guid, {displayInfoIds, inventoryTypes}});
    });

    // Creature despawn callback (online mode) - remove creature models
    gameHandler->setCreatureDespawnCallback([this](uint64_t guid) {
        despawnOnlineCreature(guid);
    });

    gameHandler->setPlayerDespawnCallback([this](uint64_t guid) {
        despawnOnlinePlayer(guid);
    });

    // GameObject spawn callback (online mode) - spawn static models (mailboxes, etc.)
    gameHandler->setGameObjectSpawnCallback([this](uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
        pendingGameObjectSpawns_.push_back({guid, entry, displayId, x, y, z, orientation, scale});
    });

    // GameObject despawn callback (online mode) - remove static models
    gameHandler->setGameObjectDespawnCallback([this](uint64_t guid) {
        despawnOnlineGameObject(guid);
    });

    // GameObject custom animation callback (e.g. chest opening)
    gameHandler->setGameObjectCustomAnimCallback([this](uint64_t guid, uint32_t /*animId*/) {
        auto it = gameObjectInstances_.find(guid);
        if (it == gameObjectInstances_.end() || !renderer) return;
        auto& info = it->second;
        if (!info.isWmo) {
            if (auto* m2r = renderer->getM2Renderer()) {
                m2r->setInstanceAnimationFrozen(info.instanceId, false);
            }
        }
    });

    // Charge callback — warrior rushes toward target
    gameHandler->setChargeCallback([this](uint64_t targetGuid, float tx, float ty, float tz) {
        if (!renderer || !renderer->getCameraController() || !gameHandler) return;

        // Get current player position in render coords
        glm::vec3 startRender = renderer->getCharacterPosition();
        // Convert target from canonical to render
        glm::vec3 targetRender = core::coords::canonicalToRender(glm::vec3(tx, ty, tz));

        // Compute direction and stop 2.0 units short (melee reach)
        glm::vec3 dir = targetRender - startRender;
        float distSq = glm::dot(dir, dir);
        if (distSq < 9.0f) return; // Too close, nothing to do
        float invDist = glm::inversesqrt(distSq);
        glm::vec3 dirNorm = dir * invDist;
        glm::vec3 endRender = targetRender - dirNorm * 2.0f;

        // Face toward target BEFORE starting charge
        float yawRad = std::atan2(dirNorm.x, dirNorm.y);
        float yawDeg = glm::degrees(yawRad);
        renderer->setCharacterYaw(yawDeg);
        // Sync canonical orientation to server so it knows we turned
        float canonicalYaw = core::coords::normalizeAngleRad(glm::radians(180.0f - yawDeg));
        gameHandler->setOrientation(canonicalYaw);
        gameHandler->sendMovement(game::Opcode::MSG_MOVE_SET_FACING);

        // Set charge state
        chargeActive_ = true;
        chargeTimer_ = 0.0f;
        chargeDuration_ = std::max(std::sqrt(distSq) / 25.0f, 0.3f); // ~25 units/sec
        chargeStartPos_ = startRender;
        chargeEndPos_ = endRender;
        chargeTargetGuid_ = targetGuid;

        // Disable player input, play charge animation
        renderer->getCameraController()->setExternalFollow(true);
        renderer->getCameraController()->clearMovementInputs();
        renderer->setCharging(true);

        // Start charge visual effect (red haze + dust)
        glm::vec3 chargeDir = glm::normalize(endRender - startRender);
        renderer->startChargeEffect(startRender, chargeDir);

        // Play charge whoosh sound (try multiple paths)
        auto& audio = audio::AudioEngine::instance();
        if (!audio.playSound2D("Sound\\Spells\\Charge.wav", 0.8f)) {
            if (!audio.playSound2D("Sound\\Spells\\charge.wav", 0.8f)) {
                if (!audio.playSound2D("Sound\\Spells\\SpellCharge.wav", 0.8f)) {
                    // Fallback: weapon whoosh
                    audio.playSound2D("Sound\\Item\\Weapons\\WeaponSwings\\mWooshLarge1.wav", 0.9f);
                }
            }
        }
    });

    // Level-up callback — play sound, cheer emote, and trigger UI ding overlay + 3D effect
    gameHandler->setLevelUpCallback([this](uint32_t newLevel) {
        if (uiManager) {
            uiManager->getGameScreen().toastManager().triggerDing(newLevel);
        }
        if (renderer) {
            renderer->triggerLevelUpEffect(renderer->getCharacterPosition());
        }
    });

    // Achievement earned callback — show toast banner
    gameHandler->setAchievementEarnedCallback([this](uint32_t achievementId, const std::string& name) {
        if (uiManager) {
            uiManager->getGameScreen().toastManager().triggerAchievementToast(achievementId, name);
        }
    });

    // Server-triggered music callback (SMSG_PLAY_MUSIC)
    // Resolves soundId → SoundEntries.dbc → MPQ path → MusicManager.
    gameHandler->setPlayMusicCallback([this](uint32_t soundId) {
        if (!assetManager || !renderer) return;
        auto* music = renderer->getMusicManager();
        if (!music) return;

        auto dbc = assetManager->loadDBC("SoundEntries.dbc");
        if (!dbc || !dbc->isLoaded()) return;

        int32_t idx = dbc->findRecordById(soundId);
        if (idx < 0) return;

        // SoundEntries.dbc (WotLK): field 2 = Name (label), fields 3-12 = File[0..9], field 23 = DirectoryBase
        const uint32_t row = static_cast<uint32_t>(idx);
        std::string dir = dbc->getString(row, 23);
        for (uint32_t f = 3; f <= 12; ++f) {
            std::string name = dbc->getString(row, f);
            if (name.empty()) continue;
            std::string path = dir.empty() ? name : dir + "\\" + name;
            music->playMusic(path, /*loop=*/false);
            return;
        }
    });

    // SMSG_PLAY_SOUND: look up SoundEntries.dbc and play 2-D sound effect
    gameHandler->setPlaySoundCallback([this](uint32_t soundId) {
        if (!assetManager) return;

        auto dbc = assetManager->loadDBC("SoundEntries.dbc");
        if (!dbc || !dbc->isLoaded()) return;

        int32_t idx = dbc->findRecordById(soundId);
        if (idx < 0) return;

        const uint32_t row = static_cast<uint32_t>(idx);
        std::string dir = dbc->getString(row, 23);
        for (uint32_t f = 3; f <= 12; ++f) {
            std::string name = dbc->getString(row, f);
            if (name.empty()) continue;
            std::string path = dir.empty() ? name : dir + "\\" + name;
            audio::AudioEngine::instance().playSound2D(path);
            return;
        }
    });

    // SMSG_PLAY_OBJECT_SOUND / SMSG_PLAY_SPELL_IMPACT: play as 3D positional sound at source entity
    gameHandler->setPlayPositionalSoundCallback([this](uint32_t soundId, uint64_t sourceGuid) {
        if (!assetManager || !gameHandler) return;

        auto dbc = assetManager->loadDBC("SoundEntries.dbc");
        if (!dbc || !dbc->isLoaded()) return;

        int32_t idx = dbc->findRecordById(soundId);
        if (idx < 0) return;

        const uint32_t row = static_cast<uint32_t>(idx);
        std::string dir = dbc->getString(row, 23);
        for (uint32_t f = 3; f <= 12; ++f) {
            std::string name = dbc->getString(row, f);
            if (name.empty()) continue;
            std::string path = dir.empty() ? name : dir + "\\" + name;

            // Play as 3D sound if source entity position is available.
            // Entity stores canonical coords; listener uses render coords (camera).
            auto entity = gameHandler->getEntityManager().getEntity(sourceGuid);
            if (entity) {
                glm::vec3 canonical{entity->getLatestX(), entity->getLatestY(), entity->getLatestZ()};
                glm::vec3 pos = core::coords::canonicalToRender(canonical);
                audio::AudioEngine::instance().playSound3D(path, pos);
            } else {
                audio::AudioEngine::instance().playSound2D(path);
            }
            return;
        }
    });

    // Other player level-up callback — trigger 3D effect + chat notification
    gameHandler->setOtherPlayerLevelUpCallback([this](uint64_t guid, uint32_t newLevel) {
        if (!gameHandler || !renderer) return;

        // Trigger 3D effect at the other player's position
        auto entity = gameHandler->getEntityManager().getEntity(guid);
        if (entity) {
            glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
            glm::vec3 renderPos = core::coords::canonicalToRender(canonical);
            renderer->triggerLevelUpEffect(renderPos);
        }

        // Show chat message if in group
        if (gameHandler->isInGroup()) {
            std::string name = gameHandler->getCachedPlayerName(guid);
            if (name.empty()) name = "A party member";
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = name + " has reached level " + std::to_string(newLevel) + "!";
            gameHandler->addLocalChatMessage(msg);
        }
    });

    // Mount callback (online mode) - defer heavy model load to next frame
    gameHandler->setMountCallback([this](uint32_t mountDisplayId) {
        if (mountDisplayId == 0) {
            // Dismount is instant (no loading needed)
            if (renderer && renderer->getCharacterRenderer() && mountInstanceId_ != 0) {
                renderer->getCharacterRenderer()->removeInstance(mountInstanceId_);
                mountInstanceId_ = 0;
            }
            mountModelId_ = 0;
            pendingMountDisplayId_ = 0;
            if (renderer) renderer->clearMount();
            LOG_INFO("Dismounted");
            return;
        }
        // Queue the mount for processing in the next update() frame
        pendingMountDisplayId_ = mountDisplayId;
    });

    // Taxi precache callback - preload terrain tiles along flight path
    gameHandler->setTaxiPrecacheCallback([this](const std::vector<glm::vec3>& path) {
        if (!renderer || !renderer->getTerrainManager()) return;

        std::set<std::pair<int, int>> uniqueTiles;

        // Sample waypoints along path and gather tiles.
        // Denser sampling + neighbor coverage reduces in-flight stream spikes.
        const size_t stride = 2;
        for (size_t i = 0; i < path.size(); i += stride) {
            const auto& waypoint = path[i];
            glm::vec3 renderPos = core::coords::canonicalToRender(waypoint);
            int tileX = static_cast<int>(32 - (renderPos.x / 533.33333f));
            int tileY = static_cast<int>(32 - (renderPos.y / 533.33333f));

            if (tileX >= 0 && tileX <= 63 && tileY >= 0 && tileY <= 63) {
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        int nx = tileX + dx;
                        int ny = tileY + dy;
                        if (nx >= 0 && nx <= 63 && ny >= 0 && ny <= 63) {
                            uniqueTiles.insert({nx, ny});
                        }
                    }
                }
            }
        }
        // Ensure final destination tile is included.
        if (!path.empty()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(path.back());
            int tileX = static_cast<int>(32 - (renderPos.x / 533.33333f));
            int tileY = static_cast<int>(32 - (renderPos.y / 533.33333f));
            if (tileX >= 0 && tileX <= 63 && tileY >= 0 && tileY <= 63) {
                for (int dx = -1; dx <= 1; ++dx) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        int nx = tileX + dx;
                        int ny = tileY + dy;
                        if (nx >= 0 && nx <= 63 && ny >= 0 && ny <= 63) {
                            uniqueTiles.insert({nx, ny});
                        }
                    }
                }
            }
        }

        std::vector<std::pair<int, int>> tilesToLoad(uniqueTiles.begin(), uniqueTiles.end());
        if (tilesToLoad.size() > 512) {
            tilesToLoad.resize(512);
        }
        LOG_INFO("Precaching ", tilesToLoad.size(), " tiles for taxi route");
        renderer->getTerrainManager()->precacheTiles(tilesToLoad);
    });

    // Taxi orientation callback - update mount rotation during flight
    gameHandler->setTaxiOrientationCallback([this](float yaw, float pitch, float roll) {
        if (renderer && renderer->getCameraController()) {
            // Taxi callback now provides render-space yaw directly.
            float yawDegrees = glm::degrees(yaw);
            renderer->getCameraController()->setFacingYaw(yawDegrees);
            renderer->setCharacterYaw(yawDegrees);
            // Set mount pitch and roll for realistic flight animation
            renderer->setMountPitchRoll(pitch, roll);
        }
    });

    // Taxi flight start callback - keep non-blocking to avoid hitching at takeoff.
    gameHandler->setTaxiFlightStartCallback([this]() {
        if (renderer && renderer->getTerrainManager() && renderer->getM2Renderer()) {
            LOG_INFO("Taxi flight start: incremental terrain/M2 streaming active");
            uint32_t m2Count = renderer->getM2Renderer()->getModelCount();
            uint32_t instCount = renderer->getM2Renderer()->getInstanceCount();
            LOG_INFO("Current M2 VRAM state: ", m2Count, " models (", instCount, " instances)");
        }
    });

    // Open dungeon finder callback — server sends SMSG_OPEN_LFG_DUNGEON_FINDER
    gameHandler->setOpenLfgCallback([this]() {
        if (uiManager) uiManager->getGameScreen().openDungeonFinder();
    });

    // Creature move callback (online mode) - update creature positions
    gameHandler->setCreatureMoveCallback([this](uint64_t guid, float x, float y, float z, uint32_t durationMs) {
        if (!renderer || !renderer->getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        bool isPlayer = false;
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) { instanceId = pit->second; isPlayer = true; }
        else {
            auto it = creatureInstances_.find(guid);
            if (it != creatureInstances_.end()) instanceId = it->second;
        }
        if (instanceId != 0) {
            glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
            float durationSec = static_cast<float>(durationMs) / 1000.0f;
            renderer->getCharacterRenderer()->moveInstanceTo(instanceId, renderPos, durationSec);
            // Play Run animation (anim 5) for the duration of the spline move.
            // WoW M2 animation IDs: 4=Walk, 5=Run.
            // Don't override Death animation (1). The per-frame sync loop will return to
            // Stand when movement stops.
            if (durationMs > 0) {
                // Player animation is managed by the local renderer state machine —
                // don't reset it here or every server movement packet restarts the
                // run cycle from frame 0, causing visible stutter.
                if (!isPlayer) {
                    uint32_t curAnimId = 0; float curT = 0.0f, curDur = 0.0f;
                    auto* cr = renderer->getCharacterRenderer();
                    bool gotState = cr->getAnimationState(instanceId, curAnimId, curT, curDur);
                    // Only start Run if not already running and not in Death animation.
                    if (!gotState || (curAnimId != 1 /*Death*/ && curAnimId != 5u /*Run*/)) {
                        cr->playAnimation(instanceId, 5u, /*loop=*/true);
                    }
                    creatureWasMoving_[guid] = true;
                }
            }
        }
    });

    gameHandler->setGameObjectMoveCallback([this](uint64_t guid, float x, float y, float z, float orientation) {
        auto it = gameObjectInstances_.find(guid);
        if (it == gameObjectInstances_.end() || !renderer) {
            return;
        }
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        auto& info = it->second;
        if (info.isWmo) {
            if (auto* wr = renderer->getWMORenderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                wr->setInstanceTransform(info.instanceId, transform);
            }
        } else {
            if (auto* mr = renderer->getM2Renderer()) {
                glm::mat4 transform(1.0f);
                transform = glm::translate(transform, renderPos);
                mr->setInstanceTransform(info.instanceId, transform);
            }
        }
    });

    // Transport spawn callback (online mode) - register transports with TransportManager
    gameHandler->setTransportSpawnCallback([this](uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation) {
        if (!renderer) return;

        // Get the GameObject instance now so late queue processing can rely on stable IDs.
        auto it = gameObjectInstances_.find(guid);
        if (it == gameObjectInstances_.end()) {
            LOG_WARNING("Transport spawn callback: GameObject instance not found for GUID 0x", std::hex, guid, std::dec);
            return;
        }

        auto pendingIt = std::find_if(
            pendingTransportRegistrations_.begin(), pendingTransportRegistrations_.end(),
            [guid](const PendingTransportRegistration& pending) { return pending.guid == guid; });
        if (pendingIt != pendingTransportRegistrations_.end()) {
            pendingIt->entry = entry;
            pendingIt->displayId = displayId;
            pendingIt->x = x;
            pendingIt->y = y;
            pendingIt->z = z;
            pendingIt->orientation = orientation;
        } else {
            pendingTransportRegistrations_.push_back(
                PendingTransportRegistration{guid, entry, displayId, x, y, z, orientation});
        }
    });

    // Transport move callback (online mode) - update transport gameobject positions
    gameHandler->setTransportMoveCallback([this](uint64_t guid, float x, float y, float z, float orientation) {
        LOG_DEBUG("Transport move callback: GUID=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ") orientation=", orientation);

        auto* transportManager = gameHandler->getTransportManager();
        if (!transportManager) {
            LOG_WARNING("Transport move callback: TransportManager is null!");
            return;
        }

        auto pendingRegIt = std::find_if(
            pendingTransportRegistrations_.begin(), pendingTransportRegistrations_.end(),
            [guid](const PendingTransportRegistration& pending) { return pending.guid == guid; });
        if (pendingRegIt != pendingTransportRegistrations_.end()) {
            pendingTransportMoves_[guid] = PendingTransportMove{x, y, z, orientation};
            LOG_DEBUG("Queued transport move for pending registration GUID=0x", std::hex, guid, std::dec);
            return;
        }

        // Check if transport exists - if not, treat this as a late spawn (reconnection/server restart)
        if (!transportManager->getTransport(guid)) {
            LOG_DEBUG("Received position update for unregistered transport 0x", std::hex, guid, std::dec,
                     " - auto-spawning from position update");

            // Get transport info from entity manager
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::GAMEOBJECT) {
                auto go = std::static_pointer_cast<game::GameObject>(entity);
                uint32_t entry = go->getEntry();
                uint32_t displayId = go->getDisplayId();

                // Find the WMO instance for this transport (should exist from earlier GameObject spawn)
                auto it = gameObjectInstances_.find(guid);
                if (it != gameObjectInstances_.end()) {
                    uint32_t wmoInstanceId = it->second.instanceId;

                    // TransportAnimation.dbc is indexed by GameObject entry
                    uint32_t pathId = entry;
                    const bool preferServerData = gameHandler && gameHandler->hasServerTransportUpdate(guid);

                    // Coordinates are already canonical (converted in game_handler.cpp)
                    glm::vec3 canonicalSpawnPos(x, y, z);

                    // Check if we have a real usable path, otherwise remap/infer/fall back to stationary.
                    const bool shipOrZeppelinDisplay =
                        (displayId == 3015 || displayId == 3031 || displayId == 7546 ||
                         displayId == 7446 || displayId == 1587 || displayId == 2454 ||
                         displayId == 807 || displayId == 808);
                    bool hasUsablePath = transportManager->hasPathForEntry(entry);
                    if (shipOrZeppelinDisplay) {
                        hasUsablePath = transportManager->hasUsableMovingPathForEntry(entry, 25.0f);
                    }

                    if (preferServerData) {
                        // Strict server-authoritative mode: no inferred/remapped fallback routes.
                        if (!hasUsablePath) {
                            std::vector<glm::vec3> path = { canonicalSpawnPos };
                            transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                            LOG_INFO("Auto-spawned transport in strict server-first mode (stationary fallback): entry=", entry,
                                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
                        } else {
                            LOG_INFO("Auto-spawned transport in server-first mode with entry DBC path: entry=", entry,
                                     " displayId=", displayId, " wmoInstance=", wmoInstanceId);
                        }
                    } else if (!hasUsablePath) {
                        bool allowZOnly = (displayId == 455 || displayId == 462);
                        uint32_t inferredPath = transportManager->inferDbcPathForSpawn(
                            canonicalSpawnPos, 1200.0f, allowZOnly);
                        if (inferredPath != 0) {
                            pathId = inferredPath;
                            LOG_INFO("Auto-spawned transport with inferred path: entry=", entry,
                                     " inferredPath=", pathId, " displayId=", displayId,
                                     " wmoInstance=", wmoInstanceId);
                        } else {
                            uint32_t remappedPath = transportManager->pickFallbackMovingPath(entry, displayId);
                            if (remappedPath != 0) {
                                pathId = remappedPath;
                                LOG_INFO("Auto-spawned transport with remapped fallback path: entry=", entry,
                                         " remappedPath=", pathId, " displayId=", displayId,
                                         " wmoInstance=", wmoInstanceId);
                            } else {
                                std::vector<glm::vec3> path = { canonicalSpawnPos };
                                transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                                LOG_INFO("Auto-spawned transport with stationary path: entry=", entry,
                                         " displayId=", displayId, " wmoInstance=", wmoInstanceId);
                            }
                        }
                    } else {
                        LOG_INFO("Auto-spawned transport with real path: entry=", entry,
                                 " displayId=", displayId, " wmoInstance=", wmoInstanceId);
                    }

                    transportManager->registerTransport(guid, wmoInstanceId, pathId, canonicalSpawnPos, entry);
                    // Keep type in sync with the spawned instance; needed for M2 lift boarding/motion.
                    if (!it->second.isWmo) {
                        if (auto* tr = transportManager->getTransport(guid)) {
                            tr->isM2 = true;
                        }
                    }
                } else {
                    pendingTransportMoves_[guid] = PendingTransportMove{x, y, z, orientation};
                    LOG_DEBUG("Cannot auto-spawn transport 0x", std::hex, guid, std::dec,
                              " - WMO instance not found yet (queued move for replay)");
                    return;
                }
            } else {
                pendingTransportMoves_[guid] = PendingTransportMove{x, y, z, orientation};
                LOG_DEBUG("Cannot auto-spawn transport 0x", std::hex, guid, std::dec,
                          " - entity not found in EntityManager (queued move for replay)");
                return;
            }
        }

        // Update TransportManager's internal state (position, rotation, transform matrices)
        // This also updates the WMO renderer automatically
        // Coordinates are already canonical (converted in game_handler.cpp when entity was created)
        glm::vec3 canonicalPos(x, y, z);
        transportManager->updateServerTransport(guid, canonicalPos, orientation);

        // Move player with transport if riding it
        if (gameHandler && gameHandler->isOnTransport() && gameHandler->getPlayerTransportGuid() == guid && renderer) {
            auto* cc = renderer->getCameraController();
            if (cc) {
                glm::vec3* ft = cc->getFollowTargetMutable();
                if (ft) {
                    // Get player world position from TransportManager (handles transform properly)
                    glm::vec3 offset = gameHandler->getPlayerTransportOffset();
                    glm::vec3 worldPos = transportManager->getPlayerWorldPosition(guid, offset);
                    *ft = worldPos;
                }
            }
        }
    });

    // NPC/player death callback (online mode) - play death animation
    gameHandler->setNpcDeathCallback([this](uint64_t guid) {
        deadCreatureGuids_.insert(guid);
        if (!renderer || !renderer->getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
        else {
            auto pit = playerInstances_.find(guid);
            if (pit != playerInstances_.end()) instanceId = pit->second;
        }
        if (instanceId != 0) {
            renderer->getCharacterRenderer()->playAnimation(instanceId, 1, false); // Death
        }
    });

    // NPC/player respawn callback (online mode) - reset to idle animation
    gameHandler->setNpcRespawnCallback([this](uint64_t guid) {
        deadCreatureGuids_.erase(guid);
        if (!renderer || !renderer->getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
        else {
            auto pit = playerInstances_.find(guid);
            if (pit != playerInstances_.end()) instanceId = pit->second;
        }
        if (instanceId != 0) {
            renderer->getCharacterRenderer()->playAnimation(instanceId, 0, true); // Idle
        }
    });

    // NPC/player swing callback (online mode) - play attack animation
    gameHandler->setNpcSwingCallback([this](uint64_t guid) {
        if (!renderer || !renderer->getCharacterRenderer()) return;
        uint32_t instanceId = 0;
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
        else {
            auto pit = playerInstances_.find(guid);
            if (pit != playerInstances_.end()) instanceId = pit->second;
        }
        if (instanceId != 0) {
            auto* cr = renderer->getCharacterRenderer();
            // Try weapon-appropriate attack anim: 17=1H, 18=2H, 16=unarmed fallback
            static const uint32_t attackAnims[] = {17, 18, 16};
            bool played = false;
            for (uint32_t anim : attackAnims) {
                if (cr->hasAnimation(instanceId, anim)) {
                    cr->playAnimation(instanceId, anim, false);
                    played = true;
                    break;
                }
            }
            if (!played) cr->playAnimation(instanceId, 16, false);
        }
    });

    // Unit animation hint callback — plays jump (38=JumpMid) animation on other players/NPCs.
    // Swim/walking state is now authoritative from the move-flags callback below.
    // animId=38 (JumpMid): airborne jump animation; land detection is via per-frame sync.
    gameHandler->setUnitAnimHintCallback([this](uint64_t guid, uint32_t animId) {
        if (!renderer) return;
        auto* cr = renderer->getCharacterRenderer();
        if (!cr) return;
        uint32_t instanceId = 0;
        {
            auto it = playerInstances_.find(guid);
            if (it != playerInstances_.end()) instanceId = it->second;
        }
        if (instanceId == 0) {
            auto it = creatureInstances_.find(guid);
            if (it != creatureInstances_.end()) instanceId = it->second;
        }
        if (instanceId == 0) return;
        // Don't override Death animation (1)
        uint32_t curAnim = 0; float curT = 0.0f, curDur = 0.0f;
        if (cr->getAnimationState(instanceId, curAnim, curT, curDur) && curAnim == 1) return;
        cr->playAnimation(instanceId, animId, /*loop=*/true);
    });

    // Unit move-flags callback — updates swimming and walking state from every MSG_MOVE_* packet.
    // This is more reliable than opcode-based hints for cold joins and heartbeats:
    // a player already swimming when we join will have SWIMMING set on the first heartbeat.
    // Walking(4) vs Running(5) is also driven here from the WALKING flag.
    gameHandler->setUnitMoveFlagsCallback([this](uint64_t guid, uint32_t moveFlags) {
        const bool isSwimming = (moveFlags & static_cast<uint32_t>(game::MovementFlags::SWIMMING)) != 0;
        const bool isWalking  = (moveFlags & static_cast<uint32_t>(game::MovementFlags::WALKING))  != 0;
        const bool isFlying   = (moveFlags & static_cast<uint32_t>(game::MovementFlags::FLYING))   != 0;
        if (isSwimming) creatureSwimmingState_[guid] = true;
        else            creatureSwimmingState_.erase(guid);
        if (isWalking)  creatureWalkingState_[guid] = true;
        else            creatureWalkingState_.erase(guid);
        if (isFlying)   creatureFlyingState_[guid] = true;
        else            creatureFlyingState_.erase(guid);
    });

    // Emote animation callback — play server-driven emote animations on NPCs and other players
    gameHandler->setEmoteAnimCallback([this](uint64_t guid, uint32_t emoteAnim) {
        if (!renderer || emoteAnim == 0) return;
        auto* cr = renderer->getCharacterRenderer();
        if (!cr) return;
        // Look up creature instance first, then online players
        {
            auto it = creatureInstances_.find(guid);
            if (it != creatureInstances_.end()) {
                cr->playAnimation(it->second, emoteAnim, false);
                return;
            }
        }
        {
            auto it = playerInstances_.find(guid);
            if (it != playerInstances_.end()) {
                cr->playAnimation(it->second, emoteAnim, false);
            }
        }
    });

    // Spell cast animation callback — play cast animation on caster (player or NPC/other player)
    gameHandler->setSpellCastAnimCallback([this](uint64_t guid, bool start, bool /*isChannel*/) {
        if (!renderer) return;
        auto* cr = renderer->getCharacterRenderer();
        if (!cr) return;
        // Animation 3 = SpellCast (one-shot; return-to-idle handled by character_renderer)
        const uint32_t castAnim = 3;
        // Check player character
        {
            uint32_t charInstId = renderer->getCharacterInstanceId();
            if (charInstId != 0 && guid == gameHandler->getPlayerGuid()) {
                if (start) cr->playAnimation(charInstId, castAnim, false);
                // On finish: playAnimation(castAnim, loop=false) will auto-return to Stand
                return;
            }
        }
        // Check creatures and other online players
        {
            auto it = creatureInstances_.find(guid);
            if (it != creatureInstances_.end()) {
                if (start) cr->playAnimation(it->second, castAnim, false);
                return;
            }
        }
        {
            auto it = playerInstances_.find(guid);
            if (it != playerInstances_.end()) {
                if (start) cr->playAnimation(it->second, castAnim, false);
            }
        }
    });

    // Ghost state callback — make player semi-transparent when in spirit form
    gameHandler->setGhostStateCallback([this](bool isGhost) {
        if (!renderer) return;
        auto* cr = renderer->getCharacterRenderer();
        if (!cr) return;
        uint32_t charInstId = renderer->getCharacterInstanceId();
        if (charInstId == 0) return;
        cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
    });

    // Stand state animation callback — map server stand state to M2 animation on player
    // and sync camera sit flag so movement is blocked while sitting
    gameHandler->setStandStateCallback([this](uint8_t standState) {
        if (!renderer) return;

        // Sync camera controller sitting flag: block movement while sitting/kneeling
        if (auto* cc = renderer->getCameraController()) {
            cc->setSitting(standState >= 1 && standState <= 8 && standState != 7);
        }

        auto* cr = renderer->getCharacterRenderer();
        if (!cr) return;
        uint32_t charInstId = renderer->getCharacterInstanceId();
        if (charInstId == 0) return;
        // WoW stand state → M2 animation ID mapping
        // 0=Stand→0, 1-6=Sit variants→27 (SitGround), 7=Dead→1, 8=Kneel→72
        // Do not force Stand(0) here: locomotion state machine already owns standing/running.
        // Forcing Stand on packet timing causes visible run-cycle hitching while steering.
        uint32_t animId = 0;
        if (standState == 0) {
            return;
        } else if (standState >= 1 && standState <= 6) {
            animId = 27;  // SitGround (covers sit-chair too; correct visual differs by chair height)
        } else if (standState == 7) {
            animId = 1;   // Death
        } else if (standState == 8) {
            animId = 72;  // Kneel
        }
        // Loop sit/kneel (not death) so the held-pose frame stays visible
        const bool loop = (animId != 1);
        cr->playAnimation(charInstId, animId, loop);
    });

    // NPC greeting callback - play voice line
    gameHandler->setNpcGreetingCallback([this](uint64_t guid, const glm::vec3& position) {
        if (renderer && renderer->getNpcVoiceManager()) {
            // Convert canonical to render coords for 3D audio
            glm::vec3 renderPos = core::coords::canonicalToRender(position);

            // Detect voice type from NPC display ID
            audio::VoiceType voiceType = audio::VoiceType::GENERIC;
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                uint32_t displayId = unit->getDisplayId();
                voiceType = detectVoiceTypeFromDisplayId(displayId);
            }

            renderer->getNpcVoiceManager()->playGreeting(guid, voiceType, renderPos);
        }
    });

    // NPC farewell callback - play farewell voice line
    gameHandler->setNpcFarewellCallback([this](uint64_t guid, const glm::vec3& position) {
        if (renderer && renderer->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);

            audio::VoiceType voiceType = audio::VoiceType::GENERIC;
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                uint32_t displayId = unit->getDisplayId();
                voiceType = detectVoiceTypeFromDisplayId(displayId);
            }

            renderer->getNpcVoiceManager()->playFarewell(guid, voiceType, renderPos);
        }
    });

    // NPC vendor callback - play vendor voice line
    gameHandler->setNpcVendorCallback([this](uint64_t guid, const glm::vec3& position) {
        if (renderer && renderer->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);

            audio::VoiceType voiceType = audio::VoiceType::GENERIC;
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                uint32_t displayId = unit->getDisplayId();
                voiceType = detectVoiceTypeFromDisplayId(displayId);
            }

            renderer->getNpcVoiceManager()->playVendor(guid, voiceType, renderPos);
        }
    });

    // NPC aggro callback - play combat start voice line
    gameHandler->setNpcAggroCallback([this](uint64_t guid, const glm::vec3& position) {
        if (renderer && renderer->getNpcVoiceManager()) {
            glm::vec3 renderPos = core::coords::canonicalToRender(position);

            audio::VoiceType voiceType = audio::VoiceType::GENERIC;
            auto entity = gameHandler->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                uint32_t displayId = unit->getDisplayId();
                voiceType = detectVoiceTypeFromDisplayId(displayId);
            }

            renderer->getNpcVoiceManager()->playAggro(guid, voiceType, renderPos);
        }
    });

    // "Create Character" button on character screen
    uiManager->getCharacterScreen().setOnCreateCharacter([this]() {
        uiManager->getCharacterCreateScreen().reset();
        // Apply expansion race/class constraints before showing the screen
        if (expansionRegistry_ && expansionRegistry_->getActive()) {
            auto* profile = expansionRegistry_->getActive();
            uiManager->getCharacterCreateScreen().setExpansionConstraints(
                profile->races, profile->classes);
        }
        uiManager->getCharacterCreateScreen().initializePreview(assetManager.get());
        setState(AppState::CHARACTER_CREATION);
    });

    // "Back" button on character screen
    uiManager->getCharacterScreen().setOnBack([this]() {
        // Disconnect from world server and reset UI state for fresh realm selection
        if (gameHandler) {
            gameHandler->disconnect();
        }
        uiManager->getRealmScreen().reset();
        uiManager->getCharacterScreen().reset();
        setState(AppState::REALM_SELECTION);
    });

    // "Delete Character" button on character screen
    uiManager->getCharacterScreen().setOnDeleteCharacter([this](uint64_t guid) {
        if (gameHandler) {
            gameHandler->deleteCharacter(guid);
        }
    });

    // Character delete result callback
    gameHandler->setCharDeleteCallback([this](bool success) {
        if (success) {
            uiManager->getCharacterScreen().setStatus("Character deleted.");
            // Refresh character list
            gameHandler->requestCharacterList();
        } else {
            uint8_t code = gameHandler ? gameHandler->getLastCharDeleteResult() : 0xFF;
            uiManager->getCharacterScreen().setStatus(
                "Delete failed (code " + std::to_string(static_cast<int>(code)) + ").", true);
        }
    });
}

void Application::spawnPlayerCharacter() {
    if (playerCharacterSpawned) return;
    if (!renderer || !renderer->getCharacterRenderer() || !renderer->getCamera()) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    auto* camera = renderer->getCamera();
    bool loaded = false;
    std::string m2Path = getPlayerModelPath();
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

                // Look up textures from CharSections.dbc for all races
                bool useCharSections = true;
                uint32_t targetRaceId = static_cast<uint32_t>(playerRace_);
                uint32_t targetSexId = (playerGender_ == game::Gender::FEMALE) ? 1u : 0u;

                // Race name for fallback texture paths
                const char* raceFolderName = "Human";
                switch (playerRace_) {
                    case game::Race::HUMAN:    raceFolderName = "Human"; break;
                    case game::Race::ORC:      raceFolderName = "Orc"; break;
                    case game::Race::DWARF:    raceFolderName = "Dwarf"; break;
                    case game::Race::NIGHT_ELF: raceFolderName = "NightElf"; break;
                    case game::Race::UNDEAD:    raceFolderName = "Scourge"; break;
                    case game::Race::TAUREN:    raceFolderName = "Tauren"; break;
                    case game::Race::GNOME:     raceFolderName = "Gnome"; break;
                    case game::Race::TROLL:     raceFolderName = "Troll"; break;
                    case game::Race::BLOOD_ELF: raceFolderName = "BloodElf"; break;
                    case game::Race::DRAENEI:   raceFolderName = "Draenei"; break;
                    default: break;
                }
                const char* genderFolder = (playerGender_ == game::Gender::FEMALE) ? "Female" : "Male";
                std::string raceGender = std::string(raceFolderName) + genderFolder;
                std::string bodySkinPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "Skin00_00.blp";
                std::string pelvisPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "NakedPelvisSkin00_00.blp";
                std::string faceLowerTexturePath;
                std::string faceUpperTexturePath;
                std::vector<std::string> underwearPaths;

                // Extract appearance bytes for texture lookups
                uint8_t charSkinId = 0, charFaceId = 0, charHairStyleId = 0, charHairColorId = 0;
                if (gameHandler) {
                    const game::Character* activeChar = gameHandler->getActiveCharacter();
                    if (activeChar) {
                        charSkinId = activeChar->appearanceBytes & 0xFF;
                        charFaceId = (activeChar->appearanceBytes >> 8) & 0xFF;
                        charHairStyleId = (activeChar->appearanceBytes >> 16) & 0xFF;
                        charHairColorId = (activeChar->appearanceBytes >> 24) & 0xFF;
                        LOG_INFO("Appearance: skin=", static_cast<int>(charSkinId), " face=", static_cast<int>(charFaceId),
                                 " hairStyle=", static_cast<int>(charHairStyleId), " hairColor=", static_cast<int>(charHairColorId));
                    }
                }

                std::string hairTexturePath;
                if (useCharSections) {
                    auto charSectionsDbc = assetManager->loadDBC("CharSections.dbc");
                    if (charSectionsDbc) {
                        LOG_INFO("CharSections.dbc loaded: ", charSectionsDbc->getRecordCount(), " records");
                        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);
                        bool foundSkin = false;
                        bool foundUnderwear = false;
                        bool foundFaceLower = false;
                        bool foundHair = false;
                        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
                            uint32_t raceId = charSectionsDbc->getUInt32(r, csF.raceId);
                            uint32_t sexId = charSectionsDbc->getUInt32(r, csF.sexId);
                            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
                            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
                            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

                            if (raceId != targetRaceId || sexId != targetSexId) continue;

                            // Section 0 = skin: match by colorIndex = skin byte
                            if (baseSection == 0 && !foundSkin && colorIndex == charSkinId) {
                                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                                if (!tex1.empty()) {
                                    bodySkinPath = tex1;
                                    foundSkin = true;
                                    LOG_INFO("  DBC body skin: ", bodySkinPath, " (skin=", static_cast<int>(charSkinId), ")");
                                }
                            }
                            // Section 3 = hair: match variation=hairStyle, color=hairColor
                            else if (baseSection == 3 && !foundHair &&
                                     variationIndex == charHairStyleId && colorIndex == charHairColorId) {
                                hairTexturePath = charSectionsDbc->getString(r, csF.texture1);
                                if (!hairTexturePath.empty()) {
                                    foundHair = true;
                                    LOG_INFO("  DBC hair texture: ", hairTexturePath,
                                             " (style=", static_cast<int>(charHairStyleId), " color=", static_cast<int>(charHairColorId), ")");
                                }
                            }
                            // Section 1 = face: match variation=faceId, colorIndex=skinId
                            // Texture1 = face lower, Texture2 = face upper
                            else if (baseSection == 1 && !foundFaceLower &&
                                     variationIndex == charFaceId && colorIndex == charSkinId) {
                                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                                if (!tex1.empty()) {
                                    faceLowerTexturePath = tex1;
                                    LOG_INFO("  DBC face lower: ", faceLowerTexturePath);
                                }
                                if (!tex2.empty()) {
                                    faceUpperTexturePath = tex2;
                                    LOG_INFO("  DBC face upper: ", faceUpperTexturePath);
                                }
                                foundFaceLower = true;
                            }
                            // Section 4 = underwear
                            else if (baseSection == 4 && !foundUnderwear && colorIndex == charSkinId) {
                                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                                    std::string tex = charSectionsDbc->getString(r, f);
                                    if (!tex.empty()) {
                                        underwearPaths.push_back(tex);
                                        LOG_INFO("  DBC underwear texture: ", tex);
                                    }
                                }
                                foundUnderwear = true;
                            }

                            if (foundSkin && foundHair && foundFaceLower && foundUnderwear) break;
                        }

                        if (!foundHair) {
                            LOG_WARNING("No DBC hair match for style=", static_cast<int>(charHairStyleId),
                                        " color=", static_cast<int>(charHairColorId),
                                        " race=", targetRaceId, " sex=", targetSexId);
                        }
                    } else {
                        LOG_WARNING("Failed to load CharSections.dbc, using hardcoded textures");
                    }

                    for (auto& tex : model.textures) {
                        if (tex.type == 1 && tex.filename.empty()) {
                            tex.filename = bodySkinPath;
                        } else if (tex.type == 6) {
                            if (!hairTexturePath.empty()) {
                                tex.filename = hairTexturePath;
                            } else if (tex.filename.empty()) {
                                tex.filename = std::string("Character\\") + raceFolderName + "\\Hair00_00.blp";
                            }
                        } else if (tex.type == 8 && tex.filename.empty()) {
                            if (!underwearPaths.empty()) {
                                tex.filename = underwearPaths[0];
                            } else {
                                tex.filename = pelvisPath;
                            }
                        }
                    }
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

                if (useCharSections) {
                    // Save skin composite state for re-compositing on equipment changes
                    // Include face textures so compositeWithRegions can rebuild the full base
                    bodySkinPath_ = bodySkinPath;
                    underwearPaths_.clear();
                    if (!faceLowerTexturePath.empty()) underwearPaths_.push_back(faceLowerTexturePath);
                    if (!faceUpperTexturePath.empty()) underwearPaths_.push_back(faceUpperTexturePath);
                    for (const auto& up : underwearPaths) underwearPaths_.push_back(up);

                    // Composite body skin + face + underwear overlays
                    {
                        std::vector<std::string> layers;
                        layers.push_back(bodySkinPath);
                        if (!faceLowerTexturePath.empty()) layers.push_back(faceLowerTexturePath);
                        if (!faceUpperTexturePath.empty()) layers.push_back(faceUpperTexturePath);
                        for (const auto& up : underwearPaths) {
                            layers.push_back(up);
                        }
                        if (layers.size() > 1) {
                            rendering::VkTexture* compositeTex = charRenderer->compositeTextures(layers);
                            if (compositeTex != 0) {
                                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                                    if (model.textures[ti].type == 1) {
                                        charRenderer->setModelTexture(1, static_cast<uint32_t>(ti), compositeTex);
                                        skinTextureSlotIndex_ = static_cast<uint32_t>(ti);
                                        LOG_INFO("Replaced type-1 texture slot ", ti, " with composited body+face+underwear");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // Override hair texture on GPU (type-6 slot) after model load
                    if (!hairTexturePath.empty()) {
                        rendering::VkTexture* hairTex = charRenderer->loadTexture(hairTexturePath);
                        if (hairTex) {
                            for (size_t ti = 0; ti < model.textures.size(); ti++) {
                                if (model.textures[ti].type == 6) {
                                    charRenderer->setModelTexture(1, static_cast<uint32_t>(ti), hairTex);
                                    LOG_INFO("Applied DBC hair texture to slot ", ti, ": ", hairTexturePath);
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    bodySkinPath_.clear();
                    underwearPaths_.clear();
                }
                // Find cloak (type-2, Object Skin) texture slot index
                for (size_t ti = 0; ti < model.textures.size(); ti++) {
                    if (model.textures[ti].type == 2) {
                        cloakTextureSlotIndex_ = static_cast<uint32_t>(ti);
                        LOG_INFO("Cloak texture slot: ", ti);
                        break;
                    }
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

	        // Default geosets for the active character (match CharacterPreview logic).
	        // Previous hardcoded values (notably always inserting 101) caused wrong hair meshes in-world.
	        std::unordered_set<uint16_t> activeGeosets;
	        // Body parts (group 0: IDs 0-99, some models use up to 27)
	        for (uint16_t i = 0; i <= 99; i++) activeGeosets.insert(i);

	        uint8_t hairStyleId = 0;
	        uint8_t facialId = 0;
	        if (gameHandler) {
	            if (const game::Character* ch = gameHandler->getActiveCharacter()) {
	                hairStyleId = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
	                facialId = ch->facialFeatures;
	            }
	        }
	        // Hair style geoset: group 1 = 100 + variation + 1
	        activeGeosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1));
	        // Facial hair geoset: group 2 = 200 + variation + 1
	        activeGeosets.insert(static_cast<uint16_t>(200 + facialId + 1));
	        activeGeosets.insert(kGeosetBareForearms);
	        activeGeosets.insert(kGeosetBareShins);
	        activeGeosets.insert(kGeosetDefaultEars);
	        activeGeosets.insert(kGeosetBareSleeves);
	        activeGeosets.insert(kGeosetDefaultKneepads);
	        activeGeosets.insert(kGeosetBarePants);
	        activeGeosets.insert(kGeosetWithCape);
	        activeGeosets.insert(kGeosetBareFeet);
	        // 1703 = DK eye glow mesh — skip for normal characters
	        // Normal eyes are part of the face texture on the body mesh
	        charRenderer->setActiveGeosets(instanceId, activeGeosets);

        // Play idle animation (Stand = animation ID 0)
        charRenderer->playAnimation(instanceId, 0, true);
        LOG_INFO("Spawned player character at (",
                static_cast<int>(spawnPos.x), ", ",
                static_cast<int>(spawnPos.y), ", ",
                static_cast<int>(spawnPos.z), ")");
        playerCharacterSpawned = true;

        // Set voice profile to match character race/gender
        if (auto* asm_ = renderer->getActivitySoundManager()) {
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
        loadEquippedWeapons();
    }
}

bool Application::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    auto m2Data = assetManager->readFile(m2Path);
    if (m2Data.empty()) return false;
    outModel = pipeline::M2Loader::load(m2Data);
    // Load skin (WotLK+ M2 format): strip .m2, append 00.skin
    std::string skinPath = m2Path;
    size_t dotPos = skinPath.rfind('.');
    if (dotPos != std::string::npos) skinPath = skinPath.substr(0, dotPos);
    skinPath += "00.skin";
    auto skinData = assetManager->readFile(skinPath);
    if (!skinData.empty() && outModel.version >= 264)
        pipeline::M2Loader::loadSkin(skinData, outModel);
    return outModel.isValid();
}

void Application::loadEquippedWeapons() {
    if (!renderer || !renderer->getCharacterRenderer() || !assetManager || !assetManager->isInitialized())
        return;
    if (!gameHandler) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    uint32_t charInstanceId = renderer->getCharacterInstanceId();
    if (charInstanceId == 0) return;

    auto& inventory = gameHandler->getInventory();

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) {
        LOG_WARNING("loadEquippedWeapons: failed to load ItemDisplayInfo.dbc");
        return;
    }
    // Mapping: EquipSlot → attachment ID (1=RightHand, 2=LeftHand)
    struct WeaponSlot {
        game::EquipSlot slot;
        uint32_t attachmentId;
    };
    WeaponSlot weaponSlots[] = {
        { game::EquipSlot::MAIN_HAND, 1 },
        { game::EquipSlot::OFF_HAND,  2 },
    };

    if (weaponsSheathed_) {
        for (const auto& ws : weaponSlots) {
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
        }
        return;
    }

    for (const auto& ws : weaponSlots) {
        const auto& equipSlot = inventory.getEquipSlot(ws.slot);

        // If slot is empty or has no displayInfoId, detach any existing weapon
        if (equipSlot.empty() || equipSlot.item.displayInfoId == 0) {
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        uint32_t displayInfoId = equipSlot.item.displayInfoId;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) {
            LOG_WARNING("loadEquippedWeapons: displayInfoId ", displayInfoId, " not found in DBC");
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
        std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModel"] : 1);
        std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);

        if (modelName.empty()) {
            LOG_WARNING("loadEquippedWeapons: empty model name for displayInfoId ", displayInfoId);
            charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
            continue;
        }

        // Convert .mdx → .m2
        std::string modelFile = modelName;
        {
            size_t dotPos = modelFile.rfind('.');
            if (dotPos != std::string::npos) {
                modelFile = modelFile.substr(0, dotPos) + ".m2";
            } else {
                modelFile += ".m2";
            }
        }

        // Try Weapon directory first, then Shield
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                LOG_WARNING("loadEquippedWeapons: failed to load ", modelFile);
                charRenderer->detachWeapon(charInstanceId, ws.attachmentId);
                continue;
            }
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
            }
        }

        uint32_t weaponModelId = nextWeaponModelId_++;
        bool ok = charRenderer->attachWeapon(charInstanceId, ws.attachmentId,
                                              weaponModel, weaponModelId, texturePath);
        if (ok) {
            LOG_INFO("Equipped weapon: ", m2Path, " at attachment ", ws.attachmentId);
        }
    }
}

bool Application::tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId) {
    if (!renderer || !renderer->getCharacterRenderer() || !assetManager || !gameHandler) return false;
    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return false;

    auto entity = gameHandler->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::UNIT) return false;
    auto unit = std::static_pointer_cast<game::Unit>(entity);
    if (!unit) return false;

    // Virtual weapons are only appropriate for humanoid-style displays.
    // Non-humanoids (wolves/boars/etc.) can expose non-zero virtual item fields
    // and otherwise end up with comedic floating weapons.
    uint32_t displayId = unit->getDisplayId();
    auto dIt = displayDataMap_.find(displayId);
    if (dIt == displayDataMap_.end()) return false;
    uint32_t extraDisplayId = dIt->second.extraDisplayId;
    if (extraDisplayId == 0 || humanoidExtraMap_.find(extraDisplayId) == humanoidExtraMap_.end()) {
        return false;
    }

    auto itemDisplayDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!itemDisplayDbc) return false;
    // Item.dbc is not distributed to clients in Vanilla 1.12; on those expansions
    // item display IDs are resolved via the server-sent item cache instead.
    auto itemDbc = assetManager->loadDBCOptional("Item.dbc");
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const auto* itemL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Item") : nullptr;

    auto resolveDisplayInfoId = [&](uint32_t rawId) -> uint32_t {
        if (rawId == 0) return 0;
        // Primary path: AzerothCore uses item entries in UNIT_VIRTUAL_ITEM_SLOT_ID.
        // Resolve strictly through Item.dbc entry -> DisplayID to avoid
        // accidental ItemDisplayInfo ID collisions (staff/hilt mismatches).
        if (itemDbc) {
            int32_t itemRec = itemDbc->findRecordById(rawId); // treat as item entry
            if (itemRec >= 0) {
                const uint32_t dispFieldPrimary = itemL ? (*itemL)["DisplayID"] : 5u;
                uint32_t displayIdA = itemDbc->getUInt32(static_cast<uint32_t>(itemRec), dispFieldPrimary);
                if (displayIdA != 0 && itemDisplayDbc->findRecordById(displayIdA) >= 0) {
                    return displayIdA;
                }
            }
        }
        // Fallback: Vanilla 1.12 does not distribute Item.dbc to clients.
        // Items arrive via SMSG_ITEM_QUERY_SINGLE_RESPONSE and are cached in
        // itemInfoCache_. Use the server-sent displayInfoId when available.
        if (!itemDbc && gameHandler) {
            if (const auto* info = gameHandler->getItemInfo(rawId)) {
                uint32_t displayIdB = info->displayInfoId;
                if (displayIdB != 0 && itemDisplayDbc->findRecordById(displayIdB) >= 0) {
                    return displayIdB;
                }
            }
        }
        return 0;
    };

    auto attachNpcWeaponDisplay = [&](uint32_t itemDisplayId, uint32_t attachmentId) -> bool {
        uint32_t resolvedDisplayId = resolveDisplayInfoId(itemDisplayId);
        if (resolvedDisplayId == 0) return false;
        int32_t recIdx = itemDisplayDbc->findRecordById(resolvedDisplayId);
        if (recIdx < 0) return false;

        const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
        const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
        const uint32_t texFieldL = idiL ? (*idiL)["LeftModelTexture"] : 3u;
        const uint32_t texFieldR = idiL ? (*idiL)["RightModelTexture"] : 4u;
        // Prefer LeftModel (stock player equipment path uses LeftModel and avoids
        // the "hilt-only" variants seen when forcing RightModel).
        std::string modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        std::string textureName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), texFieldL);
        if (modelName.empty()) {
            modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
            textureName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), texFieldR);
        }
        if (modelName.empty()) return false;

        std::string modelFile = modelName;
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
        modelFile += ".m2";

        // Main-hand NPC weapon path: only use actual weapon models.
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) return false;

        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager->fileExists(texturePath)) texturePath.clear();
        }

        uint32_t weaponModelId = nextWeaponModelId_++;
        return charRenderer->attachWeapon(instanceId, attachmentId, weaponModel, weaponModelId, texturePath);
    };

    auto hasResolvableWeaponModel = [&](uint32_t itemDisplayId) -> bool {
        uint32_t resolvedDisplayId = resolveDisplayInfoId(itemDisplayId);
        if (resolvedDisplayId == 0) return false;
        int32_t recIdx = itemDisplayDbc->findRecordById(resolvedDisplayId);
        if (recIdx < 0) return false;
        const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
        const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
        std::string modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        if (modelName.empty()) {
            modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
        }
        if (modelName.empty()) return false;
        std::string modelFile = modelName;
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
        modelFile += ".m2";
        return assetManager->fileExists("Item\\ObjectComponents\\Weapon\\" + modelFile);
    };

    bool attachedMain = false;
    bool hadWeaponCandidate = false;

    const uint16_t candidateBases[] = {56, 57, 58, 70, 148, 149, 150, 151, 152};
    for (uint16_t base : candidateBases) {
        uint32_t v0 = entity->getField(static_cast<uint16_t>(base + 0));
        if (v0 != 0) hadWeaponCandidate = true;
        if (!attachedMain && v0 != 0) attachedMain = attachNpcWeaponDisplay(v0, 1);
        if (attachedMain) break;
    }

    uint16_t unitEnd = game::fieldIndex(game::UF::UNIT_END);
    uint16_t scanLo = 60;
    uint16_t scanHi = (unitEnd != 0xFFFF) ? static_cast<uint16_t>(unitEnd + 96) : 320;
    std::map<uint16_t, uint32_t> candidateByIndex;
    for (const auto& [idx, val] : entity->getFields()) {
        if (idx < scanLo || idx > scanHi) continue;
        if (val == 0) continue;
        if (hasResolvableWeaponModel(val)) {
            candidateByIndex[idx] = val;
            hadWeaponCandidate = true;
        }
    }
    for (const auto& [idx, val] : candidateByIndex) {
        if (!attachedMain) attachedMain = attachNpcWeaponDisplay(val, 1);
        if (attachedMain) break;
    }

    // Force off-hand clear in NPC path to avoid incorrect shields/placeholder hilts.
    charRenderer->detachWeapon(instanceId, 2);
    // Success if main-hand attached when there was at least one candidate.
    return hadWeaponCandidate && attachedMain;
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

void Application::loadOnlineWorldTerrain(uint32_t mapId, float x, float y, float z) {
    if (!renderer || !assetManager || !assetManager->isInitialized()) {
        LOG_WARNING("Cannot load online terrain: renderer or assets not ready");
        return;
    }

    // Guard against re-entrant calls.  The worldEntryCallback defers new
    // entries while this flag is set; we process them at the end.
    loadingWorld_ = true;
    pendingWorldEntry_.reset();

    // --- Loading screen for online mode ---
    rendering::LoadingScreen loadingScreen;
    loadingScreen.setVkContext(window->getVkContext());
    loadingScreen.setSDLWindow(window->getSDLWindow());
    bool loadingScreenOk = loadingScreen.initialize();

    auto showProgress = [&](const char* msg, float progress) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                window->setShouldClose(true);
                loadingScreen.shutdown();
                return;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                int w = event.window.data1;
                int h = event.window.data2;
                window->setSize(w, h);
                // Vulkan viewport set in command buffer
                if (renderer && renderer->getCamera()) {
                    renderer->getCamera()->setAspectRatio(static_cast<float>(w) / h);
                }
            }
        }
        if (!loadingScreenOk) return;
        loadingScreen.setStatus(msg);
        loadingScreen.setProgress(progress);
        loadingScreen.render();
        window->swapBuffers();
    };

    // Set zone name on loading screen — prefer friendly display name, then DBC
    {
        const char* friendly = mapDisplayName(mapId);
        if (friendly) {
            loadingScreen.setZoneName(friendly);
        } else if (gameHandler) {
            std::string dbcName = gameHandler->getMapName(mapId);
            if (!dbcName.empty())
                loadingScreen.setZoneName(dbcName);
            else
                loadingScreen.setZoneName("Loading...");
        }
    }

    showProgress("Entering world...", 0.0f);

    // --- Clean up previous map's state on map change ---
    // (Same cleanup as logout, but preserves player identity and renderer objects.)
    LOG_WARNING("loadOnlineWorldTerrain: mapId=", mapId, " loadedMapId_=", loadedMapId_);
    bool hasRendererData = renderer && (renderer->getWMORenderer() || renderer->getM2Renderer());
    if (loadedMapId_ != 0xFFFFFFFF || hasRendererData) {
        LOG_WARNING("Map change: cleaning up old map ", loadedMapId_, " before loading map ", mapId);

        // Clear pending queues first (these don't touch GPU resources)
        pendingCreatureSpawns_.clear();
        pendingCreatureSpawnGuids_.clear();
        creatureSpawnRetryCounts_.clear();
        pendingPlayerSpawns_.clear();
        pendingPlayerSpawnGuids_.clear();
        pendingOnlinePlayerEquipment_.clear();
        deferredEquipmentQueue_.clear();
        pendingGameObjectSpawns_.clear();
        pendingTransportMoves_.clear();
        pendingTransportRegistrations_.clear();
        pendingTransportDoodadBatches_.clear();

        if (renderer) {
            // Clear all world geometry from old map (including textures/models).
            // WMO clearAll and M2 clear both call vkDeviceWaitIdle internally,
            // ensuring no GPU command buffers reference old resources.
            if (auto* wmo = renderer->getWMORenderer()) {
                wmo->clearAll();
            }
            if (auto* m2 = renderer->getM2Renderer()) {
                m2->clear();
            }

            // Full clear of character renderer: removes all instances, models,
            // textures, and resets descriptor pools.  This prevents stale GPU
            // resources from accumulating across map changes (old creature
            // models, bone buffers, texture descriptor sets) which can cause
            // VK_ERROR_DEVICE_LOST on some drivers.
            if (auto* cr = renderer->getCharacterRenderer()) {
                cr->clear();
                renderer->setCharacterFollow(0);
            }
            // Reset equipment dirty tracking so composited textures are rebuilt
            // after spawnPlayerCharacter() recreates the character instance.
            if (gameHandler) {
                gameHandler->resetEquipmentDirtyTracking();
            }

            if (auto* terrain = renderer->getTerrainManager()) {
                terrain->softReset();
                terrain->setStreamingEnabled(true);  // Re-enable in case previous map disabled it
            }
            if (auto* questMarkers = renderer->getQuestMarkerRenderer()) {
                questMarkers->clear();
            }
            renderer->clearMount();
        }

        // Clear application-level instance tracking (after renderer cleanup)
        creatureInstances_.clear();
        creatureModelIds_.clear();
        creatureRenderPosCache_.clear();
        creatureWeaponsAttached_.clear();
        creatureWeaponAttachAttempts_.clear();
        deadCreatureGuids_.clear();
        nonRenderableCreatureDisplayIds_.clear();
        creaturePermanentFailureGuids_.clear();
        modelIdIsWolfLike_.clear();
        displayIdTexturesApplied_.clear();
        charSectionsCache_.clear();
        charSectionsCacheBuilt_ = false;
        for (auto& load : asyncCreatureLoads_) {
            if (load.future.valid()) load.future.wait();
        }
        asyncCreatureLoads_.clear();
        asyncCreatureDisplayLoads_.clear();

        playerInstances_.clear();
        onlinePlayerAppearance_.clear();

        gameObjectInstances_.clear();
        gameObjectDisplayIdModelCache_.clear();
        gameObjectDisplayIdWmoCache_.clear();
        gameObjectDisplayIdFailedCache_.clear();

        // Force player character re-spawn on new map
        playerCharacterSpawned = false;
    }

    // Resolve map folder name from Map.dbc (authoritative for world/instance maps).
    // This is required for instances like DeeprunTram (map 369) that are not Azeroth/Kalimdor.
    if (!mapNameCacheLoaded_ && assetManager) {
        mapNameCacheLoaded_ = true;
        if (auto mapDbc = assetManager->loadDBC("Map.dbc"); mapDbc && mapDbc->isLoaded()) {
            mapNameById_.reserve(mapDbc->getRecordCount());
            const auto* mapL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Map") : nullptr;
            for (uint32_t i = 0; i < mapDbc->getRecordCount(); i++) {
                uint32_t id = mapDbc->getUInt32(i, mapL ? (*mapL)["ID"] : 0);
                std::string internalName = mapDbc->getString(i, mapL ? (*mapL)["InternalName"] : 1);
                if (!internalName.empty() && mapNameById_.find(id) == mapNameById_.end()) {
                    mapNameById_[id] = std::move(internalName);
                }
            }
            LOG_INFO("Loaded Map.dbc map-name cache: ", mapNameById_.size(), " entries");
        } else {
            LOG_WARNING("Map.dbc not available; using fallback map-id mapping");
        }
    }

    std::string mapName;
    if (auto it = mapNameById_.find(mapId); it != mapNameById_.end()) {
        mapName = it->second;
    } else {
        mapName = mapIdToName(mapId);
    }
    if (mapName.empty()) {
        LOG_WARNING("Unknown mapId ", mapId, " (no Map.dbc entry); falling back to Azeroth");
        mapName = "Azeroth";
    }
    LOG_INFO("Loading online world terrain for map '", mapName, "' (ID ", mapId, ")");

    // Cancel any stale preload (if it was for a different map, the file cache
    // still retains whatever was loaded — it doesn't hurt).
    if (worldPreload_) {
        if (worldPreload_->mapId == mapId) {
            LOG_INFO("World preload: cache-warm hit for map '", mapName, "'");
        } else {
            LOG_INFO("World preload: map mismatch (preloaded ", worldPreload_->mapName,
                     ", entering ", mapName, ")");
        }
    }
    cancelWorldPreload();

    // Save this world info for next session's early preload
    saveLastWorldInfo(mapId, mapName, x, y);

    // Convert server coordinates to canonical WoW coordinates
    // Server sends: X=West (canonical.Y), Y=North (canonical.X), Z=Up
    glm::vec3 spawnCanonical = core::coords::serverToCanonical(glm::vec3(x, y, z));
    glm::vec3 spawnRender = core::coords::canonicalToRender(spawnCanonical);

    // Set camera position and facing from server orientation
    if (renderer->getCameraController()) {
        float yawDeg = 0.0f;
        if (gameHandler) {
            float canonicalYaw = gameHandler->getMovementInfo().orientation;
            yawDeg = 180.0f - glm::degrees(canonicalYaw);
        }
        renderer->getCameraController()->setOnlineMode(true);
        renderer->getCameraController()->setDefaultSpawn(spawnRender, yawDeg, -15.0f);
        renderer->getCameraController()->reset();
    }

    // Set map name for WMO renderer and reset instance mode
    if (renderer->getWMORenderer()) {
        renderer->getWMORenderer()->setMapName(mapName);
        renderer->getWMORenderer()->setWMOOnlyMap(false);
    }

    // Set map name for terrain manager
    if (renderer->getTerrainManager()) {
        renderer->getTerrainManager()->setMapName(mapName);
    }

    // NOTE: TransportManager renderer connection moved to after initializeRenderers (later in this function)

    // Connect WMORenderer to M2Renderer (for hierarchical transforms: doodads following WMO parents)
    if (renderer->getWMORenderer() && renderer->getM2Renderer()) {
        renderer->getWMORenderer()->setM2Renderer(renderer->getM2Renderer());
        LOG_INFO("WMORenderer connected to M2Renderer for hierarchical doodad transforms");
    }

    showProgress("Loading character model...", 0.05f);

    // Build faction hostility map for this character's race
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            buildFactionHostilityMap(static_cast<uint8_t>(activeChar->race));
        }
    }

    // Spawn player model for online mode (skip if already spawned, e.g. teleport)
    if (gameHandler) {
        const game::Character* activeChar = gameHandler->getActiveCharacter();
        if (activeChar) {
            const uint64_t activeGuid = gameHandler->getActiveCharacterGuid();
            const bool appearanceChanged =
                (activeGuid != spawnedPlayerGuid_) ||
                (activeChar->appearanceBytes != spawnedAppearanceBytes_) ||
                (activeChar->facialFeatures != spawnedFacialFeatures_) ||
                (activeChar->race != playerRace_) ||
                (activeChar->gender != playerGender_) ||
                (activeChar->characterClass != playerClass_);

            if (!playerCharacterSpawned || appearanceChanged) {
                if (appearanceChanged) {
                    LOG_INFO("Respawning player model for new/changed character: guid=0x",
                             std::hex, activeGuid, std::dec);
                }
                // Remove old instance so we don't keep stale visuals.
                if (renderer && renderer->getCharacterRenderer()) {
                    uint32_t oldInst = renderer->getCharacterInstanceId();
                    if (oldInst > 0) {
                        renderer->setCharacterFollow(0);
                        renderer->clearMount();
                        renderer->getCharacterRenderer()->removeInstance(oldInst);
                    }
                }
                playerCharacterSpawned = false;
                spawnedPlayerGuid_ = 0;
                spawnedAppearanceBytes_ = 0;
                spawnedFacialFeatures_ = 0;

                playerRace_ = activeChar->race;
                playerGender_ = activeChar->gender;
                playerClass_ = activeChar->characterClass;
                spawnSnapToGround = false;
                weaponsSheathed_ = false;
                loadEquippedWeapons(); // will no-op until instance exists
                spawnPlayerCharacter();
            }
            renderer->getCharacterPosition() = spawnRender;
            LOG_INFO("Online player at render pos (", spawnRender.x, ", ", spawnRender.y, ", ", spawnRender.z, ")");
        } else {
            LOG_WARNING("No active character found for player model spawning");
        }
    }

    showProgress("Loading terrain...", 0.20f);

    // Check WDT to detect WMO-only maps (dungeons, raids, BGs)
    bool isWMOOnlyMap = false;
    pipeline::WDTInfo wdtInfo;
    {
        std::string wdtPath = "World\\Maps\\" + mapName + "\\" + mapName + ".wdt";
        LOG_WARNING("Reading WDT: ", wdtPath);
        std::vector<uint8_t> wdtData = assetManager->readFile(wdtPath);
        if (!wdtData.empty()) {
            wdtInfo = pipeline::parseWDT(wdtData);
            isWMOOnlyMap = wdtInfo.isWMOOnly() && !wdtInfo.rootWMOPath.empty();
            LOG_WARNING("WDT result: isWMOOnly=", isWMOOnlyMap, " rootWMO='", wdtInfo.rootWMOPath, "'");
        } else {
            LOG_WARNING("No WDT file found at ", wdtPath);
        }
    }

    bool terrainOk = false;

    if (isWMOOnlyMap) {
        // ---- WMO-only map (dungeon/raid/BG): load root WMO directly ----
        LOG_WARNING("WMO-only map detected — loading root WMO: ", wdtInfo.rootWMOPath);
        showProgress("Loading instance geometry...", 0.25f);

        // Initialize renderers if they don't exist yet (first login to a WMO-only map).
        // On map change, renderers already exist from the previous map.
        if (!renderer->getWMORenderer() || !renderer->getTerrainManager()) {
            renderer->initializeRenderers(assetManager.get(), mapName);
        }

        // Set map name on WMO renderer and disable terrain streaming (no ADT tiles for instances)
        if (renderer->getWMORenderer()) {
            renderer->getWMORenderer()->setMapName(mapName);
            renderer->getWMORenderer()->setWMOOnlyMap(true);
        }
        if (renderer->getTerrainManager()) {
            renderer->getTerrainManager()->setStreamingEnabled(false);
        }

        // Spawn player character now that renderers are initialized
        if (!playerCharacterSpawned) {
            spawnPlayerCharacter();
            loadEquippedWeapons();
        }

        // Load the root WMO
        auto* wmoRenderer = renderer->getWMORenderer();
        LOG_WARNING("WMO-only: wmoRenderer=", (wmoRenderer ? "valid" : "NULL"));
        if (wmoRenderer) {
            LOG_WARNING("WMO-only: reading root WMO file: ", wdtInfo.rootWMOPath);
            std::vector<uint8_t> wmoData = assetManager->readFile(wdtInfo.rootWMOPath);
            LOG_WARNING("WMO-only: root WMO data size=", wmoData.size());
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_WARNING("WMO-only: parsed WMO model, nGroups=", wmoModel.nGroups);

                if (wmoModel.nGroups > 0) {
                    showProgress("Loading instance groups...", 0.35f);
                    std::string basePath = wdtInfo.rootWMOPath;
                    std::string extension;
                    if (basePath.size() > 4) {
                        extension = basePath.substr(basePath.size() - 4);
                        std::string extLower = extension;
                        for (char& c : extLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (extLower == ".wmo") {
                            basePath = basePath.substr(0, basePath.size() - 4);
                        }
                    }

                    uint32_t loadedGroups = 0;
                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        char groupSuffix[16];
                        snprintf(groupSuffix, sizeof(groupSuffix), "_%03u%s", gi, extension.c_str());
                        std::string groupPath = basePath + groupSuffix;
                        std::vector<uint8_t> groupData = assetManager->readFile(groupPath);
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
                            groupData = assetManager->readFile(basePath + groupSuffix);
                        }
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.WMO", gi);
                            groupData = assetManager->readFile(basePath + groupSuffix);
                        }
                        if (!groupData.empty()) {
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                        }

                        // Update loading progress
                        if (wmoModel.nGroups > 1) {
                            float groupProgress = 0.35f + 0.30f * static_cast<float>(gi + 1) / wmoModel.nGroups;
                            char buf[128];
                            snprintf(buf, sizeof(buf), "Loading instance groups... %u / %u", gi + 1, wmoModel.nGroups);
                            showProgress(buf, groupProgress);
                        }
                    }

                    LOG_INFO("Loaded ", loadedGroups, " / ", wmoModel.nGroups, " WMO groups for instance");
                }

                // WMO-only maps: MODF uses same format as ADT MODF.
                // Apply the same rotation conversion that outdoor WMOs get
                // (including the implicit +180° Z yaw), but skip the ZEROPOINT
                // position offset for zero-position instances (server sends
                // coordinates relative to the WMO, not relative to map corner).
                glm::vec3 wmoPos(0.0f);
                glm::vec3 wmoRot(
                    -wdtInfo.rotation[2] * 3.14159f / 180.0f,
                    -wdtInfo.rotation[0] * 3.14159f / 180.0f,
                    (wdtInfo.rotation[1] + 180.0f) * 3.14159f / 180.0f
                );
                if (wdtInfo.position[0] != 0.0f || wdtInfo.position[1] != 0.0f || wdtInfo.position[2] != 0.0f) {
                    wmoPos = core::coords::adtToWorld(
                        wdtInfo.position[0], wdtInfo.position[1], wdtInfo.position[2]);
                }

                showProgress("Uploading instance geometry...", 0.70f);
                uint32_t wmoModelId = 900000 + mapId;  // Unique ID range for instance WMOs
                if (wmoRenderer->loadModel(wmoModel, wmoModelId)) {
                    uint32_t instanceId = wmoRenderer->createInstance(wmoModelId, wmoPos, wmoRot, 1.0f);
                    if (instanceId > 0) {
                        LOG_WARNING("Instance WMO loaded: modelId=", wmoModelId,
                                " instanceId=", instanceId);
                        LOG_WARNING("  MOHD bbox local: (",
                                   wmoModel.boundingBoxMin.x, ", ", wmoModel.boundingBoxMin.y, ", ", wmoModel.boundingBoxMin.z,
                                   ") to (", wmoModel.boundingBoxMax.x, ", ", wmoModel.boundingBoxMax.y, ", ", wmoModel.boundingBoxMax.z, ")");
                        LOG_WARNING("  WMO pos: (", wmoPos.x, ", ", wmoPos.y, ", ", wmoPos.z,
                                   ") rot: (", wmoRot.x, ", ", wmoRot.y, ", ", wmoRot.z, ")");
                        LOG_WARNING("  Player render pos: (", spawnRender.x, ", ", spawnRender.y, ", ", spawnRender.z, ")");
                        LOG_WARNING("  Player canonical: (", spawnCanonical.x, ", ", spawnCanonical.y, ", ", spawnCanonical.z, ")");
                        // Show player position in WMO local space
                        {
                            glm::mat4 instMat(1.0f);
                            instMat = glm::translate(instMat, wmoPos);
                            instMat = glm::rotate(instMat, wmoRot.z, glm::vec3(0,0,1));
                            instMat = glm::rotate(instMat, wmoRot.y, glm::vec3(0,1,0));
                            instMat = glm::rotate(instMat, wmoRot.x, glm::vec3(1,0,0));
                            glm::mat4 invMat = glm::inverse(instMat);
                            glm::vec3 localPlayer = glm::vec3(invMat * glm::vec4(spawnRender, 1.0f));
                            LOG_WARNING("  Player in WMO local: (", localPlayer.x, ", ", localPlayer.y, ", ", localPlayer.z, ")");
                            bool inside = localPlayer.x >= wmoModel.boundingBoxMin.x && localPlayer.x <= wmoModel.boundingBoxMax.x &&
                                          localPlayer.y >= wmoModel.boundingBoxMin.y && localPlayer.y <= wmoModel.boundingBoxMax.y &&
                                          localPlayer.z >= wmoModel.boundingBoxMin.z && localPlayer.z <= wmoModel.boundingBoxMax.z;
                            LOG_WARNING("  Player inside MOHD bbox: ", inside ? "YES" : "NO");
                        }

                        // Load doodads from the specified doodad set
                        auto* m2Renderer = renderer->getM2Renderer();
                        if (m2Renderer && !wmoModel.doodadSets.empty() && !wmoModel.doodads.empty()) {
                            uint32_t setIdx = std::min(static_cast<uint32_t>(wdtInfo.doodadSet),
                                                       static_cast<uint32_t>(wmoModel.doodadSets.size() - 1));
                            const auto& doodadSet = wmoModel.doodadSets[setIdx];

                            showProgress("Loading instance doodads...", 0.75f);
                            glm::mat4 wmoMatrix(1.0f);
                            wmoMatrix = glm::translate(wmoMatrix, wmoPos);
                            wmoMatrix = glm::rotate(wmoMatrix, wmoRot.z, glm::vec3(0, 0, 1));
                            wmoMatrix = glm::rotate(wmoMatrix, wmoRot.y, glm::vec3(0, 1, 0));
                            wmoMatrix = glm::rotate(wmoMatrix, wmoRot.x, glm::vec3(1, 0, 0));

                            uint32_t loadedDoodads = 0;
                            for (uint32_t di = 0; di < doodadSet.count; di++) {
                                uint32_t doodadIdx = doodadSet.startIndex + di;
                                if (doodadIdx >= wmoModel.doodads.size()) break;

                                const auto& doodad = wmoModel.doodads[doodadIdx];
                                auto nameIt = wmoModel.doodadNames.find(doodad.nameIndex);
                                if (nameIt == wmoModel.doodadNames.end()) continue;

                                std::string m2Path = nameIt->second;
                                if (m2Path.empty()) continue;

                                if (m2Path.size() > 4) {
                                    std::string ext = m2Path.substr(m2Path.size() - 4);
                                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (ext == ".mdx" || ext == ".mdl") {
                                        m2Path = m2Path.substr(0, m2Path.size() - 4) + ".m2";
                                    }
                                }

                                std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
                                if (m2Data.empty()) continue;

                                pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
                                if (m2Model.name.empty()) m2Model.name = m2Path;

                                std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
                                std::vector<uint8_t> skinData = assetManager->readFile(skinPath);
                                if (!skinData.empty() && m2Model.version >= 264) {
                                    pipeline::M2Loader::loadSkin(skinData, m2Model);
                                }
                                if (!m2Model.isValid()) continue;

                                glm::quat fixedRotation(doodad.rotation.w, doodad.rotation.x,
                                                        doodad.rotation.y, doodad.rotation.z);
                                glm::mat4 doodadLocal(1.0f);
                                doodadLocal = glm::translate(doodadLocal, doodad.position);
                                doodadLocal *= glm::mat4_cast(fixedRotation);
                                doodadLocal = glm::scale(doodadLocal, glm::vec3(doodad.scale));

                                glm::mat4 worldMatrix = wmoMatrix * doodadLocal;
                                glm::vec3 worldPos = glm::vec3(worldMatrix[3]);

                                uint32_t doodadModelId = static_cast<uint32_t>(std::hash<std::string>{}(m2Path));
                                if (!m2Renderer->loadModel(m2Model, doodadModelId)) continue;
                                uint32_t doodadInstId = m2Renderer->createInstanceWithMatrix(doodadModelId, worldMatrix, worldPos);
                                if (doodadInstId) m2Renderer->setSkipCollision(doodadInstId, true);
                                loadedDoodads++;
                            }
                            LOG_INFO("Loaded ", loadedDoodads, " instance WMO doodads");
                        }
                    } else {
                        LOG_WARNING("Failed to create instance WMO instance");
                    }
                } else {
                    LOG_WARNING("Failed to load instance WMO model");
                }
            } else {
                LOG_WARNING("Failed to read root WMO file: ", wdtInfo.rootWMOPath);
            }

            // Build collision cache for the instance WMO
            showProgress("Building collision cache...", 0.88f);
            if (loadingScreenOk) { loadingScreen.render(); window->swapBuffers(); }
            wmoRenderer->loadFloorCache();
            if (wmoRenderer->getFloorCacheSize() == 0) {
                showProgress("Computing walkable surfaces...", 0.90f);
                if (loadingScreenOk) { loadingScreen.render(); window->swapBuffers(); }
                wmoRenderer->precomputeFloorCache();
            }
        }

        // Snap player to WMO floor so they don't fall through on first frame
        if (wmoRenderer && renderer) {
            glm::vec3 playerPos = renderer->getCharacterPosition();
            // Query floor with generous height margin above spawn point
            auto floor = wmoRenderer->getFloorHeight(playerPos.x, playerPos.y, playerPos.z + 50.0f);
            if (floor) {
                playerPos.z = *floor + 0.1f;  // Small offset above floor
                renderer->getCharacterPosition() = playerPos;
                if (gameHandler) {
                    glm::vec3 canonical = core::coords::renderToCanonical(playerPos);
                    gameHandler->setPosition(canonical.x, canonical.y, canonical.z);
                }
                LOG_INFO("Snapped player to instance WMO floor: z=", *floor);
            } else {
                LOG_WARNING("Could not find WMO floor at player spawn (",
                           playerPos.x, ", ", playerPos.y, ", ", playerPos.z, ")");
            }
        }

        // Diagnostic: verify WMO renderer state after instance loading
        LOG_WARNING("=== INSTANCE WMO LOAD COMPLETE ===");
        LOG_WARNING("  wmoRenderer models loaded: ", wmoRenderer->getLoadedModelCount());
        LOG_WARNING("  wmoRenderer instances: ", wmoRenderer->getInstanceCount());
        LOG_WARNING("  wmoRenderer floor cache: ", wmoRenderer->getFloorCacheSize());

        terrainOk = true;  // Mark as OK so post-load setup runs
    } else {
        // ---- Normal ADT-based map ----
        // Compute ADT tile from canonical coordinates
        auto [tileX, tileY] = core::coords::canonicalToTile(spawnCanonical.x, spawnCanonical.y);
        std::string adtPath = "World\\Maps\\" + mapName + "\\" + mapName + "_" +
                              std::to_string(tileX) + "_" + std::to_string(tileY) + ".adt";
        LOG_INFO("Loading ADT tile [", tileX, ",", tileY, "] from canonical (",
                 spawnCanonical.x, ", ", spawnCanonical.y, ", ", spawnCanonical.z, ")");

        // Load the initial terrain tile
        terrainOk = renderer->loadTestTerrain(assetManager.get(), adtPath);
        if (!terrainOk) {
            LOG_WARNING("Could not load terrain for online world - atmospheric rendering only");
        } else {
            LOG_INFO("Online world terrain loading initiated");
        }

        // Set map name on WMO renderer (initializeRenderers handles terrain/minimap/worldMap)
        if (renderer->getWMORenderer()) {
            renderer->getWMORenderer()->setMapName(mapName);
        }

        // Character renderer is created inside loadTestTerrain(), so spawn the
        // player model now that the renderer actually exists.
        if (!playerCharacterSpawned) {
            spawnPlayerCharacter();
            loadEquippedWeapons();
        }

        showProgress("Streaming terrain tiles...", 0.35f);

        // Wait for surrounding terrain tiles to stream in
        if (terrainOk && renderer->getTerrainManager() && renderer->getCamera()) {
            auto* terrainMgr = renderer->getTerrainManager();
            auto* camera = renderer->getCamera();

            // Use a small radius for the initial load (just immediate tiles),
            // then restore the full radius after entering the game.
            // This matches WoW's behavior: load quickly, stream the rest in-game.
            const int savedLoadRadius = 4;
            terrainMgr->setLoadRadius(3);   // 7x7=49 tiles — prevents hitches on spawn
            terrainMgr->setUnloadRadius(7);

            // Trigger tile streaming for surrounding area
            terrainMgr->update(*camera, 1.0f);

            auto startTime = std::chrono::high_resolution_clock::now();
            auto lastProgressTime = startTime;
            const float maxWaitSeconds = 60.0f;
            const float stallSeconds = 10.0f;
            int initialRemaining = terrainMgr->getRemainingTileCount();
            if (initialRemaining < 1) initialRemaining = 1;
            int lastRemaining = initialRemaining;

            // Wait until all pending + ready-queue tiles are finalized
            while (terrainMgr->getRemainingTileCount() > 0) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) {
                        window->setShouldClose(true);
                        loadingScreen.shutdown();
                        return;
                    }
                    if (event.type == SDL_WINDOWEVENT &&
                        event.window.event == SDL_WINDOWEVENT_RESIZED) {
                        int w = event.window.data1;
                        int h = event.window.data2;
                        window->setSize(w, h);
                        // Vulkan viewport set in command buffer
                        if (renderer->getCamera()) {
                            renderer->getCamera()->setAspectRatio(static_cast<float>(w) / h);
                        }
                    }
                }

                // Trigger new streaming — enqueue tiles for background workers
                terrainMgr->update(*camera, 0.016f);

                // Process ONE tile per iteration so the progress bar updates
                // smoothly between tiles instead of stalling on large batches.
                terrainMgr->processOneReadyTile();

                int remaining = terrainMgr->getRemainingTileCount();
                int loaded = terrainMgr->getLoadedTileCount();
                int total = loaded + remaining;
                if (total < 1) total = 1;
                float tileProgress = static_cast<float>(loaded) / static_cast<float>(total);
                float progress = 0.35f + tileProgress * 0.50f;

                auto now = std::chrono::high_resolution_clock::now();
                float elapsedSec = std::chrono::duration<float>(now - startTime).count();

                char buf[192];
                if (loaded > 0 && remaining > 0) {
                    float tilesPerSec = static_cast<float>(loaded) / std::max(elapsedSec, 0.1f);
                    float etaSec = static_cast<float>(remaining) / std::max(tilesPerSec, 0.1f);
                    snprintf(buf, sizeof(buf), "Loading terrain... %d / %d tiles (%.0f tiles/s, ~%.0fs remaining)",
                             loaded, total, tilesPerSec, etaSec);
                } else {
                    snprintf(buf, sizeof(buf), "Loading terrain... %d / %d tiles",
                             loaded, total);
                }

                if (loadingScreenOk) {
                    loadingScreen.setStatus(buf);
                    loadingScreen.setProgress(progress);
                    loadingScreen.render();
                    window->swapBuffers();
                }

                if (remaining != lastRemaining) {
                    lastRemaining = remaining;
                    lastProgressTime = now;
                }

                auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
                if (std::chrono::duration<float>(elapsed).count() > maxWaitSeconds) {
                    LOG_WARNING("Online terrain streaming timeout after ", maxWaitSeconds, "s");
                    break;
                }
                auto stalledFor = std::chrono::high_resolution_clock::now() - lastProgressTime;
                if (std::chrono::duration<float>(stalledFor).count() > stallSeconds) {
                    LOG_WARNING("Online terrain streaming stalled for ", stallSeconds,
                                "s (remaining=", lastRemaining, "), continuing without full preload");
                    break;
                }

                // Don't sleep if there are more tiles to finalize — keep processing
                if (remaining > 0 && terrainMgr->getReadyQueueCount() == 0) {
                    SDL_Delay(16);
                }
            }

            LOG_INFO("Online terrain streaming complete: ", terrainMgr->getLoadedTileCount(), " tiles loaded");

            // Restore full load radius — remaining tiles stream in-game
            terrainMgr->setLoadRadius(savedLoadRadius);

            // Load/precompute collision cache
            if (renderer->getWMORenderer()) {
                showProgress("Building collision cache...", 0.88f);
                if (loadingScreenOk) { loadingScreen.render(); window->swapBuffers(); }
                renderer->getWMORenderer()->loadFloorCache();
                if (renderer->getWMORenderer()->getFloorCacheSize() == 0) {
                    showProgress("Computing walkable surfaces...", 0.90f);
                    if (loadingScreenOk) { loadingScreen.render(); window->swapBuffers(); }
                    renderer->getWMORenderer()->precomputeFloorCache();
                }
            }
        }
    }

    // Snap player to loaded terrain so they don't spawn underground
    if (renderer->getCameraController()) {
        renderer->getCameraController()->reset();
    }

    // Test transport disabled — real transports come from server via UPDATEFLAG_TRANSPORT
    showProgress("Finalizing world...", 0.94f);
    // setupTestTransport();

    // Connect TransportManager to renderers (must happen AFTER initializeRenderers)
    if (gameHandler && gameHandler->getTransportManager()) {
        auto* tm = gameHandler->getTransportManager();
        if (renderer->getWMORenderer()) tm->setWMORenderer(renderer->getWMORenderer());
        if (renderer->getM2Renderer()) tm->setM2Renderer(renderer->getM2Renderer());
        LOG_WARNING("TransportManager connected: wmoR=", (renderer->getWMORenderer() ? "yes" : "NULL"),
                   " m2R=", (renderer->getM2Renderer() ? "yes" : "NULL"));
    }

    // Set up NPC animation callbacks (for online creatures)
    showProgress("Preparing creatures...", 0.97f);
    if (gameHandler && renderer && renderer->getCharacterRenderer()) {
        auto* cr = renderer->getCharacterRenderer();
        auto* app = this;

        gameHandler->setNpcDeathCallback([cr, app](uint64_t guid) {
            app->deadCreatureGuids_.insert(guid);
            uint32_t instanceId = 0;
            auto it = app->creatureInstances_.find(guid);
            if (it != app->creatureInstances_.end()) instanceId = it->second;
            else {
                auto pit = app->playerInstances_.find(guid);
                if (pit != app->playerInstances_.end()) instanceId = pit->second;
            }
            if (instanceId != 0 && cr) {
                cr->playAnimation(instanceId, 1, false); // animation ID 1 = Death
            }
        });

        gameHandler->setNpcRespawnCallback([cr, app](uint64_t guid) {
            app->deadCreatureGuids_.erase(guid);
            uint32_t instanceId = 0;
            auto it = app->creatureInstances_.find(guid);
            if (it != app->creatureInstances_.end()) instanceId = it->second;
            else {
                auto pit = app->playerInstances_.find(guid);
                if (pit != app->playerInstances_.end()) instanceId = pit->second;
            }
            if (instanceId != 0 && cr) {
                cr->playAnimation(instanceId, 0, true); // animation ID 0 = Idle
            }
        });

        gameHandler->setNpcSwingCallback([cr, app](uint64_t guid) {
            uint32_t instanceId = 0;
            auto it = app->creatureInstances_.find(guid);
            if (it != app->creatureInstances_.end()) instanceId = it->second;
            else {
                auto pit = app->playerInstances_.find(guid);
                if (pit != app->playerInstances_.end()) instanceId = pit->second;
            }
            if (instanceId != 0 && cr) {
                cr->playAnimation(instanceId, 16, false); // animation ID 16 = Attack1
            }
        });
    }

    // Keep the loading screen visible until all spawn/equipment/gameobject queues
    // are fully drained. This ensures the player sees a fully populated world
    // (character clothed, NPCs placed, game objects loaded) when the screen drops.
    {
        const float kMinWarmupSeconds = 2.0f;   // minimum time to drain network packets
        const float kMaxWarmupSeconds = 25.0f;  // hard cap to avoid infinite stall
        const auto warmupStart = std::chrono::high_resolution_clock::now();
        // Track consecutive idle iterations (all queues empty) to detect convergence
        int idleIterations = 0;
        const int kIdleThreshold = 5;  // require 5 consecutive empty loops (~80ms)

        while (true) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    window->setShouldClose(true);
                    if (loadingScreenOk) loadingScreen.shutdown();
                    return;
                }
                if (event.type == SDL_WINDOWEVENT &&
                    event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int w = event.window.data1;
                    int h = event.window.data2;
                    window->setSize(w, h);
                    if (renderer && renderer->getCamera()) {
                        renderer->getCamera()->setAspectRatio(static_cast<float>(w) / h);
                    }
                }
            }

            // Drain network and process deferred spawn/composite queues while hidden.
            if (gameHandler) gameHandler->update(1.0f / 60.0f);

            // If a new world entry was deferred during packet processing,
            // stop warming up this map — we'll load the new one after cleanup.
            if (pendingWorldEntry_) {
                LOG_WARNING("loadOnlineWorldTerrain(map ", mapId,
                            ") — deferred world entry pending, stopping warmup");
                break;
            }

            if (world) world->update(1.0f / 60.0f);
            processPlayerSpawnQueue();

            // Keep warmup bounded: unbounded queue draining can stall the main thread
            // long enough to trigger socket timeouts.
            processCreatureSpawnQueue(false);
            processAsyncNpcCompositeResults(false);
            // Process equipment queue with a small bounded burst during warmup.
            for (int i = 0; i < 2 && (!deferredEquipmentQueue_.empty() || !asyncEquipmentLoads_.empty()); i++) {
                processDeferredEquipmentQueue();
            }
            if (auto* cr = renderer ? renderer->getCharacterRenderer() : nullptr) {
                cr->processPendingNormalMaps(4);
            }

            // Keep warmup responsive: process gameobject queue with the same bounded
            // budget logic used in-world instead of draining everything in one tick.
            processGameObjectSpawnQueue();

            processPendingTransportRegistrations();
            processPendingTransportDoodads();
            processPendingMount();
            updateQuestMarkers();

            // Update renderer (terrain streaming, animations)
            if (renderer) {
                renderer->update(1.0f / 60.0f);
            }

            const auto now = std::chrono::high_resolution_clock::now();
            const float elapsed = std::chrono::duration<float>(now - warmupStart).count();

            // Check if all queues are drained
            bool queuesEmpty =
                pendingCreatureSpawns_.empty() &&
                asyncCreatureLoads_.empty() &&
                asyncNpcCompositeLoads_.empty() &&
                deferredEquipmentQueue_.empty() &&
                asyncEquipmentLoads_.empty() &&
                pendingGameObjectSpawns_.empty() &&
                asyncGameObjectLoads_.empty() &&
                pendingPlayerSpawns_.empty();

            if (queuesEmpty) {
                idleIterations++;
            } else {
                idleIterations = 0;
            }

            // Don't exit warmup until the ground under the player exists.
            // In cities like Stormwind, players stand on WMO floors, not terrain.
            // Check BOTH terrain AND WMO floor — require at least one to be valid.
            bool groundReady = false;
            if (renderer) {
                glm::vec3 renderSpawn = core::coords::canonicalToRender(
                    glm::vec3(x, y, z));
                float rx = renderSpawn.x, ry = renderSpawn.y, rz = renderSpawn.z;

                // Check WMO floor FIRST (cities like Stormwind stand on WMO floors).
                // Terrain exists below WMOs but at the wrong height.
                if (auto* wmo = renderer->getWMORenderer()) {
                    auto wmoH = wmo->getFloorHeight(rx, ry, rz + 5.0f);
                    if (wmoH.has_value() && std::abs(*wmoH - rz) < 15.0f) {
                        groundReady = true;
                    }
                }
                // Check terrain — but only if it's close to spawn Z (within 15 units).
                // Terrain far below a WMO city doesn't count as ground.
                if (!groundReady) {
                    if (auto* tm = renderer->getTerrainManager()) {
                        auto tH = tm->getHeightAt(rx, ry);
                        if (tH.has_value() && std::abs(*tH - rz) < 15.0f) {
                            groundReady = true;
                        }
                    }
                }
                // After 5s with enough tiles loaded, accept terrain as ready even if
                // the height sample doesn't match spawn Z exactly. This handles cases
                // where getHeightAt returns a slightly different value than the server's
                // spawn Z (e.g. terrain LOD, MCNK chunk boundaries, or spawn inside a
                // building where floor height differs from terrain below).
                if (!groundReady && elapsed >= 5.0f) {
                    if (auto* tm = renderer->getTerrainManager()) {
                        if (tm->getLoadedTileCount() >= 4) {
                            groundReady = true;
                            LOG_WARNING("Warmup: using tile-count fallback (", tm->getLoadedTileCount(), " tiles) after ", elapsed, "s");
                        }
                    }
                }

                if (!groundReady && elapsed > 5.0f && static_cast<int>(elapsed * 2) % 3 == 0) {
                    LOG_WARNING("Warmup: ground not ready at spawn (", rx, ",", ry, ",", rz,
                                ") after ", elapsed, "s");
                }
            }

            // Exit when: (min time passed AND queues drained AND ground ready) OR hard cap
            bool readyToExit = (elapsed >= kMinWarmupSeconds && idleIterations >= kIdleThreshold && groundReady);
            if (readyToExit || elapsed >= kMaxWarmupSeconds) {
                if (elapsed >= kMaxWarmupSeconds && !groundReady) {
                    LOG_WARNING("Warmup hit hard cap (", kMaxWarmupSeconds, "s), ground NOT ready — may fall through world");
                } else if (elapsed >= kMaxWarmupSeconds) {
                    LOG_WARNING("Warmup hit hard cap (", kMaxWarmupSeconds, "s), entering world with pending work");
                }
                break;
            }

            const float t = std::clamp(elapsed / kMaxWarmupSeconds, 0.0f, 1.0f);
            showProgress("Finalizing world sync...", 0.97f + t * 0.025f);
            SDL_Delay(16);
        }
    }

    // Start intro pan right before entering gameplay so it's visible after loading.
    if (renderer->getCameraController()) {
        renderer->getCameraController()->startIntroPan(2.8f, 140.0f);
    }

    showProgress("Entering world...", 1.0f);

    // Ensure all GPU resources (textures, buffers, pipelines) created during
    // world load are fully flushed before the first render frame. Without this,
    // vkCmdBeginRenderPass can crash on NVIDIA 590.x when resources from async
    // uploads haven't completed their queue operations.
    if (renderer && renderer->getVkContext()) {
        vkDeviceWaitIdle(renderer->getVkContext()->getDevice());
    }

    if (loadingScreenOk) {
        loadingScreen.shutdown();
    }

    // Track which map we actually loaded (used by same-map teleport check).
    loadedMapId_ = mapId;

    // Clear loading flag and process any deferred world entry.
    // A deferred entry occurs when SMSG_NEW_WORLD arrived during our warmup
    // (e.g., an area trigger in a dungeon immediately teleporting the player out).
    loadingWorld_ = false;
    if (pendingWorldEntry_) {
        auto entry = *pendingWorldEntry_;
        pendingWorldEntry_.reset();
        LOG_WARNING("Processing deferred world entry: map ", entry.mapId);
        worldEntryMovementGraceTimer_ = 2.0f;
        taxiLandingClampTimer_ = 0.0f;
        lastTaxiFlight_ = false;
        // Recursive call — sets loadedMapId_ and IN_GAME state for the final map.
        loadOnlineWorldTerrain(entry.mapId, entry.x, entry.y, entry.z);
        return;  // The recursive call handles setState(IN_GAME).
    }

    // Only enter IN_GAME when this is the final map (no deferred entry pending).
    setState(AppState::IN_GAME);

    // Load addons once per session on first world entry
    if (addonManager_ && !addonsLoaded_) {
        // Set character name for per-character SavedVariables
        if (gameHandler) {
            const std::string& charName = gameHandler->lookupName(gameHandler->getPlayerGuid());
            if (!charName.empty()) {
                addonManager_->setCharacterName(charName);
            } else {
                // Fallback: find name from character list
                for (const auto& c : gameHandler->getCharacters()) {
                    if (c.guid == gameHandler->getPlayerGuid()) {
                        addonManager_->setCharacterName(c.name);
                        break;
                    }
                }
            }
        }
        addonManager_->loadAllAddons();
        addonsLoaded_ = true;
        addonManager_->fireEvent("VARIABLES_LOADED");
        addonManager_->fireEvent("PLAYER_LOGIN");
        addonManager_->fireEvent("PLAYER_ENTERING_WORLD");
    } else if (addonManager_ && addonsLoaded_) {
        // Subsequent world entries (e.g. teleport, instance entry)
        addonManager_->fireEvent("PLAYER_ENTERING_WORLD");
    }
}

void Application::buildCharSectionsCache() {
    if (charSectionsCacheBuilt_ || !assetManager || !assetManager->isInitialized()) return;
    auto dbc = assetManager->loadDBC("CharSections.dbc");
    if (!dbc) return;
    const auto* csL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
    auto csF = pipeline::detectCharSectionsFields(dbc.get(), csL);
    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t race = dbc->getUInt32(r, csF.raceId);
        uint32_t sex = dbc->getUInt32(r, csF.sexId);
        uint32_t section = dbc->getUInt32(r, csF.baseSection);
        uint32_t variation = dbc->getUInt32(r, csF.variationIndex);
        uint32_t color = dbc->getUInt32(r, csF.colorIndex);
        // We only cache sections 0 (skin), 1 (face), 3 (hair), 4 (underwear)
        if (section != 0 && section != 1 && section != 3 && section != 4) continue;
        for (int ti = 0; ti < 3; ti++) {
            std::string tex = dbc->getString(r, csF.texture1 + ti);
            if (tex.empty()) continue;
            // Key: race(8)|sex(4)|section(4)|variation(8)|color(8)|texIndex(2) packed into 64 bits
            uint64_t key = (static_cast<uint64_t>(race) << 26) |
                           (static_cast<uint64_t>(sex & 0xF) << 22) |
                           (static_cast<uint64_t>(section & 0xF) << 18) |
                           (static_cast<uint64_t>(variation & 0xFF) << 10) |
                           (static_cast<uint64_t>(color & 0xFF) << 2) |
                           static_cast<uint64_t>(ti);
            charSectionsCache_.emplace(key, tex);
        }
    }
    charSectionsCacheBuilt_ = true;
    LOG_INFO("CharSections cache built: ", charSectionsCache_.size(), " entries");
}

std::string Application::lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                           uint8_t variation, uint8_t color, int texIndex) const {
    uint64_t key = (static_cast<uint64_t>(race) << 26) |
                   (static_cast<uint64_t>(sex & 0xF) << 22) |
                   (static_cast<uint64_t>(section & 0xF) << 18) |
                   (static_cast<uint64_t>(variation & 0xFF) << 10) |
                   (static_cast<uint64_t>(color & 0xFF) << 2) |
                   static_cast<uint64_t>(texIndex);
    auto it = charSectionsCache_.find(key);
    return (it != charSectionsCache_.end()) ? it->second : std::string();
}

void Application::buildCreatureDisplayLookups() {
    if (creatureLookupsBuilt_ || !assetManager || !assetManager->isInitialized()) return;

    LOG_INFO("Building creature display lookups from DBC files");

    // CreatureDisplayInfo.dbc structure (3.3.5a):
    // Col 0: displayId
    // Col 1: modelId
    // Col 3: extendedDisplayInfoID (link to CreatureDisplayInfoExtra.dbc)
    // Col 6: Skin1 (texture name)
    // Col 7: Skin2
    // Col 8: Skin3
    if (auto cdi = assetManager->loadDBC("CreatureDisplayInfo.dbc"); cdi && cdi->isLoaded()) {
        const auto* cdiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < cdi->getRecordCount(); i++) {
            CreatureDisplayData data;
            data.modelId = cdi->getUInt32(i, cdiL ? (*cdiL)["ModelID"] : 1);
            data.extraDisplayId = cdi->getUInt32(i, cdiL ? (*cdiL)["ExtraDisplayId"] : 3);
            data.skin1 = cdi->getString(i, cdiL ? (*cdiL)["Skin1"] : 6);
            data.skin2 = cdi->getString(i, cdiL ? (*cdiL)["Skin2"] : 7);
            data.skin3 = cdi->getString(i, cdiL ? (*cdiL)["Skin3"] : 8);
            displayDataMap_[cdi->getUInt32(i, cdiL ? (*cdiL)["ID"] : 0)] = data;
        }
        LOG_INFO("Loaded ", displayDataMap_.size(), " display→model mappings");
    }

    // CreatureDisplayInfoExtra.dbc structure (3.3.5a):
    // Col 0: ID
    // Col 1: DisplayRaceID
    // Col 2: DisplaySexID
    // Col 3: SkinID
    // Col 4: FaceID
    // Col 5: HairStyleID
    // Col 6: HairColorID
    // Col 7: FacialHairID
    // CreatureDisplayInfoExtra.dbc field layout depends on actual field count:
    //   19 fields: 10 equip slots (8-17), BakeName=18 (no Flags field)
    //   21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
    if (auto cdie = assetManager->loadDBC("CreatureDisplayInfoExtra.dbc"); cdie && cdie->isLoaded()) {
        const auto* cdieL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfoExtra") : nullptr;
        const uint32_t cdieEquip0 = cdieL ? (*cdieL)["EquipDisplay0"] : 8;
        // Detect actual field count to determine equip slot count and BakeName position
        const uint32_t dbcFieldCount = cdie->getFieldCount();
        int numEquipSlots;
        uint32_t bakeField;
        if (dbcFieldCount <= 19) {
            // 19 fields: 10 equip slots (8-17), BakeName at 18
            numEquipSlots = 10;
            bakeField = 18;
        } else {
            // 21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
            numEquipSlots = 11;
            bakeField = cdieL ? (*cdieL)["BakeName"] : 20;
        }
        uint32_t withBakeName = 0;
        for (uint32_t i = 0; i < cdie->getRecordCount(); i++) {
            HumanoidDisplayExtra extra;
            extra.raceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["RaceID"] : 1));
            extra.sexId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SexID"] : 2));
            extra.skinId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SkinID"] : 3));
            extra.faceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FaceID"] : 4));
            extra.hairStyleId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairStyleID"] : 5));
            extra.hairColorId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairColorID"] : 6));
            extra.facialHairId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FacialHairID"] : 7));
            for (int eq = 0; eq < numEquipSlots; eq++) {
                extra.equipDisplayId[eq] = cdie->getUInt32(i, cdieEquip0 + eq);
            }
            extra.bakeName = cdie->getString(i, bakeField);
            if (!extra.bakeName.empty()) withBakeName++;
            humanoidExtraMap_[cdie->getUInt32(i, cdieL ? (*cdieL)["ID"] : 0)] = extra;
        }
        LOG_WARNING("Loaded ", humanoidExtraMap_.size(), " humanoid display extra entries (",
                 withBakeName, " with baked textures, ", numEquipSlots, " equip slots, ",
                 dbcFieldCount, " DBC fields, bakeField=", bakeField, ")");
    }

    // CreatureModelData.dbc: modelId (col 0) → modelPath (col 2, .mdx → .m2)
    if (auto cmd = assetManager->loadDBC("CreatureModelData.dbc"); cmd && cmd->isLoaded()) {
        const auto* cmdL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureModelData") : nullptr;
        for (uint32_t i = 0; i < cmd->getRecordCount(); i++) {
            std::string mdx = cmd->getString(i, cmdL ? (*cmdL)["ModelPath"] : 2);
            if (mdx.empty()) continue;
            if (mdx.size() >= 4) {
                mdx = mdx.substr(0, mdx.size() - 4) + ".m2";
            }
            modelIdToPath_[cmd->getUInt32(i, cmdL ? (*cmdL)["ID"] : 0)] = mdx;
        }
        LOG_INFO("Loaded ", modelIdToPath_.size(), " model→path mappings");
    }

    // Resolve gryphon/wyvern display IDs by exact model path so taxi mounts have textures.
    auto toLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto normalizePath = [&](const std::string& p) {
        std::string s = p;
        for (char& c : s) if (c == '/') c = '\\';
        return toLower(s);
    };
    auto resolveDisplayIdForExactPath = [&](const std::string& exactPath) -> uint32_t {
        const std::string target = normalizePath(exactPath);
        // Collect ALL model IDs that map to this path (multiple model IDs can
        // share the same .m2 file, e.g. modelId 147 and 792 both → Gryphon.m2)
        std::vector<uint32_t> modelIds;
        for (const auto& [mid, path] : modelIdToPath_) {
            if (normalizePath(path) == target) {
                modelIds.push_back(mid);
            }
        }
        if (modelIds.empty()) return 0;
        uint32_t bestDisplayId = 0;
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            bool matches = false;
            for (uint32_t mid : modelIds) {
                if (data.modelId == mid) { matches = true; break; }
            }
            if (!matches) continue;
            int score = 0;
            if (!data.skin1.empty()) score += 3;
            if (!data.skin2.empty()) score += 2;
            if (!data.skin3.empty()) score += 1;
            if (score > bestScore) {
                bestScore = score;
                bestDisplayId = dispId;
            }
        }
        return bestDisplayId;
    };

    gryphonDisplayId_ = resolveDisplayIdForExactPath("Creature\\Gryphon\\Gryphon.m2");
    wyvernDisplayId_  = resolveDisplayIdForExactPath("Creature\\Wyvern\\Wyvern.m2");
    gameServices_.gryphonDisplayId = gryphonDisplayId_;
    gameServices_.wyvernDisplayId  = wyvernDisplayId_;
    LOG_INFO("Taxi mount displayIds: gryphon=", gryphonDisplayId_, " wyvern=", wyvernDisplayId_);

    // CharHairGeosets.dbc: maps (race, sex, hairStyleId) → skinSectionId for hair mesh
    // Col 0: ID, Col 1: RaceID, Col 2: SexID, Col 3: VariationID, Col 4: GeosetID, Col 5: Showscalp
    if (auto chg = assetManager->loadDBC("CharHairGeosets.dbc"); chg && chg->isLoaded()) {
        const auto* chgL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharHairGeosets") : nullptr;
        for (uint32_t i = 0; i < chg->getRecordCount(); i++) {
            uint32_t raceId = chg->getUInt32(i, chgL ? (*chgL)["RaceID"] : 1);
            uint32_t sexId = chg->getUInt32(i, chgL ? (*chgL)["SexID"] : 2);
            uint32_t variation = chg->getUInt32(i, chgL ? (*chgL)["Variation"] : 3);
            uint32_t geosetId = chg->getUInt32(i, chgL ? (*chgL)["GeosetID"] : 4);
            uint32_t key = (raceId << 16) | (sexId << 8) | variation;
            hairGeosetMap_[key] = static_cast<uint16_t>(geosetId);
        }
        LOG_INFO("Loaded ", hairGeosetMap_.size(), " hair geoset mappings from CharHairGeosets.dbc");
    }

    // CharacterFacialHairStyles.dbc: maps (race, sex, facialHairId) → geoset IDs
    // No ID column: Col 0: RaceID, Col 1: SexID, Col 2: VariationID
    // Col 3: Geoset100, Col 4: Geoset300, Col 5: Geoset200
    if (auto cfh = assetManager->loadDBC("CharacterFacialHairStyles.dbc"); cfh && cfh->isLoaded()) {
        const auto* cfhL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
        for (uint32_t i = 0; i < cfh->getRecordCount(); i++) {
            uint32_t raceId = cfh->getUInt32(i, cfhL ? (*cfhL)["RaceID"] : 0);
            uint32_t sexId = cfh->getUInt32(i, cfhL ? (*cfhL)["SexID"] : 1);
            uint32_t variation = cfh->getUInt32(i, cfhL ? (*cfhL)["Variation"] : 2);
            uint32_t key = (raceId << 16) | (sexId << 8) | variation;
            FacialHairGeosets fhg;
            fhg.geoset100 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset100"] : 3));
            fhg.geoset300 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset300"] : 4));
            fhg.geoset200 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset200"] : 5));
            facialHairGeosetMap_[key] = fhg;
        }
        LOG_INFO("Loaded ", facialHairGeosetMap_.size(), " facial hair geoset mappings from CharacterFacialHairStyles.dbc");
    }

    creatureLookupsBuilt_ = true;
}

std::string Application::getModelPathForDisplayId(uint32_t displayId) const {
    if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
    if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";

    // WotLK servers can send display IDs that do not exist in older/local
    // CreatureDisplayInfo datasets. Keep those creatures visible by falling
    // back to a close base model instead of dropping spawn entirely.
    switch (displayId) {
        case 31048: // Diseased Young Wolf variants (AzerothCore WotLK)
        case 31049: // Diseased Wolf variants (AzerothCore WotLK)
            return "Creature\\Wolf\\Wolf.m2";
        default:
            break;
    }

    auto itData = displayDataMap_.find(displayId);
    if (itData == displayDataMap_.end()) {
        // Some sources (e.g., taxi nodes) may provide a modelId directly.
        auto itPath = modelIdToPath_.find(displayId);
        if (itPath != modelIdToPath_.end()) {
            return itPath->second;
        }
        if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
        if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";
        if (warnedMissingDisplayDataIds_.insert(displayId).second) {
            LOG_WARNING("No display data for displayId ", displayId,
                        " (displayDataMap_ has ", displayDataMap_.size(), " entries)");
        }
        return "";
    }

    auto itPath = modelIdToPath_.find(itData->second.modelId);
    if (itPath == modelIdToPath_.end()) {
        if (warnedMissingModelPathIds_.insert(displayId).second) {
            LOG_WARNING("No model path for modelId ", itData->second.modelId,
                        " from displayId ", displayId,
                        " (modelIdToPath_ has ", modelIdToPath_.size(), " entries)");
        }
        return "";
    }

    return itPath->second;
}

audio::VoiceType Application::detectVoiceTypeFromDisplayId(uint32_t displayId) const {
    // Look up display data
    auto itDisplay = displayDataMap_.find(displayId);
    if (itDisplay == displayDataMap_.end() || itDisplay->second.extraDisplayId == 0) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no display data)");
        return audio::VoiceType::GENERIC;  // Not a humanoid or no extra data
    }

    // Look up humanoid extra data (race/sex info)
    auto itExtra = humanoidExtraMap_.find(itDisplay->second.extraDisplayId);
    if (itExtra == humanoidExtraMap_.end()) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no humanoid extra data)");
        return audio::VoiceType::GENERIC;
    }

    uint8_t raceId = itExtra->second.raceId;
    uint8_t sexId = itExtra->second.sexId;

    const char* raceName = "Unknown";
    const char* sexName = (sexId == 0) ? "Male" : "Female";

    // Map (raceId, sexId) to VoiceType
    // Race IDs: 1=Human, 2=Orc, 3=Dwarf, 4=NightElf, 5=Undead, 6=Tauren, 7=Gnome, 8=Troll
    // Sex IDs: 0=Male, 1=Female
    audio::VoiceType result;
    switch (raceId) {
        case 1: raceName = "Human"; result = (sexId == 0) ? audio::VoiceType::HUMAN_MALE : audio::VoiceType::HUMAN_FEMALE; break;
        case 2: raceName = "Orc"; result = (sexId == 0) ? audio::VoiceType::ORC_MALE : audio::VoiceType::ORC_FEMALE; break;
        case 3: raceName = "Dwarf"; result = (sexId == 0) ? audio::VoiceType::DWARF_MALE : audio::VoiceType::DWARF_FEMALE; break;
        case 4: raceName = "NightElf"; result = (sexId == 0) ? audio::VoiceType::NIGHTELF_MALE : audio::VoiceType::NIGHTELF_FEMALE; break;
        case 5: raceName = "Undead"; result = (sexId == 0) ? audio::VoiceType::UNDEAD_MALE : audio::VoiceType::UNDEAD_FEMALE; break;
        case 6: raceName = "Tauren"; result = (sexId == 0) ? audio::VoiceType::TAUREN_MALE : audio::VoiceType::TAUREN_FEMALE; break;
        case 7: raceName = "Gnome"; result = (sexId == 0) ? audio::VoiceType::GNOME_MALE : audio::VoiceType::GNOME_FEMALE; break;
        case 8: raceName = "Troll"; result = (sexId == 0) ? audio::VoiceType::TROLL_MALE : audio::VoiceType::TROLL_FEMALE; break;
        case 10: raceName = "BloodElf"; result = (sexId == 0) ? audio::VoiceType::BLOODELF_MALE : audio::VoiceType::BLOODELF_FEMALE; break;
        case 11: raceName = "Draenei"; result = (sexId == 0) ? audio::VoiceType::DRAENEI_MALE : audio::VoiceType::DRAENEI_FEMALE; break;
        default: result = audio::VoiceType::GENERIC; break;
    }

    LOG_INFO("Voice detection: displayId ", displayId, " -> ", raceName, " ", sexName, " (race=", static_cast<int>(raceId), ", sex=", static_cast<int>(sexId), ")");
    return result;
}

void Application::buildGameObjectDisplayLookups() {
    if (gameObjectLookupsBuilt_ || !assetManager || !assetManager->isInitialized()) return;

    LOG_INFO("Building gameobject display lookups from DBC files");

    // GameObjectDisplayInfo.dbc structure (3.3.5a):
    // Col 0: ID (displayId)
    // Col 1: ModelName
    if (auto godi = assetManager->loadDBC("GameObjectDisplayInfo.dbc"); godi && godi->isLoaded()) {
        const auto* godiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < godi->getRecordCount(); i++) {
            uint32_t displayId = godi->getUInt32(i, godiL ? (*godiL)["ID"] : 0);
            std::string modelName = godi->getString(i, godiL ? (*godiL)["ModelName"] : 1);
            if (modelName.empty()) continue;
            if (modelName.size() >= 4) {
                std::string ext = modelName.substr(modelName.size() - 4);
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".mdx") {
                    modelName = modelName.substr(0, modelName.size() - 4) + ".m2";
                }
            }
            gameObjectDisplayIdToPath_[displayId] = modelName;
        }
        LOG_INFO("Loaded ", gameObjectDisplayIdToPath_.size(), " gameobject display mappings");
    }

    gameObjectLookupsBuilt_ = true;
}

std::string Application::getGameObjectModelPathForDisplayId(uint32_t displayId) const {
    auto it = gameObjectDisplayIdToPath_.find(displayId);
    if (it == gameObjectDisplayIdToPath_.end()) return "";
    return it->second;
}

bool Application::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (!renderer || !renderer->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler && guid == gameHandler->getPlayerGuid()) {
        instanceId = renderer->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer->getCharacterRenderer()->getInstanceBounds(instanceId, outCenter, outRadius);
}

bool Application::getRenderFootZForGuid(uint64_t guid, float& outFootZ) const {
    if (!renderer || !renderer->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler && guid == gameHandler->getPlayerGuid()) {
        instanceId = renderer->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer->getCharacterRenderer()->getInstanceFootZ(instanceId, outFootZ);
}

bool Application::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    if (!renderer || !renderer->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler && guid == gameHandler->getPlayerGuid()) {
        instanceId = renderer->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer->getCharacterRenderer()->getInstancePosition(instanceId, outPos);
}

pipeline::M2Model Application::loadCreatureM2Sync(const std::string& m2Path) {
    auto m2Data = assetManager->readFile(m2Path);
    if (m2Data.empty()) {
        LOG_WARNING("Failed to read creature M2: ", m2Path);
        return {};
    }

    pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
    if (model.vertices.empty()) {
        LOG_WARNING("Failed to parse creature M2: ", m2Path);
        return {};
    }

    // Load skin file (only for WotLK M2s - vanilla has embedded skin)
    if (model.version >= 264) {
        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
        auto skinData = assetManager->readFile(skinPath);
        if (!skinData.empty()) {
            pipeline::M2Loader::loadSkin(skinData, model);
        } else {
            LOG_WARNING("Missing skin file for WotLK creature M2: ", skinPath);
        }
    }

    // Load external .anim files for sequences without flag 0x20
    std::string basePath = m2Path.substr(0, m2Path.size() - 3);
    for (uint32_t si = 0; si < model.sequences.size(); si++) {
        if (!(model.sequences[si].flags & 0x20)) {
            char animFileName[256];
            snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                basePath.c_str(), model.sequences[si].id, model.sequences[si].variationIndex);
            auto animData = assetManager->readFileOptional(animFileName);
            if (!animData.empty()) {
                pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
            }
        }
    }

    return model;
}

void Application::spawnOnlineCreature(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer || !renderer->getCharacterRenderer() || !assetManager) return;

    // Skip if lookups not yet built (asset manager not ready)
    if (!creatureLookupsBuilt_) return;

    // Skip if already spawned
    if (creatureInstances_.count(guid)) return;
    if (nonRenderableCreatureDisplayIds_.count(displayId)) {
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }

    // Get model path from displayId
    std::string m2Path = getModelPathForDisplayId(displayId);
    if (m2Path.empty()) {
        nonRenderableCreatureDisplayIds_.insert(displayId);
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }
    {
        // Intentionally invisible helper creatures should not consume retry budget.
        std::string lowerPath = m2Path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerPath.find("invisiblestalker") != std::string::npos ||
            lowerPath.find("invisible_stalker") != std::string::npos) {
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }
    }

    auto* charRenderer = renderer->getCharacterRenderer();

    // Check model cache - reuse if same displayId was already loaded
    uint32_t modelId = 0;
    auto cacheIt = displayIdModelCache_.find(displayId);
    if (cacheIt != displayIdModelCache_.end()) {
        modelId = cacheIt->second;
    } else {
        // Load model from disk (only once per displayId)
        modelId = nextCreatureModelId_++;

        pipeline::M2Model model = loadCreatureM2Sync(m2Path);
        if (!model.isValid()) {
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }

        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("Failed to load creature model: ", m2Path);
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }

        displayIdModelCache_[displayId] = modelId;
    }

    // Apply skin textures from CreatureDisplayInfo.dbc (only once per displayId model).
    // Track separately from model cache because async loading may upload the model
    // before textures are applied.
    auto itDisplayData = displayDataMap_.find(displayId);
    bool needsTextures = (displayIdTexturesApplied_.find(displayId) == displayIdTexturesApplied_.end());
    if (needsTextures && itDisplayData != displayDataMap_.end()) {
        auto texStart = std::chrono::steady_clock::now();
        displayIdTexturesApplied_.insert(displayId);
        const auto& dispData = itDisplayData->second;

        // Use pre-decoded textures from async creature load (if available)
        auto itPreDec = displayIdPredecodedTextures_.find(displayId);
        bool hasPreDec = (itPreDec != displayIdPredecodedTextures_.end());
        if (hasPreDec) {
            charRenderer->setPredecodedBLPCache(&itPreDec->second);
        }

        // Get model directory for texture path construction
        std::string modelDir;
        size_t lastSlash = m2Path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            modelDir = m2Path.substr(0, lastSlash + 1);
        }

        LOG_DEBUG("DisplayId ", displayId, " skins: '", dispData.skin1, "', '", dispData.skin2, "', '", dispData.skin3,
                  "' extraDisplayId=", dispData.extraDisplayId);

        // Get model data from CharacterRenderer for texture iteration
        const auto* modelData = charRenderer->getModelData(modelId);
        if (!modelData) {
            LOG_WARNING("Model data not found for modelId ", modelId);
        }

        // Log texture types in the model
        if (modelData) {
        for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
            LOG_DEBUG("  Model texture ", ti, ": type=", modelData->textures[ti].type, " filename='", modelData->textures[ti].filename, "'");
        }
        }

        // Check if this is a humanoid NPC with extra display info
        bool hasHumanoidTexture = false;
        if (dispData.extraDisplayId != 0) {
            auto itExtra = humanoidExtraMap_.find(dispData.extraDisplayId);
            if (itExtra != humanoidExtraMap_.end()) {
                const auto& extra = itExtra->second;
                LOG_DEBUG("  Found humanoid extra: raceId=", static_cast<int>(extra.raceId), " sexId=", static_cast<int>(extra.sexId),
                          " hairStyle=", static_cast<int>(extra.hairStyleId), " hairColor=", static_cast<int>(extra.hairColorId),
                          " bakeName='", extra.bakeName, "'");

                // Collect model texture slot info (type 1 = skin, type 6 = hair)
                std::vector<uint32_t> skinSlots, hairSlots;
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        uint32_t texType = modelData->textures[ti].type;
                        if (texType == 1 || texType == 11 || texType == 12 || texType == 13)
                            skinSlots.push_back(static_cast<uint32_t>(ti));
                        if (texType == 6)
                            hairSlots.push_back(static_cast<uint32_t>(ti));
                    }
                }

                // Copy extra data for the async task (avoid dangling reference)
                HumanoidDisplayExtra extraCopy = extra;

                // Launch async task: ALL DBC lookups, path resolution, and BLP pre-decode
                // happen on a background thread. Only GPU texture upload runs on main thread
                // (in processAsyncNpcCompositeResults).
                auto* am = assetManager.get();
                AsyncNpcCompositeLoad load;
                load.future = std::async(std::launch::async,
                    [am, extraCopy, skinSlots = std::move(skinSlots),
                     hairSlots = std::move(hairSlots), modelId, displayId]() mutable -> PreparedNpcComposite {
                        PreparedNpcComposite result;
                        DeferredNpcComposite& def = result.info;
                        def.modelId = modelId;
                        def.displayId = displayId;
                        def.skinTextureSlots = std::move(skinSlots);
                        def.hairTextureSlots = std::move(hairSlots);

                        std::vector<std::string> allPaths;  // paths to pre-decode

                        // --- Baked skin texture ---
                        if (!extraCopy.bakeName.empty()) {
                            def.bakedSkinPath = "Textures\\BakedNpcTextures\\" + extraCopy.bakeName;
                            def.hasBakedSkin = true;
                            allPaths.push_back(def.bakedSkinPath);
                        }

                        // --- CharSections fallback (skin/face/underwear) ---
                        if (!def.hasBakedSkin) {
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t npcRace = static_cast<uint32_t>(extraCopy.raceId);
                                uint32_t npcSex = static_cast<uint32_t>(extraCopy.sexId);
                                uint32_t npcSkin = static_cast<uint32_t>(extraCopy.skinId);
                                uint32_t npcFace = static_cast<uint32_t>(extraCopy.faceId);
                                std::string npcFaceLower, npcFaceUpper;
                                std::vector<std::string> npcUnderwear;

                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t rId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sId = csDbc->getUInt32(r, csF.sexId);
                                    if (rId != npcRace || sId != npcSex) continue;

                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t color = csDbc->getUInt32(r, csF.colorIndex);

                                    if (section == 0 && def.basePath.empty() && color == npcSkin) {
                                        def.basePath = csDbc->getString(r, csF.texture1);
                                    } else if (section == 1 && npcFaceLower.empty() &&
                                               variation == npcFace && color == npcSkin) {
                                        npcFaceLower = csDbc->getString(r, csF.texture1);
                                        npcFaceUpper = csDbc->getString(r, csF.texture2);
                                    } else if (section == 4 && npcUnderwear.empty() && color == npcSkin) {
                                        for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                                            std::string tex = csDbc->getString(r, f);
                                            if (!tex.empty()) npcUnderwear.push_back(tex);
                                        }
                                    }
                                }

                                if (!def.basePath.empty()) {
                                    allPaths.push_back(def.basePath);
                                    if (!npcFaceLower.empty()) { def.overlayPaths.push_back(npcFaceLower); allPaths.push_back(npcFaceLower); }
                                    if (!npcFaceUpper.empty()) { def.overlayPaths.push_back(npcFaceUpper); allPaths.push_back(npcFaceUpper); }
                                    for (const auto& uw : npcUnderwear) { def.overlayPaths.push_back(uw); allPaths.push_back(uw); }
                                }
                            }
                        }

                        // --- Equipment region layers (ItemDisplayInfo DBC) ---
                        auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                        if (idiDbc) {
                            static constexpr const char* componentDirs[] = {
                                "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
                                "TorsoUpperTexture", "TorsoLowerTexture",
                                "LegUpperTexture", "LegLowerTexture", "FootTexture",
                            };
                            const auto* idiL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                            const uint32_t texRegionFields[8] = {
                                idiL ? (*idiL)["TextureArmUpper"]  : 14u,
                                idiL ? (*idiL)["TextureArmLower"]  : 15u,
                                idiL ? (*idiL)["TextureHand"]      : 16u,
                                idiL ? (*idiL)["TextureTorsoUpper"]: 17u,
                                idiL ? (*idiL)["TextureTorsoLower"]: 18u,
                                idiL ? (*idiL)["TextureLegUpper"]  : 19u,
                                idiL ? (*idiL)["TextureLegLower"]  : 20u,
                                idiL ? (*idiL)["TextureFoot"]      : 21u,
                            };
                            const bool npcIsFemale = (extraCopy.sexId == 1);
                            const bool npcHasArmArmor = (extraCopy.equipDisplayId[7] != 0 || extraCopy.equipDisplayId[8] != 0);

                            auto regionAllowedForNpcSlot = [](int eqSlot, int region) -> bool {
                                switch (eqSlot) {
                                    case 2: case 3: return region <= 4;
                                    case 4: return false;
                                    case 5: return region == 5 || region == 6;
                                    case 6: return region == 7;
                                    case 7: return false;
                                    case 8: return region == 2;
                                    case 9: return region == 3 || region == 4;
                                    default: return false;
                                }
                            };

                            for (int eqSlot = 0; eqSlot < 11; eqSlot++) {
                                uint32_t did = extraCopy.equipDisplayId[eqSlot];
                                if (did == 0) continue;
                                int32_t recIdx = idiDbc->findRecordById(did);
                                if (recIdx < 0) continue;

                                for (int region = 0; region < 8; region++) {
                                    if (!regionAllowedForNpcSlot(eqSlot, region)) continue;
                                    if (eqSlot == 2 && !npcHasArmArmor && !(region == 3 || region == 4)) continue;
                                    std::string texName = idiDbc->getString(
                                        static_cast<uint32_t>(recIdx), texRegionFields[region]);
                                    if (texName.empty()) continue;

                                    std::string base = "Item\\TextureComponents\\" +
                                        std::string(componentDirs[region]) + "\\" + texName;
                                    std::string genderPath = base + (npcIsFemale ? "_F.blp" : "_M.blp");
                                    std::string unisexPath = base + "_U.blp";
                                    std::string basePath = base + ".blp";
                                    std::string fullPath;
                                    if (am->fileExists(genderPath)) fullPath = genderPath;
                                    else if (am->fileExists(unisexPath)) fullPath = unisexPath;
                                    else if (am->fileExists(basePath)) fullPath = basePath;
                                    else continue;

                                    def.regionLayers.emplace_back(region, fullPath);
                                    allPaths.push_back(fullPath);
                                }
                            }
                        }

                        // Determine compositing mode
                        if (!def.basePath.empty()) {
                            bool needsComposite = !def.overlayPaths.empty() || !def.regionLayers.empty();
                            if (needsComposite && !def.skinTextureSlots.empty()) {
                                def.hasComposite = true;
                            } else if (!def.skinTextureSlots.empty()) {
                                def.hasSimpleSkin = true;
                            }
                        }

                        // --- Hair texture from CharSections (section 3) ---
                        {
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t targetRace = static_cast<uint32_t>(extraCopy.raceId);
                                uint32_t targetSex = static_cast<uint32_t>(extraCopy.sexId);

                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t raceId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sexId = csDbc->getUInt32(r, csF.sexId);
                                    if (raceId != targetRace || sexId != targetSex) continue;
                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    if (section != 3) continue;
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t colorIdx = csDbc->getUInt32(r, csF.colorIndex);
                                    if (variation != static_cast<uint32_t>(extraCopy.hairStyleId)) continue;
                                    if (colorIdx != static_cast<uint32_t>(extraCopy.hairColorId)) continue;
                                    def.hairTexturePath = csDbc->getString(r, csF.texture1);
                                    break;
                                }

                                if (!def.hairTexturePath.empty()) {
                                    allPaths.push_back(def.hairTexturePath);
                                } else if (def.hasBakedSkin && !def.hairTextureSlots.empty()) {
                                    def.useBakedForHair = true;
                                    // bakedSkinPath already in allPaths
                                }
                            }
                        }

                        // --- Pre-decode all BLP textures on this background thread ---
                        for (const auto& path : allPaths) {
                            std::string key = path;
                            std::replace(key.begin(), key.end(), '/', '\\');
                            std::transform(key.begin(), key.end(), key.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (result.predecodedTextures.count(key)) continue;
                            auto blp = am->loadTexture(key);
                            if (blp.isValid()) {
                                result.predecodedTextures[key] = std::move(blp);
                            }
                        }

                        return result;
                    });
                asyncNpcCompositeLoads_.push_back(std::move(load));
                hasHumanoidTexture = true;  // skip non-humanoid skin block
            } else {
                LOG_WARNING("  extraDisplayId ", dispData.extraDisplayId, " not found in humanoidExtraMap");
            }
        }

        // Apply creature skin textures (for non-humanoid creatures)
        if (!hasHumanoidTexture && modelData) {
            auto resolveCreatureSkinPath = [&](const std::string& skinField) -> std::string {
                if (skinField.empty()) return "";

                std::string raw = skinField;
                std::replace(raw.begin(), raw.end(), '/', '\\');
                auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
                raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [&](unsigned char c) { return !isSpace(c); }));
                raw.erase(std::find_if(raw.rbegin(), raw.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
                if (raw.empty()) return "";

                auto hasBlpExt = [](const std::string& p) {
                    if (p.size() < 4) return false;
                    std::string ext = p.substr(p.size() - 4);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return ext == ".blp";
                };
                auto addCandidate = [](std::vector<std::string>& out, const std::string& p) {
                    if (p.empty()) return;
                    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
                };

                std::vector<std::string> candidates;
                const bool hasDir = (raw.find('\\') != std::string::npos || raw.find('/') != std::string::npos);
                const bool hasExt = hasBlpExt(raw);

                if (hasDir) {
                    addCandidate(candidates, raw);
                    if (!hasExt) addCandidate(candidates, raw + ".blp");
                } else {
                    addCandidate(candidates, modelDir + raw);
                    if (!hasExt) addCandidate(candidates, modelDir + raw + ".blp");
                    addCandidate(candidates, raw);
                    if (!hasExt) addCandidate(candidates, raw + ".blp");
                }

                for (const auto& c : candidates) {
                    if (assetManager->fileExists(c)) return c;
                }
                return "";
            };

            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                const auto& tex = modelData->textures[ti];
                std::string skinPath;

                // Creature skin types: 11 = skin1, 12 = skin2, 13 = skin3
                if (tex.type == 11 && !dispData.skin1.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin1);
                } else if (tex.type == 12 && !dispData.skin2.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin2);
                } else if (tex.type == 13 && !dispData.skin3.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin3);
                }

                if (!skinPath.empty()) {
                    rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                    if (skinTex) {
                        charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                        LOG_DEBUG("Applied creature skin texture: ", skinPath, " to slot ", ti);
                    }
                } else if ((tex.type == 11 && !dispData.skin1.empty()) ||
                           (tex.type == 12 && !dispData.skin2.empty()) ||
                           (tex.type == 13 && !dispData.skin3.empty())) {
                    LOG_WARNING("Creature skin texture not found for displayId ", displayId,
                                " slot ", ti, " type ", tex.type,
                                " (skin fields: '", dispData.skin1, "', '",
                                dispData.skin2, "', '", dispData.skin3, "')");
                }
            }
        }

        // Clear pre-decoded cache after applying all display textures
        charRenderer->setPredecodedBLPCache(nullptr);
        displayIdPredecodedTextures_.erase(displayId);
        {
            auto texEnd = std::chrono::steady_clock::now();
            float texMs = std::chrono::duration<float, std::milli>(texEnd - texStart).count();
            if (texMs > 50.0f) {
                LOG_WARNING("spawnCreature texture setup took ", texMs, "ms displayId=", displayId,
                            " hasPreDec=", hasPreDec, " extra=", dispData.extraDisplayId);
            }
        }
    }

    // Use the entity's latest server-authoritative position rather than the stale spawn
    // position. Movement packets (SMSG_MONSTER_MOVE) can arrive while a creature is still
    // queued in pendingCreatureSpawns_ and get silently dropped. getLatestX/Y/Z returns
    // the movement destination if the entity is mid-move, which is always up-to-date
    // regardless of distance culling (unlike getX/Y/Z which requires updateMovement).
    if (gameHandler) {
        if (auto entity = gameHandler->getEntityManager().getEntity(guid)) {
            x = entity->getLatestX();
            y = entity->getLatestY();
            z = entity->getLatestZ();
            orientation = entity->getOrientation();
        }
    }

    // Convert canonical → render coordinates
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));

    // Keep authoritative server Z for online creature spawns.
    // Terrain-based lifting can incorrectly move elevated NPCs (e.g. flight masters on
    // Stormwind ramparts) to bad heights relative to WMO geometry.

    // Convert canonical WoW orientation (0=north) -> render yaw (0=west)
    float renderYaw = orientation + glm::radians(90.0f);

    // Create instance (apply server-provided scale from OBJECT_FIELD_SCALE_X)
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos,
        glm::vec3(0.0f, 0.0f, renderYaw), scale);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create creature instance for guid 0x", std::hex, guid, std::dec);
        return;
    }

    // Per-instance hair/skin texture overrides — runs for ALL NPCs (including cached models)
    // so that each NPC gets its own hair/skin color regardless of model sharing.
    // Uses pre-built CharSections cache (O(1) lookup instead of O(N) DBC scan).
    {
        if (!charSectionsCacheBuilt_) buildCharSectionsCache();
        auto itDD = displayDataMap_.find(displayId);
        if (itDD != displayDataMap_.end() && itDD->second.extraDisplayId != 0) {
            auto itExtra2 = humanoidExtraMap_.find(itDD->second.extraDisplayId);
            if (itExtra2 != humanoidExtraMap_.end()) {
                const auto& extra = itExtra2->second;
                const auto* md = charRenderer->getModelData(modelId);
                if (md) {
                        // Look up hair texture (section 3) via cache
                        rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                        std::string hairPath = lookupCharSection(
                            extra.raceId, extra.sexId, 3, extra.hairStyleId, extra.hairColorId, 0);
                        if (!hairPath.empty()) {
                            rendering::VkTexture* hairTex = charRenderer->loadTexture(hairPath);
                            if (hairTex && hairTex != whiteTex) {
                                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                    if (md->textures[ti].type == 6) {
                                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), hairTex);
                                    }
                                }
                            }
                        }

                        // Look up skin texture (section 0) for per-instance skin color.
                        // Skip when the NPC has a baked texture or composited equipment —
                        // those already encode armor over skin and must not be replaced.
                        bool hasEquipOrBake = !extra.bakeName.empty();
                        if (!hasEquipOrBake) {
                            for (int s = 0; s < 11 && !hasEquipOrBake; s++)
                                if (extra.equipDisplayId[s] != 0) hasEquipOrBake = true;
                        }
                        if (!hasEquipOrBake) {
                            std::string skinPath = lookupCharSection(
                                extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                            if (!skinPath.empty()) {
                                rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                                if (skinTex) {
                                    for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                        uint32_t tt = md->textures[ti].type;
                                        if (tt == 1 || tt == 11) {
                                            charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), skinTex);
                                        }
                                    }
                                }
                            }
                        }
                }
            }
        }
    }

    // Optional humanoid NPC geoset mask. Disabled by default because forcing geosets
    // causes long-standing visual artifacts on some models (missing waist, phantom
    // bracers, flickering apron overlays). Prefer model defaults.
    static constexpr bool kEnableNpcSafeGeosetMask = false;
    if (kEnableNpcSafeGeosetMask &&
        itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            std::unordered_set<uint16_t> safeGeosets;
            std::unordered_set<uint16_t> modelGeosets;
            std::unordered_map<uint16_t, uint16_t> firstGeosetByGroup;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (const auto& b : md->batches) {
                    const uint16_t sid = b.submeshId;
                    modelGeosets.insert(sid);
                    const uint16_t group = static_cast<uint16_t>(sid / 100);
                    auto it = firstGeosetByGroup.find(group);
                    if (it == firstGeosetByGroup.end() || sid < it->second) {
                        firstGeosetByGroup[group] = sid;
                    }
                }
            }
            auto addSafeGeoset = [&](uint16_t preferredId) {
                if (preferredId < 100 || modelGeosets.empty()) {
                    safeGeosets.insert(preferredId);
                    return;
                }
                if (modelGeosets.count(preferredId) > 0) {
                    safeGeosets.insert(preferredId);
                    return;
                }
                const uint16_t group = static_cast<uint16_t>(preferredId / 100);
                auto it = firstGeosetByGroup.find(group);
                if (it != firstGeosetByGroup.end()) {
                    safeGeosets.insert(it->second);
                }
            };
            uint16_t hairGeoset = 1;
            uint32_t hairKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                               (static_cast<uint32_t>(extra.sexId) << 8) |
                               static_cast<uint32_t>(extra.hairStyleId);
            auto itHairGeo = hairGeosetMap_.find(hairKey);
            if (itHairGeo != hairGeosetMap_.end() && itHairGeo->second > 0) {
                hairGeoset = itHairGeo->second;
            }
            const uint16_t selectedHairScalp = (hairGeoset > 0 ? hairGeoset : 1);
            std::unordered_set<uint16_t> hairScalpGeosetsForRaceSex;
            for (const auto& [k, v] : hairGeosetMap_) {
                uint8_t race = static_cast<uint8_t>((k >> 16) & 0xFF);
                uint8_t sex = static_cast<uint8_t>((k >> 8) & 0xFF);
                if (race == extra.raceId && sex == extra.sexId && v > 0 && v < 100) {
                    hairScalpGeosetsForRaceSex.insert(v);
                }
            }
            // Group 0 contains both base body parts and race/sex hair scalp variants.
            // Keep all non-hair body submeshes, but only the selected hair scalp.
            for (uint16_t sid : modelGeosets) {
                if (sid >= 100) continue;
                if (hairScalpGeosetsForRaceSex.count(sid) > 0 && sid != selectedHairScalp) continue;
                safeGeosets.insert(sid);
            }
            safeGeosets.insert(selectedHairScalp);
            addSafeGeoset(static_cast<uint16_t>(100 + std::max<uint16_t>(hairGeoset, 1)));

            uint32_t facialKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                                 (static_cast<uint32_t>(extra.sexId) << 8) |
                                 static_cast<uint32_t>(extra.facialHairId);
            auto itFacial = facialHairGeosetMap_.find(facialKey);
            if (itFacial != facialHairGeosetMap_.end()) {
                const auto& fhg = itFacial->second;
                addSafeGeoset(static_cast<uint16_t>(200 + std::max<uint16_t>(fhg.geoset200, 1)));
                addSafeGeoset(static_cast<uint16_t>(300 + std::max<uint16_t>(fhg.geoset300, 1)));
            } else {
                addSafeGeoset(201);
                addSafeGeoset(301);
            }

            // Force pants (1301) and avoid robe skirt variants unless we re-enable full slot-accurate geosets.
            addSafeGeoset(301);
            addSafeGeoset(kGeosetBareForearms);
            addSafeGeoset(402);
            addSafeGeoset(501);
            addSafeGeoset(701);
            addSafeGeoset(kGeosetBareSleeves);
            addSafeGeoset(901);
            addSafeGeoset(kGeosetDefaultTabard);
            addSafeGeoset(kGeosetBarePants);
            addSafeGeoset(kGeosetBareFeet);

            charRenderer->setActiveGeosets(instanceId, safeGeosets);
        }
    }

    // NOTE: Custom humanoid NPC geoset/equipment overrides are currently too
    // aggressive and can make NPCs invisible (targetable but not rendered).
    // Keep default model geosets for online creatures until this path is made
    // data-accurate per display model.
    static constexpr bool kEnableNpcHumanoidOverrides = false;

    // Set geosets for humanoid NPCs based on CreatureDisplayInfoExtra
    if (kEnableNpcHumanoidOverrides &&
        itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            std::unordered_set<uint16_t> activeGeosets;

            // Group 0: body base (id=0 always) + hair scalp mesh from CharHairGeosets.dbc
            activeGeosets.insert(0);  // Body base mesh

            // Hair: CharHairGeosets.dbc maps (race, sex, hairStyleId) → group 0 scalp submeshId
            uint32_t hairKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                               (static_cast<uint32_t>(extra.sexId) << 8) |
                               static_cast<uint32_t>(extra.hairStyleId);
            auto itHairGeo = hairGeosetMap_.find(hairKey);
            uint16_t hairScalpId = (itHairGeo != hairGeosetMap_.end()) ? itHairGeo->second : 0;
            if (hairScalpId > 0) {
                activeGeosets.insert(hairScalpId);                        // Group 0 scalp/hair mesh
                activeGeosets.insert(static_cast<uint16_t>(100 + hairScalpId)); // Group 1 connector (if exists)
            } else {
                // Bald (geosetId=0): body base has a hole at the crown, so include
                // submeshId=1 (bald scalp cap with body skin texture) to cover it.
                activeGeosets.insert(1);    // Group 0 bald scalp mesh
                activeGeosets.insert(kGeosetDefaultConnector);  // Group 1 connector
            }
            uint16_t hairGeoset = (hairScalpId > 0) ? hairScalpId : 1;

            // Facial hair geosets from CharFacialHairStyles.dbc lookup
            uint32_t facialKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                                 (static_cast<uint32_t>(extra.sexId) << 8) |
                                 static_cast<uint32_t>(extra.facialHairId);
            auto itFacial = facialHairGeosetMap_.find(facialKey);
            if (itFacial != facialHairGeosetMap_.end()) {
                const auto& fhg = itFacial->second;
                // DBC values are variation indices within each group; add group base
                activeGeosets.insert(static_cast<uint16_t>(100 + std::max(fhg.geoset100, static_cast<uint16_t>(1))));
                activeGeosets.insert(static_cast<uint16_t>(300 + std::max(fhg.geoset300, static_cast<uint16_t>(1))));
                activeGeosets.insert(static_cast<uint16_t>(200 + std::max(fhg.geoset200, static_cast<uint16_t>(1))));
            } else {
                activeGeosets.insert(kGeosetDefaultConnector); // Default group 1: no extra
                activeGeosets.insert(201); // Default group 2: no facial hair
                activeGeosets.insert(301); // Default group 3: no facial hair
            }

            // Default equipment geosets (bare/no armor)
            // CharGeosets: group 4=gloves(forearm), 5=boots(shin), 8=sleeves, 12=tabard, 13=pants
            std::unordered_set<uint16_t> modelGeosets;
            std::unordered_map<uint16_t, uint16_t> firstByGroup;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (const auto& b : md->batches) {
                    const uint16_t sid = b.submeshId;
                    modelGeosets.insert(sid);
                    const uint16_t group = static_cast<uint16_t>(sid / 100);
                    auto it = firstByGroup.find(group);
                    if (it == firstByGroup.end() || sid < it->second) {
                        firstByGroup[group] = sid;
                    }
                }
            }
            auto pickGeoset = [&](uint16_t preferred, uint16_t group) -> uint16_t {
                if (preferred != 0 && modelGeosets.count(preferred) > 0) return preferred;
                auto it = firstByGroup.find(group);
                if (it != firstByGroup.end()) return it->second;
                return preferred;
            };

            uint16_t geosetGloves = pickGeoset(kGeosetBareForearms, 4);
            uint16_t geosetBoots = pickGeoset(kGeosetBareShins, 5);
            uint16_t geosetSleeves = pickGeoset(kGeosetBareSleeves, 8);
            uint16_t geosetPants = pickGeoset(kGeosetBarePants, 13);
            uint16_t geosetCape = 0;       // Group 15 disabled unless cape is equipped
            uint16_t geosetTabard = pickGeoset(kGeosetDefaultTabard, 12);
            uint16_t geosetBelt = 0;       // Group 18 disabled unless belt is equipped
            rendering::VkTexture* npcCapeTextureId = nullptr;

            // Load equipment geosets from ItemDisplayInfo.dbc
            // DBC columns: 7=GeosetGroup[0], 8=GeosetGroup[1], 9=GeosetGroup[2]
            auto itemDisplayDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
            const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
            if (itemDisplayDbc) {
                // Equipment slots: 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
                const uint32_t fGG1 = idiL ? (*idiL)["GeosetGroup1"] : 7;

                auto readGeosetGroup = [&](int slot, const char* slotName) -> uint32_t {
                    uint32_t did = extra.equipDisplayId[slot];
                    if (did == 0) return 0;
                    int32_t idx = itemDisplayDbc->findRecordById(did);
                    if (idx < 0) {
                        LOG_DEBUG("NPC equip slot ", slotName, " displayId=", did, " NOT FOUND in ItemDisplayInfo.dbc");
                        return 0;
                    }
                    uint32_t gg = itemDisplayDbc->getUInt32(static_cast<uint32_t>(idx), fGG1);
                    LOG_DEBUG("NPC equip slot ", slotName, " displayId=", did, " GeosetGroup1=", gg);
                    return gg;
                };

                // Chest (slot 3) → group 8 (sleeves/wristbands)
                {
                    uint32_t gg = readGeosetGroup(3, "chest");
                    if (gg > 0) geosetSleeves = pickGeoset(static_cast<uint16_t>(kGeosetBareSleeves + gg), 8);
                }

                // Legs (slot 5) → group 13 (trousers)
                {
                    uint32_t gg = readGeosetGroup(5, "legs");
                    if (gg > 0) geosetPants = pickGeoset(static_cast<uint16_t>(kGeosetBarePants + gg), 13);
                }

                // Feet (slot 6) → group 5 (boots/shins)
                {
                    uint32_t gg = readGeosetGroup(6, "feet");
                    if (gg > 0) geosetBoots = pickGeoset(static_cast<uint16_t>(501 + gg), 5);
                }

                // Hands (slot 8) → group 4 (gloves/forearms)
                {
                    uint32_t gg = readGeosetGroup(8, "hands");
                    if (gg > 0) geosetGloves = pickGeoset(static_cast<uint16_t>(kGeosetBareForearms + gg), 4);
                }

                // Wrists (slot 7) → group 8 (sleeves, only if chest didn't set it)
                {
                    uint32_t gg = readGeosetGroup(7, "wrist");
                    if (gg > 0 && geosetSleeves == pickGeoset(kGeosetBareSleeves, 8))
                        geosetSleeves = pickGeoset(static_cast<uint16_t>(kGeosetBareSleeves + gg), 8);
                }

                // Belt (slot 4) → group 18 (buckle)
                {
                    uint32_t gg = readGeosetGroup(4, "belt");
                    if (gg > 0) geosetBelt = static_cast<uint16_t>(1801 + gg);
                }

                // Tabard (slot 9) → group 12 (tabard/robe mesh)
                {
                    uint32_t gg = readGeosetGroup(9, "tabard");
                    if (gg > 0) geosetTabard = pickGeoset(static_cast<uint16_t>(1200 + gg), 12);
                }

                // Cape (slot 10) → group 15
                if (extra.equipDisplayId[10] != 0) {
                    int32_t idx = itemDisplayDbc->findRecordById(extra.equipDisplayId[10]);
                    if (idx >= 0) {
                        geosetCape = kGeosetWithCape;
                        const bool npcIsFemale = (extra.sexId == 1);
                        const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                        std::vector<std::string> capeNames;
                        auto addName = [&](const std::string& n) {
                            if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                                capeNames.push_back(n);
                            }
                        };
                        std::string leftName = itemDisplayDbc->getString(static_cast<uint32_t>(idx), leftTexField);
                        addName(leftName);

                        auto hasBlpExt = [](const std::string& p) {
                            if (p.size() < 4) return false;
                            std::string ext = p.substr(p.size() - 4);
                            std::transform(ext.begin(), ext.end(), ext.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return ext == ".blp";
                        };
                        std::vector<std::string> capeCandidates;
                        auto addCapeCandidate = [&](const std::string& p) {
                            if (p.empty()) return;
                            if (std::find(capeCandidates.begin(), capeCandidates.end(), p) == capeCandidates.end()) {
                                capeCandidates.push_back(p);
                            }
                        };
                        for (const auto& nameRaw : capeNames) {
                            std::string name = nameRaw;
                            std::replace(name.begin(), name.end(), '/', '\\');
                            const bool hasDir = (name.find('\\') != std::string::npos);
                            const bool hasExt = hasBlpExt(name);
                            if (hasDir) {
                                addCapeCandidate(name);
                                if (!hasExt) addCapeCandidate(name + ".blp");
                            } else {
                                std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name;
                                std::string baseTex = "Item\\TextureComponents\\Cape\\" + name;
                                addCapeCandidate(baseObj);
                                addCapeCandidate(baseTex);
                                if (!hasExt) {
                                    addCapeCandidate(baseObj + ".blp");
                                    addCapeCandidate(baseTex + ".blp");
                                }
                                addCapeCandidate(baseObj + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                addCapeCandidate(baseObj + "_U.blp");
                                addCapeCandidate(baseTex + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                addCapeCandidate(baseTex + "_U.blp");
                            }
                        }
                        const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                        for (const auto& candidate : capeCandidates) {
                            rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                            if (tex && tex != whiteTex) {
                                npcCapeTextureId = tex;
                                break;
                            }
                        }
                    }
                }
            }

            // Apply equipment geosets
            activeGeosets.insert(geosetGloves);
            activeGeosets.insert(geosetBoots);
            activeGeosets.insert(geosetSleeves);
            activeGeosets.insert(geosetPants);
            if (geosetCape != 0) {
                activeGeosets.insert(geosetCape);
            }
            if (geosetTabard != 0) {
                activeGeosets.insert(geosetTabard);
            }
            if (geosetBelt != 0) {
                activeGeosets.insert(geosetBelt);
            }
            activeGeosets.insert(pickGeoset(kGeosetDefaultEars, 7));
            activeGeosets.insert(pickGeoset(kGeosetDefaultKneepads, 9));
            activeGeosets.insert(pickGeoset(kGeosetBareFeet, 20));
            // Keep all model-present torso variants active to avoid missing male
            // abdomen/waist sections when a single 5xx pick is wrong.
            for (uint16_t sid : modelGeosets) {
                if ((sid / 100) == 5) activeGeosets.insert(sid);
            }
            // Keep all model-present pelvis variants active to avoid missing waist/belt
            // sections on some humanoid males when a single 9xx variant is wrong.
            for (uint16_t sid : modelGeosets) {
                if ((sid / 100) == 9) activeGeosets.insert(sid);
            }

            // Hide hair under helmets: replace style-specific scalp with bald scalp
            if (extra.equipDisplayId[0] != 0 && hairGeoset > 1) {
                activeGeosets.erase(hairGeoset);                              // Remove style scalp
                activeGeosets.erase(static_cast<uint16_t>(100 + hairGeoset)); // Remove style group 1
                activeGeosets.insert(1);    // Bald scalp cap (group 0)
                activeGeosets.insert(kGeosetDefaultConnector);  // Default group 1 connector
            }

            charRenderer->setActiveGeosets(instanceId, activeGeosets);
            if (geosetCape != 0 && npcCapeTextureId) {
                charRenderer->setGroupTextureOverride(instanceId, 15, npcCapeTextureId);
                if (const auto* md = charRenderer->getModelData(modelId)) {
                    for (size_t ti = 0; ti < md->textures.size(); ti++) {
                        if (md->textures[ti].type == 2) {
                            charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), npcCapeTextureId);
                        }
                    }
                }
            }
            LOG_DEBUG("Set humanoid geosets: hair=", static_cast<int>(hairGeoset),
                      " sleeves=", geosetSleeves, " pants=", geosetPants,
                      " boots=", geosetBoots, " gloves=", geosetGloves);

            // NOTE: NPC helmet attachment with fallback logic to use bone 0 if attachment
            // point 11 is missing. This improves compatibility with models that don't have
            // attachment 11 explicitly defined.
            static constexpr bool kEnableNpcHelmetAttachmentsMainPath = true;
            // Load and attach helmet model if equipped
            if (kEnableNpcHelmetAttachmentsMainPath && extra.equipDisplayId[0] != 0 && itemDisplayDbc) {
                int32_t helmIdx = itemDisplayDbc->findRecordById(extra.equipDisplayId[0]);
                if (helmIdx >= 0) {
                    // Get helmet model name from ItemDisplayInfo.dbc (LeftModel)
                    std::string helmModelName = itemDisplayDbc->getString(static_cast<uint32_t>(helmIdx), idiL ? (*idiL)["LeftModel"] : 1);
                    if (!helmModelName.empty()) {
                        // Convert .mdx to .m2
                        size_t dotPos = helmModelName.rfind('.');
                        if (dotPos != std::string::npos) {
                            helmModelName = helmModelName.substr(0, dotPos);
                        }

                        // WoW helmet M2 files have per-race/gender variants with a suffix
                        // e.g. Helm_Plate_B_01Stormwind_HuM.M2 for Human Male
                        // ChrRaces.dbc ClientPrefix values (raceId → prefix):
                        static const std::unordered_map<uint8_t, std::string> racePrefix = {
                            {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                            {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                        };
                        std::string genderSuffix = (extra.sexId == 0) ? "M" : "F";
                        std::string raceSuffix;
                        auto itRace = racePrefix.find(extra.raceId);
                        if (itRace != racePrefix.end()) {
                            raceSuffix = "_" + itRace->second + genderSuffix;
                        }

                        // Try race/gender-specific variant first, then base name
                        std::string helmPath;
                        std::vector<uint8_t> helmData;
                        if (!raceSuffix.empty()) {
                            helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + raceSuffix + ".m2";
                            helmData = assetManager->readFile(helmPath);
                        }
                        if (helmData.empty()) {
                            helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + ".m2";
                            helmData = assetManager->readFile(helmPath);
                        }

                        if (!helmData.empty()) {
                            auto helmModel = pipeline::M2Loader::load(helmData);
                            // Load skin (only for WotLK M2s)
                            std::string skinPath = helmPath.substr(0, helmPath.size() - 3) + "00.skin";
                            auto skinData = assetManager->readFile(skinPath);
                            if (!skinData.empty() && helmModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, helmModel);
                            }

                            if (helmModel.isValid()) {
                                // Attachment point 11 = Head
                                uint32_t helmModelId = nextCreatureModelId_++;
                                // Get texture from ItemDisplayInfo (LeftModelTexture)
                                std::string helmTexName = itemDisplayDbc->getString(static_cast<uint32_t>(helmIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
                                std::string helmTexPath;
                                if (!helmTexName.empty()) {
                                    // Try race/gender suffixed texture first
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Head\\" + helmTexName + raceSuffix + ".blp";
                                        if (assetManager->fileExists(suffixedTex)) {
                                            helmTexPath = suffixedTex;
                                        }
                                    }
                                    if (helmTexPath.empty()) {
                                        helmTexPath = "Item\\ObjectComponents\\Head\\" + helmTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 0, helmModel, helmModelId, helmTexPath);
                                if (!attached) {
                                    attached = charRenderer->attachWeapon(instanceId, 11, helmModel, helmModelId, helmTexPath);
                                }
                                if (attached) {
                                    LOG_DEBUG("Attached helmet model: ", helmPath, " tex: ", helmTexPath);
                                }
                            }
                        }
                    }
                }
            }

            // NPC shoulder attachment: slot 1 = shoulder in the NPC equipment array.
            // Shoulders have TWO M2 models (left + right) at attachment points 5 and 6.
            if (extra.equipDisplayId[1] != 0) {
                int32_t shoulderIdx = itemDisplayDbc->findRecordById(extra.equipDisplayId[1]);
                if (shoulderIdx >= 0) {
                    const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
                    const uint32_t rightModelField = idiL ? (*idiL)["RightModel"] : 2u;
                    const uint32_t leftTexFieldS = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                    const uint32_t rightTexFieldS = idiL ? (*idiL)["RightModelTexture"] : 4u;

                    static const std::unordered_map<uint8_t, std::string> shoulderRacePrefix = {
                        {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                        {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                    };
                    std::string genderSuffix = (extra.sexId == 0) ? "M" : "F";
                    std::string raceSuffix;
                    {
                        auto itRace = shoulderRacePrefix.find(extra.raceId);
                        if (itRace != shoulderRacePrefix.end()) {
                            raceSuffix = "_" + itRace->second + genderSuffix;
                        }
                    }

                    // Left shoulder (attachment point 5) using LeftModel
                    std::string leftModelName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), leftModelField);
                    if (!leftModelName.empty()) {
                        size_t dotPos = leftModelName.rfind('.');
                        if (dotPos != std::string::npos) leftModelName = leftModelName.substr(0, dotPos);

                        std::string leftPath;
                        std::vector<uint8_t> leftData;
                        if (!raceSuffix.empty()) {
                            leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + raceSuffix + ".m2";
                            leftData = assetManager->readFile(leftPath);
                        }
                        if (leftData.empty()) {
                            leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + ".m2";
                            leftData = assetManager->readFile(leftPath);
                        }
                        if (!leftData.empty()) {
                            auto leftModel = pipeline::M2Loader::load(leftData);
                            std::string skinPath = leftPath.substr(0, leftPath.size() - 3) + "00.skin";
                            auto skinData = assetManager->readFile(skinPath);
                            if (!skinData.empty() && leftModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, leftModel);
                            }
                            if (leftModel.isValid()) {
                                uint32_t leftModelId = nextCreatureModelId_++;
                                std::string leftTexName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), leftTexFieldS);
                                std::string leftTexPath;
                                if (!leftTexName.empty()) {
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + raceSuffix + ".blp";
                                        if (assetManager->fileExists(suffixedTex)) leftTexPath = suffixedTex;
                                    }
                                    if (leftTexPath.empty()) {
                                        leftTexPath = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 5, leftModel, leftModelId, leftTexPath);
                                if (attached) {
                                    LOG_DEBUG("NPC attached left shoulder: ", leftPath, " tex: ", leftTexPath);
                                }
                            }
                        }
                    }

                    // Right shoulder (attachment point 6) using RightModel
                    std::string rightModelName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), rightModelField);
                    if (!rightModelName.empty()) {
                        size_t dotPos = rightModelName.rfind('.');
                        if (dotPos != std::string::npos) rightModelName = rightModelName.substr(0, dotPos);

                        std::string rightPath;
                        std::vector<uint8_t> rightData;
                        if (!raceSuffix.empty()) {
                            rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + raceSuffix + ".m2";
                            rightData = assetManager->readFile(rightPath);
                        }
                        if (rightData.empty()) {
                            rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + ".m2";
                            rightData = assetManager->readFile(rightPath);
                        }
                        if (!rightData.empty()) {
                            auto rightModel = pipeline::M2Loader::load(rightData);
                            std::string skinPath = rightPath.substr(0, rightPath.size() - 3) + "00.skin";
                            auto skinData = assetManager->readFile(skinPath);
                            if (!skinData.empty() && rightModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, rightModel);
                            }
                            if (rightModel.isValid()) {
                                uint32_t rightModelId = nextCreatureModelId_++;
                                std::string rightTexName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), rightTexFieldS);
                                std::string rightTexPath;
                                if (!rightTexName.empty()) {
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + raceSuffix + ".blp";
                                        if (assetManager->fileExists(suffixedTex)) rightTexPath = suffixedTex;
                                    }
                                    if (rightTexPath.empty()) {
                                        rightTexPath = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 6, rightModel, rightModelId, rightTexPath);
                                if (attached) {
                                    LOG_DEBUG("NPC attached right shoulder: ", rightPath, " tex: ", rightTexPath);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // With full humanoid overrides disabled, some character-style NPC models still render
    // conflicting clothing geosets at once (global capes, robe skirts over trousers).
    // Normalize only clothing groups while leaving all other model batches untouched.
    if (const auto* md = charRenderer->getModelData(modelId)) {
        std::unordered_set<uint16_t> allGeosets;
        std::unordered_map<uint16_t, uint16_t> firstByGroup;
        bool hasGroup3 = false;  // glove/forearm variants
        bool hasGroup4 = false;  // glove/forearm variants (some models)
        bool hasGroup8 = false;  // sleeve/wrist variants
        bool hasGroup12 = false; // tabard variants
        bool hasGroup13 = false; // trousers/robe skirt variants
        bool hasGroup15 = false; // cloak variants
        for (const auto& b : md->batches) {
            const uint16_t sid = b.submeshId;
            const uint16_t group = static_cast<uint16_t>(sid / 100);
            allGeosets.insert(sid);
            auto itFirst = firstByGroup.find(group);
            if (itFirst == firstByGroup.end() || sid < itFirst->second) {
                firstByGroup[group] = sid;
            }
            if (group == 3) hasGroup3 = true;
            if (group == 4) hasGroup4 = true;
            if (group == 8) hasGroup8 = true;
            if (group == 12) hasGroup12 = true;
            if (group == 13) hasGroup13 = true;
            if (group == 15) hasGroup15 = true;
        }

        // Only apply to humanoid-like clothing models.
        if (hasGroup3 || hasGroup4 || hasGroup8 || hasGroup12 || hasGroup13 || hasGroup15) {
            bool hasRenderableCape = false;
            bool hasEquippedTabard = false;
            bool hasHumanoidExtra = false;
            uint8_t extraRaceId = 0;
            uint8_t extraSexId = 0;
            uint16_t selectedHairScalp = 1;
            uint16_t selectedFacial200 = 200;
            uint16_t selectedFacial300 = 300;
            uint16_t selectedFacial300Alt = 300;
            bool wantsFacialHair = false;
            uint32_t equipChestGG = 0, equipLegsGG = 0, equipFeetGG = 0;
            std::unordered_set<uint16_t> hairScalpGeosetsForRaceSex;
            if (itDisplayData != displayDataMap_.end() &&
                itDisplayData->second.extraDisplayId != 0) {
                auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
                if (itExtra != humanoidExtraMap_.end()) {
                    hasHumanoidExtra = true;
                    extraRaceId = itExtra->second.raceId;
                    extraSexId = itExtra->second.sexId;
                    hasEquippedTabard = (itExtra->second.equipDisplayId[9] != 0);
                    uint32_t hairKey = (static_cast<uint32_t>(extraRaceId) << 16) |
                                       (static_cast<uint32_t>(extraSexId) << 8) |
                                       static_cast<uint32_t>(itExtra->second.hairStyleId);
                    auto itHairGeo = hairGeosetMap_.find(hairKey);
                    if (itHairGeo != hairGeosetMap_.end() && itHairGeo->second > 0) {
                        selectedHairScalp = itHairGeo->second;
                    }
                    uint32_t facialKey = (static_cast<uint32_t>(extraRaceId) << 16) |
                                         (static_cast<uint32_t>(extraSexId) << 8) |
                                         static_cast<uint32_t>(itExtra->second.facialHairId);
                    wantsFacialHair = (itExtra->second.facialHairId != 0);
                    auto itFacial = facialHairGeosetMap_.find(facialKey);
                    if (itFacial != facialHairGeosetMap_.end()) {
                        selectedFacial200 = static_cast<uint16_t>(200 + itFacial->second.geoset200);
                        selectedFacial300 = static_cast<uint16_t>(300 + itFacial->second.geoset300);
                        selectedFacial300Alt = static_cast<uint16_t>(300 + itFacial->second.geoset200);
                    }
                    for (const auto& [k, v] : hairGeosetMap_) {
                        uint8_t race = static_cast<uint8_t>((k >> 16) & 0xFF);
                        uint8_t sex = static_cast<uint8_t>((k >> 8) & 0xFF);
                        if (race == extraRaceId && sex == extraSexId && v > 0 && v < 100) {
                            hairScalpGeosetsForRaceSex.insert(v);
                        }
                    }
                    auto itemDisplayDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
                    const auto* idiL = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

                    uint32_t capeDisplayId = itExtra->second.equipDisplayId[10];
                    if (capeDisplayId != 0 && itemDisplayDbc) {
                            int32_t recIdx = itemDisplayDbc->findRecordById(capeDisplayId);
                            if (recIdx >= 0) {
                                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                                const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;
                                std::vector<std::string> capeNames;
                                auto addName = [&](const std::string& n) {
                                    if (!n.empty() &&
                                        std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                                        capeNames.push_back(n);
                                    }
                                };
                                addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), leftTexField));
                                addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), rightTexField));

                                auto hasBlpExt = [](const std::string& p) {
                                    if (p.size() < 4) return false;
                                    std::string ext = p.substr(p.size() - 4);
                                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    return ext == ".blp";
                                };

                                const bool npcIsFemale = (itExtra->second.sexId == 1);
                                std::vector<std::string> candidates;
                                auto addCandidate = [&](const std::string& p) {
                                    if (p.empty()) return;
                                    if (std::find(candidates.begin(), candidates.end(), p) == candidates.end()) {
                                        candidates.push_back(p);
                                    }
                                };

                                for (const auto& raw : capeNames) {
                                    std::string name = raw;
                                    std::replace(name.begin(), name.end(), '/', '\\');
                                    const bool hasDir = (name.find('\\') != std::string::npos);
                                    const bool hasExt = hasBlpExt(name);
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
                                        addCandidate(baseObj + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                        addCandidate(baseObj + "_U.blp");
                                        addCandidate(baseTex + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                        addCandidate(baseTex + "_U.blp");
                                    }
                                }

                                for (const auto& p : candidates) {
                                    if (assetManager->fileExists(p)) {
                                        hasRenderableCape = true;
                                        break;
                                    }
                                }
                            }
                    }

                    // Read GeosetGroup1 from equipment to drive clothed mesh selection
                    if (itemDisplayDbc) {
                        const uint32_t fGG1 = idiL ? (*idiL)["GeosetGroup1"] : 7;
                        auto readGG = [&](uint32_t did) -> uint32_t {
                            if (did == 0) return 0;
                            int32_t idx = itemDisplayDbc->findRecordById(did);
                            return (idx >= 0) ? itemDisplayDbc->getUInt32(static_cast<uint32_t>(idx), fGG1) : 0;
                        };
                        equipChestGG = readGG(itExtra->second.equipDisplayId[3]);
                        if (equipChestGG == 0) equipChestGG = readGG(itExtra->second.equipDisplayId[2]); // shirt fallback
                        equipLegsGG = readGG(itExtra->second.equipDisplayId[5]);
                        equipFeetGG = readGG(itExtra->second.equipDisplayId[6]);
                    }
                }
            }

            std::unordered_set<uint16_t> normalizedGeosets;
            for (uint16_t sid : allGeosets) {
                const uint16_t group = static_cast<uint16_t>(sid / 100);
                if (group == 3 || group == 4 || group == 8 || group == 12 || group == 13 || group == 15) continue;
                // Some humanoid models carry cloak cloth in group 16. Strip this too
                // when no cape is equipped to avoid "everyone has a cape".
                if (!hasRenderableCape && group == 16) continue;
                // Group 0 can contain multiple scalp/hair meshes. Keep only the selected
                // race/sex/style scalp to avoid overlapping broken hair.
                if (hasHumanoidExtra && sid < 100 && hairScalpGeosetsForRaceSex.count(sid) > 0 && sid != selectedHairScalp) {
                    continue;
                }
                // Group 1 contains connector variants that mirror scalp style.
                if (hasHumanoidExtra && group == 1) {
                    const uint16_t selectedConnector = static_cast<uint16_t>(100 + std::max<uint16_t>(selectedHairScalp, 1));
                    if (sid != selectedConnector) {
                        // Keep fallback connector only when selected one does not exist on this model.
                        if (sid != 101 || allGeosets.count(selectedConnector) > 0) {
                            continue;
                        }
                    }
                }
                // Group 2 facial variants: keep selected variant; fallback only if missing.
                if (hasHumanoidExtra && group == 2) {
                    if (!wantsFacialHair) {
                        continue;
                    }
                    if (sid != selectedFacial200) {
                        if (sid != 200 && sid != 201) {
                            continue;
                        }
                        if (allGeosets.count(selectedFacial200) > 0) {
                            continue;
                        }
                    }
                }
                normalizedGeosets.insert(sid);
            }

            auto pickFromGroup = [&](uint16_t preferredSid, uint16_t group) -> uint16_t {
                if (allGeosets.count(preferredSid) > 0) return preferredSid;
                auto it = firstByGroup.find(group);
                if (it != firstByGroup.end()) return it->second;
                return 0;
            };

            // Intentionally do not add group 3 (glove/forearm accessory meshes).
            // Even "bare" variants can produce unwanted looped arm geometry on NPCs.

            if (hasGroup4) {
                uint16_t wantBoots = (equipFeetGG > 0) ? static_cast<uint16_t>(400 + equipFeetGG) : kGeosetBareForearms;
                uint16_t bootsSid = pickFromGroup(wantBoots, 4);
                if (bootsSid != 0) normalizedGeosets.insert(bootsSid);
            }

            // Add sleeve/wrist meshes when chest armor calls for them.
            if (hasGroup8 && equipChestGG > 0) {
                uint16_t wantSleeves = static_cast<uint16_t>(800 + equipChestGG);
                uint16_t sleeveSid = pickFromGroup(wantSleeves, 8);
                if (sleeveSid != 0) normalizedGeosets.insert(sleeveSid);
            }

            // Show tabard mesh only when CreatureDisplayInfoExtra equips one.
            if (hasGroup12 && hasEquippedTabard) {
                uint16_t wantTabard = kGeosetDefaultTabard;  // Default fallback

                // Try to read tabard geoset variant from ItemDisplayInfo.dbc (slot 9)
                if (hasHumanoidExtra && itDisplayData != displayDataMap_.end() &&
                    itDisplayData->second.extraDisplayId != 0) {
                    auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
                    if (itExtra != humanoidExtraMap_.end()) {
                        uint32_t tabardDisplayId = itExtra->second.equipDisplayId[9];
                        if (tabardDisplayId != 0) {
                            auto itemDisplayDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
                            const auto* idiL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                            if (itemDisplayDbc && idiL) {
                                int32_t tabardIdx = itemDisplayDbc->findRecordById(tabardDisplayId);
                                if (tabardIdx >= 0) {
                                    // Get geoset variant from ItemDisplayInfo GeosetGroup1 field
                                    const uint32_t ggField = (*idiL)["GeosetGroup1"];
                                    uint32_t tabardGG = itemDisplayDbc->getUInt32(static_cast<uint32_t>(tabardIdx), ggField);
                                    if (tabardGG > 0) {
                                        wantTabard = static_cast<uint16_t>(1200 + tabardGG);
                                    }
                                }
                            }
                        }
                    }
                }

                uint16_t tabardSid = pickFromGroup(wantTabard, 12);
                if (tabardSid != 0) normalizedGeosets.insert(tabardSid);
            }

            // Some mustache/goatee variants are authored in facial group 3xx.
            // Re-add selected facial 3xx plus low-index facial fallbacks.
            if (hasHumanoidExtra && wantsFacialHair) {
                // Prefer alt channel first (often chin-beard), then primary.
                uint16_t facial300Sid = pickFromGroup(selectedFacial300Alt, 3);
                if (facial300Sid == 0) facial300Sid = pickFromGroup(selectedFacial300, 3);
                if (facial300Sid != 0) normalizedGeosets.insert(facial300Sid);
                if (facial300Sid == 0) {
                    if (allGeosets.count(300) > 0) normalizedGeosets.insert(300);
                    else if (allGeosets.count(301) > 0) normalizedGeosets.insert(301);
                }
            }

            // Prefer trousers geoset; use covered variant when legs armor exists.
            if (hasGroup13) {
                uint16_t wantPants = (equipLegsGG > 0) ? static_cast<uint16_t>(1300 + equipLegsGG) : kGeosetBarePants;
                uint16_t pantsSid = pickFromGroup(wantPants, 13);
                if (pantsSid != 0) normalizedGeosets.insert(pantsSid);
            }

            // Prefer explicit cloak variant only when a cape is equipped.
            if (hasGroup15 && hasRenderableCape) {
                uint16_t capeSid = pickFromGroup(kGeosetWithCape, 15);
                if (capeSid != 0) normalizedGeosets.insert(capeSid);
            }

            if (!normalizedGeosets.empty()) {
                charRenderer->setActiveGeosets(instanceId, normalizedGeosets);
            }
        }
    }

    // Try attaching NPC held weapons; if update fields are not ready yet,
    // IN_GAME retry loop will attempt again shortly.
    bool weaponsAttachedNow = tryAttachCreatureVirtualWeapons(guid, instanceId);

    // Spawn in the correct pose. If the server marked this creature dead before
    // the queued spawn was processed, start directly in death animation.
    if (deadCreatureGuids_.count(guid)) {
        charRenderer->playAnimation(instanceId, 1, false); // Death
    } else {
        charRenderer->playAnimation(instanceId, 0, true); // Idle
    }
    charRenderer->startFadeIn(instanceId, 0.5f);

    // Track instance
    creatureInstances_[guid] = instanceId;
    creatureModelIds_[guid] = modelId;
    creatureRenderPosCache_[guid] = renderPos;
    if (weaponsAttachedNow) {
        creatureWeaponsAttached_.insert(guid);
        creatureWeaponAttachAttempts_.erase(guid);
    } else {
        creatureWeaponsAttached_.erase(guid);
        creatureWeaponAttachAttempts_[guid] = 1;
    }
    LOG_DEBUG("Spawned creature: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

void Application::spawnOnlinePlayer(uint64_t guid,
                                    uint8_t raceId,
                                    uint8_t genderId,
                                    uint32_t appearanceBytes,
                                    uint8_t facialFeatures,
                                    float x, float y, float z, float orientation) {
    if (!renderer || !renderer->getCharacterRenderer() || !assetManager || !assetManager->isInitialized()) return;
    if (playerInstances_.count(guid)) return;

    // Skip local player — already spawned as the main character
    if (gameHandler) {
        uint64_t localGuid = gameHandler->getPlayerGuid();
        uint64_t activeGuid = gameHandler->getActiveCharacterGuid();
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_)) {
            return;
        }
    }
    auto* charRenderer = renderer->getCharacterRenderer();

    // Base geometry model: cache by (race, gender)
    uint32_t cacheKey = (static_cast<uint32_t>(raceId) << 8) | static_cast<uint32_t>(genderId & 0xFF);
    uint32_t modelId = 0;
    auto itCache = playerModelCache_.find(cacheKey);
    if (itCache != playerModelCache_.end()) {
        modelId = itCache->second;
    } else {
        game::Race race = static_cast<game::Race>(raceId);
        game::Gender gender = (genderId == 1) ? game::Gender::FEMALE : game::Gender::MALE;
        std::string m2Path = game::getPlayerModelPath(race, gender);
        if (m2Path.empty()) {
            LOG_WARNING("spawnOnlinePlayer: unknown race/gender for guid 0x", std::hex, guid, std::dec,
                        " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId));
            return;
        }

        // Parse modelDir/baseName for skin/anim loading
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
            if (dot != std::string::npos) baseName = baseName.substr(0, dot);
        }

        auto m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            LOG_WARNING("spawnOnlinePlayer: failed to read M2: ", m2Path);
            return;
        }

        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.vertices.empty()) {
            LOG_WARNING("spawnOnlinePlayer: failed to parse M2: ", m2Path);
            return;
        }

        // Skin file (only for WotLK M2s - vanilla has embedded skin)
        std::string skinPath = modelDir + baseName + "00.skin";
        auto skinData = assetManager->readFile(skinPath);
        if (!skinData.empty() && model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, model);
        }

        // After skin loading, full model must be valid (vertices + indices)
        if (!model.isValid()) {
            LOG_WARNING("spawnOnlinePlayer: failed to load skin for M2: ", m2Path);
            return;
        }

        // Load only core external animations (stand/walk/run) to avoid stalls
        for (uint32_t si = 0; si < model.sequences.size(); si++) {
            if (!(model.sequences[si].flags & 0x20)) {
                uint32_t animId = model.sequences[si].id;
                if (animId != 0 && animId != 4 && animId != 5) continue;
                char animFileName[256];
                snprintf(animFileName, sizeof(animFileName),
                         "%s%s%04u-%02u.anim",
                         modelDir.c_str(),
                         baseName.c_str(),
                         animId,
                         model.sequences[si].variationIndex);
                auto animData = assetManager->readFileOptional(animFileName);
                if (!animData.empty()) {
                    pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
                }
            }
        }

        modelId = nextPlayerModelId_++;
        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("spawnOnlinePlayer: failed to load model to GPU: ", m2Path);
            return;
        }

        playerModelCache_[cacheKey] = modelId;
    }

    // Determine texture slots once per model
    {
        auto [slotIt, inserted] = playerTextureSlotsByModelId_.try_emplace(modelId);
        if (inserted) {
            PlayerTextureSlots slots;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    uint32_t t = md->textures[ti].type;
                    if (t == 1 && slots.skin < 0) slots.skin = static_cast<int>(ti);
                    else if (t == 6 && slots.hair < 0) slots.hair = static_cast<int>(ti);
                    else if (t == 8 && slots.underwear < 0) slots.underwear = static_cast<int>(ti);
                }
            }
            slotIt->second = slots;
        }
    }

    // Create instance at server position
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    float renderYaw = orientation + glm::radians(90.0f);
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), 1.0f);
    if (instanceId == 0) return;

    // Resolve skin/hair texture paths via CharSections, then apply as per-instance overrides
    const char* raceFolderName = "Human";
    switch (static_cast<game::Race>(raceId)) {
        case game::Race::HUMAN: raceFolderName = "Human"; break;
        case game::Race::ORC: raceFolderName = "Orc"; break;
        case game::Race::DWARF: raceFolderName = "Dwarf"; break;
        case game::Race::NIGHT_ELF: raceFolderName = "NightElf"; break;
        case game::Race::UNDEAD: raceFolderName = "Scourge"; break;
        case game::Race::TAUREN: raceFolderName = "Tauren"; break;
        case game::Race::GNOME: raceFolderName = "Gnome"; break;
        case game::Race::TROLL: raceFolderName = "Troll"; break;
        case game::Race::BLOOD_ELF: raceFolderName = "BloodElf"; break;
        case game::Race::DRAENEI: raceFolderName = "Draenei"; break;
        default: break;
    }
    const char* genderFolder = (genderId == 1) ? "Female" : "Male";
    std::string raceGender = std::string(raceFolderName) + genderFolder;
    std::string bodySkinPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "Skin00_00.blp";
    std::string pelvisPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "NakedPelvisSkin00_00.blp";
    std::vector<std::string> underwearPaths;
    std::string hairTexturePath;
    std::string faceLowerPath;
    std::string faceUpperPath;

    uint8_t skinId = appearanceBytes & 0xFF;
    uint8_t faceId = (appearanceBytes >> 8) & 0xFF;
    uint8_t hairStyleId = (appearanceBytes >> 16) & 0xFF;
    uint8_t hairColorId = (appearanceBytes >> 24) & 0xFF;

    if (auto charSectionsDbc = assetManager->loadDBC("CharSections.dbc"); charSectionsDbc && charSectionsDbc->isLoaded()) {
        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);
        uint32_t targetRaceId = raceId;
        uint32_t targetSexId = genderId;

        bool foundSkin = false;
        bool foundUnderwear = false;
        bool foundHair = false;
        bool foundFaceLower = false;

        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
            uint32_t rRace = charSectionsDbc->getUInt32(r, csF.raceId);
            uint32_t rSex = charSectionsDbc->getUInt32(r, csF.sexId);
            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

            if (rRace != targetRaceId || rSex != targetSexId) continue;

            if (baseSection == 0 && !foundSkin && colorIndex == skinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) { bodySkinPath = tex1; foundSkin = true; }
            } else if (baseSection == 3 && !foundHair &&
                       variationIndex == hairStyleId && colorIndex == hairColorId) {
                hairTexturePath = charSectionsDbc->getString(r, csF.texture1);
                if (!hairTexturePath.empty()) foundHair = true;
            } else if (baseSection == 4 && !foundUnderwear && colorIndex == skinId) {
                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                    std::string tex = charSectionsDbc->getString(r, f);
                    if (!tex.empty()) underwearPaths.push_back(tex);
                }
                foundUnderwear = true;
            } else if (baseSection == 1 && !foundFaceLower &&
                       variationIndex == faceId && colorIndex == skinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) faceLowerPath = tex1;
                if (!tex2.empty()) faceUpperPath = tex2;
                foundFaceLower = true;
            }

            if (foundSkin && foundUnderwear && foundHair && foundFaceLower) break;
        }
    }

    // Composite base skin + face + underwear overlays
    rendering::VkTexture* compositeTex = nullptr;
    {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath);
        if (!faceLowerPath.empty()) layers.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) layers.push_back(faceUpperPath);
        for (const auto& up : underwearPaths) layers.push_back(up);
        if (layers.size() > 1) {
            compositeTex = charRenderer->compositeTextures(layers);
        } else {
            compositeTex = charRenderer->loadTexture(bodySkinPath);
        }
    }

    rendering::VkTexture* hairTex = nullptr;
    if (!hairTexturePath.empty()) {
        hairTex = charRenderer->loadTexture(hairTexturePath);
    }
    rendering::VkTexture* underwearTex = nullptr;
    if (!underwearPaths.empty()) underwearTex = charRenderer->loadTexture(underwearPaths[0]);
    else underwearTex = charRenderer->loadTexture(pelvisPath);

    const PlayerTextureSlots& slots = playerTextureSlotsByModelId_[modelId];
    if (slots.skin >= 0 && compositeTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skin), compositeTex);
    }
    if (slots.hair >= 0 && hairTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.hair), hairTex);
    }
    if (slots.underwear >= 0 && underwearTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.underwear), underwearTex);
    }

    // Geosets: body + hair/facial hair selections
    std::unordered_set<uint16_t> activeGeosets;
    // Body parts (group 0: IDs 0-99, some models use up to 27)
    for (uint16_t i = 0; i <= 99; i++) activeGeosets.insert(i);
    activeGeosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1));
    activeGeosets.insert(static_cast<uint16_t>(200 + facialFeatures + 1));
    activeGeosets.insert(kGeosetBareForearms);
    activeGeosets.insert(kGeosetBareShins);
    activeGeosets.insert(kGeosetDefaultEars);
    activeGeosets.insert(kGeosetBareSleeves);
    activeGeosets.insert(kGeosetDefaultKneepads);
    activeGeosets.insert(kGeosetBarePants);
    activeGeosets.insert(kGeosetWithCape);
    activeGeosets.insert(kGeosetBareFeet);
    charRenderer->setActiveGeosets(instanceId, activeGeosets);

    charRenderer->playAnimation(instanceId, 0, true);
    playerInstances_[guid] = instanceId;

    OnlinePlayerAppearanceState st;
    st.instanceId = instanceId;
    st.modelId = modelId;
    st.raceId = raceId;
    st.genderId = genderId;
    st.appearanceBytes = appearanceBytes;
    st.facialFeatures = facialFeatures;
    st.bodySkinPath = bodySkinPath;
    // Include face textures so compositeWithRegions can rebuild the full base
    if (!faceLowerPath.empty()) st.underwearPaths.push_back(faceLowerPath);
    if (!faceUpperPath.empty()) st.underwearPaths.push_back(faceUpperPath);
    for (const auto& up : underwearPaths) st.underwearPaths.push_back(up);
    onlinePlayerAppearance_[guid] = std::move(st);
}

void Application::setOnlinePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    if (!renderer || !renderer->getCharacterRenderer() || !assetManager || !assetManager->isInitialized()) return;

    // Skip local player — equipment handled by GameScreen::updateCharacterGeosets/Textures
    // via consumeOnlineEquipmentDirty(), which fires on the same server update.
    if (gameHandler) {
        uint64_t localGuid = gameHandler->getPlayerGuid();
        if (localGuid != 0 && guid == localGuid) return;
    }

    // If the player isn't spawned yet, store equipment until spawn.
    auto appIt = onlinePlayerAppearance_.find(guid);
    if (!playerInstances_.count(guid) || appIt == onlinePlayerAppearance_.end()) {
        pendingOnlinePlayerEquipment_[guid] = {displayInfoIds, inventoryTypes};
        return;
    }

    const OnlinePlayerAppearanceState& st = appIt->second;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;
    if (st.instanceId == 0 || st.modelId == 0) return;

    if (st.bodySkinPath.empty()) {
        LOG_WARNING("setOnlinePlayerEquipment: bodySkinPath empty for guid=0x", std::hex, guid, std::dec,
                    " instanceId=", st.instanceId, " — skipping equipment");
        return;
    }

    int nonZeroDisplay = 0;
    for (uint32_t d : displayInfoIds) if (d != 0) nonZeroDisplay++;
    LOG_WARNING("setOnlinePlayerEquipment: guid=0x", std::hex, guid, std::dec,
                " instanceId=", st.instanceId, " nonZeroDisplayIds=", nonZeroDisplay,
                " head=", displayInfoIds[0], " chest=", displayInfoIds[4],
                " legs=", displayInfoIds[6], " mainhand=", displayInfoIds[15]);

    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    auto findDisplayIdByInvType = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0 || displayInfoIds[s] == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return displayInfoIds[s];
            }
        }
        return 0;
    };

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return true;
            }
        }
        return false;
    };

    // --- Geosets ---
    // Mirror the same group-range logic as CharacterPreview::applyEquipment to
    // keep other-player rendering consistent with the local character preview.
    // Group 4 (4xx) = forearms/gloves, 5 (5xx) = shins/boots, 8 (8xx) = wrists/sleeves,
    // 13 (13xx) = legs/trousers.  Missing defaults caused the shin-mesh gap (status.md).
    std::unordered_set<uint16_t> geosets;
    // Body parts (group 0: IDs 0-99, some models use up to 27)
    for (uint16_t i = 0; i <= 99; i++) geosets.insert(i);

    uint8_t hairStyleId = static_cast<uint8_t>((st.appearanceBytes >> 16) & 0xFF);
    geosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1));
    geosets.insert(static_cast<uint16_t>(200 + st.facialFeatures + 1));
    geosets.insert(701);                  // Ears
    geosets.insert(kGeosetDefaultKneepads); // Kneepads
    geosets.insert(kGeosetBareFeet);        // Bare feet mesh

    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9;

    // Per-group defaults — overridden below when equipment provides a geoset value.
    uint16_t geosetGloves  = kGeosetBareForearms;
    uint16_t geosetBoots   = kGeosetBareShins;
    uint16_t geosetSleeves = kGeosetBareSleeves;
    uint16_t geosetPants   = kGeosetBarePants;

    // Chest/Shirt/Robe (invType 4,5,20) → wrist/sleeve group 8
    {
        uint32_t did = findDisplayIdByInvType({4, 5, 20});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetSleeves = static_cast<uint16_t>(kGeosetBareSleeves + gg1);
        // Robe kilt → leg group 13
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0) geosetPants = static_cast<uint16_t>(kGeosetBarePants + gg3);
    }

    // Legs (invType 7) → leg group 13
    {
        uint32_t did = findDisplayIdByInvType({7});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetPants = static_cast<uint16_t>(kGeosetBarePants + gg1);
    }

    // Feet/Boots (invType 8) → shin group 5
    {
        uint32_t did = findDisplayIdByInvType({8});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetBoots = static_cast<uint16_t>(501 + gg1);
    }

    // Hands/Gloves (invType 10) → forearm group 4
    {
        uint32_t did = findDisplayIdByInvType({10});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetGloves = static_cast<uint16_t>(kGeosetBareForearms + gg1);
    }

    // Wrists/Bracers (invType 9) → sleeve group 8 (only if chest/shirt didn't set it)
    {
        uint32_t did = findDisplayIdByInvType({9});
        if (did != 0 && geosetSleeves == kGeosetBareSleeves) {
            uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
            if (gg1 > 0) geosetSleeves = static_cast<uint16_t>(kGeosetBareSleeves + gg1);
        }
    }

    // Waist/Belt (invType 6) → buckle group 18
    uint16_t geosetBelt = 0;
    {
        uint32_t did = findDisplayIdByInvType({6});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetBelt = static_cast<uint16_t>(1801 + gg1);
    }

    geosets.insert(geosetGloves);
    geosets.insert(geosetBoots);
    geosets.insert(geosetSleeves);
    geosets.insert(geosetPants);
    if (geosetBelt != 0) geosets.insert(geosetBelt);
    // Back/Cloak (invType 16)
    geosets.insert(hasInvType({16}) ? kGeosetWithCape : kGeosetNoCape);
    // Tabard (invType 19)
    if (hasInvType({19})) geosets.insert(kGeosetDefaultTabard);

    // Hide hair under helmets: replace style-specific scalp with bald scalp
    // HEAD slot is index 0 in the 19-element equipment array
    if (displayInfoIds[0] != 0 && hairStyleId > 0) {
        uint16_t hairGeoset = static_cast<uint16_t>(hairStyleId + 1);
        geosets.erase(static_cast<uint16_t>(100 + hairGeoset)); // Remove style group 1
        geosets.insert(kGeosetDefaultConnector);  // Default group 1 connector
    }

    charRenderer->setActiveGeosets(st.instanceId, geosets);

    // --- Helmet model attachment ---
    // HEAD slot is index 0 in the 19-element equipment array.
    // Helmet M2s are race/gender-specific (e.g. Helm_Plate_B_01_HuM.m2 for Human Male).
    if (displayInfoIds[0] != 0) {
        // Detach any previously attached helmet before attaching a new one
        charRenderer->detachWeapon(st.instanceId, 0);
        charRenderer->detachWeapon(st.instanceId, 11);

        int32_t helmIdx = displayInfoDbc->findRecordById(displayInfoIds[0]);
        if (helmIdx >= 0) {
            const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
            std::string helmModelName = displayInfoDbc->getString(static_cast<uint32_t>(helmIdx), leftModelField);
            if (!helmModelName.empty()) {
                // Strip .mdx/.m2 extension
                size_t dotPos = helmModelName.rfind('.');
                if (dotPos != std::string::npos) helmModelName = helmModelName.substr(0, dotPos);

                // Race/gender suffix for helmet variants
                static const std::unordered_map<uint8_t, std::string> racePrefix = {
                    {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                    {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                };
                std::string genderSuffix = (st.genderId == 0) ? "M" : "F";
                std::string raceSuffix;
                auto itRace = racePrefix.find(st.raceId);
                if (itRace != racePrefix.end()) {
                    raceSuffix = "_" + itRace->second + genderSuffix;
                }

                // Try race/gender-specific variant first, then base name
                std::string helmPath;
                pipeline::M2Model helmModel;
                if (!raceSuffix.empty()) {
                    helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
                }
                if (!helmModel.isValid()) {
                    helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + ".m2";
                    loadWeaponM2(helmPath, helmModel);
                }

                if (helmModel.isValid()) {
                    uint32_t helmModelId = nextWeaponModelId_++;
                    // Get texture from ItemDisplayInfo (LeftModelTexture)
                    const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                    std::string helmTexName = displayInfoDbc->getString(static_cast<uint32_t>(helmIdx), leftTexField);
                    std::string helmTexPath;
                    if (!helmTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Head\\" + helmTexName + raceSuffix + ".blp";
                            if (assetManager->fileExists(suffixedTex)) helmTexPath = suffixedTex;
                        }
                        if (helmTexPath.empty()) {
                            helmTexPath = "Item\\ObjectComponents\\Head\\" + helmTexName + ".blp";
                        }
                    }
                    // Attachment point 0 (head bone), fallback to 11 (explicit head attachment)
                    bool attached = charRenderer->attachWeapon(st.instanceId, 0, helmModel, helmModelId, helmTexPath);
                    if (!attached) {
                        attached = charRenderer->attachWeapon(st.instanceId, 11, helmModel, helmModelId, helmTexPath);
                    }
                    if (attached) {
                        LOG_DEBUG("Attached player helmet: ", helmPath, " tex: ", helmTexPath);
                    }
                }
            }
        }
    } else {
        // No helmet equipped — detach any existing helmet model
        charRenderer->detachWeapon(st.instanceId, 0);
        charRenderer->detachWeapon(st.instanceId, 11);
    }

    // --- Shoulder model attachment ---
    // SHOULDERS slot is index 2 in the 19-element equipment array.
    // Shoulders have TWO M2 models (left + right) attached at points 5 and 6.
    // ItemDisplayInfo.dbc: LeftModel → left shoulder, RightModel → right shoulder.
    if (displayInfoIds[2] != 0) {
        // Detach any previously attached shoulder models
        charRenderer->detachWeapon(st.instanceId, 5);
        charRenderer->detachWeapon(st.instanceId, 6);

        int32_t shoulderIdx = displayInfoDbc->findRecordById(displayInfoIds[2]);
        if (shoulderIdx >= 0) {
            const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
            const uint32_t rightModelField = idiL ? (*idiL)["RightModel"] : 2u;
            const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
            const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;

            // Race/gender suffix for shoulder variants (same as helmets)
            static const std::unordered_map<uint8_t, std::string> shoulderRacePrefix = {
                {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
            };
            std::string genderSuffix = (st.genderId == 0) ? "M" : "F";
            std::string raceSuffix;
            auto itRace = shoulderRacePrefix.find(st.raceId);
            if (itRace != shoulderRacePrefix.end()) {
                raceSuffix = "_" + itRace->second + genderSuffix;
            }

            // Attach left shoulder (attachment point 5) using LeftModel
            std::string leftModelName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), leftModelField);
            if (!leftModelName.empty()) {
                size_t dotPos = leftModelName.rfind('.');
                if (dotPos != std::string::npos) leftModelName = leftModelName.substr(0, dotPos);

                std::string leftPath;
                pipeline::M2Model leftModel;
                if (!raceSuffix.empty()) {
                    leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(leftPath, leftModel)) leftModel = {};
                }
                if (!leftModel.isValid()) {
                    leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + ".m2";
                    loadWeaponM2(leftPath, leftModel);
                }

                if (leftModel.isValid()) {
                    uint32_t leftModelId = nextWeaponModelId_++;
                    std::string leftTexName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), leftTexField);
                    std::string leftTexPath;
                    if (!leftTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + raceSuffix + ".blp";
                            if (assetManager->fileExists(suffixedTex)) leftTexPath = suffixedTex;
                        }
                        if (leftTexPath.empty()) {
                            leftTexPath = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + ".blp";
                        }
                    }
                    bool attached = charRenderer->attachWeapon(st.instanceId, 5, leftModel, leftModelId, leftTexPath);
                    if (attached) {
                        LOG_DEBUG("Attached left shoulder: ", leftPath, " tex: ", leftTexPath);
                    }
                }
            }

            // Attach right shoulder (attachment point 6) using RightModel
            std::string rightModelName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), rightModelField);
            if (!rightModelName.empty()) {
                size_t dotPos = rightModelName.rfind('.');
                if (dotPos != std::string::npos) rightModelName = rightModelName.substr(0, dotPos);

                std::string rightPath;
                pipeline::M2Model rightModel;
                if (!raceSuffix.empty()) {
                    rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(rightPath, rightModel)) rightModel = {};
                }
                if (!rightModel.isValid()) {
                    rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + ".m2";
                    loadWeaponM2(rightPath, rightModel);
                }

                if (rightModel.isValid()) {
                    uint32_t rightModelId = nextWeaponModelId_++;
                    std::string rightTexName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), rightTexField);
                    std::string rightTexPath;
                    if (!rightTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + raceSuffix + ".blp";
                            if (assetManager->fileExists(suffixedTex)) rightTexPath = suffixedTex;
                        }
                        if (rightTexPath.empty()) {
                            rightTexPath = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + ".blp";
                        }
                    }
                    bool attached = charRenderer->attachWeapon(st.instanceId, 6, rightModel, rightModelId, rightTexPath);
                    if (attached) {
                        LOG_DEBUG("Attached right shoulder: ", rightPath, " tex: ", rightTexPath);
                    }
                }
            }
        }
    } else {
        // No shoulders equipped — detach any existing shoulder models
        charRenderer->detachWeapon(st.instanceId, 5);
        charRenderer->detachWeapon(st.instanceId, 6);
    }

    // --- Cape texture (group 15 / texture type 2) ---
    // The geoset above enables the cape mesh, but without a texture it renders blank.
    if (hasInvType({16})) {
        // Back/cloak is WoW equipment slot 14 (BACK) in the 19-element array.
        uint32_t capeDid = displayInfoIds[14];
        if (capeDid != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDid);
            if (capeRecIdx >= 0) {
                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                std::string capeName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), leftTexField);

                if (!capeName.empty()) {
                    std::replace(capeName.begin(), capeName.end(), '/', '\\');

                    auto hasBlpExt = [](const std::string& p) {
                        if (p.size() < 4) return false;
                        std::string ext = p.substr(p.size() - 4);
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return ext == ".blp";
                    };

                    const bool hasDir = (capeName.find('\\') != std::string::npos);
                    const bool hasExt = hasBlpExt(capeName);

                    std::vector<std::string> capeCandidates;
                    auto addCapeCandidate = [&](const std::string& p) {
                        if (p.empty()) return;
                        if (std::find(capeCandidates.begin(), capeCandidates.end(), p) == capeCandidates.end()) {
                            capeCandidates.push_back(p);
                        }
                    };

                    if (hasDir) {
                        addCapeCandidate(capeName);
                        if (!hasExt) addCapeCandidate(capeName + ".blp");
                    } else {
                        std::string baseObj = "Item\\ObjectComponents\\Cape\\" + capeName;
                        std::string baseTex = "Item\\TextureComponents\\Cape\\" + capeName;
                        addCapeCandidate(baseObj);
                        addCapeCandidate(baseTex);
                        if (!hasExt) {
                            addCapeCandidate(baseObj + ".blp");
                            addCapeCandidate(baseTex + ".blp");
                        }
                        addCapeCandidate(baseObj + (st.genderId == 1 ? "_F.blp" : "_M.blp"));
                        addCapeCandidate(baseObj + "_U.blp");
                        addCapeCandidate(baseTex + (st.genderId == 1 ? "_F.blp" : "_M.blp"));
                        addCapeCandidate(baseTex + "_U.blp");
                    }

                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    rendering::VkTexture* capeTexture = nullptr;
                    for (const auto& candidate : capeCandidates) {
                        rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                        if (tex && tex != whiteTex) {
                            capeTexture = tex;
                            break;
                        }
                    }

                    if (capeTexture) {
                        charRenderer->setGroupTextureOverride(st.instanceId, 15, capeTexture);
                        if (const auto* md = charRenderer->getModelData(st.modelId)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer->setTextureSlotOverride(
                                        st.instanceId, static_cast<uint16_t>(ti), capeTexture);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Textures (skin atlas compositing) ---
    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture",
        "ArmLowerTexture",
        "HandTexture",
        "TorsoUpperTexture",
        "TorsoLowerTexture",
        "LegUpperTexture",
        "LegLowerTexture",
        "FootTexture",
    };

    // Texture component region fields from DBC layout
    // Binary DBC (23 fields) has textures at 14+
    const uint32_t texRegionFields[8] = {
        idiL ? (*idiL)["TextureArmUpper"]  : 14u,
        idiL ? (*idiL)["TextureArmLower"]  : 15u,
        idiL ? (*idiL)["TextureHand"]      : 16u,
        idiL ? (*idiL)["TextureTorsoUpper"]: 17u,
        idiL ? (*idiL)["TextureTorsoLower"]: 18u,
        idiL ? (*idiL)["TextureLegUpper"]  : 19u,
        idiL ? (*idiL)["TextureLegLower"]  : 20u,
        idiL ? (*idiL)["TextureFoot"]      : 21u,
    };

    std::vector<std::pair<int, std::string>> regionLayers;
    const bool isFemale = (st.genderId == 1);

    for (int s = 0; s < 19; s++) {
        uint32_t did = displayInfoIds[s];
        if (did == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(did);
        if (recIdx < 0) continue;

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            std::string base = "Item\\TextureComponents\\" + std::string(componentDirs[region]) + "\\" + texName;
            std::string genderPath = base + (isFemale ? "_F.blp" : "_M.blp");
            std::string unisexPath = base + "_U.blp";
            std::string fullPath;
            if (assetManager->fileExists(genderPath)) fullPath = genderPath;
            else if (assetManager->fileExists(unisexPath)) fullPath = unisexPath;
            else fullPath = base + ".blp";

            regionLayers.emplace_back(region, fullPath);
        }
    }

    const auto slotsIt = playerTextureSlotsByModelId_.find(st.modelId);
    if (slotsIt == playerTextureSlotsByModelId_.end()) return;
    const PlayerTextureSlots& slots = slotsIt->second;
    if (slots.skin < 0) return;

    rendering::VkTexture* newTex = charRenderer->compositeWithRegions(st.bodySkinPath, st.underwearPaths, regionLayers);
    if (newTex) {
        charRenderer->setTextureSlotOverride(st.instanceId, static_cast<uint16_t>(slots.skin), newTex);
    }

    // --- Weapon model attachment ---
    // Slot indices in the 19-element EquipSlot array:
    //   15 = MAIN_HAND → attachment 1 (right hand)
    //   16 = OFF_HAND  → attachment 2 (left hand)
    struct OnlineWeaponSlot {
        int slotIndex;
        uint32_t attachmentId;
    };
    static constexpr OnlineWeaponSlot weaponSlots[] = {
        { 15, 1 },  // MAIN_HAND → right hand
        { 16, 2 },  // OFF_HAND  → left hand
    };

    const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
    const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
    const uint32_t texFieldL   = idiL ? (*idiL)["LeftModelTexture"] : 3u;
    const uint32_t texFieldR   = idiL ? (*idiL)["RightModelTexture"] : 4u;

    for (const auto& ws : weaponSlots) {
        uint32_t weapDisplayId = displayInfoIds[ws.slotIndex];
        if (weapDisplayId == 0) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        int32_t recIdx = displayInfoDbc->findRecordById(weapDisplayId);
        if (recIdx < 0) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        // Prefer LeftModel (full weapon), fall back to RightModel (hilt variants)
        std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texFieldL);
        if (modelName.empty()) {
            modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
            textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texFieldR);
        }
        if (modelName.empty()) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        // Convert .mdx → .m2
        std::string modelFile = modelName;
        {
            size_t dotPos = modelFile.rfind('.');
            if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
            modelFile += ".m2";
        }

        // Try Weapon directory first, then Shield
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
                continue;
            }
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                if (!assetManager->fileExists(texturePath)) texturePath.clear();
            }
        }

        uint32_t weaponModelId = nextWeaponModelId_++;
        charRenderer->attachWeapon(st.instanceId, ws.attachmentId,
                                   weaponModel, weaponModelId, texturePath);
    }
}

void Application::despawnOnlinePlayer(uint64_t guid) {
    if (!renderer || !renderer->getCharacterRenderer()) return;
    auto it = playerInstances_.find(guid);
    if (it == playerInstances_.end()) return;
    renderer->getCharacterRenderer()->removeInstance(it->second);
    playerInstances_.erase(it);
    onlinePlayerAppearance_.erase(guid);
    pendingOnlinePlayerEquipment_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
}

void Application::spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer || !assetManager) return;

    if (!gameObjectLookupsBuilt_) {
        buildGameObjectDisplayLookups();
    }
    if (!gameObjectLookupsBuilt_) return;

    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        // Already have a render instance — update its position (e.g. transport re-creation)
        auto& info = goIt->second;
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        LOG_DEBUG("GameObject position update: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ")");
        if (renderer) {
            if (info.isWmo) {
                if (auto* wr = renderer->getWMORenderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                    wr->setInstanceTransform(info.instanceId, transform);
                }
            } else {
                if (auto* mr = renderer->getM2Renderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    mr->setInstanceTransform(info.instanceId, transform);
                }
            }
        }
        return;
    }

    std::string modelPath;

        // Override model path for transports with wrong displayIds (preloaded transports)
        // Check if this GUID is a known transport
        bool isTransport = gameHandler && gameHandler->isTransportGuid(guid);
        if (isTransport) {
            // Map common transport displayIds to correct WMO paths
            // NOTE: displayIds 455/462 are elevators in Thunder Bluff and should NOT be forced to ships.
            // Keep ship/zeppelin overrides entry-driven where possible.
            // DisplayIds 807, 808 = Zeppelins
            // DisplayIds 2454, 1587 = Special ships/icebreakers
            if (entry == 20808 || entry == 176231 || entry == 176310) {
                modelPath = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
                LOG_INFO("Overriding transport entry/display ", entry, "/", displayId, " → transportship.wmo");
            } else if (displayId == 807 || displayId == 808 || displayId == 175080 || displayId == 176495 || displayId == 164871) {
                modelPath = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → transport_zeppelin.wmo");
            } else if (displayId == 1587) {
                modelPath = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → Transport_Horde_Zeppelin.wmo");
            } else if (displayId == 2454 || displayId == 181688 || displayId == 190536) {
                modelPath = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → Transport_Icebreaker_ship.wmo");
            } else if (displayId == 3831) {
                // Deeprun Tram car
                modelPath = "World\\Generic\\Gnome\\Passive Doodads\\Subway\\SubwayCar.m2";
                LOG_WARNING("Overriding transport displayId ", displayId, " → SubwayCar.m2");
            }
        }

    // Fallback to normal displayId lookup if not a transport or no override matched
    if (modelPath.empty()) {
        modelPath = getGameObjectModelPathForDisplayId(displayId);
    }

    if (modelPath.empty()) {
        LOG_WARNING("No model path for gameobject displayId ", displayId, " (guid 0x", std::hex, guid, std::dec, ")");
        return;
    }

    // Log spawns to help debug duplicate objects (e.g., cathedral issue)
    LOG_DEBUG("GameObject spawn: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
             " model=", modelPath, " pos=(", x, ", ", y, ", ", z, ")");

    std::string lowerPath = modelPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";

    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const float renderYawWmo = orientation;
    // M2 game objects: model default faces +renderX. renderYaw = canonical + 90° = server_yaw
    // (same offset as creature/character renderer so all M2 models face consistently)
    const float renderYawM2go = orientation + glm::radians(90.0f);

    bool loadedAsWmo = false;
    if (isWmo) {
        auto* wmoRenderer = renderer->getWMORenderer();
        if (!wmoRenderer) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
            // Only use cached entry if the model is still resident in the renderer
            if (wmoRenderer->isModelLoaded(modelId)) {
                loadedAsWmo = true;
            } else {
                gameObjectDisplayIdWmoCache_.erase(itCache);
                modelId = 0;
            }
        }
        if (!loadedAsWmo && modelId == 0) {
            auto wmoData = assetManager->readFile(modelPath);
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_DEBUG("Gameobject WMO root loaded: ", modelPath, " nGroups=", wmoModel.nGroups);
                int loadedGroups = 0;
                if (wmoModel.nGroups > 0) {
                    std::string basePath = modelPath;
                    std::string extension;
                    if (basePath.size() > 4) {
                        extension = basePath.substr(basePath.size() - 4);
                        std::string extLower = extension;
                        for (char& c : extLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (extLower == ".wmo") {
                            basePath = basePath.substr(0, basePath.size() - 4);
                        }
                    }

                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        char groupSuffix[16];
                        snprintf(groupSuffix, sizeof(groupSuffix), "_%03u%s", gi, extension.c_str());
                        std::string groupPath = basePath + groupSuffix;
                        std::vector<uint8_t> groupData = assetManager->readFile(groupPath);
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
                            groupData = assetManager->readFile(basePath + groupSuffix);
                        }
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.WMO", gi);
                            groupData = assetManager->readFile(basePath + groupSuffix);
                        }
                        if (!groupData.empty()) {
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                        } else {
                            LOG_WARNING("  Failed to load WMO group ", gi, " for: ", basePath);
                        }
                    }
                }

                if (loadedGroups > 0 || wmoModel.nGroups == 0) {
                    modelId = nextGameObjectWmoModelId_++;
                    if (wmoRenderer->loadModel(wmoModel, modelId)) {
                        gameObjectDisplayIdWmoCache_[displayId] = modelId;
                        loadedAsWmo = true;
                    } else {
                        LOG_WARNING("Failed to load gameobject WMO model: ", modelPath);
                    }
                } else {
                    LOG_WARNING("No WMO groups loaded for gameobject: ", modelPath,
                                " — falling back to M2");
                }
            } else {
                LOG_WARNING("Failed to read gameobject WMO: ", modelPath, " — falling back to M2");
            }
        }

        if (loadedAsWmo) {
            uint32_t instanceId = wmoRenderer->createInstance(modelId, renderPos,
                glm::vec3(0.0f, 0.0f, renderYawWmo), scale);
            if (instanceId == 0) {
                LOG_WARNING("Failed to create gameobject WMO instance for guid 0x", std::hex, guid, std::dec);
                return;
            }

            gameObjectInstances_[guid] = {modelId, instanceId, true};
            LOG_DEBUG("Spawned gameobject WMO: guid=0x", std::hex, guid, std::dec,
                     " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");

            // Spawn transport WMO doodads (chairs, furniture, etc.) as child M2 instances
            bool isTransport = false;
            if (gameHandler) {
                std::string lowerModelPath = modelPath;
                std::transform(lowerModelPath.begin(), lowerModelPath.end(), lowerModelPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isTransport = (lowerModelPath.find("transport") != std::string::npos);
            }

            auto* m2Renderer = renderer->getM2Renderer();
            if (m2Renderer && isTransport) {
                const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
                if (doodadTemplates && !doodadTemplates->empty()) {
                    constexpr size_t kMaxTransportDoodads = 192;
                    const size_t doodadBudget = std::min(doodadTemplates->size(), kMaxTransportDoodads);
                    LOG_DEBUG("Queueing ", doodadBudget, "/", doodadTemplates->size(),
                             " transport doodads for WMO instance ", instanceId);
                    pendingTransportDoodadBatches_.push_back(PendingTransportDoodadBatch{
                        guid,
                        modelId,
                        instanceId,
                        0,
                        doodadBudget,
                        0,
                        x, y, z,
                        orientation
                    });
                } else {
                LOG_DEBUG("Transport WMO has no doodads or templates not available");
            }
            }

            // Transport GameObjects are not always named "transport" in their WMO path
            // (e.g. elevators/lifts). If the server marks it as a transport, always
            // notify so TransportManager can animate/carry passengers.
            bool isTG = gameHandler && gameHandler->isTransportGuid(guid);
            LOG_WARNING("WMO GO spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " isTransport=", isTG,
                       " pos=(", x, ", ", y, ", ", z, ")");
            if (isTG) {
                gameHandler->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }

            return;
        }

        // WMO failed — fall through to try as M2
        // Convert .wmo path to .m2 for fallback
        modelPath = modelPath.substr(0, modelPath.size() - 4) + ".m2";
    }

    {
        auto* m2Renderer = renderer->getM2Renderer();
        if (!m2Renderer) return;

        // Skip displayIds that permanently failed to load (e.g. empty/unsupported M2s).
        // Without this guard the same empty model is re-parsed every frame, causing
        // sustained log spam and wasted CPU.
        if (gameObjectDisplayIdFailedCache_.count(displayId)) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdModelCache_.find(displayId);
        if (itCache != gameObjectDisplayIdModelCache_.end()) {
            modelId = itCache->second;
            if (!m2Renderer->hasModel(modelId)) {
                LOG_WARNING("GO M2 cache hit but model gone: displayId=", displayId,
                            " modelId=", modelId, " path=", modelPath,
                            " — reloading");
                gameObjectDisplayIdModelCache_.erase(itCache);
                itCache = gameObjectDisplayIdModelCache_.end();
            }
        }
        if (itCache == gameObjectDisplayIdModelCache_.end()) {
            modelId = nextGameObjectModelId_++;

            auto m2Data = assetManager->readFile(modelPath);
            if (m2Data.empty()) {
                LOG_WARNING("Failed to read gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
            if (model.vertices.empty()) {
                LOG_WARNING("Failed to parse gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            std::string skinPath = modelPath.substr(0, modelPath.size() - 3) + "00.skin";
            auto skinData = assetManager->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            }

            if (!m2Renderer->loadModel(model, modelId)) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            gameObjectDisplayIdModelCache_[displayId] = modelId;
        }

        uint32_t instanceId = m2Renderer->createInstance(modelId, renderPos,
            glm::vec3(0.0f, 0.0f, renderYawM2go), scale);
        if (instanceId == 0) {
            LOG_WARNING("Failed to create gameobject instance for guid 0x", std::hex, guid, std::dec);
            return;
        }

        // Freeze animation for static gameobjects, but let portals/effects/transports animate
        bool isTransportGO = gameHandler && gameHandler->isTransportGuid(guid);
        std::string lowerPath = modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool isAnimatedEffect = (lowerPath.find("instanceportal") != std::string::npos ||
                                  lowerPath.find("instancenewportal") != std::string::npos ||
                                  lowerPath.find("portalfx") != std::string::npos ||
                                  lowerPath.find("spellportal") != std::string::npos);
        if (!isAnimatedEffect && !isTransportGO) {
            m2Renderer->setInstanceAnimationFrozen(instanceId, true);
        }

        gameObjectInstances_[guid] = {modelId, instanceId, false};

        // Notify transport system for M2 transports (e.g. Deeprun Tram cars)
        if (gameHandler && gameHandler->isTransportGuid(guid)) {
            LOG_WARNING("M2 transport spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " instanceId=", instanceId);
            gameHandler->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
        }
    }

    LOG_DEBUG("Spawned gameobject: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

void Application::processAsyncCreatureResults(bool unlimited) {
    // Check completed async model loads and finalize on main thread (GPU upload + instance creation).
    // Limit GPU model uploads per tick to avoid long main-thread stalls that can starve socket updates.
    // Even in unlimited mode (load screen), keep a small cap and budget to prevent multi-second stalls.
    static constexpr int kMaxModelUploadsPerTick = 1;
    static constexpr int kMaxModelUploadsPerTickWarmup = 1;
    static constexpr float kFinalizeBudgetMs = 2.0f;
    static constexpr float kFinalizeBudgetWarmupMs = 2.0f;
    const int maxUploadsThisTick = unlimited ? kMaxModelUploadsPerTickWarmup : kMaxModelUploadsPerTick;
    const float budgetMs = unlimited ? kFinalizeBudgetWarmupMs : kFinalizeBudgetMs;
    const auto tickStart = std::chrono::steady_clock::now();
    int modelUploads = 0;

    for (auto it = asyncCreatureLoads_.begin(); it != asyncCreatureLoads_.end(); ) {
        if (std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - tickStart).count() >= budgetMs) {
            break;
        }

        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        auto result = it->future.get();
        it = asyncCreatureLoads_.erase(it);
        asyncCreatureDisplayLoads_.erase(result.displayId);

        // Failures and cache hits need no GPU work — process them even when the
        // upload budget is exhausted. Previously the budget check was above this
        // point, blocking ALL ready futures (including zero-cost ones) after a
        // single upload, which throttled creature spawn throughput during world load.
        if (result.permanent_failure) {
            nonRenderableCreatureDisplayIds_.insert(result.displayId);
            creaturePermanentFailureGuids_.insert(result.guid);
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }
        if (!result.valid || !result.model) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }

        // Another async result may have already uploaded this displayId while this
        // task was still running; in that case, skip duplicate GPU upload.
        if (displayIdModelCache_.find(result.displayId) != displayIdModelCache_.end()) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            if (!creatureInstances_.count(result.guid) &&
                !creaturePermanentFailureGuids_.count(result.guid)) {
                PendingCreatureSpawn s{};
                s.guid = result.guid;
                s.displayId = result.displayId;
                s.x = result.x;
                s.y = result.y;
                s.z = result.z;
                s.orientation = result.orientation;
                s.scale = result.scale;
                pendingCreatureSpawns_.push_back(s);
                pendingCreatureSpawnGuids_.insert(result.guid);
            }
            continue;
        }

        // Only actual GPU uploads count toward the per-tick budget.
        if (modelUploads >= maxUploadsThisTick) {
            // Re-queue this result — it needs a GPU upload but we're at budget.
            // Push a new pending spawn so it's retried next frame.
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            PendingCreatureSpawn s{};
            s.guid = result.guid;
            s.displayId = result.displayId;
            s.x = result.x; s.y = result.y; s.z = result.z;
            s.orientation = result.orientation;
            s.scale = result.scale;
            pendingCreatureSpawns_.push_back(s);
            pendingCreatureSpawnGuids_.insert(result.guid);
            continue;
        }

        // Model parsed on background thread — upload to GPU on main thread.
        auto* charRenderer = renderer ? renderer->getCharacterRenderer() : nullptr;
        if (!charRenderer) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            continue;
        }

        // Count upload attempts toward the frame budget even if upload fails.
        // Otherwise repeated failures can consume an unbounded amount of frame time.
        modelUploads++;

        // Upload model to GPU (must happen on main thread)
        // Use pre-decoded BLP cache to skip main-thread texture decode
        auto uploadStart = std::chrono::steady_clock::now();
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);
        if (!charRenderer->loadModel(*result.model, result.modelId)) {
            charRenderer->setPredecodedBLPCache(nullptr);
            nonRenderableCreatureDisplayIds_.insert(result.displayId);
            creaturePermanentFailureGuids_.insert(result.guid);
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }
        charRenderer->setPredecodedBLPCache(nullptr);
        {
            auto uploadEnd = std::chrono::steady_clock::now();
            float uploadMs = std::chrono::duration<float, std::milli>(uploadEnd - uploadStart).count();
            if (uploadMs > 100.0f) {
                LOG_WARNING("charRenderer->loadModel took ", uploadMs, "ms displayId=", result.displayId,
                            " preDecoded=", result.predecodedTextures.size());
            }
        }
        // Save remaining pre-decoded textures (display skins) for spawnOnlineCreature
        if (!result.predecodedTextures.empty()) {
            displayIdPredecodedTextures_[result.displayId] = std::move(result.predecodedTextures);
        }
        displayIdModelCache_[result.displayId] = result.modelId;
        pendingCreatureSpawnGuids_.erase(result.guid);
        creatureSpawnRetryCounts_.erase(result.guid);

        // Re-queue as a normal pending spawn — model is now cached, so sync spawn is fast
        // (only creates instance + applies textures, no file I/O).
        if (!creatureInstances_.count(result.guid) &&
            !creaturePermanentFailureGuids_.count(result.guid)) {
            PendingCreatureSpawn s{};
            s.guid = result.guid;
            s.displayId = result.displayId;
            s.x = result.x;
            s.y = result.y;
            s.z = result.z;
            s.orientation = result.orientation;
            s.scale = result.scale;
            pendingCreatureSpawns_.push_back(s);
            pendingCreatureSpawnGuids_.insert(result.guid);
        }
    }
}

void Application::processAsyncNpcCompositeResults(bool unlimited) {
    auto* charRenderer = renderer ? renderer->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;

    // Budget: 2ms per frame to avoid stalling when many NPCs complete skin compositing
    // simultaneously. In unlimited mode (load screen), process everything without cap.
    static constexpr float kCompositeBudgetMs = 2.0f;
    auto startTime = std::chrono::steady_clock::now();

    for (auto it = asyncNpcCompositeLoads_.begin(); it != asyncNpcCompositeLoads_.end(); ) {
        if (!unlimited) {
            float elapsed = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= kCompositeBudgetMs) break;
        }
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto result = it->future.get();
        it = asyncNpcCompositeLoads_.erase(it);

        const auto& info = result.info;

        // Set pre-decoded cache so texture loads skip synchronous BLP decode
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);

        // --- Apply skin to type-1 slots ---
        rendering::VkTexture* skinTex = nullptr;

        if (info.hasBakedSkin) {
            // Baked skin: load from pre-decoded cache
            skinTex = charRenderer->loadTexture(info.bakedSkinPath);
        }

        if (info.hasComposite) {
            // Composite with face/underwear/equipment regions on top of base skin
            rendering::VkTexture* compositeTex = nullptr;
            if (!info.regionLayers.empty()) {
                compositeTex = charRenderer->compositeWithRegions(info.basePath,
                    info.overlayPaths, info.regionLayers);
            } else if (!info.overlayPaths.empty()) {
                std::vector<std::string> skinLayers;
                skinLayers.push_back(info.basePath);
                for (const auto& op : info.overlayPaths) skinLayers.push_back(op);
                compositeTex = charRenderer->compositeTextures(skinLayers);
            }
            if (compositeTex) skinTex = compositeTex;
        } else if (info.hasSimpleSkin) {
            // Simple skin: just base texture, no compositing
            auto* baseTex = charRenderer->loadTexture(info.basePath);
            if (baseTex) skinTex = baseTex;
        }

        if (skinTex) {
            for (uint32_t slot : info.skinTextureSlots) {
                charRenderer->setModelTexture(info.modelId, slot, skinTex);
            }
        }

        // --- Apply hair texture to type-6 slots ---
        if (!info.hairTexturePath.empty()) {
            rendering::VkTexture* hairTex = charRenderer->loadTexture(info.hairTexturePath);
            rendering::VkTexture* whTex = charRenderer->loadTexture("");
            if (hairTex && hairTex != whTex) {
                for (uint32_t slot : info.hairTextureSlots) {
                    charRenderer->setModelTexture(info.modelId, slot, hairTex);
                }
            }
        } else if (info.useBakedForHair && skinTex) {
            // Bald NPC: use skin/baked texture for scalp cap
            for (uint32_t slot : info.hairTextureSlots) {
                charRenderer->setModelTexture(info.modelId, slot, skinTex);
            }
        }

        charRenderer->setPredecodedBLPCache(nullptr);
    }
}

void Application::processCreatureSpawnQueue(bool unlimited) {
    auto startTime = std::chrono::steady_clock::now();
    // Budget: max 2ms per frame for creature spawning to prevent stutter.
    // In unlimited mode (load screen), process everything without budget cap.
    static constexpr float kSpawnBudgetMs = 2.0f;

    // First, finalize any async model loads that completed on background threads.
    processAsyncCreatureResults(unlimited);
    {
        auto now = std::chrono::steady_clock::now();
        float asyncMs = std::chrono::duration<float, std::milli>(now - startTime).count();
        if (asyncMs > 100.0f) {
            LOG_WARNING("processAsyncCreatureResults took ", asyncMs, "ms");
        }
    }

    if (pendingCreatureSpawns_.empty()) return;
    if (!creatureLookupsBuilt_) {
        buildCreatureDisplayLookups();
        if (!creatureLookupsBuilt_) return;
    }

    int processed = 0;
    int asyncLaunched = 0;
    size_t rotationsLeft = pendingCreatureSpawns_.size();
    while (!pendingCreatureSpawns_.empty() &&
           (unlimited || processed < MAX_SPAWNS_PER_FRAME) &&
           rotationsLeft > 0) {
        // Check time budget every iteration (including first — async results may
        // have already consumed the budget via GPU model uploads).
        if (!unlimited) {
            auto now = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(now - startTime).count();
            if (elapsedMs >= kSpawnBudgetMs) break;
        }

        PendingCreatureSpawn s = pendingCreatureSpawns_.front();
        pendingCreatureSpawns_.pop_front();

        if (nonRenderableCreatureDisplayIds_.count(s.displayId)) {
            pendingCreatureSpawnGuids_.erase(s.guid);
            creatureSpawnRetryCounts_.erase(s.guid);
            processed++;
            rotationsLeft = pendingCreatureSpawns_.size();
            continue;
        }

        const bool needsNewModel = (displayIdModelCache_.find(s.displayId) == displayIdModelCache_.end());

        // For new models: launch async load on background thread instead of blocking.
        if (needsNewModel) {
            // Keep exactly one background load per displayId. Additional spawns for
            // the same displayId stay queued and will spawn once cache is populated.
            if (asyncCreatureDisplayLoads_.count(s.displayId)) {
                pendingCreatureSpawns_.push_back(s);
                rotationsLeft--;
                continue;
            }

            const int maxAsync = unlimited ? (MAX_ASYNC_CREATURE_LOADS * 4) : MAX_ASYNC_CREATURE_LOADS;
            if (static_cast<int>(asyncCreatureLoads_.size()) + asyncLaunched >= maxAsync) {
                // Too many in-flight — defer to next frame
                pendingCreatureSpawns_.push_back(s);
                rotationsLeft--;
                continue;
            }

            std::string m2Path = getModelPathForDisplayId(s.displayId);
            if (m2Path.empty()) {
                nonRenderableCreatureDisplayIds_.insert(s.displayId);
                creaturePermanentFailureGuids_.insert(s.guid);
                pendingCreatureSpawnGuids_.erase(s.guid);
                creatureSpawnRetryCounts_.erase(s.guid);
                processed++;
                rotationsLeft = pendingCreatureSpawns_.size();
                continue;
            }

            // Check for invisible stalkers
            {
                std::string lowerPath = m2Path;
                std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerPath.find("invisiblestalker") != std::string::npos ||
                    lowerPath.find("invisible_stalker") != std::string::npos) {
                    nonRenderableCreatureDisplayIds_.insert(s.displayId);
                    creaturePermanentFailureGuids_.insert(s.guid);
                    pendingCreatureSpawnGuids_.erase(s.guid);
                    processed++;
                    rotationsLeft = pendingCreatureSpawns_.size();
                    continue;
                }
            }

            // Launch async M2 load — file I/O and parsing happen off the main thread.
            uint32_t modelId = nextCreatureModelId_++;
            auto* am = assetManager.get();

            // Collect display skin texture paths for background pre-decode
            std::vector<std::string> displaySkinPaths;
            {
                auto itDD = displayDataMap_.find(s.displayId);
                if (itDD != displayDataMap_.end()) {
                    std::string modelDir;
                    size_t lastSlash = m2Path.find_last_of("\\/");
                    if (lastSlash != std::string::npos) modelDir = m2Path.substr(0, lastSlash + 1);

                    auto resolveForAsync = [&](const std::string& skinField) {
                        if (skinField.empty()) return;
                        std::string raw = skinField;
                        std::replace(raw.begin(), raw.end(), '/', '\\');
                        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
                        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
                        if (raw.empty()) return;
                        bool hasExt = raw.size() >= 4 && raw.substr(raw.size()-4) == ".blp";
                        bool hasDir = raw.find('\\') != std::string::npos;
                        std::vector<std::string> candidates;
                        if (hasDir) {
                            candidates.push_back(raw);
                            if (!hasExt) candidates.push_back(raw + ".blp");
                        } else {
                            candidates.push_back(modelDir + raw);
                            if (!hasExt) candidates.push_back(modelDir + raw + ".blp");
                            candidates.push_back(raw);
                            if (!hasExt) candidates.push_back(raw + ".blp");
                        }
                        for (const auto& c : candidates) {
                            if (am->fileExists(c)) { displaySkinPaths.push_back(c); return; }
                        }
                    };
                    resolveForAsync(itDD->second.skin1);
                    resolveForAsync(itDD->second.skin2);
                    resolveForAsync(itDD->second.skin3);

                    // Pre-decode humanoid NPC textures (bake, skin, face, underwear, hair, equipment)
                    if (itDD->second.extraDisplayId != 0) {
                        auto itHE = humanoidExtraMap_.find(itDD->second.extraDisplayId);
                        if (itHE != humanoidExtraMap_.end()) {
                            const auto& he = itHE->second;
                            // Baked texture
                            if (!he.bakeName.empty()) {
                                displaySkinPaths.push_back("Textures\\BakedNpcTextures\\" + he.bakeName);
                            }
                            // CharSections: skin, face, underwear
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t nRace = static_cast<uint32_t>(he.raceId);
                                uint32_t nSex = static_cast<uint32_t>(he.sexId);
                                uint32_t nSkin = static_cast<uint32_t>(he.skinId);
                                uint32_t nFace = static_cast<uint32_t>(he.faceId);
                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t rId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sId = csDbc->getUInt32(r, csF.sexId);
                                    if (rId != nRace || sId != nSex) continue;
                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t color = csDbc->getUInt32(r, csF.colorIndex);
                                    if (section == 0 && color == nSkin) {
                                        std::string t = csDbc->getString(r, csF.texture1);
                                        if (!t.empty()) displaySkinPaths.push_back(t);
                                    } else if (section == 1 && variation == nFace && color == nSkin) {
                                        std::string t1 = csDbc->getString(r, csF.texture1);
                                        std::string t2 = csDbc->getString(r, csF.texture2);
                                        if (!t1.empty()) displaySkinPaths.push_back(t1);
                                        if (!t2.empty()) displaySkinPaths.push_back(t2);
                                    } else if (section == 3 && variation == static_cast<uint32_t>(he.hairStyleId)
                                               && color == static_cast<uint32_t>(he.hairColorId)) {
                                        std::string t = csDbc->getString(r, csF.texture1);
                                        if (!t.empty()) displaySkinPaths.push_back(t);
                                    } else if (section == 4 && color == nSkin) {
                                        for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                                            std::string t = csDbc->getString(r, f);
                                            if (!t.empty()) displaySkinPaths.push_back(t);
                                        }
                                    }
                                }
                            }
                            // Equipment region textures
                            auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                            if (idiDbc) {
                                static constexpr const char* compDirs[] = {
                                    "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
                                    "TorsoUpperTexture", "TorsoLowerTexture",
                                    "LegUpperTexture", "LegLowerTexture", "FootTexture",
                                };
                                const auto* idiL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                                const uint32_t trf[8] = {
                                    idiL ? (*idiL)["TextureArmUpper"]  : 14u,
                                    idiL ? (*idiL)["TextureArmLower"]  : 15u,
                                    idiL ? (*idiL)["TextureHand"]      : 16u,
                                    idiL ? (*idiL)["TextureTorsoUpper"]: 17u,
                                    idiL ? (*idiL)["TextureTorsoLower"]: 18u,
                                    idiL ? (*idiL)["TextureLegUpper"]  : 19u,
                                    idiL ? (*idiL)["TextureLegLower"]  : 20u,
                                    idiL ? (*idiL)["TextureFoot"]      : 21u,
                                };
                                const bool isFem = (he.sexId == 1);
                                for (int eq = 0; eq < 11; eq++) {
                                    uint32_t did = he.equipDisplayId[eq];
                                    if (did == 0) continue;
                                    int32_t recIdx = idiDbc->findRecordById(did);
                                    if (recIdx < 0) continue;
                                    for (int region = 0; region < 8; region++) {
                                        std::string texName = idiDbc->getString(static_cast<uint32_t>(recIdx), trf[region]);
                                        if (texName.empty()) continue;
                                        std::string base = "Item\\TextureComponents\\" +
                                            std::string(compDirs[region]) + "\\" + texName;
                                        std::string gp = base + (isFem ? "_F.blp" : "_M.blp");
                                        std::string up = base + "_U.blp";
                                        if (am->fileExists(gp)) displaySkinPaths.push_back(gp);
                                        else if (am->fileExists(up)) displaySkinPaths.push_back(up);
                                        else displaySkinPaths.push_back(base + ".blp");
                                    }
                                }
                            }
                        }
                    }
                }
            }

            AsyncCreatureLoad load;
            load.future = std::async(std::launch::async,
                [am, m2Path, modelId, s, skinPaths = std::move(displaySkinPaths)]() -> PreparedCreatureModel {
                    PreparedCreatureModel result;
                    result.guid = s.guid;
                    result.displayId = s.displayId;
                    result.modelId = modelId;
                    result.x = s.x;
                    result.y = s.y;
                    result.z = s.z;
                    result.orientation = s.orientation;
                    result.scale = s.scale;

                    auto m2Data = am->readFile(m2Path);
                    if (m2Data.empty()) {
                        result.permanent_failure = true;
                        return result;
                    }

                    auto model = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(m2Data));
                    if (model->vertices.empty()) {
                        result.permanent_failure = true;
                        return result;
                    }

                    // Load skin file
                    if (model->version >= 264) {
                        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
                        auto skinData = am->readFile(skinPath);
                        if (!skinData.empty()) {
                            pipeline::M2Loader::loadSkin(skinData, *model);
                        }
                    }

                    // Load external .anim files
                    std::string basePath = m2Path.substr(0, m2Path.size() - 3);
                    for (uint32_t si = 0; si < model->sequences.size(); si++) {
                        if (!(model->sequences[si].flags & 0x20)) {
                            char animFileName[256];
                            snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                                basePath.c_str(), model->sequences[si].id, model->sequences[si].variationIndex);
                            auto animData = am->readFileOptional(animFileName);
                            if (!animData.empty()) {
                                pipeline::M2Loader::loadAnimFile(m2Data, animData, si, *model);
                            }
                        }
                    }

                    // Pre-decode model textures on background thread
                    for (const auto& tex : model->textures) {
                        if (tex.filename.empty()) continue;
                        std::string texKey = tex.filename;
                        std::replace(texKey.begin(), texKey.end(), '/', '\\');
                        std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.find(texKey) != result.predecodedTextures.end()) continue;
                        auto blp = am->loadTexture(texKey);
                        if (blp.isValid()) {
                            result.predecodedTextures[texKey] = std::move(blp);
                        }
                    }

                    // Pre-decode display skin textures (skin1/skin2/skin3 from CreatureDisplayInfo)
                    for (const auto& sp : skinPaths) {
                        std::string key = sp;
                        std::replace(key.begin(), key.end(), '/', '\\');
                        std::transform(key.begin(), key.end(), key.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.count(key)) continue;
                        auto blp = am->loadTexture(key);
                        if (blp.isValid()) {
                            result.predecodedTextures[key] = std::move(blp);
                        }
                    }

                    result.model = std::move(model);
                    result.valid = true;
                    return result;
                });
            asyncCreatureLoads_.push_back(std::move(load));
            asyncCreatureDisplayLoads_.insert(s.displayId);
            asyncLaunched++;
            // Don't erase from pendingCreatureSpawnGuids_ — the async result handler will do it
            rotationsLeft = pendingCreatureSpawns_.size();
            processed++;
            continue;
        }

        // Cached model — spawn is fast (no file I/O, just instance creation + texture setup)
        {
            auto spawnStart = std::chrono::steady_clock::now();
            spawnOnlineCreature(s.guid, s.displayId, s.x, s.y, s.z, s.orientation, s.scale);
            auto spawnEnd = std::chrono::steady_clock::now();
            float spawnMs = std::chrono::duration<float, std::milli>(spawnEnd - spawnStart).count();
            if (spawnMs > 100.0f) {
                LOG_WARNING("spawnOnlineCreature took ", spawnMs, "ms displayId=", s.displayId);
            }
        }
        pendingCreatureSpawnGuids_.erase(s.guid);

        // If spawn still failed, retry for a limited number of frames.
        if (!creatureInstances_.count(s.guid)) {
            if (creaturePermanentFailureGuids_.erase(s.guid) > 0) {
                creatureSpawnRetryCounts_.erase(s.guid);
                processed++;
                continue;
            }
            uint16_t retries = 0;
            auto it = creatureSpawnRetryCounts_.find(s.guid);
            if (it != creatureSpawnRetryCounts_.end()) {
                retries = it->second;
            }
            if (retries < MAX_CREATURE_SPAWN_RETRIES) {
                creatureSpawnRetryCounts_[s.guid] = static_cast<uint16_t>(retries + 1);
                pendingCreatureSpawns_.push_back(s);
                pendingCreatureSpawnGuids_.insert(s.guid);
            } else {
                creatureSpawnRetryCounts_.erase(s.guid);
                LOG_WARNING("Dropping creature spawn after retries: guid=0x", std::hex, s.guid, std::dec,
                            " displayId=", s.displayId);
            }
        } else {
            creatureSpawnRetryCounts_.erase(s.guid);
        }
        rotationsLeft = pendingCreatureSpawns_.size();
        processed++;
    }
}

void Application::processPlayerSpawnQueue() {
    if (pendingPlayerSpawns_.empty()) return;
    if (!assetManager || !assetManager->isInitialized()) return;

    int processed = 0;
    while (!pendingPlayerSpawns_.empty() && processed < MAX_SPAWNS_PER_FRAME) {
        PendingPlayerSpawn s = pendingPlayerSpawns_.front();
        pendingPlayerSpawns_.erase(pendingPlayerSpawns_.begin());
        pendingPlayerSpawnGuids_.erase(s.guid);

        // Skip if already spawned (could have been spawned by a previous update this frame)
        if (playerInstances_.count(s.guid)) {
            processed++;
            continue;
        }

        spawnOnlinePlayer(s.guid, s.raceId, s.genderId, s.appearanceBytes, s.facialFeatures, s.x, s.y, s.z, s.orientation);
        // Apply any equipment updates that arrived before the player was spawned.
        auto pit = pendingOnlinePlayerEquipment_.find(s.guid);
        if (pit != pendingOnlinePlayerEquipment_.end()) {
            deferredEquipmentQueue_.push_back({s.guid, pit->second});
            pendingOnlinePlayerEquipment_.erase(pit);
        }
        processed++;
    }
}

std::vector<std::string> Application::resolveEquipmentTexturePaths(uint64_t guid,
    const std::array<uint32_t, 19>& displayInfoIds,
    const std::array<uint8_t, 19>& /*inventoryTypes*/) const {
    std::vector<std::string> paths;

    auto it = onlinePlayerAppearance_.find(guid);
    if (it == onlinePlayerAppearance_.end()) return paths;
    const OnlinePlayerAppearanceState& st = it->second;

    // Add base skin + underwear paths
    if (!st.bodySkinPath.empty()) paths.push_back(st.bodySkinPath);
    for (const auto& up : st.underwearPaths) {
        if (!up.empty()) paths.push_back(up);
    }

    // Resolve equipment region texture paths (same logic as setOnlinePlayerEquipment)
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return paths;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
        "TorsoUpperTexture", "TorsoLowerTexture",
        "LegUpperTexture", "LegLowerTexture", "FootTexture",
    };
    const uint32_t texRegionFields[8] = {
        idiL ? (*idiL)["TextureArmUpper"]  : 14u,
        idiL ? (*idiL)["TextureArmLower"]  : 15u,
        idiL ? (*idiL)["TextureHand"]      : 16u,
        idiL ? (*idiL)["TextureTorsoUpper"]: 17u,
        idiL ? (*idiL)["TextureTorsoLower"]: 18u,
        idiL ? (*idiL)["TextureLegUpper"]  : 19u,
        idiL ? (*idiL)["TextureLegLower"]  : 20u,
        idiL ? (*idiL)["TextureFoot"]      : 21u,
    };
    const bool isFemale = (st.genderId == 1);

    for (int s = 0; s < 19; s++) {
        uint32_t did = displayInfoIds[s];
        if (did == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(did);
        if (recIdx < 0) continue;
        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;
            std::string base = "Item\\TextureComponents\\" +
                std::string(componentDirs[region]) + "\\" + texName;
            std::string genderPath = base + (isFemale ? "_F.blp" : "_M.blp");
            std::string unisexPath = base + "_U.blp";
            if (assetManager->fileExists(genderPath)) paths.push_back(genderPath);
            else if (assetManager->fileExists(unisexPath)) paths.push_back(unisexPath);
            else paths.push_back(base + ".blp");
        }
    }
    return paths;
}

void Application::processAsyncEquipmentResults() {
    for (auto it = asyncEquipmentLoads_.begin(); it != asyncEquipmentLoads_.end(); ) {
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto result = it->future.get();
        it = asyncEquipmentLoads_.erase(it);

        auto* charRenderer = renderer ? renderer->getCharacterRenderer() : nullptr;
        if (!charRenderer) continue;

        // Set pre-decoded cache so compositeWithRegions skips synchronous BLP decode
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);
        setOnlinePlayerEquipment(result.guid, result.displayInfoIds, result.inventoryTypes);
        charRenderer->setPredecodedBLPCache(nullptr);
    }
}

void Application::processDeferredEquipmentQueue() {
    // First, finalize any completed async pre-decodes
    processAsyncEquipmentResults();

    if (deferredEquipmentQueue_.empty()) return;
    // Limit in-flight async equipment loads
    if (asyncEquipmentLoads_.size() >= 2) return;

    auto [guid, equipData] = deferredEquipmentQueue_.front();
    deferredEquipmentQueue_.erase(deferredEquipmentQueue_.begin());

    // Resolve all texture paths that compositeWithRegions will need
    auto texturePaths = resolveEquipmentTexturePaths(guid, equipData.first, equipData.second);

    if (texturePaths.empty()) {
        // No textures to pre-decode — just apply directly (fast path)
        LOG_WARNING("Equipment fast path for guid=0x", std::hex, guid, std::dec,
                    " (no textures to pre-decode)");
        setOnlinePlayerEquipment(guid, equipData.first, equipData.second);
        return;
    }
    LOG_WARNING("Equipment async pre-decode for guid=0x", std::hex, guid, std::dec,
                " textures=", texturePaths.size());

    // Launch background BLP pre-decode
    auto* am = assetManager.get();
    auto displayInfoIds = equipData.first;
    auto inventoryTypes = equipData.second;
    AsyncEquipmentLoad load;
    load.future = std::async(std::launch::async,
        [am, guid, displayInfoIds, inventoryTypes, paths = std::move(texturePaths)]() -> PreparedEquipmentUpdate {
            PreparedEquipmentUpdate result;
            result.guid = guid;
            result.displayInfoIds = displayInfoIds;
            result.inventoryTypes = inventoryTypes;
            for (const auto& path : paths) {
                std::string key = path;
                std::replace(key.begin(), key.end(), '/', '\\');
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (result.predecodedTextures.count(key)) continue;
                auto blp = am->loadTexture(key);
                if (blp.isValid()) {
                    result.predecodedTextures[key] = std::move(blp);
                }
            }
            return result;
        });
    asyncEquipmentLoads_.push_back(std::move(load));
}

void Application::processAsyncGameObjectResults() {
    for (auto it = asyncGameObjectLoads_.begin(); it != asyncGameObjectLoads_.end(); ) {
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        auto result = it->future.get();
        it = asyncGameObjectLoads_.erase(it);

        if (!result.valid || !result.isWmo || !result.wmoModel) {
            // Fallback: spawn via sync path (likely an M2 or failed WMO)
            spawnOnlineGameObject(result.guid, result.entry, result.displayId,
                                 result.x, result.y, result.z, result.orientation, result.scale);
            continue;
        }

        // WMO parsed on background thread — do GPU upload + instance creation on main thread
        auto* wmoRenderer = renderer ? renderer->getWMORenderer() : nullptr;
        if (!wmoRenderer) continue;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(result.displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
        } else {
            modelId = nextGameObjectWmoModelId_++;
            wmoRenderer->setPredecodedBLPCache(&result.predecodedTextures);
            if (!wmoRenderer->loadModel(*result.wmoModel, modelId)) {
                wmoRenderer->setPredecodedBLPCache(nullptr);
                LOG_WARNING("Failed to load async gameobject WMO: ", result.modelPath);
                continue;
            }
            wmoRenderer->setPredecodedBLPCache(nullptr);
            gameObjectDisplayIdWmoCache_[result.displayId] = modelId;
        }

        glm::vec3 renderPos = core::coords::canonicalToRender(
            glm::vec3(result.x, result.y, result.z));
        uint32_t instanceId = wmoRenderer->createInstance(
            modelId, renderPos, glm::vec3(0.0f, 0.0f, result.orientation), result.scale);
        if (instanceId == 0) continue;

        gameObjectInstances_[result.guid] = {modelId, instanceId, true};

        // Queue transport doodad loading if applicable
        std::string lowerPath = result.modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerPath.find("transport") != std::string::npos) {
            const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
            if (doodadTemplates && !doodadTemplates->empty()) {
                PendingTransportDoodadBatch batch;
                batch.guid = result.guid;
                batch.modelId = modelId;
                batch.instanceId = instanceId;
                batch.x = result.x;
                batch.y = result.y;
                batch.z = result.z;
                batch.orientation = result.orientation;
                batch.doodadBudget = doodadTemplates->size();
                pendingTransportDoodadBatches_.push_back(batch);
            }
        }
    }
}

void Application::processGameObjectSpawnQueue() {
    // Finalize any completed async WMO loads first
    processAsyncGameObjectResults();

    if (pendingGameObjectSpawns_.empty()) return;

    // Process spawns: cached WMOs and M2s go sync (cheap), uncached WMOs go async
    auto startTime = std::chrono::steady_clock::now();
    static constexpr float kBudgetMs = 2.0f;
    static constexpr int kMaxAsyncLoads = 2;

    while (!pendingGameObjectSpawns_.empty()) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kBudgetMs) break;

        auto& s = pendingGameObjectSpawns_.front();

        // Check if this is an uncached WMO that needs async loading
        std::string modelPath;
        if (gameObjectLookupsBuilt_) {
            // Check transport overrides first
            bool isTransport = gameHandler && gameHandler->isTransportGuid(s.guid);
            if (isTransport) {
                if (s.entry == 20808 || s.entry == 176231 || s.entry == 176310)
                    modelPath = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
                else if (s.displayId == 807 || s.displayId == 808 || s.displayId == 175080 || s.displayId == 176495 || s.displayId == 164871)
                    modelPath = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
                else if (s.displayId == 1587)
                    modelPath = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
                else if (s.displayId == 2454 || s.displayId == 181688 || s.displayId == 190536)
                    modelPath = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
            }
            if (modelPath.empty())
                modelPath = getGameObjectModelPathForDisplayId(s.displayId);
        }

        std::string lowerPath = modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";
        bool isCached = isWmo && gameObjectDisplayIdWmoCache_.count(s.displayId);

        if (isWmo && !isCached && !modelPath.empty() &&
            static_cast<int>(asyncGameObjectLoads_.size()) < kMaxAsyncLoads) {
            // Launch async WMO load — file I/O + parse on background thread
            auto* am = assetManager.get();
            PendingGameObjectSpawn capture = s;
            std::string capturePath = modelPath;
            AsyncGameObjectLoad load;
            load.future = std::async(std::launch::async,
                [am, capture, capturePath]() -> PreparedGameObjectWMO {
                    PreparedGameObjectWMO result;
                    result.guid = capture.guid;
                    result.entry = capture.entry;
                    result.displayId = capture.displayId;
                    result.x = capture.x;
                    result.y = capture.y;
                    result.z = capture.z;
                    result.orientation = capture.orientation;
                    result.scale = capture.scale;
                    result.modelPath = capturePath;
                    result.isWmo = true;

                    auto wmoData = am->readFile(capturePath);
                    if (wmoData.empty()) return result;

                    auto wmo = std::make_shared<pipeline::WMOModel>(
                        pipeline::WMOLoader::load(wmoData));

                    // Load groups
                    if (wmo->nGroups > 0) {
                        std::string basePath = capturePath;
                        std::string ext;
                        if (basePath.size() > 4) {
                            ext = basePath.substr(basePath.size() - 4);
                            basePath = basePath.substr(0, basePath.size() - 4);
                        }
                        for (uint32_t gi = 0; gi < wmo->nGroups; gi++) {
                            char suffix[16];
                            snprintf(suffix, sizeof(suffix), "_%03u%s", gi, ext.c_str());
                            auto groupData = am->readFile(basePath + suffix);
                            if (groupData.empty()) {
                                snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
                                groupData = am->readFile(basePath + suffix);
                            }
                            if (!groupData.empty()) {
                                pipeline::WMOLoader::loadGroup(groupData, *wmo, gi);
                            }
                        }
                    }

                    // Pre-decode WMO textures on background thread
                    for (const auto& texPath : wmo->textures) {
                        if (texPath.empty()) continue;
                        std::string texKey = texPath;
                        size_t nul = texKey.find('\0');
                        if (nul != std::string::npos) texKey.resize(nul);
                        std::replace(texKey.begin(), texKey.end(), '/', '\\');
                        std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (texKey.empty()) continue;
                        // Convert to .blp extension
                        if (texKey.size() >= 4) {
                            std::string ext = texKey.substr(texKey.size() - 4);
                            if (ext == ".tga" || ext == ".dds") {
                                texKey = texKey.substr(0, texKey.size() - 4) + ".blp";
                            }
                        }
                        if (result.predecodedTextures.find(texKey) != result.predecodedTextures.end()) continue;
                        auto blp = am->loadTexture(texKey);
                        if (blp.isValid()) {
                            result.predecodedTextures[texKey] = std::move(blp);
                        }
                    }

                    result.wmoModel = wmo;
                    result.valid = true;
                    return result;
                });
            asyncGameObjectLoads_.push_back(std::move(load));
            pendingGameObjectSpawns_.erase(pendingGameObjectSpawns_.begin());
            continue;
        }

        // Cached WMO or M2 — spawn synchronously (cheap)
        spawnOnlineGameObject(s.guid, s.entry, s.displayId, s.x, s.y, s.z, s.orientation, s.scale);
        pendingGameObjectSpawns_.erase(pendingGameObjectSpawns_.begin());
    }
}

void Application::processPendingTransportRegistrations() {
    if (pendingTransportRegistrations_.empty()) return;
    if (!gameHandler || !renderer) return;

    auto* transportManager = gameHandler->getTransportManager();
    if (!transportManager) return;

    auto startTime = std::chrono::steady_clock::now();
    static constexpr int kMaxRegistrationsPerFrame = 2;
    static constexpr float kRegistrationBudgetMs = 2.0f;
    int processed = 0;

    for (auto it = pendingTransportRegistrations_.begin();
         it != pendingTransportRegistrations_.end() && processed < kMaxRegistrationsPerFrame;) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kRegistrationBudgetMs) break;

        const PendingTransportRegistration pending = *it;
        auto goIt = gameObjectInstances_.find(pending.guid);
        if (goIt == gameObjectInstances_.end()) {
            it = pendingTransportRegistrations_.erase(it);
            continue;
        }

        if (transportManager->getTransport(pending.guid)) {
            transportManager->updateServerTransport(
                pending.guid, glm::vec3(pending.x, pending.y, pending.z), pending.orientation);
            it = pendingTransportRegistrations_.erase(it);
            continue;
        }

        const uint32_t wmoInstanceId = goIt->second.instanceId;
        LOG_WARNING("Registering server transport: GUID=0x", std::hex, pending.guid, std::dec,
                 " entry=", pending.entry, " displayId=", pending.displayId, " wmoInstance=", wmoInstanceId,
                 " pos=(", pending.x, ", ", pending.y, ", ", pending.z, ")");

        // TransportAnimation.dbc is indexed by GameObject entry.
        uint32_t pathId = pending.entry;
        const bool preferServerData = gameHandler->hasServerTransportUpdate(pending.guid);

        bool clientAnim = transportManager->isClientSideAnimation();
        LOG_DEBUG("Transport spawn callback: clientAnimation=", clientAnim,
                 " guid=0x", std::hex, pending.guid, std::dec,
                 " entry=", pending.entry, " pathId=", pathId,
                 " preferServer=", preferServerData);

        glm::vec3 canonicalSpawnPos(pending.x, pending.y, pending.z);
        const bool shipOrZeppelinDisplay =
            (pending.displayId == 3015 || pending.displayId == 3031 || pending.displayId == 7546 ||
             pending.displayId == 7446 || pending.displayId == 1587 || pending.displayId == 2454 ||
             pending.displayId == 807 || pending.displayId == 808);
        bool hasUsablePath = transportManager->hasPathForEntry(pending.entry);
        if (shipOrZeppelinDisplay) {
            hasUsablePath = transportManager->hasUsableMovingPathForEntry(pending.entry, 25.0f);
        }

        LOG_WARNING("Transport path check: entry=", pending.entry, " hasUsablePath=", hasUsablePath,
                 " preferServerData=", preferServerData, " shipOrZepDisplay=", shipOrZeppelinDisplay);

        if (preferServerData) {
            if (!hasUsablePath) {
                std::vector<glm::vec3> path = { canonicalSpawnPos };
                transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                LOG_WARNING("Server-first strict registration: stationary fallback for GUID 0x",
                         std::hex, pending.guid, std::dec, " entry=", pending.entry);
            } else {
                LOG_WARNING("Server-first transport registration: using entry DBC path for entry ", pending.entry);
            }
        } else if (!hasUsablePath) {
            bool allowZOnly = (pending.displayId == 455 || pending.displayId == 462);
            uint32_t inferredPath = transportManager->inferDbcPathForSpawn(
                canonicalSpawnPos, 1200.0f, allowZOnly);
            if (inferredPath != 0) {
                pathId = inferredPath;
                LOG_WARNING("Using inferred transport path ", pathId, " for entry ", pending.entry);
            } else {
                uint32_t remappedPath = transportManager->pickFallbackMovingPath(pending.entry, pending.displayId);
                if (remappedPath != 0) {
                    pathId = remappedPath;
                    LOG_WARNING("Using remapped fallback transport path ", pathId,
                             " for entry ", pending.entry, " displayId=", pending.displayId,
                             " (usableEntryPath=", transportManager->hasPathForEntry(pending.entry), ")");
                } else {
                    LOG_WARNING("No TransportAnimation.dbc path for entry ", pending.entry,
                                " - transport will be stationary");
                    std::vector<glm::vec3> path = { canonicalSpawnPos };
                    transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                }
            }
        } else {
            LOG_WARNING("Using real transport path from TransportAnimation.dbc for entry ", pending.entry);
        }

        transportManager->registerTransport(pending.guid, wmoInstanceId, pathId, canonicalSpawnPos, pending.entry);

        if (!goIt->second.isWmo) {
            if (auto* tr = transportManager->getTransport(pending.guid)) {
                tr->isM2 = true;
            }
        }

        transportManager->updateServerTransport(
            pending.guid, glm::vec3(pending.x, pending.y, pending.z), pending.orientation);

        auto moveIt = pendingTransportMoves_.find(pending.guid);
        if (moveIt != pendingTransportMoves_.end()) {
            const PendingTransportMove latestMove = moveIt->second;
            transportManager->updateServerTransport(
                pending.guid, glm::vec3(latestMove.x, latestMove.y, latestMove.z), latestMove.orientation);
            LOG_DEBUG("Replayed queued transport move for GUID=0x", std::hex, pending.guid, std::dec,
                     " pos=(", latestMove.x, ", ", latestMove.y, ", ", latestMove.z,
                     ") orientation=", latestMove.orientation);
            pendingTransportMoves_.erase(moveIt);
        }

        if (glm::dot(canonicalSpawnPos, canonicalSpawnPos) < 1.0f) {
            auto goData = gameHandler->getCachedGameObjectInfo(pending.entry);
            if (goData && goData->type == 15 && goData->hasData && goData->data[0] != 0) {
                uint32_t taxiPathId = goData->data[0];
                if (transportManager->hasTaxiPath(taxiPathId)) {
                    transportManager->assignTaxiPathToTransport(pending.entry, taxiPathId);
                    LOG_DEBUG("Assigned cached TaxiPathNode path for MO_TRANSPORT entry=", pending.entry,
                             " taxiPathId=", taxiPathId);
                }
            }
        }

        if (auto* tr = transportManager->getTransport(pending.guid); tr) {
            LOG_WARNING("Transport registered: guid=0x", std::hex, pending.guid, std::dec,
                     " entry=", pending.entry, " displayId=", pending.displayId,
                     " pathId=", tr->pathId,
                     " mode=", (tr->useClientAnimation ? "client" : "server"),
                     " serverUpdates=", tr->serverUpdateCount);
        } else {
            LOG_DEBUG("Transport registered: guid=0x", std::hex, pending.guid, std::dec,
                     " entry=", pending.entry, " displayId=", pending.displayId,
                     " (TransportManager instance missing)");
        }

        ++processed;
        it = pendingTransportRegistrations_.erase(it);
    }
}

void Application::processPendingTransportDoodads() {
    if (pendingTransportDoodadBatches_.empty()) return;
    if (!renderer || !assetManager) return;

    auto* wmoRenderer = renderer->getWMORenderer();
    auto* m2Renderer = renderer->getM2Renderer();
    if (!wmoRenderer || !m2Renderer) return;

    auto startTime = std::chrono::steady_clock::now();
    static constexpr float kDoodadBudgetMs = 4.0f;

    // Batch all GPU uploads into a single async command buffer submission so that
    // N doodads with multiple textures each don't each block on vkQueueSubmit +
    // vkWaitForFences. Without batching, 30+ doodads × several textures = hundreds
    // of sync GPU submits → the 490ms stall that preceded the VK_ERROR_DEVICE_LOST.
    auto* vkCtx = renderer->getVkContext();
    if (vkCtx) vkCtx->beginUploadBatch();

    size_t budgetLeft = MAX_TRANSPORT_DOODADS_PER_FRAME;
    for (auto it = pendingTransportDoodadBatches_.begin();
         it != pendingTransportDoodadBatches_.end() && budgetLeft > 0;) {
        // Time budget check
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kDoodadBudgetMs) break;
        auto goIt = gameObjectInstances_.find(it->guid);
        if (goIt == gameObjectInstances_.end() || !goIt->second.isWmo ||
            goIt->second.instanceId != it->instanceId || goIt->second.modelId != it->modelId) {
            it = pendingTransportDoodadBatches_.erase(it);
            continue;
        }

        const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(it->modelId);
        if (!doodadTemplates || doodadTemplates->empty()) {
            it = pendingTransportDoodadBatches_.erase(it);
            continue;
        }

        const size_t maxIndex = std::min(it->doodadBudget, doodadTemplates->size());
        while (it->nextIndex < maxIndex && budgetLeft > 0) {
            // Per-doodad time budget (each does synchronous file I/O + parse + GPU upload)
            float innerMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - startTime).count();
            if (innerMs >= kDoodadBudgetMs) { budgetLeft = 0; break; }

            const auto& doodadTemplate = (*doodadTemplates)[it->nextIndex];
            it->nextIndex++;
            budgetLeft--;

            uint32_t doodadModelId = static_cast<uint32_t>(std::hash<std::string>{}(doodadTemplate.m2Path));
            auto m2Data = assetManager->readFile(doodadTemplate.m2Path);
            if (m2Data.empty()) continue;

            pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
            std::string skinPath = doodadTemplate.m2Path.substr(0, doodadTemplate.m2Path.size() - 3) + "00.skin";
            std::vector<uint8_t> skinData = assetManager->readFile(skinPath);
            if (!skinData.empty() && m2Model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, m2Model);
            }
            if (!m2Model.isValid()) continue;

            if (!m2Renderer->loadModel(m2Model, doodadModelId)) continue;
            uint32_t m2InstanceId = m2Renderer->createInstance(doodadModelId, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
            if (m2InstanceId == 0) continue;
            m2Renderer->setSkipCollision(m2InstanceId, true);

            wmoRenderer->addDoodadToInstance(it->instanceId, m2InstanceId, doodadTemplate.localTransform);
            it->spawnedDoodads++;
        }

        if (it->nextIndex >= maxIndex) {
            if (it->spawnedDoodads > 0) {
                LOG_DEBUG("Spawned ", it->spawnedDoodads,
                         " transport doodads for WMO instance ", it->instanceId);
                glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(it->x, it->y, it->z));
                glm::mat4 wmoTransform(1.0f);
                wmoTransform = glm::translate(wmoTransform, renderPos);
                wmoTransform = glm::rotate(wmoTransform, it->orientation, glm::vec3(0, 0, 1));
                wmoRenderer->setInstanceTransform(it->instanceId, wmoTransform);
            }
            it = pendingTransportDoodadBatches_.erase(it);
        } else {
            ++it;
        }
    }

    // Finalize the upload batch — submit all GPU copies in one shot (async, no wait).
    if (vkCtx) vkCtx->endUploadBatch();
}

void Application::processPendingMount() {
    if (pendingMountDisplayId_ == 0) return;
    uint32_t mountDisplayId = pendingMountDisplayId_;
    pendingMountDisplayId_ = 0;
    LOG_INFO("processPendingMount: loading displayId ", mountDisplayId);

    if (!renderer || !renderer->getCharacterRenderer() || !assetManager) return;
    auto* charRenderer = renderer->getCharacterRenderer();

    std::string m2Path = getModelPathForDisplayId(mountDisplayId);
    if (m2Path.empty()) {
        LOG_WARNING("No model path for mount displayId ", mountDisplayId);
        return;
    }

    // Check model cache
    uint32_t modelId = 0;
    auto cacheIt = displayIdModelCache_.find(mountDisplayId);
    if (cacheIt != displayIdModelCache_.end()) {
        modelId = cacheIt->second;
    } else {
        modelId = nextCreatureModelId_++;

        auto m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            LOG_WARNING("Failed to read mount M2: ", m2Path);
            return;
        }

        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.vertices.empty()) {
            LOG_WARNING("Failed to parse mount M2: ", m2Path);
            return;
        }

        // Load skin file (only for WotLK M2s - vanilla has embedded skin)
        if (model.version >= 264) {
            std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
            auto skinData = assetManager->readFile(skinPath);
            if (!skinData.empty()) {
                pipeline::M2Loader::loadSkin(skinData, model);
            } else {
                LOG_WARNING("Missing skin file for WotLK mount M2: ", skinPath);
            }
        }

        // Load external .anim files (only idle + run needed for mounts)
        std::string basePath = m2Path.substr(0, m2Path.size() - 3);
        for (uint32_t si = 0; si < model.sequences.size(); si++) {
            if (!(model.sequences[si].flags & 0x20)) {
                uint32_t animId = model.sequences[si].id;
                // Only load stand(0), walk(4), run(5) anims to avoid hang
                if (animId != 0 && animId != 4 && animId != 5) continue;
                char animFileName[256];
                snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                    basePath.c_str(), animId, model.sequences[si].variationIndex);
                auto animData = assetManager->readFileOptional(animFileName);
                if (!animData.empty()) {
                    pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
                }
            }
        }

        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("Failed to load mount model: ", m2Path);
            return;
        }

        displayIdModelCache_[mountDisplayId] = modelId;
    }

    // Apply creature skin textures from CreatureDisplayInfo.dbc.
    // Re-apply even for cached models so transient failures can self-heal.
    std::string modelDir;
    size_t lastSlash = m2Path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        modelDir = m2Path.substr(0, lastSlash + 1);
    }

    auto itDisplayData = displayDataMap_.find(mountDisplayId);
    bool haveDisplayData = false;
    CreatureDisplayData dispData{};
    if (itDisplayData != displayDataMap_.end()) {
        dispData = itDisplayData->second;
        haveDisplayData = true;
    } else {
        // Some taxi mount display IDs are sparse; recover skins by matching model path.
        std::string lowerMountPath = m2Path;
        std::transform(lowerMountPath.begin(), lowerMountPath.end(), lowerMountPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            auto pit = modelIdToPath_.find(data.modelId);
            if (pit == modelIdToPath_.end()) continue;
            std::string p = pit->second;
            std::transform(p.begin(), p.end(), p.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (p != lowerMountPath) continue;
            int score = 0;
            if (!data.skin1.empty()) {
                std::string p1 = modelDir + data.skin1 + ".blp";
                score += assetManager->fileExists(p1) ? 30 : 3;
            }
            if (!data.skin2.empty()) {
                std::string p2 = modelDir + data.skin2 + ".blp";
                score += assetManager->fileExists(p2) ? 20 : 2;
            }
            if (!data.skin3.empty()) {
                std::string p3 = modelDir + data.skin3 + ".blp";
                score += assetManager->fileExists(p3) ? 10 : 1;
            }
            if (score > bestScore) {
                bestScore = score;
                dispData = data;
                haveDisplayData = true;
            }
        }
        if (haveDisplayData) {
            LOG_INFO("Recovered mount display data by model path for displayId=", mountDisplayId,
                     " skin1='", dispData.skin1, "' skin2='", dispData.skin2,
                     "' skin3='", dispData.skin3, "'");
        }
    }
    if (haveDisplayData) {
        // If this displayId has no skins, try to find another displayId for the same model with skins.
        if (dispData.skin1.empty() && dispData.skin2.empty() && dispData.skin3.empty()) {
            uint32_t sourceModelId = dispData.modelId;
            int bestScore = -1;
            for (const auto& [dispId, data] : displayDataMap_) {
                if (data.modelId != sourceModelId) continue;
                int score = 0;
                if (!data.skin1.empty()) {
                    std::string p = modelDir + data.skin1 + ".blp";
                    score += assetManager->fileExists(p) ? 30 : 3;
                }
                if (!data.skin2.empty()) {
                    std::string p = modelDir + data.skin2 + ".blp";
                    score += assetManager->fileExists(p) ? 20 : 2;
                }
                if (!data.skin3.empty()) {
                    std::string p = modelDir + data.skin3 + ".blp";
                    score += assetManager->fileExists(p) ? 10 : 1;
                }
                if (score > bestScore) {
                    bestScore = score;
                    dispData = data;
                }
            }
            LOG_INFO("Mount skin fallback for displayId=", mountDisplayId,
                     " modelId=", sourceModelId, " skin1='", dispData.skin1,
                     "' skin2='", dispData.skin2, "' skin3='", dispData.skin3, "'");
        }
        const auto* md = charRenderer->getModelData(modelId);
        if (md) {
            LOG_INFO("Mount model textures: ", md->textures.size(), " slots, skin1='", dispData.skin1,
                     "' skin2='", dispData.skin2, "' skin3='", dispData.skin3, "'");
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                LOG_INFO("  tex[", ti, "] type=", md->textures[ti].type,
                         " filename='", md->textures[ti].filename, "'");
            }

            int replaced = 0;
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                const auto& tex = md->textures[ti];
                std::string texPath;
                if (tex.type == 11 && !dispData.skin1.empty()) {
                    texPath = modelDir + dispData.skin1 + ".blp";
                } else if (tex.type == 12 && !dispData.skin2.empty()) {
                    texPath = modelDir + dispData.skin2 + ".blp";
                } else if (tex.type == 13 && !dispData.skin3.empty()) {
                    texPath = modelDir + dispData.skin3 + ".blp";
                }
                if (!texPath.empty()) {
                    rendering::VkTexture* skinTex = charRenderer->loadTexture(texPath);
                    if (skinTex) {
                        charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                        LOG_INFO("  Applied skin texture slot ", ti, ": ", texPath);
                        replaced++;
                    } else {
                        LOG_WARNING("  Failed to load skin texture slot ", ti, ": ", texPath);
                    }
                }
            }

            // Force skin textures onto type-0 (hardcoded) slots that have no filename
            if (replaced == 0) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    const auto& tex = md->textures[ti];
                    if (tex.type == 0 && tex.filename.empty()) {
                        // Empty hardcoded slot — try skin1 then skin2
                        std::string texPath;
                        if (!dispData.skin1.empty() && replaced == 0) {
                            texPath = modelDir + dispData.skin1 + ".blp";
                        } else if (!dispData.skin2.empty()) {
                            texPath = modelDir + dispData.skin2 + ".blp";
                        }
                        if (!texPath.empty()) {
                            rendering::VkTexture* skinTex = charRenderer->loadTexture(texPath);
                            if (skinTex) {
                                charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                                LOG_INFO("  Forced skin on empty hardcoded slot ", ti, ": ", texPath);
                                replaced++;
                            }
                        }
                    }
                }
            }

            // If still no textures, try hardcoded model texture filenames
            if (replaced == 0) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    if (!md->textures[ti].filename.empty()) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(md->textures[ti].filename);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), texId);
                            LOG_INFO("  Used model embedded texture slot ", ti, ": ", md->textures[ti].filename);
                            replaced++;
                        }
                    }
                }
            }

            // Final fallback for gryphon/wyvern: try well-known skin texture names
            if (replaced == 0 && !md->textures.empty()) {
                std::string lowerMountPath = m2Path;
                std::transform(lowerMountPath.begin(), lowerMountPath.end(), lowerMountPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerMountPath.find("gryphon") != std::string::npos) {
                    const char* gryphonSkins[] = {
                        "Creature\\Gryphon\\Gryphon_Skin.blp",
                        "Creature\\Gryphon\\Gryphon_Skin01.blp",
                        "Creature\\Gryphon\\GRYPHON_SKIN01.BLP",
                        nullptr
                    };
                    for (const char** p = gryphonSkins; *p; ++p) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(*p);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, 0, texId);
                            LOG_INFO("  Forced gryphon skin fallback: ", *p);
                            replaced++;
                            break;
                        }
                    }
                } else if (lowerMountPath.find("wyvern") != std::string::npos) {
                    const char* wyvernSkins[] = {
                        "Creature\\Wyvern\\Wyvern_Skin.blp",
                        "Creature\\Wyvern\\Wyvern_Skin01.blp",
                        nullptr
                    };
                    for (const char** p = wyvernSkins; *p; ++p) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(*p);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, 0, texId);
                            LOG_INFO("  Forced wyvern skin fallback: ", *p);
                            replaced++;
                            break;
                        }
                    }
                }
            }
            LOG_INFO("Mount texture setup: ", replaced, " textures applied");
        }
    }

    mountModelId_ = modelId;

    // Create mount instance at player position
    glm::vec3 mountPos = renderer->getCharacterPosition();
    float yawRad = glm::radians(renderer->getCharacterYaw());
    uint32_t instanceId = charRenderer->createInstance(modelId, mountPos,
        glm::vec3(0.0f, 0.0f, yawRad), 1.0f);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create mount instance");
        return;
    }

    mountInstanceId_ = instanceId;

    // Compute height offset — place player above mount's back
    // Use tight bounds from actual vertices (M2 header bounds can be inaccurate)
    const auto* modelData = charRenderer->getModelData(modelId);
    float heightOffset = 1.8f;
    if (modelData && !modelData->vertices.empty()) {
        float minZ =  std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        for (const auto& v : modelData->vertices) {
            if (v.position.z < minZ) minZ = v.position.z;
            if (v.position.z > maxZ) maxZ = v.position.z;
        }
        float extentZ = maxZ - minZ;
        LOG_INFO("Mount tight bounds: minZ=", minZ, " maxZ=", maxZ, " extentZ=", extentZ);
        if (extentZ > 0.5f) {
            // Saddle point is roughly 75% up the model, measured from model origin
            heightOffset = maxZ * 0.8f;
            if (heightOffset < 1.0f) heightOffset = extentZ * 0.75f;
            if (heightOffset < 1.0f) heightOffset = 1.8f;
        }
    }

    renderer->setMounted(instanceId, mountDisplayId, heightOffset, m2Path);

    // For taxi mounts, start with flying animation; for ground mounts, start with stand
    bool isTaxi = gameHandler && gameHandler->isOnTaxiFlight();
    uint32_t startAnim = 0; // ANIM_STAND
    if (isTaxi) {
        // Try WotLK fly anims first, then Vanilla-friendly fallbacks
        uint32_t taxiCandidates[] = {159, 158, 234, 229, 233, 141, 369, 6, 5}; // FlyForward, FlyIdle, FlyRun(234), FlyStand(229), FlyWalk(233), FlyMounted, FlyRun, Fly, Run
        for (uint32_t anim : taxiCandidates) {
            if (charRenderer->hasAnimation(instanceId, anim)) {
                startAnim = anim;
                break;
            }
        }
        // If none found, startAnim stays 0 (Stand/hover) which is fine for flying creatures
    }
    charRenderer->playAnimation(instanceId, startAnim, true);

    LOG_INFO("processPendingMount: DONE displayId=", mountDisplayId, " model=", m2Path, " heightOffset=", heightOffset);
}

void Application::despawnOnlineCreature(uint64_t guid) {
    // If this guid is a PLAYER, it will be tracked in playerInstances_.
    // Route to the correct despawn path so we don't leak instances.
    if (playerInstances_.count(guid)) {
        despawnOnlinePlayer(guid);
        return;
    }

    pendingCreatureSpawnGuids_.erase(guid);
    creatureSpawnRetryCounts_.erase(guid);
    creaturePermanentFailureGuids_.erase(guid);
    deadCreatureGuids_.erase(guid);

    auto it = creatureInstances_.find(guid);
    if (it == creatureInstances_.end()) return;

    if (renderer && renderer->getCharacterRenderer()) {
        renderer->getCharacterRenderer()->removeInstance(it->second);
    }

    creatureInstances_.erase(it);
    creatureModelIds_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureWeaponsAttached_.erase(guid);
    creatureWeaponAttachAttempts_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);

    LOG_DEBUG("Despawned creature: guid=0x", std::hex, guid, std::dec);
}

void Application::despawnOnlineGameObject(uint64_t guid) {
    pendingTransportDoodadBatches_.erase(
        std::remove_if(pendingTransportDoodadBatches_.begin(), pendingTransportDoodadBatches_.end(),
                       [guid](const PendingTransportDoodadBatch& b) { return b.guid == guid; }),
        pendingTransportDoodadBatches_.end());

    auto it = gameObjectInstances_.find(guid);
    if (it == gameObjectInstances_.end()) return;

    if (renderer) {
        if (it->second.isWmo) {
            if (auto* wmoRenderer = renderer->getWMORenderer()) {
                wmoRenderer->removeInstance(it->second.instanceId);
            }
        } else {
            if (auto* m2Renderer = renderer->getM2Renderer()) {
                m2Renderer->removeInstance(it->second.instanceId);
            }
        }
    }

    gameObjectInstances_.erase(it);

    LOG_DEBUG("Despawned gameobject: guid=0x", std::hex, guid, std::dec);
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
                qmr->initialize(vkCtx, pfl, assetManager.get());
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
    if (testTransportSetup_) return;
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

    testTransportSetup_ = true;
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

// ─── World Preloader ─────────────────────────────────────────────────────────
// Pre-warms AssetManager file cache with ADT files (and their _obj0 variants)
// for tiles around the expected spawn position.  Runs in background so that
// when loadOnlineWorldTerrain eventually asks TerrainManager workers to parse
// the same files, every readFile() is an instant cache hit instead of disk I/O.

void Application::startWorldPreload(uint32_t mapId, const std::string& mapName,
                                     float serverX, float serverY) {
    cancelWorldPreload();
    if (!assetManager || !assetManager->isInitialized() || mapName.empty()) return;

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(serverX, serverY, 0.0f));
    auto [tileX, tileY] = core::coords::canonicalToTile(canonical.x, canonical.y);

    worldPreload_ = std::make_unique<WorldPreload>();
    worldPreload_->mapId = mapId;
    worldPreload_->mapName = mapName;
    worldPreload_->centerTileX = tileX;
    worldPreload_->centerTileY = tileY;

    LOG_INFO("World preload: starting for map '", mapName, "' tile [", tileX, ",", tileY, "]");

    // Build list of tiles to preload (radius 1 = 3x3 = 9 tiles, matching load screen)
    struct TileJob { int x, y; };
    auto jobs = std::make_shared<std::vector<TileJob>>();
    // Center tile first (most important)
    jobs->push_back({tileX, tileY});
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            if (dx == 0 && dy == 0) continue;
            int tx = tileX + dx, ty = tileY + dy;
            if (tx < 0 || tx > 63 || ty < 0 || ty > 63) continue;
            jobs->push_back({tx, ty});
        }
    }

    // Spawn worker threads (one per tile for maximum parallelism)
    auto cancelFlag = &worldPreload_->cancel;
    auto* am = assetManager.get();
    std::string mn = mapName;

    int numWorkers = std::min(static_cast<int>(jobs->size()), 4);
    auto nextJob = std::make_shared<std::atomic<int>>(0);

    for (int w = 0; w < numWorkers; w++) {
        worldPreload_->workers.emplace_back([am, mn, jobs, nextJob, cancelFlag]() {
            while (!cancelFlag->load(std::memory_order_relaxed)) {
                int idx = nextJob->fetch_add(1, std::memory_order_relaxed);
                if (idx >= static_cast<int>(jobs->size())) break;

                int tx = (*jobs)[idx].x;
                int ty = (*jobs)[idx].y;

                // Read ADT file (warms file cache)
                std::string adtPath = "World\\Maps\\" + mn + "\\" + mn + "_" +
                                      std::to_string(tx) + "_" + std::to_string(ty) + ".adt";
                am->readFile(adtPath);
                if (cancelFlag->load(std::memory_order_relaxed)) break;

                // Read obj0 variant
                std::string objPath = "World\\Maps\\" + mn + "\\" + mn + "_" +
                                      std::to_string(tx) + "_" + std::to_string(ty) + "_obj0.adt";
                am->readFile(objPath);
            }
            LOG_DEBUG("World preload worker finished");
        });
    }
}

void Application::cancelWorldPreload() {
    if (!worldPreload_) return;
    worldPreload_->cancel.store(true, std::memory_order_relaxed);
    for (auto& t : worldPreload_->workers) {
        if (t.joinable()) t.join();
    }
    LOG_INFO("World preload: cancelled (map=", worldPreload_->mapName,
             " tile=[", worldPreload_->centerTileX, ",", worldPreload_->centerTileY, "])");
    worldPreload_.reset();
}

void Application::saveLastWorldInfo(uint32_t mapId, const std::string& mapName,
                                     float serverX, float serverY) {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    std::string dir = base ? std::string(base) + "\\wowee" : ".";
#else
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.wowee" : ".";
#endif
    std::filesystem::create_directories(dir);
    std::ofstream f(dir + "/last_world.cfg");
    if (f) {
        f << mapId << "\n" << mapName << "\n" << serverX << "\n" << serverY << "\n";
    }
}

Application::LastWorldInfo Application::loadLastWorldInfo() const {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    std::string dir = base ? std::string(base) + "\\wowee" : ".";
#else
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.wowee" : ".";
#endif
    LastWorldInfo info;
    std::ifstream f(dir + "/last_world.cfg");
    if (!f) return info;
    std::string line;
    try {
        if (std::getline(f, line)) info.mapId = static_cast<uint32_t>(std::stoul(line));
        if (std::getline(f, line)) info.mapName = line;
        if (std::getline(f, line)) info.x = std::stof(line);
        if (std::getline(f, line)) info.y = std::stof(line);
    } catch (...) {
        LOG_WARNING("Malformed last_world.cfg, ignoring saved position");
        return info;
    }
    info.valid = !info.mapName.empty();
    return info;
}

} // namespace core
} // namespace wowee
