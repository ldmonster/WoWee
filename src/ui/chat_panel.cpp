#include "ui/chat_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "ui/ui_colors.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <cctype>
#include <chrono>
#include <ctime>
#include <unordered_set>
#include <unordered_map>

namespace {
    // Common ImGui colors (aliases)
    using namespace wowee::ui::colors;
    constexpr auto& kColorRed        = kRed;
    constexpr auto& kColorGreen      = kGreen;
    constexpr auto& kColorBrightGreen= kBrightGreen;
    constexpr auto& kColorYellow     = kYellow;
    constexpr auto& kColorGray       = kGray;
    constexpr auto& kColorDarkGray   = kDarkGray;

    // Common ImGui window flags for popup dialogs
    const ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;

    std::string trim(const std::string& s) {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    bool isPortBotTarget(const std::string& target) {
        std::string t = toLower(trim(target));
        return t == "portbot" || t == "gmbot" || t == "telebot";
    }

    std::string buildPortBotCommand(const std::string& rawInput) {
        std::string input = trim(rawInput);
        if (input.empty()) return "";

        std::string lower = toLower(input);
        if (lower == "help" || lower == "?") {
            return "__help__";
        }

        if (lower.rfind(".tele ", 0) == 0 || lower.rfind(".go ", 0) == 0) {
            return input;
        }

        if (lower.rfind("xyz ", 0) == 0) {
            return ".go " + input;
        }

        if (lower == "sw" || lower == "stormwind") return ".tele stormwind";
        if (lower == "if" || lower == "ironforge") return ".tele ironforge";
        if (lower == "darn" || lower == "darnassus") return ".tele darnassus";
        if (lower == "org" || lower == "orgrimmar") return ".tele orgrimmar";
        if (lower == "tb" || lower == "thunderbluff") return ".tele thunderbluff";
        if (lower == "uc" || lower == "undercity") return ".tele undercity";
        if (lower == "shatt" || lower == "shattrath") return ".tele shattrath";
        if (lower == "dal" || lower == "dalaran") return ".tele dalaran";

        return ".tele " + input;
    }

    std::string getEntityName(const std::shared_ptr<wowee::game::Entity>& entity) {
        if (entity->getType() == wowee::game::ObjectType::PLAYER) {
            auto player = std::static_pointer_cast<wowee::game::Player>(entity);
            if (!player->getName().empty()) return player->getName();
        } else if (entity->getType() == wowee::game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<wowee::game::Unit>(entity);
            if (!unit->getName().empty()) return unit->getName();
        } else if (entity->getType() == wowee::game::ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<wowee::game::GameObject>(entity);
            if (!go->getName().empty()) return go->getName();
        }
        return "Unknown";
    }
}

namespace wowee { namespace ui {

ChatPanel::ChatPanel() {
    initChatTabs();
}

void ChatPanel::initChatTabs() {
    chatTabs_.clear();
    // General tab: shows everything
    chatTabs_.push_back({"General", ~0ULL});
    // Combat tab: system, loot, skills, achievements, and NPC speech/emotes
    chatTabs_.push_back({"Combat", (1ULL << static_cast<uint8_t>(game::ChatType::SYSTEM)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::LOOT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::SKILL)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::ACHIEVEMENT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::GUILD_ACHIEVEMENT)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_SAY)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_YELL)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_EMOTE)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_WHISPER)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::MONSTER_PARTY)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::RAID_BOSS_WHISPER)) |
                                    (1ULL << static_cast<uint8_t>(game::ChatType::RAID_BOSS_EMOTE))});
    // Whispers tab
    chatTabs_.push_back({"Whispers", (1ULL << static_cast<uint8_t>(game::ChatType::WHISPER)) |
                                      (1ULL << static_cast<uint8_t>(game::ChatType::WHISPER_INFORM))});
    // Guild tab: guild and officer chat
    chatTabs_.push_back({"Guild", (1ULL << static_cast<uint8_t>(game::ChatType::GUILD)) |
                                   (1ULL << static_cast<uint8_t>(game::ChatType::OFFICER)) |
                                   (1ULL << static_cast<uint8_t>(game::ChatType::GUILD_ACHIEVEMENT))});
    // Trade/LFG tab: channel messages
    chatTabs_.push_back({"Trade/LFG", (1ULL << static_cast<uint8_t>(game::ChatType::CHANNEL))});
    // Reset unread counts to match new tab list
    chatTabUnread_.assign(chatTabs_.size(), 0);
    chatTabSeenCount_ = 0;
}

bool ChatPanel::shouldShowMessage(const game::MessageChatData& msg, int tabIndex) const {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(chatTabs_.size())) return true;
    const auto& tab = chatTabs_[tabIndex];
    if (tab.typeMask == ~0ULL) return true;  // General tab shows all

    uint64_t typeBit = 1ULL << static_cast<uint8_t>(msg.type);

    // For Trade/LFG tab (now index 4), also filter by channel name
    if (tabIndex == 4 && msg.type == game::ChatType::CHANNEL) {
        const std::string& ch = msg.channelName;
        if (ch.find("Trade") == std::string::npos &&
            ch.find("General") == std::string::npos &&
            ch.find("LookingForGroup") == std::string::npos &&
            ch.find("Local") == std::string::npos) {
            return false;
        }
        return true;
    }

    return (tab.typeMask & typeBit) != 0;
}


// Forward declaration — defined below
static std::vector<std::string> allMacroCommands(const std::string& macroText);
static std::string evaluateMacroConditionals(const std::string& rawArg,
                                              game::GameHandler& gameHandler,
                                              uint64_t& targetOverride);

void ChatPanel::render(game::GameHandler& gameHandler,
                       InventoryScreen& inventoryScreen,
                       SpellbookScreen& spellbookScreen,
                       QuestLogScreen& questLogScreen) {
    auto* window = services_.window;
    auto* assetMgr = services_.assetManager;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
    float chatW = std::min(500.0f, screenW * 0.4f);
    float chatH = 220.0f;
    float chatX = 8.0f;
    float chatY = screenH - chatH - 80.0f;  // Above action bar
    if (chatWindowLocked_) {
        // Always recompute position from current window size when locked
        chatWindowPos_ = ImVec2(chatX, chatY);
        ImGui::SetNextWindowSize(ImVec2(chatW, chatH), ImGuiCond_Always);
        ImGui::SetNextWindowPos(chatWindowPos_, ImGuiCond_Always);
    } else {
        if (!chatWindowPosInit_) {
            chatWindowPos_ = ImVec2(chatX, chatY);
            chatWindowPosInit_ = true;
        }
        ImGui::SetNextWindowSize(ImVec2(chatW, chatH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(chatWindowPos_, ImGuiCond_FirstUseEver);
    }
    ImGuiWindowFlags flags = kDialogFlags;
    if (chatWindowLocked_) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
    }
    ImGui::Begin("Chat", nullptr, flags);

    if (!chatWindowLocked_) {
        chatWindowPos_ = ImGui::GetWindowPos();
    }

    // Update unread counts: scan any new messages since last frame
    {
        const auto& history = gameHandler.getChatHistory();
        // Ensure unread array is sized correctly (guards against late init)
        if (chatTabUnread_.size() != chatTabs_.size())
            chatTabUnread_.assign(chatTabs_.size(), 0);
        // If history shrank (e.g. cleared), reset
        if (chatTabSeenCount_ > history.size()) chatTabSeenCount_ = 0;
        for (size_t mi = chatTabSeenCount_; mi < history.size(); ++mi) {
            const auto& msg = history[mi];
            // For each non-General (non-0) tab that isn't currently active, check visibility
            for (int ti = 1; ti < static_cast<int>(chatTabs_.size()); ++ti) {
                if (ti == activeChatTab) continue;
                if (shouldShowMessage(msg, ti)) {
                    chatTabUnread_[ti]++;
                }
            }
        }
        chatTabSeenCount_ = history.size();
    }

    // Chat tabs
    if (ImGui::BeginTabBar("ChatTabs")) {
        for (int i = 0; i < static_cast<int>(chatTabs_.size()); ++i) {
            // Build label with unread count suffix for non-General tabs
            std::string tabLabel = chatTabs_[i].name;
            if (i > 0 && i < static_cast<int>(chatTabUnread_.size()) && chatTabUnread_[i] > 0) {
                tabLabel += " (" + std::to_string(chatTabUnread_[i]) + ")";
            }
            // Flash tab text color when unread messages exist
            bool hasUnread = (i > 0 && i < static_cast<int>(chatTabUnread_.size()) && chatTabUnread_[i] > 0);
            if (hasUnread) {
                float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f * pulse, 0.2f * pulse, 1.0f));
            }
            if (ImGui::BeginTabItem(tabLabel.c_str())) {
                if (activeChatTab != i) {
                    activeChatTab = i;
                    // Clear unread count when tab becomes active
                    if (i < static_cast<int>(chatTabUnread_.size()))
                        chatTabUnread_[i] = 0;
                }
                ImGui::EndTabItem();
            }
            if (hasUnread) ImGui::PopStyleColor();
        }
        ImGui::EndTabBar();
    }

    // Chat history
    const auto& chatHistory = gameHandler.getChatHistory();

    // Apply chat font size scaling
    float chatScale = chatFontSize == 0 ? 0.85f : (chatFontSize == 2 ? 1.2f : 1.0f);
    ImGui::SetWindowFontScale(chatScale);

    ImGui::BeginChild("ChatHistory", ImVec2(0, -70), true, ImGuiWindowFlags_HorizontalScrollbar);
    bool chatHistoryHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Helper: parse WoW color code |cAARRGGBB → ImVec4
    auto parseWowColor = [](const std::string& text, size_t pos) -> ImVec4 {
        // |cAARRGGBB (10 chars total: |c + 8 hex)
        if (pos + 10 > text.size()) return colors::kWhite;
        auto hexByte = [&](size_t offset) -> float {
            const char* s = text.c_str() + pos + offset;
            char buf[3] = {s[0], s[1], '\0'};
            return static_cast<float>(strtol(buf, nullptr, 16)) / 255.0f;
        };
        float a = hexByte(2);
        float r = hexByte(4);
        float g = hexByte(6);
        float b = hexByte(8);
        return ImVec4(r, g, b, a);
    };

    // Helper: render an item tooltip from ItemQueryResponseData
    auto renderItemLinkTooltip = [&](uint32_t itemEntry) {
        const auto* info = gameHandler.getItemInfo(itemEntry);
        if (!info || !info->valid) return;
        auto findComparableEquipped = [&](uint8_t inventoryType) -> const game::ItemSlot* {
            using ES = game::EquipSlot;
            const auto& inv = gameHandler.getInventory();
            auto slotPtr = [&](ES slot) -> const game::ItemSlot* {
                const auto& s = inv.getEquipSlot(slot);
                return s.empty() ? nullptr : &s;
            };
            switch (inventoryType) {
                case 1: return slotPtr(ES::HEAD);
                case 2: return slotPtr(ES::NECK);
                case 3: return slotPtr(ES::SHOULDERS);
                case 4: return slotPtr(ES::SHIRT);
                case 5:
                case 20: return slotPtr(ES::CHEST);
                case 6: return slotPtr(ES::WAIST);
                case 7: return slotPtr(ES::LEGS);
                case 8: return slotPtr(ES::FEET);
                case 9: return slotPtr(ES::WRISTS);
                case 10: return slotPtr(ES::HANDS);
                case 11: {
                    if (auto* s = slotPtr(ES::RING1)) return s;
                    return slotPtr(ES::RING2);
                }
                case 12: {
                    if (auto* s = slotPtr(ES::TRINKET1)) return s;
                    return slotPtr(ES::TRINKET2);
                }
                case 13:
                    if (auto* s = slotPtr(ES::MAIN_HAND)) return s;
                    return slotPtr(ES::OFF_HAND);
                case 14:
                case 22:
                case 23: return slotPtr(ES::OFF_HAND);
                case 15:
                case 25:
                case 26: return slotPtr(ES::RANGED);
                case 16: return slotPtr(ES::BACK);
                case 17:
                case 21: return slotPtr(ES::MAIN_HAND);
                case 18:
                    for (int i = 0; i < game::Inventory::NUM_BAG_SLOTS; ++i) {
                        auto slot = static_cast<ES>(static_cast<int>(ES::BAG1) + i);
                        if (auto* s = slotPtr(slot)) return s;
                    }
                    return nullptr;
                case 19: return slotPtr(ES::TABARD);
                default: return nullptr;
            }
        };

        ImGui::BeginTooltip();
        // Quality color for name
        auto qColor = ui::getQualityColor(static_cast<game::ItemQuality>(info->quality));
        ImGui::TextColored(qColor, "%s", info->name.c_str());

        // Heroic indicator (green, matches WoW tooltip style)
        constexpr uint32_t kFlagHeroic         = 0x8;
        constexpr uint32_t kFlagUniqueEquipped = 0x1000000;
        if (info->itemFlags & kFlagHeroic)
            ImGui::TextColored(ImVec4(0.0f, 0.8f, 0.0f, 1.0f), "Heroic");

        // Bind type (appears right under name in WoW)
        switch (info->bindType) {
            case 1: ImGui::TextDisabled("Binds when picked up");   break;
            case 2: ImGui::TextDisabled("Binds when equipped");    break;
            case 3: ImGui::TextDisabled("Binds when used");        break;
            case 4: ImGui::TextDisabled("Quest Item");             break;
        }
        // Unique / Unique-Equipped
        if (info->maxCount == 1)
            ImGui::TextColored(ui::colors::kTooltipGold, "Unique");
        else if (info->itemFlags & kFlagUniqueEquipped)
            ImGui::TextColored(ui::colors::kTooltipGold, "Unique-Equipped");

        // Slot type
        if (info->inventoryType > 0) {
            const char* slotName = ui::getInventorySlotName(info->inventoryType);
            if (slotName[0]) {
                if (!info->subclassName.empty())
                    ImGui::TextColored(ui::colors::kLightGray, "%s  %s", slotName, info->subclassName.c_str());
                else
                    ImGui::TextColored(ui::colors::kLightGray, "%s", slotName);
            }
        }
        auto isWeaponInventoryType = [](uint32_t invType) {
            switch (invType) {
                case 13: // One-Hand
                case 15: // Ranged
                case 17: // Two-Hand
                case 21: // Main Hand
                case 25: // Thrown
                case 26: // Ranged Right
                    return true;
                default:
                    return false;
            }
        };
        const bool isWeapon = isWeaponInventoryType(info->inventoryType);

        // Item level (after slot/subclass)
        if (info->itemLevel > 0)
            ImGui::TextDisabled("Item Level %u", info->itemLevel);

        if (isWeapon && info->damageMax > 0.0f && info->delayMs > 0) {
            float speed = static_cast<float>(info->delayMs) / 1000.0f;
            float dps = ((info->damageMin + info->damageMax) * 0.5f) / speed;
            // WoW-style: "22 - 41 Damage" with speed right-aligned on same row
            char dmgBuf[64], spdBuf[32];
            std::snprintf(dmgBuf, sizeof(dmgBuf), "%d - %d Damage",
                          static_cast<int>(info->damageMin), static_cast<int>(info->damageMax));
            std::snprintf(spdBuf, sizeof(spdBuf), "Speed %.2f", speed);
            float spdW = ImGui::CalcTextSize(spdBuf).x;
            ImGui::Text("%s", dmgBuf);
            ImGui::SameLine(ImGui::GetWindowWidth() - spdW - 16.0f);
            ImGui::Text("%s", spdBuf);
            ImGui::TextDisabled("(%.1f damage per second)", dps);
        }
        ImVec4 green(0.0f, 1.0f, 0.0f, 1.0f);
        auto appendBonus = [](std::string& out, int32_t val, const char* shortName) {
            if (val <= 0) return;
            if (!out.empty()) out += "  ";
            out += "+" + std::to_string(val) + " ";
            out += shortName;
        };
        std::string bonusLine;
        appendBonus(bonusLine, info->strength, "Str");
        appendBonus(bonusLine, info->agility, "Agi");
        appendBonus(bonusLine, info->stamina, "Sta");
        appendBonus(bonusLine, info->intellect, "Int");
        appendBonus(bonusLine, info->spirit, "Spi");
        if (!bonusLine.empty()) {
            ImGui::TextColored(green, "%s", bonusLine.c_str());
        }
        if (info->armor > 0) {
            ImGui::Text("%d Armor", info->armor);
        }
        // Elemental resistances (fire resist gear, nature resist gear, etc.)
        {
            const int32_t resVals[6] = {
                info->holyRes, info->fireRes, info->natureRes,
                info->frostRes, info->shadowRes, info->arcaneRes
            };
            static constexpr const char* resLabels[6] = {
                "Holy Resistance", "Fire Resistance", "Nature Resistance",
                "Frost Resistance", "Shadow Resistance", "Arcane Resistance"
            };
            for (int ri = 0; ri < 6; ++ri)
                if (resVals[ri] > 0) ImGui::Text("+%d %s", resVals[ri], resLabels[ri]);
        }
        // Extra stats (hit/crit/haste/sp/ap/expertise/resilience/etc.)
        if (!info->extraStats.empty()) {
            auto statName = [](uint32_t t) -> const char* {
                switch (t) {
                    case 12: return "Defense Rating";
                    case 13: return "Dodge Rating";
                    case 14: return "Parry Rating";
                    case 15: return "Block Rating";
                    case 16: case 17: case 18: case 31: return "Hit Rating";
                    case 19: case 20: case 21: case 32: return "Critical Strike Rating";
                    case 28: case 29: case 30: case 35: return "Haste Rating";
                    case 34: return "Resilience Rating";
                    case 36: return "Expertise Rating";
                    case 37: return "Attack Power";
                    case 38: return "Ranged Attack Power";
                    case 45: return "Spell Power";
                    case 46: return "Healing Power";
                    case 47: return "Spell Damage";
                    case 49: return "Mana per 5 sec.";
                    case 43: return "Spell Penetration";
                    case 44: return "Block Value";
                    default: return nullptr;
                }
            };
            for (const auto& es : info->extraStats) {
                const char* nm = statName(es.statType);
                if (nm && es.statValue > 0)
                    ImGui::TextColored(green, "+%d %s", es.statValue, nm);
            }
        }
        // Gem sockets (WotLK only — socketColor != 0 means socket present)
        // socketColor bitmask: 1=Meta, 2=Red, 4=Yellow, 8=Blue
        {
            const auto& kSocketTypes = ui::kSocketTypes;
            bool hasSocket = false;
            for (int s = 0; s < 3; ++s) {
                if (info->socketColor[s] == 0) continue;
                if (!hasSocket) { ImGui::Spacing(); hasSocket = true; }
                for (const auto& st : kSocketTypes) {
                    if (info->socketColor[s] & st.mask) {
                        ImGui::TextColored(st.col, "%s", st.label);
                        break;
                    }
                }
            }
            if (hasSocket && info->socketBonus != 0) {
                // Socket bonus ID maps to SpellItemEnchantment.dbc — lazy-load names
                static std::unordered_map<uint32_t, std::string> s_enchantNames;
                static bool s_enchantNamesLoaded = false;
                if (!s_enchantNamesLoaded && assetMgr) {
                    s_enchantNamesLoaded = true;
                    auto dbc = assetMgr->loadDBC("SpellItemEnchantment.dbc");
                    if (dbc && dbc->isLoaded()) {
                        const auto* lay = pipeline::getActiveDBCLayout()
                            ? pipeline::getActiveDBCLayout()->getLayout("SpellItemEnchantment") : nullptr;
                        uint32_t nameField = lay ? lay->field("Name") : 8u;
                        if (nameField == 0xFFFFFFFF) nameField = 8;
                        uint32_t fc = dbc->getFieldCount();
                        for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                            uint32_t eid = dbc->getUInt32(r, 0);
                            if (eid == 0 || nameField >= fc) continue;
                            std::string ename = dbc->getString(r, nameField);
                            if (!ename.empty()) s_enchantNames[eid] = std::move(ename);
                        }
                    }
                }
                auto enchIt = s_enchantNames.find(info->socketBonus);
                if (enchIt != s_enchantNames.end())
                    ImGui::TextColored(colors::kSocketGreen, "Socket Bonus: %s", enchIt->second.c_str());
                else
                    ImGui::TextColored(colors::kSocketGreen, "Socket Bonus: (id %u)", info->socketBonus);
            }
        }
        // Item set membership
        if (info->itemSetId != 0) {
            struct SetEntry {
                std::string name;
                std::array<uint32_t, 10> itemIds{};
                std::array<uint32_t, 10> spellIds{};
                std::array<uint32_t, 10> thresholds{};
            };
            static std::unordered_map<uint32_t, SetEntry> s_setData;
            static bool s_setDataLoaded = false;
            if (!s_setDataLoaded && assetMgr) {
                s_setDataLoaded = true;
                auto dbc = assetMgr->loadDBC("ItemSet.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemSet") : nullptr;
                    auto lf = [&](const char* k, uint32_t def) -> uint32_t {
                        return layout ? (*layout)[k] : def;
                    };
                    uint32_t idF = lf("ID", 0), nameF = lf("Name", 1);
                    const auto& itemKeys = ui::kItemSetItemKeys;
                    const auto& spellKeys = ui::kItemSetSpellKeys;
                    const auto& thrKeys = ui::kItemSetThresholdKeys;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t id = dbc->getUInt32(r, idF);
                        if (!id) continue;
                        SetEntry e;
                        e.name = dbc->getString(r, nameF);
                        for (int i = 0; i < 10; ++i) {
                            e.itemIds[i]    = dbc->getUInt32(r, layout ? (*layout)[itemKeys[i]]  : uint32_t(18 + i));
                            e.spellIds[i]   = dbc->getUInt32(r, layout ? (*layout)[spellKeys[i]] : uint32_t(28 + i));
                            e.thresholds[i] = dbc->getUInt32(r, layout ? (*layout)[thrKeys[i]]   : uint32_t(38 + i));
                        }
                        s_setData[id] = std::move(e);
                    }
                }
            }
            ImGui::Spacing();
            const auto& inv = gameHandler.getInventory();
            auto setIt = s_setData.find(info->itemSetId);
            if (setIt != s_setData.end()) {
                const SetEntry& se = setIt->second;
                int equipped = 0, total = 0;
                for (int i = 0; i < 10; ++i) {
                    if (se.itemIds[i] == 0) continue;
                    ++total;
                    for (int sl = 0; sl < game::Inventory::NUM_EQUIP_SLOTS; sl++) {
                        const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(sl));
                        if (!eq.empty() && eq.item.itemId == se.itemIds[i]) { ++equipped; break; }
                    }
                }
                if (total > 0)
                    ImGui::TextColored(ui::colors::kTooltipGold,
                        "%s (%d/%d)", se.name.empty() ? "Set" : se.name.c_str(), equipped, total);
                else if (!se.name.empty())
                    ImGui::TextColored(ui::colors::kTooltipGold, "%s", se.name.c_str());
                for (int i = 0; i < 10; ++i) {
                    if (se.spellIds[i] == 0 || se.thresholds[i] == 0) continue;
                    const std::string& bname = gameHandler.getSpellName(se.spellIds[i]);
                    bool active = (equipped >= static_cast<int>(se.thresholds[i]));
                    ImVec4 col = active ? colors::kActiveGreen : colors::kInactiveGray;
                    if (!bname.empty())
                        ImGui::TextColored(col, "(%u) %s", se.thresholds[i], bname.c_str());
                    else
                        ImGui::TextColored(col, "(%u) Set Bonus", se.thresholds[i]);
                }
            } else {
                ImGui::TextColored(ui::colors::kTooltipGold, "Set (id %u)", info->itemSetId);
            }
        }
        // Item spell effects (Use / Equip / Chance on Hit / Teaches)
        for (const auto& sp : info->spells) {
            if (sp.spellId == 0) continue;
            const char* triggerLabel = nullptr;
            switch (sp.spellTrigger) {
                case 0: triggerLabel = "Use";          break;
                case 1: triggerLabel = "Equip";        break;
                case 2: triggerLabel = "Chance on Hit"; break;
                case 5: triggerLabel = "Teaches";      break;
            }
            if (!triggerLabel) continue;
            // Use full spell description if available (matches inventory tooltip style)
            const std::string& spDesc = gameHandler.getSpellDescription(sp.spellId);
            const std::string& spText = !spDesc.empty() ? spDesc
                                        : gameHandler.getSpellName(sp.spellId);
            if (!spText.empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 300.0f);
                ImGui::TextColored(colors::kCyan,
                                   "%s: %s", triggerLabel, spText.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        // Required level
        if (info->requiredLevel > 1)
            ImGui::TextDisabled("Requires Level %u", info->requiredLevel);
        // Required skill (e.g. "Requires Blacksmithing (300)")
        if (info->requiredSkill != 0 && info->requiredSkillRank > 0) {
            static std::unordered_map<uint32_t, std::string> s_skillNames;
            static bool s_skillNamesLoaded = false;
            if (!s_skillNamesLoaded && assetMgr) {
                s_skillNamesLoaded = true;
                auto dbc = assetMgr->loadDBC("SkillLine.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("SkillLine") : nullptr;
                    uint32_t idF   = layout ? (*layout)["ID"]   : 0u;
                    uint32_t nameF = layout ? (*layout)["Name"] : 2u;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t sid = dbc->getUInt32(r, idF);
                        if (!sid) continue;
                        std::string sname = dbc->getString(r, nameF);
                        if (!sname.empty()) s_skillNames[sid] = std::move(sname);
                    }
                }
            }
            uint32_t playerSkillVal = 0;
            const auto& skills = gameHandler.getPlayerSkills();
            auto skPit = skills.find(info->requiredSkill);
            if (skPit != skills.end()) playerSkillVal = skPit->second.effectiveValue();
            bool meetsSkill = (playerSkillVal == 0 || playerSkillVal >= info->requiredSkillRank);
            ImVec4 skColor = meetsSkill ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
            auto skIt = s_skillNames.find(info->requiredSkill);
            if (skIt != s_skillNames.end())
                ImGui::TextColored(skColor, "Requires %s (%u)", skIt->second.c_str(), info->requiredSkillRank);
            else
                ImGui::TextColored(skColor, "Requires Skill %u (%u)", info->requiredSkill, info->requiredSkillRank);
        }
        // Required reputation (e.g. "Requires Exalted with Argent Dawn")
        if (info->requiredReputationFaction != 0 && info->requiredReputationRank > 0) {
            static std::unordered_map<uint32_t, std::string> s_factionNames;
            static bool s_factionNamesLoaded = false;
            if (!s_factionNamesLoaded && assetMgr) {
                s_factionNamesLoaded = true;
                auto dbc = assetMgr->loadDBC("Faction.dbc");
                if (dbc && dbc->isLoaded()) {
                    const auto* layout = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("Faction") : nullptr;
                    uint32_t idF   = layout ? (*layout)["ID"]   : 0u;
                    uint32_t nameF = layout ? (*layout)["Name"] : 20u;
                    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
                        uint32_t fid = dbc->getUInt32(r, idF);
                        if (!fid) continue;
                        std::string fname = dbc->getString(r, nameF);
                        if (!fname.empty()) s_factionNames[fid] = std::move(fname);
                    }
                }
            }
            static constexpr const char* kRepRankNames[] = {
                "Hated", "Hostile", "Unfriendly", "Neutral",
                "Friendly", "Honored", "Revered", "Exalted"
            };
            const char* rankName = (info->requiredReputationRank < 8)
                ? kRepRankNames[info->requiredReputationRank] : "Unknown";
            auto fIt = s_factionNames.find(info->requiredReputationFaction);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.75f), "Requires %s with %s",
                rankName,
                fIt != s_factionNames.end() ? fIt->second.c_str() : "Unknown Faction");
        }
        // Class restriction (e.g. "Classes: Paladin, Warrior")
        if (info->allowableClass != 0) {
            const auto& kClasses = ui::kClassMasks;
            int matchCount = 0;
            for (const auto& kc : kClasses)
                if (info->allowableClass & kc.mask) ++matchCount;
            if (matchCount > 0 && matchCount < 10) {
                char classBuf[128] = "Classes: ";
                bool first = true;
                for (const auto& kc : kClasses) {
                    if (!(info->allowableClass & kc.mask)) continue;
                    if (!first) strncat(classBuf, ", ", sizeof(classBuf) - strlen(classBuf) - 1);
                    strncat(classBuf, kc.name, sizeof(classBuf) - strlen(classBuf) - 1);
                    first = false;
                }
                uint8_t pc = gameHandler.getPlayerClass();
                uint32_t pmask = (pc > 0 && pc <= 10) ? (1u << (pc - 1)) : 0u;
                bool playerAllowed = (pmask == 0 || (info->allowableClass & pmask));
                ImVec4 clColor = playerAllowed ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
                ImGui::TextColored(clColor, "%s", classBuf);
            }
        }
        // Race restriction (e.g. "Races: Night Elf, Human")
        if (info->allowableRace != 0) {
            const auto& kRaces = ui::kRaceMasks;
            constexpr uint32_t kAllPlayable = 1|2|4|8|16|32|64|128|512|1024;
            if ((info->allowableRace & kAllPlayable) != kAllPlayable) {
                int matchCount = 0;
                for (const auto& kr : kRaces)
                    if (info->allowableRace & kr.mask) ++matchCount;
                if (matchCount > 0) {
                    char raceBuf[160] = "Races: ";
                    bool first = true;
                    for (const auto& kr : kRaces) {
                        if (!(info->allowableRace & kr.mask)) continue;
                        if (!first) strncat(raceBuf, ", ", sizeof(raceBuf) - strlen(raceBuf) - 1);
                        strncat(raceBuf, kr.name, sizeof(raceBuf) - strlen(raceBuf) - 1);
                        first = false;
                    }
                    uint8_t pr = gameHandler.getPlayerRace();
                    uint32_t pmask = (pr > 0 && pr <= 11) ? (1u << (pr - 1)) : 0u;
                    bool playerAllowed = (pmask == 0 || (info->allowableRace & pmask));
                    ImVec4 rColor = playerAllowed ? ImVec4(1.0f, 1.0f, 1.0f, 0.75f) : colors::kPaleRed;
                    ImGui::TextColored(rColor, "%s", raceBuf);
                }
            }
        }
        // Flavor / lore text (shown in gold italic in WoW, use a yellow-ish dim color here)
        if (!info->description.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(300.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 0.85f), "\"%s\"", info->description.c_str());
            ImGui::PopTextWrapPos();
        }
        if (info->sellPrice > 0) {
            ImGui::TextDisabled("Sell:"); ImGui::SameLine(0, 4);
            renderCoinsFromCopper(info->sellPrice);
        }

        if (ImGui::GetIO().KeyShift && info->inventoryType > 0) {
            if (const auto* eq = findComparableEquipped(static_cast<uint8_t>(info->inventoryType))) {
                ImGui::Separator();
                ImGui::TextDisabled("Equipped:");
                VkDescriptorSet eqIcon = inventoryScreen.getItemIcon(eq->item.displayInfoId);
                if (eqIcon) {
                    ImGui::Image((ImTextureID)(uintptr_t)eqIcon, ImVec2(18.0f, 18.0f));
                    ImGui::SameLine();
                }
                ImGui::TextColored(InventoryScreen::getQualityColor(eq->item.quality), "%s", eq->item.name.c_str());
                if (isWeaponInventoryType(eq->item.inventoryType) &&
                    eq->item.damageMax > 0.0f && eq->item.delayMs > 0) {
                    float speed = static_cast<float>(eq->item.delayMs) / 1000.0f;
                    float dps = ((eq->item.damageMin + eq->item.damageMax) * 0.5f) / speed;
                    char eqDmg[64], eqSpd[32];
                    std::snprintf(eqDmg, sizeof(eqDmg), "%d - %d Damage",
                                  static_cast<int>(eq->item.damageMin), static_cast<int>(eq->item.damageMax));
                    std::snprintf(eqSpd, sizeof(eqSpd), "Speed %.2f", speed);
                    float eqSpdW = ImGui::CalcTextSize(eqSpd).x;
                    ImGui::Text("%s", eqDmg);
                    ImGui::SameLine(ImGui::GetWindowWidth() - eqSpdW - 16.0f);
                    ImGui::Text("%s", eqSpd);
                    ImGui::TextDisabled("(%.1f damage per second)", dps);
                }
                if (eq->item.armor > 0) {
                    ImGui::Text("%d Armor", eq->item.armor);
                }
                std::string eqBonusLine;
                appendBonus(eqBonusLine, eq->item.strength, "Str");
                appendBonus(eqBonusLine, eq->item.agility, "Agi");
                appendBonus(eqBonusLine, eq->item.stamina, "Sta");
                appendBonus(eqBonusLine, eq->item.intellect, "Int");
                appendBonus(eqBonusLine, eq->item.spirit, "Spi");
                if (!eqBonusLine.empty()) {
                    ImGui::TextColored(green, "%s", eqBonusLine.c_str());
                }
                // Extra stats for the equipped item
                for (const auto& es : eq->item.extraStats) {
                    const char* nm = nullptr;
                    switch (es.statType) {
                        case 12: nm = "Defense Rating"; break;
                        case 13: nm = "Dodge Rating"; break;
                        case 14: nm = "Parry Rating"; break;
                        case 16: case 17: case 18: case 31: nm = "Hit Rating"; break;
                        case 19: case 20: case 21: case 32: nm = "Critical Strike Rating"; break;
                        case 28: case 29: case 30: case 35: nm = "Haste Rating"; break;
                        case 34: nm = "Resilience Rating"; break;
                        case 36: nm = "Expertise Rating"; break;
                        case 37: nm = "Attack Power"; break;
                        case 38: nm = "Ranged Attack Power"; break;
                        case 45: nm = "Spell Power"; break;
                        case 46: nm = "Healing Power"; break;
                        case 49: nm = "Mana per 5 sec."; break;
                        default: break;
                    }
                    if (nm && es.statValue > 0)
                        ImGui::TextColored(green, "+%d %s", es.statValue, nm);
                }
            }
        }
        ImGui::EndTooltip();
    };

    // Helper: render text with clickable URLs and WoW item links
    auto renderTextWithLinks = [&](const std::string& text, const ImVec4& color) {
        size_t pos = 0;
        while (pos < text.size()) {
            // Find next special element: URL or WoW link
            size_t urlStart = text.find("https://", pos);

            // Find next WoW link (may be colored with |c prefix or bare |H)
            size_t linkStart = text.find("|c", pos);
            // Also handle bare |H links without color prefix
            size_t bareItem  = text.find("|Hitem:",  pos);
            size_t bareSpell = text.find("|Hspell:", pos);
            size_t bareQuest = text.find("|Hquest:", pos);
            size_t bareLinkStart = std::min({bareItem, bareSpell, bareQuest});

            // Determine which comes first
            size_t nextSpecial = std::min({urlStart, linkStart, bareLinkStart});

            if (nextSpecial == std::string::npos) {
                // No more special elements, render remaining text
                std::string remaining = text.substr(pos);
                if (!remaining.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextWrapped("%s", remaining.c_str());
                    ImGui::PopStyleColor();
                }
                break;
            }

            // Render plain text before special element
            if (nextSpecial > pos) {
                std::string before = text.substr(pos, nextSpecial - pos);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s", before.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 0);
            }

            // Handle WoW item link
            if (nextSpecial == linkStart || nextSpecial == bareLinkStart) {
                ImVec4 linkColor = color;
                size_t hStart = std::string::npos;

                if (nextSpecial == linkStart && text.size() > linkStart + 10) {
                    // Parse |cAARRGGBB color
                    linkColor = parseWowColor(text, linkStart);
                    // Find the nearest |H link of any supported type
                    size_t hItem  = text.find("|Hitem:",        linkStart + 10);
                    size_t hSpell = text.find("|Hspell:",       linkStart + 10);
                    size_t hQuest = text.find("|Hquest:",       linkStart + 10);
                    size_t hAch   = text.find("|Hachievement:", linkStart + 10);
                    hStart = std::min({hItem, hSpell, hQuest, hAch});
                } else if (nextSpecial == bareLinkStart) {
                    hStart = bareLinkStart;
                }

                if (hStart != std::string::npos) {
                    // Determine link type
                    const bool isSpellLink = (text.compare(hStart, 8, "|Hspell:") == 0);
                    const bool isQuestLink = (text.compare(hStart, 8, "|Hquest:") == 0);
                    const bool isAchievLink = (text.compare(hStart, 14, "|Hachievement:") == 0);
                    // Default: item link

                    // Parse the first numeric ID after |Htype:
                    size_t idOffset = isSpellLink ? 8 : (isQuestLink ? 8 : (isAchievLink ? 14 : 7));
                    size_t entryStart = hStart + idOffset;
                    size_t entryEnd = text.find(':', entryStart);
                    uint32_t linkId = 0;
                    if (entryEnd != std::string::npos) {
                        linkId = static_cast<uint32_t>(strtoul(
                            text.substr(entryStart, entryEnd - entryStart).c_str(), nullptr, 10));
                    }

                    // Find display name: |h[Name]|h
                    size_t nameTagStart = text.find("|h[", hStart);
                    size_t nameTagEnd = (nameTagStart != std::string::npos)
                        ? text.find("]|h", nameTagStart + 3) : std::string::npos;

                    std::string linkName = isSpellLink ? "Unknown Spell"
                                        : isQuestLink  ? "Unknown Quest"
                                        : isAchievLink ? "Unknown Achievement"
                                        : "Unknown Item";
                    if (nameTagStart != std::string::npos && nameTagEnd != std::string::npos) {
                        linkName = text.substr(nameTagStart + 3, nameTagEnd - nameTagStart - 3);
                    }

                    // Find end of entire link sequence (|r or after ]|h)
                    size_t linkEnd = (nameTagEnd != std::string::npos) ? nameTagEnd + 3 : hStart + idOffset;
                    size_t resetPos = text.find("|r", linkEnd);
                    if (resetPos != std::string::npos && resetPos <= linkEnd + 2) {
                        linkEnd = resetPos + 2;
                    }

                    if (!isSpellLink && !isQuestLink && !isAchievLink) {
                        // --- Item link ---
                        uint32_t itemEntry = linkId;
                        if (itemEntry > 0) {
                            gameHandler.ensureItemInfo(itemEntry);
                        }

                        // Show small icon before item link if available
                        if (itemEntry > 0) {
                            const auto* chatInfo = gameHandler.getItemInfo(itemEntry);
                            if (chatInfo && chatInfo->valid && chatInfo->displayInfoId != 0) {
                                VkDescriptorSet chatIcon = inventoryScreen.getItemIcon(chatInfo->displayInfoId);
                                if (chatIcon) {
                                    ImGui::Image((ImTextureID)(uintptr_t)chatIcon, ImVec2(12, 12));
                                    if (ImGui::IsItemHovered()) {
                                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                        renderItemLinkTooltip(itemEntry);
                                    }
                                    ImGui::SameLine(0, 2);
                                }
                            }
                        }

                        // Render bracketed item name in quality color
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (itemEntry > 0) {
                                renderItemLinkTooltip(itemEntry);
                            }
                        }
                    } else if (isSpellLink) {
                        // --- Spell link: |Hspell:SPELLID:RANK|h[Name]|h ---
                        // Small icon (use spell icon cache if available)
                        VkDescriptorSet spellIcon = (linkId > 0) ? getSpellIcon(linkId, assetMgr) : VK_NULL_HANDLE;
                        if (spellIcon) {
                            ImGui::Image((ImTextureID)(uintptr_t)spellIcon, ImVec2(12, 12));
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                                spellbookScreen.renderSpellInfoTooltip(linkId, gameHandler, assetMgr);
                            }
                            ImGui::SameLine(0, 2);
                        }

                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if (linkId > 0) {
                                spellbookScreen.renderSpellInfoTooltip(linkId, gameHandler, assetMgr);
                            }
                        }
                    } else if (isQuestLink) {
                        // --- Quest link: |Hquest:QUESTID:QUESTLEVEL|h[Name]|h ---
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, colors::kWarmGold); // gold
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::BeginTooltip();
                            ImGui::TextColored(colors::kWarmGold, "%s", linkName.c_str());
                            // Parse quest level (second field after questId)
                            if (entryEnd != std::string::npos) {
                                size_t lvlEnd = text.find(':', entryEnd + 1);
                                if (lvlEnd == std::string::npos) lvlEnd = text.find('|', entryEnd + 1);
                                if (lvlEnd != std::string::npos) {
                                    uint32_t qLvl = static_cast<uint32_t>(strtoul(
                                        text.substr(entryEnd + 1, lvlEnd - entryEnd - 1).c_str(), nullptr, 10));
                                    if (qLvl > 0) ImGui::TextDisabled("Level %u Quest", qLvl);
                                }
                            }
                            ImGui::TextDisabled("Click quest log to view details");
                            ImGui::EndTooltip();
                        }
                        // Click: open quest log and select this quest if we have it
                        if (ImGui::IsItemClicked() && linkId > 0) {
                            questLogScreen.openAndSelectQuest(linkId);
                        }
                    } else {
                        // --- Achievement link ---
                        std::string display = "[" + linkName + "]";
                        ImGui::PushStyleColor(ImGuiCol_Text, colors::kBrightGold); // gold
                        ImGui::TextWrapped("%s", display.c_str());
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::SetTooltip("Achievement: %s", linkName.c_str());
                        }
                    }

                    // Shift-click: insert entire link back into chat input
                    if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift) {
                        std::string linkText = text.substr(nextSpecial, linkEnd - nextSpecial);
                        size_t curLen = strlen(chatInputBuffer_);
                        if (curLen + linkText.size() + 1 < sizeof(chatInputBuffer_)) {
                            strncat(chatInputBuffer_, linkText.c_str(), sizeof(chatInputBuffer_) - curLen - 1);
                            chatInputMoveCursorToEnd_ = true;
                        }
                    }

                    pos = linkEnd;
                    continue;
                }

                // Not an item link — treat as colored text: |cAARRGGBB...text...|r
                if (nextSpecial == linkStart && text.size() > linkStart + 10) {
                    ImVec4 cColor = parseWowColor(text, linkStart);
                    size_t textStart = linkStart + 10; // after |cAARRGGBB
                    size_t resetPos2 = text.find("|r", textStart);
                    std::string coloredText;
                    if (resetPos2 != std::string::npos) {
                        coloredText = text.substr(textStart, resetPos2 - textStart);
                        pos = resetPos2 + 2; // skip |r
                    } else {
                        coloredText = text.substr(textStart);
                        pos = text.size();
                    }
                    // Strip any remaining WoW markup from the colored segment
                    // (e.g. |H...|h pairs that aren't item links)
                    std::string clean;
                    for (size_t i = 0; i < coloredText.size(); i++) {
                        if (coloredText[i] == '|' && i + 1 < coloredText.size()) {
                            char next = coloredText[i + 1];
                            if (next == 'H') {
                                // Skip |H...|h
                                size_t hEnd = coloredText.find("|h", i + 2);
                                if (hEnd != std::string::npos) { i = hEnd + 1; continue; }
                            } else if (next == 'h') {
                                i += 1; continue; // skip |h
                            } else if (next == 'r') {
                                i += 1; continue; // skip |r
                            }
                        }
                        clean += coloredText[i];
                    }
                    if (!clean.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, cColor);
                        ImGui::TextWrapped("%s", clean.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0, 0);
                    }
                } else {
                    // Bare |c without enough chars for color — render literally
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextWrapped("|c");
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 0);
                    pos = nextSpecial + 2;
                }
                continue;
            }

            // Handle URL
            if (nextSpecial == urlStart) {
                size_t urlEnd = text.find_first_of(" \t\n\r", urlStart);
                if (urlEnd == std::string::npos) urlEnd = text.size();
                std::string url = text.substr(urlStart, urlEnd - urlStart);

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                ImGui::TextWrapped("%s", url.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Open: %s", url.c_str());
                }
                if (ImGui::IsItemClicked()) {
                    std::string cmd = "xdg-open '" + url + "' &";
                    [[maybe_unused]] int result = system(cmd.c_str());
                }
                ImGui::PopStyleColor();

                pos = urlEnd;
                continue;
            }
        }
    };

    // Determine local player name for mention detection (case-insensitive)
    std::string selfNameLower;
    {
        const auto* ch = gameHandler.getActiveCharacter();
        if (ch && !ch->name.empty()) {
            selfNameLower = ch->name;
            for (auto& c : selfNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    // Scan NEW messages (beyond chatMentionSeenCount_) for mentions and play notification sound
    if (!selfNameLower.empty() && chatHistory.size() > chatMentionSeenCount_) {
        for (size_t mi = chatMentionSeenCount_; mi < chatHistory.size(); ++mi) {
            const auto& mMsg = chatHistory[mi];
            // Skip outgoing whispers, system, and monster messages
            if (mMsg.type == game::ChatType::WHISPER_INFORM ||
                mMsg.type == game::ChatType::SYSTEM) continue;
            // Case-insensitive search in message body
            std::string bodyLower = mMsg.message;
            for (auto& c : bodyLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (bodyLower.find(selfNameLower) != std::string::npos) {
                if (auto* ac = services_.audioCoordinator) {
                    if (auto* ui = ac->getUiSoundManager())
                        ui->playWhisperReceived();
                }
                break; // play at most once per scan pass
            }
        }
        chatMentionSeenCount_ = chatHistory.size();
    } else if (chatHistory.size() <= chatMentionSeenCount_) {
        chatMentionSeenCount_ = chatHistory.size();  // reset if history was cleared
    }

    // Whisper toast scanning left in GameScreen (will move to ToastManager later)

    int chatMsgIdx = 0;
    for (const auto& msg : chatHistory) {
        if (!shouldShowMessage(msg, activeChatTab)) continue;
        std::string processedMessage = replaceGenderPlaceholders(msg.message, gameHandler);

        // Resolve sender name at render time in case it wasn't available at parse time.
        // This handles the race where SMSG_MESSAGECHAT arrives before the entity spawns.
        const std::string& resolvedSenderName = [&]() -> const std::string& {
            if (!msg.senderName.empty()) return msg.senderName;
            if (msg.senderGuid == 0) return msg.senderName;
            const std::string& cached = gameHandler.lookupName(msg.senderGuid);
            if (!cached.empty()) return cached;
            return msg.senderName;
        }();

        ImVec4 color = getChatTypeColor(msg.type);

        // Optional timestamp prefix
        std::string tsPrefix;
        if (chatShowTimestamps) {
            auto tt = std::chrono::system_clock::to_time_t(msg.timestamp);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            char tsBuf[16];
            snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d] ", tm.tm_hour, tm.tm_min);
            tsPrefix = tsBuf;
        }

        // Build chat tag prefix: <GM>, <AFK>, <DND> from chatTag bitmask
        std::string tagPrefix;
        if (msg.chatTag & 0x04) tagPrefix = "<GM> ";
        else if (msg.chatTag & 0x01) tagPrefix = "<AFK> ";
        else if (msg.chatTag & 0x02) tagPrefix = "<DND> ";

        // Build full message string for this entry
        std::string fullMsg;
        if (msg.type == game::ChatType::SYSTEM || msg.type == game::ChatType::TEXT_EMOTE) {
            fullMsg = tsPrefix + processedMessage;
        } else if (!resolvedSenderName.empty()) {
            if (msg.type == game::ChatType::SAY ||
                msg.type == game::ChatType::MONSTER_SAY || msg.type == game::ChatType::MONSTER_PARTY) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " says: " + processedMessage;
            } else if (msg.type == game::ChatType::YELL || msg.type == game::ChatType::MONSTER_YELL) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " yells: " + processedMessage;
            } else if (msg.type == game::ChatType::WHISPER ||
                       msg.type == game::ChatType::MONSTER_WHISPER || msg.type == game::ChatType::RAID_BOSS_WHISPER) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " whispers: " + processedMessage;
            } else if (msg.type == game::ChatType::WHISPER_INFORM) {
                const std::string& target = !msg.receiverName.empty() ? msg.receiverName : resolvedSenderName;
                fullMsg = tsPrefix + "To " + target + ": " + processedMessage;
            } else if (msg.type == game::ChatType::EMOTE ||
                       msg.type == game::ChatType::MONSTER_EMOTE || msg.type == game::ChatType::RAID_BOSS_EMOTE) {
                fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " " + processedMessage;
            } else if (msg.type == game::ChatType::CHANNEL && !msg.channelName.empty()) {
                int chIdx = gameHandler.getChannelIndex(msg.channelName);
                std::string chDisplay = chIdx > 0
                    ? "[" + std::to_string(chIdx) + ". " + msg.channelName + "]"
                    : "[" + msg.channelName + "]";
                fullMsg = tsPrefix + chDisplay + " [" + tagPrefix + resolvedSenderName + "]: " + processedMessage;
            } else {
                fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + tagPrefix + resolvedSenderName + ": " + processedMessage;
            }
        } else {
            bool isGroupType =
                msg.type == game::ChatType::PARTY ||
                msg.type == game::ChatType::GUILD ||
                msg.type == game::ChatType::OFFICER ||
                msg.type == game::ChatType::RAID ||
                msg.type == game::ChatType::RAID_LEADER ||
                msg.type == game::ChatType::RAID_WARNING ||
                msg.type == game::ChatType::BATTLEGROUND ||
                msg.type == game::ChatType::BATTLEGROUND_LEADER;
            if (isGroupType) {
                fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + processedMessage;
            } else {
                fullMsg = tsPrefix + processedMessage;
            }
        }

        // Detect mention: does this message contain the local player's name?
        bool isMention = false;
        if (!selfNameLower.empty() &&
            msg.type != game::ChatType::WHISPER_INFORM &&
            msg.type != game::ChatType::SYSTEM) {
            std::string msgLower = fullMsg;
            for (auto& c : msgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            isMention = (msgLower.find(selfNameLower) != std::string::npos);
        }

        // Render message in a group so we can attach a right-click context menu
        ImGui::PushID(chatMsgIdx++);
        ImGui::BeginGroup();
        renderTextWithLinks(fullMsg, isMention ? ImVec4(1.0f, 0.9f, 0.35f, 1.0f) : color);
        ImGui::EndGroup();
        if (isMention) {
            // Draw highlight AFTER rendering so the rect covers all wrapped lines,
            // not just the first. Previously used a pre-render single-lineH rect.
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            float availW = ImGui::GetContentRegionAvail().x + ImGui::GetCursorScreenPos().x - rMin.x;
            ImGui::GetWindowDrawList()->AddRectFilled(
                rMin, ImVec2(rMin.x + availW, rMax.y),
                IM_COL32(255, 200, 50, 45));  // soft golden tint
        }

        // Right-click context menu (only for player messages with a sender)
        bool isPlayerMsg = !resolvedSenderName.empty() &&
            msg.type != game::ChatType::SYSTEM &&
            msg.type != game::ChatType::TEXT_EMOTE &&
            msg.type != game::ChatType::MONSTER_SAY &&
            msg.type != game::ChatType::MONSTER_YELL &&
            msg.type != game::ChatType::MONSTER_WHISPER &&
            msg.type != game::ChatType::MONSTER_EMOTE &&
            msg.type != game::ChatType::MONSTER_PARTY &&
            msg.type != game::ChatType::RAID_BOSS_WHISPER &&
            msg.type != game::ChatType::RAID_BOSS_EMOTE;

        if (isPlayerMsg && ImGui::BeginPopupContextItem("ChatMsgCtx")) {
            ImGui::TextDisabled("%s", resolvedSenderName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Whisper")) {
                selectedChatType_ = 4; // WHISPER
                strncpy(whisperTargetBuffer_, resolvedSenderName.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                refocusChatInput_ = true;
            }
            if (ImGui::MenuItem("Invite to Group")) {
                gameHandler.inviteToGroup(resolvedSenderName);
            }
            if (ImGui::MenuItem("Add Friend")) {
                gameHandler.addFriend(resolvedSenderName);
            }
            if (ImGui::MenuItem("Ignore")) {
                gameHandler.addIgnore(resolvedSenderName);
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    // Auto-scroll to bottom; track whether user has scrolled up
    {
        float scrollY    = ImGui::GetScrollY();
        float scrollMaxY = ImGui::GetScrollMaxY();
        bool atBottom = (scrollMaxY <= 0.0f) || (scrollY >= scrollMaxY - 2.0f);
        if (atBottom || chatForceScrollToBottom_) {
            ImGui::SetScrollHereY(1.0f);
            chatScrolledUp_ = false;
            chatForceScrollToBottom_ = false;
        } else {
            chatScrolledUp_ = true;
        }
    }

    ImGui::EndChild();

    // Reset font scale after chat history
    ImGui::SetWindowFontScale(1.0f);

    // "Jump to bottom" indicator when scrolled up
    if (chatScrolledUp_) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.2f, 0.35f, 0.7f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f,  0.9f, 1.0f));
        if (ImGui::SmallButton("  v  New messages  ")) {
            chatForceScrollToBottom_ = true;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // Lock toggle
    ImGui::Checkbox("Lock", &chatWindowLocked_);
    ImGui::SameLine();
    ImGui::TextDisabled(chatWindowLocked_ ? "(locked)" : "(movable)");

    // Chat input
    ImGui::Text("Type:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* chatTypes[] = { "SAY", "YELL", "PARTY", "GUILD", "WHISPER", "RAID", "OFFICER", "BATTLEGROUND", "RAID WARNING", "INSTANCE", "CHANNEL" };
    ImGui::Combo("##ChatType", &selectedChatType_, chatTypes, 11);

    // Auto-fill whisper target when switching to WHISPER mode
    if (selectedChatType_ == 4 && lastChatType_ != 4) {
        // Just switched to WHISPER mode
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::PLAYER) {
                auto player = std::static_pointer_cast<game::Player>(target);
                if (!player->getName().empty()) {
                    strncpy(whisperTargetBuffer_, player->getName().c_str(), sizeof(whisperTargetBuffer_) - 1);
                    whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                }
            }
        }
    }
    lastChatType_ = selectedChatType_;

    // Show whisper target field if WHISPER is selected
    if (selectedChatType_ == 4) {
        ImGui::SameLine();
        ImGui::Text("To:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##WhisperTarget", whisperTargetBuffer_, sizeof(whisperTargetBuffer_));
    }

    // Show channel picker if CHANNEL is selected
    if (selectedChatType_ == 10) {
        const auto& channels = gameHandler.getJoinedChannels();
        if (channels.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no channels joined)");
        } else {
            ImGui::SameLine();
            if (selectedChannelIdx_ >= static_cast<int>(channels.size())) selectedChannelIdx_ = 0;
            ImGui::SetNextItemWidth(140);
            if (ImGui::BeginCombo("##ChannelPicker", channels[selectedChannelIdx_].c_str())) {
                for (int ci = 0; ci < static_cast<int>(channels.size()); ++ci) {
                    bool selected = (ci == selectedChannelIdx_);
                    if (ImGui::Selectable(channels[ci].c_str(), selected)) selectedChannelIdx_ = ci;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::SameLine();
    ImGui::Text("Message:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(-1);
    if (refocusChatInput_) {
        ImGui::SetKeyboardFocusHere();
        refocusChatInput_ = false;
    }

    // Detect chat channel prefix as user types and switch the dropdown
    {
        std::string buf(chatInputBuffer_);
        if (buf.size() >= 2 && buf[0] == '/') {
            // Find the command and check if there's a space after it
            size_t sp = buf.find(' ', 1);
            if (sp != std::string::npos) {
                std::string cmd = buf.substr(1, sp - 1);
                for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                int detected = -1;
                bool isReply = false;
                if (cmd == "s" || cmd == "say") detected = 0;
                else if (cmd == "y" || cmd == "yell" || cmd == "shout") detected = 1;
                else if (cmd == "p" || cmd == "party") detected = 2;
                else if (cmd == "g" || cmd == "guild") detected = 3;
                else if (cmd == "w" || cmd == "whisper" || cmd == "tell" || cmd == "t") detected = 4;
                else if (cmd == "r" || cmd == "reply") { detected = 4; isReply = true; }
                else if (cmd == "raid" || cmd == "rsay" || cmd == "ra") detected = 5;
                else if (cmd == "o" || cmd == "officer" || cmd == "osay") detected = 6;
                else if (cmd == "bg" || cmd == "battleground") detected = 7;
                else if (cmd == "rw" || cmd == "raidwarning") detected = 8;
                else if (cmd == "i" || cmd == "instance") detected = 9;
                else if (cmd.size() == 1 && cmd[0] >= '1' && cmd[0] <= '9') detected = 10; // /1, /2 etc.
                if (detected >= 0 && (selectedChatType_ != detected || detected == 10 || isReply)) {
                    // For channel shortcuts, also update selectedChannelIdx_
                    if (detected == 10) {
                        int chanIdx = cmd[0] - '1'; // /1 -> index 0, /2 -> index 1, etc.
                        const auto& chans = gameHandler.getJoinedChannels();
                        if (chanIdx >= 0 && chanIdx < static_cast<int>(chans.size())) {
                            selectedChannelIdx_ = chanIdx;
                        }
                    }
                    selectedChatType_ = detected;
                    // Strip the prefix, keep only the message part
                    std::string remaining = buf.substr(sp + 1);
                    // /r reply: pre-fill whisper target from last whisper sender
                    if (detected == 4 && isReply) {
                        std::string lastSender = gameHandler.getLastWhisperSender();
                        if (!lastSender.empty()) {
                            strncpy(whisperTargetBuffer_, lastSender.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                        }
                        // remaining is the message — don't extract a target from it
                    } else if (detected == 4) {
                        // For whisper, first word after /w is the target
                        size_t msgStart = remaining.find(' ');
                        if (msgStart != std::string::npos) {
                            std::string wTarget = remaining.substr(0, msgStart);
                            strncpy(whisperTargetBuffer_, wTarget.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                            remaining = remaining.substr(msgStart + 1);
                        } else {
                            // Just the target name so far, no message yet
                            strncpy(whisperTargetBuffer_, remaining.c_str(), sizeof(whisperTargetBuffer_) - 1);
                            whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                            remaining = "";
                        }
                    }
                    strncpy(chatInputBuffer_, remaining.c_str(), sizeof(chatInputBuffer_) - 1);
                    chatInputBuffer_[sizeof(chatInputBuffer_) - 1] = '\0';
                    chatInputMoveCursorToEnd_ = true;
                }
            }
        }
    }

    // Color the input text based on current chat type
    ImVec4 inputColor;
    switch (selectedChatType_) {
        case 1: inputColor = kColorRed; break;  // YELL - red
        case 2: inputColor = colors::kLightBlue; break;  // PARTY - blue
        case 3: inputColor = kColorBrightGreen; break;  // GUILD - green
        case 4: inputColor = ImVec4(1.0f, 0.5f, 1.0f, 1.0f); break;  // WHISPER - pink
        case 5: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // RAID - orange
        case 6: inputColor = kColorBrightGreen; break;  // OFFICER - green
        case 7: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // BG - orange
        case 8: inputColor = ImVec4(1.0f, 0.3f, 0.0f, 1.0f); break;  // RAID WARNING - red-orange
        case 9:  inputColor = colors::kLightBlue; break;  // INSTANCE - blue
        case 10: inputColor = ImVec4(0.3f, 0.9f, 0.9f, 1.0f); break; // CHANNEL - cyan
        default: inputColor = ui::colors::kWhite; break; // SAY - white
    }
    ImGui::PushStyleColor(ImGuiCol_Text, inputColor);

    auto inputCallback = [](ImGuiInputTextCallbackData* data) -> int {
        auto* self = static_cast<ChatPanel*>(data->UserData);
        if (!self) return 0;

        // Cursor-to-end after channel switch
        if (self->chatInputMoveCursorToEnd_) {
            int len = static_cast<int>(std::strlen(data->Buf));
            data->CursorPos = len;
            data->SelectionStart = len;
            data->SelectionEnd = len;
            self->chatInputMoveCursorToEnd_ = false;
        }

        // Tab: slash-command autocomplete
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            if (data->BufTextLen > 0 && data->Buf[0] == '/') {
                // Split buffer into command word and trailing args
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                std::string word = (spacePos != std::string::npos) ? fullBuf.substr(0, spacePos) : fullBuf;
                std::string rest = (spacePos != std::string::npos) ? fullBuf.substr(spacePos) : "";

                // Normalize to lowercase for matching
                std::string lowerWord = word;
                for (auto& ch : lowerWord) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

                static const std::vector<std::string> kCmds = {
                    "/afk", "/assist", "/away",
                    "/cancelaura", "/cancelform", "/cancellogout", "/cancelshapeshift",
                    "/cast", "/castsequence", "/chathelp", "/clear", "/clearfocus",
                    "/clearmainassist", "/clearmaintank", "/cleartarget", "/cloak",
                    "/combatlog", "/dance", "/difficulty", "/dismount", "/dnd", "/do", "/duel", "/dump",
                    "/e", "/emote", "/equip", "/equipset", "/exit",
                    "/focus", "/follow", "/forfeit", "/friend",
                    "/g", "/gdemote", "/ginvite", "/gkick", "/gleader", "/gmotd",
                    "/gmticket", "/gpromote", "/gquit", "/grouploot", "/groster",
                    "/guild", "/guildinfo",
                    "/helm", "/help",
                    "/i", "/ignore", "/inspect", "/instance", "/invite",
                    "/j", "/join", "/kick", "/kneel",
                    "/l", "/leave", "/leaveparty", "/loc", "/local", "/logout",
                    "/lootmethod", "/lootthreshold",
                    "/macrohelp", "/mainassist", "/maintank", "/mark", "/me",
                    "/notready",
                    "/p", "/party", "/petaggressive", "/petattack", "/petdefensive",
                    "/petdismiss", "/petfollow", "/pethalt", "/petpassive", "/petstay",
                    "/played", "/pvp",
                    "/quit",
                    "/r", "/raid", "/raidconvert", "/raidinfo", "/raidwarning", "/random", "/ready",
                    "/readycheck", "/reload", "/reloadui", "/removefriend",
                    "/reply", "/rl", "/roll", "/run",
                    "/s", "/say", "/score", "/screenshot", "/script", "/setloot",
                    "/shout", "/sit", "/stand",
                    "/startattack", "/stopattack", "/stopcasting", "/stopfollow", "/stopmacro",
                    "/t", "/target", "/targetenemy", "/targetfriend", "/targetlast",
                    "/threat", "/ticket", "/time", "/trade",
                    "/unignore", "/uninvite", "/unstuck", "/use",
                    "/w", "/whisper", "/who", "/wts", "/wtb",
                    "/y", "/yell", "/zone"
                };

                // New session if prefix changed
                if (self->chatTabMatchIdx_ < 0 || self->chatTabPrefix_ != lowerWord) {
                    self->chatTabPrefix_ = lowerWord;
                    self->chatTabMatches_.clear();
                    for (const auto& cmd : kCmds) {
                        if (cmd.size() >= lowerWord.size() &&
                            cmd.compare(0, lowerWord.size(), lowerWord) == 0)
                            self->chatTabMatches_.push_back(cmd);
                    }
                    self->chatTabMatchIdx_ = 0;
                } else {
                    // Cycle forward through matches
                    ++self->chatTabMatchIdx_;
                    if (self->chatTabMatchIdx_ >= static_cast<int>(self->chatTabMatches_.size()))
                        self->chatTabMatchIdx_ = 0;
                }

                if (!self->chatTabMatches_.empty()) {
                    std::string match = self->chatTabMatches_[self->chatTabMatchIdx_];
                    // Append trailing space when match is unambiguous
                    if (self->chatTabMatches_.size() == 1 && rest.empty())
                        match += ' ';
                    std::string newBuf = match + rest;
                    data->DeleteChars(0, data->BufTextLen);
                    data->InsertChars(0, newBuf.c_str());
                }
            } else if (data->BufTextLen > 0) {
                // Player name tab-completion for commands like /w, /whisper, /invite, /trade, /duel
                // Also works for plain text (completes nearby player names)
                std::string fullBuf(data->Buf, data->BufTextLen);
                size_t spacePos = fullBuf.find(' ');
                bool isNameCommand = false;
                std::string namePrefix;
                size_t replaceStart = 0;

                if (fullBuf[0] == '/' && spacePos != std::string::npos) {
                    std::string cmd = fullBuf.substr(0, spacePos);
                    for (char& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    // Commands that take a player name as the first argument after the command
                    if (cmd == "/w" || cmd == "/whisper" || cmd == "/invite" ||
                        cmd == "/trade" || cmd == "/duel" || cmd == "/follow" ||
                        cmd == "/inspect" || cmd == "/friend" || cmd == "/removefriend" ||
                        cmd == "/ignore" || cmd == "/unignore" || cmd == "/who" ||
                        cmd == "/t" || cmd == "/target" || cmd == "/kick" ||
                        cmd == "/uninvite" || cmd == "/ginvite" || cmd == "/gkick") {
                        // Extract the partial name after the space
                        namePrefix = fullBuf.substr(spacePos + 1);
                        // Only complete the first word after the command
                        size_t nameSpace = namePrefix.find(' ');
                        if (nameSpace == std::string::npos) {
                            isNameCommand = true;
                            replaceStart = spacePos + 1;
                        }
                    }
                }

                if (isNameCommand && !namePrefix.empty()) {
                    std::string lowerPrefix = namePrefix;
                    for (char& c : lowerPrefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    if (self->chatTabMatchIdx_ < 0 || self->chatTabPrefix_ != lowerPrefix) {
                        self->chatTabPrefix_ = lowerPrefix;
                        self->chatTabMatches_.clear();
                        // Search player name cache and nearby entities
                        auto* gh = self->cachedGameHandler_;
                        // Party/raid members
                        for (const auto& m : gh->getPartyData().members) {
                            if (m.name.empty()) continue;
                            std::string lname = m.name;
                            for (char& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0)
                                self->chatTabMatches_.push_back(m.name);
                        }
                        // Friends
                        for (const auto& c : gh->getContacts()) {
                            if (!c.isFriend() || c.name.empty()) continue;
                            std::string lname = c.name;
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                // Avoid duplicates from party
                                bool dup = false;
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == c.name) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.push_back(c.name);
                            }
                        }
                        // Nearby visible players
                        for (const auto& [guid, entity] : gh->getEntityManager().getEntities()) {
                            if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;
                            auto player = std::static_pointer_cast<game::Player>(entity);
                            if (player->getName().empty()) continue;
                            std::string lname = player->getName();
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == player->getName()) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.push_back(player->getName());
                            }
                        }
                        // Last whisper sender
                        if (!gh->getLastWhisperSender().empty()) {
                            std::string lname = gh->getLastWhisperSender();
                            for (char& cc : lname) cc = static_cast<char>(std::tolower(static_cast<unsigned char>(cc)));
                            if (lname.compare(0, lowerPrefix.size(), lowerPrefix) == 0) {
                                bool dup = false;
                                for (const auto& em : self->chatTabMatches_)
                                    if (em == gh->getLastWhisperSender()) { dup = true; break; }
                                if (!dup) self->chatTabMatches_.insert(self->chatTabMatches_.begin(), gh->getLastWhisperSender());
                            }
                        }
                        self->chatTabMatchIdx_ = 0;
                    } else {
                        ++self->chatTabMatchIdx_;
                        if (self->chatTabMatchIdx_ >= static_cast<int>(self->chatTabMatches_.size()))
                            self->chatTabMatchIdx_ = 0;
                    }

                    if (!self->chatTabMatches_.empty()) {
                        std::string match = self->chatTabMatches_[self->chatTabMatchIdx_];
                        std::string prefix = fullBuf.substr(0, replaceStart);
                        std::string newBuf = prefix + match;
                        if (self->chatTabMatches_.size() == 1) newBuf += ' ';
                        data->DeleteChars(0, data->BufTextLen);
                        data->InsertChars(0, newBuf.c_str());
                    }
                }
            }
            return 0;
        }

        // Up/Down arrow: cycle through sent message history
        if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            // Any history navigation resets autocomplete
            self->chatTabMatchIdx_ = -1;
            self->chatTabMatches_.clear();

            const int histSize = static_cast<int>(self->chatSentHistory_.size());
            if (histSize == 0) return 0;

            if (data->EventKey == ImGuiKey_UpArrow) {
                // Go back in history
                if (self->chatHistoryIdx_ == -1)
                    self->chatHistoryIdx_ = histSize - 1;
                else if (self->chatHistoryIdx_ > 0)
                    --self->chatHistoryIdx_;
            } else if (data->EventKey == ImGuiKey_DownArrow) {
                if (self->chatHistoryIdx_ == -1) return 0;
                ++self->chatHistoryIdx_;
                if (self->chatHistoryIdx_ >= histSize) {
                    self->chatHistoryIdx_ = -1;
                    data->DeleteChars(0, data->BufTextLen);
                    return 0;
                }
            }

            if (self->chatHistoryIdx_ >= 0 && self->chatHistoryIdx_ < histSize) {
                const std::string& entry = self->chatSentHistory_[self->chatHistoryIdx_];
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, entry.c_str());
            }
        }
        return 0;
    };

    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_CallbackAlways |
                                     ImGuiInputTextFlags_CallbackHistory |
                                     ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputText("##ChatInput", chatInputBuffer_, sizeof(chatInputBuffer_), inputFlags, inputCallback, this)) {
        sendChatMessage(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        // Close chat input on send so movement keys work immediately.
        refocusChatInput_ = false;
        ImGui::ClearActiveID();
    }
    ImGui::PopStyleColor();

    if (ImGui::IsItemActive()) {
        chatInputActive_ = true;
    } else {
        chatInputActive_ = false;
    }

    // Click in chat history area (received messages) → focus input.
    {
        if (chatHistoryHovered && ImGui::IsMouseClicked(0)) {
            refocusChatInput_ = true;
        }
    }

    ImGui::End();
}


// Collect all non-comment, non-empty lines from a macro body.
static std::vector<std::string> allMacroCommands(const std::string& macroText) {
    std::vector<std::string> cmds;
    size_t pos = 0;
    while (pos <= macroText.size()) {
        size_t nl = macroText.find('\n', pos);
        std::string line = (nl != std::string::npos) ? macroText.substr(pos, nl - pos) : macroText.substr(pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start != std::string::npos) line = line.substr(start);
        if (!line.empty() && line.front() != '#')
            cmds.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return cmds;
}

// ---------------------------------------------------------------------------
// WoW macro conditional evaluator
// Parses:  [cond1,cond2] Spell1; [cond3] Spell2; DefaultSpell
// Returns the first matching alternative's argument, or "" if none matches.
// targetOverride is set to a specific GUID if [target=X] was in the conditions,
// or left as UINT64_MAX to mean "use the normal target".
// ---------------------------------------------------------------------------
static std::string evaluateMacroConditionals(const std::string& rawArg,
                                              game::GameHandler& gameHandler,
                                              uint64_t& targetOverride) {
    targetOverride = static_cast<uint64_t>(-1);

    auto& input = core::Input::getInstance();

    const bool shiftHeld = input.isKeyPressed(SDL_SCANCODE_LSHIFT) ||
                           input.isKeyPressed(SDL_SCANCODE_RSHIFT);
    const bool ctrlHeld  = input.isKeyPressed(SDL_SCANCODE_LCTRL)  ||
                           input.isKeyPressed(SDL_SCANCODE_RCTRL);
    const bool altHeld   = input.isKeyPressed(SDL_SCANCODE_LALT)   ||
                           input.isKeyPressed(SDL_SCANCODE_RALT);
    const bool anyMod    = shiftHeld || ctrlHeld || altHeld;

    // Split rawArg on ';' → alternatives
    std::vector<std::string> alts;
    {
        std::string cur;
        for (char c : rawArg) {
            if (c == ';') { alts.push_back(cur); cur.clear(); }
            else            cur += c;
        }
        alts.push_back(cur);
    }

    // Evaluate a single comma-separated condition token.
    // tgt is updated if a target= or @ specifier is found.
    auto evalCond = [&](const std::string& raw, uint64_t& tgt) -> bool {
        std::string c = raw;
        // trim
        size_t s = c.find_first_not_of(" \t"); if (s) c = (s != std::string::npos) ? c.substr(s) : "";
        size_t e = c.find_last_not_of(" \t");  if (e != std::string::npos) c.resize(e + 1);
        if (c.empty()) return true;

        // @target specifiers: @player, @focus, @pet, @mouseover, @target
        if (!c.empty() && c[0] == '@') {
            std::string spec = c.substr(1);
            if (spec == "player")          tgt = gameHandler.getPlayerGuid();
            else if (spec == "focus")      tgt = gameHandler.getFocusGuid();
            else if (spec == "target")     tgt = gameHandler.getTargetGuid();
            else if (spec == "pet") {
                uint64_t pg = gameHandler.getPetGuid();
                if (pg != 0) tgt = pg;
                else return false;  // no pet — skip this alternative
            }
            else if (spec == "mouseover") {
                uint64_t mo = gameHandler.getMouseoverGuid();
                if (mo != 0) tgt = mo;
                else return false;  // no mouseover — skip this alternative
            }
            return true;
        }
        // target=X specifiers
        if (c.rfind("target=", 0) == 0) {
            std::string spec = c.substr(7);
            if (spec == "player")          tgt = gameHandler.getPlayerGuid();
            else if (spec == "focus")      tgt = gameHandler.getFocusGuid();
            else if (spec == "target")     tgt = gameHandler.getTargetGuid();
            else if (spec == "pet") {
                uint64_t pg = gameHandler.getPetGuid();
                if (pg != 0) tgt = pg;
                else return false;  // no pet — skip this alternative
            }
            else if (spec == "mouseover") {
                uint64_t mo = gameHandler.getMouseoverGuid();
                if (mo != 0) tgt = mo;
                else return false;  // no mouseover — skip this alternative
            }
            return true;
        }

        // mod / nomod
        if (c == "nomod" || c == "mod:none") return !anyMod;
        if (c.rfind("mod:", 0) == 0) {
            std::string mods = c.substr(4);
            bool ok = true;
            if (mods.find("shift") != std::string::npos && !shiftHeld) ok = false;
            if (mods.find("ctrl")  != std::string::npos && !ctrlHeld)  ok = false;
            if (mods.find("alt")   != std::string::npos && !altHeld)   ok = false;
            return ok;
        }

        // combat / nocombat
        if (c == "combat")   return gameHandler.isInCombat();
        if (c == "nocombat") return !gameHandler.isInCombat();

        // Helper to get the effective target entity
        auto effTarget = [&]() -> std::shared_ptr<game::Entity> {
            if (tgt != static_cast<uint64_t>(-1) && tgt != 0)
                return gameHandler.getEntityManager().getEntity(tgt);
            return gameHandler.getTarget();
        };

        // exists / noexists
        if (c == "exists")   return effTarget() != nullptr;
        if (c == "noexists") return effTarget() == nullptr;

        // dead / nodead
        if (c == "dead")   {
            auto t = effTarget();
            auto u = t ? std::dynamic_pointer_cast<game::Unit>(t) : nullptr;
            return u && u->getHealth() == 0;
        }
        if (c == "nodead") {
            auto t = effTarget();
            auto u = t ? std::dynamic_pointer_cast<game::Unit>(t) : nullptr;
            return u && u->getHealth() > 0;
        }

        // help (friendly) / harm (hostile) and their no- variants
        auto unitHostile = [&](const std::shared_ptr<game::Entity>& t) -> bool {
            if (!t) return false;
            auto u = std::dynamic_pointer_cast<game::Unit>(t);
            return u && gameHandler.isHostileFactionPublic(u->getFactionTemplate());
        };
        if (c == "harm" || c == "nohelp") { return unitHostile(effTarget()); }
        if (c == "help" || c == "noharm") { return !unitHostile(effTarget()); }

        // mounted / nomounted
        if (c == "mounted")   return gameHandler.isMounted();
        if (c == "nomounted") return !gameHandler.isMounted();

        // swimming / noswimming
        if (c == "swimming")   return gameHandler.isSwimming();
        if (c == "noswimming") return !gameHandler.isSwimming();

        // flying / noflying (CAN_FLY + FLYING flags active)
        if (c == "flying")   return gameHandler.isPlayerFlying();
        if (c == "noflying") return !gameHandler.isPlayerFlying();

        // channeling / nochanneling
        if (c == "channeling")   return gameHandler.isCasting() && gameHandler.isChanneling();
        if (c == "nochanneling") return !(gameHandler.isCasting() && gameHandler.isChanneling());

        // stealthed / nostealthed (unit flag 0x02000000 = UNIT_FLAG_SNEAKING)
        auto isStealthedFn = [&]() -> bool {
            auto pe = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
            if (!pe) return false;
            auto pu = std::dynamic_pointer_cast<game::Unit>(pe);
            return pu && (pu->getUnitFlags() & 0x02000000u) != 0;
        };
        if (c == "stealthed")   return isStealthedFn();
        if (c == "nostealthed") return !isStealthedFn();

        // pet / nopet — player has an active pet (hunters, warlocks, DKs)
        if (c == "pet")   return gameHandler.hasPet();
        if (c == "nopet") return !gameHandler.hasPet();

        // indoors / outdoors — WMO interior detection (affects mount type selection)
        if (c == "indoors" || c == "nooutdoors") {
            auto* r = core::Application::getInstance().getRenderer();
            return r && r->isPlayerIndoors();
        }
        if (c == "outdoors" || c == "noindoors") {
            auto* r = core::Application::getInstance().getRenderer();
            return !r || !r->isPlayerIndoors();
        }

        // group / nogroup — player is in a party or raid
        if (c == "group" || c == "party") return gameHandler.isInGroup();
        if (c == "nogroup")               return !gameHandler.isInGroup();

        // raid / noraid — player is in a raid group (groupType == 1)
        if (c == "raid") return gameHandler.isInGroup() && gameHandler.getPartyData().groupType == 1;
        if (c == "noraid") return !gameHandler.isInGroup() || gameHandler.getPartyData().groupType != 1;

        // spec:N — active talent spec (1-based: spec:1 = primary, spec:2 = secondary)
        if (c.rfind("spec:", 0) == 0) {
            uint8_t wantSpec = 0;
            try { wantSpec = static_cast<uint8_t>(std::stoul(c.substr(5))); } catch (...) {}
            return wantSpec > 0 && gameHandler.getActiveTalentSpec() == (wantSpec - 1);
        }

        // noform / nostance — player is NOT in a shapeshift/stance
        if (c == "noform" || c == "nostance") {
            for (const auto& a : gameHandler.getPlayerAuras())
                if (!a.isEmpty() && a.maxDurationMs == -1) return false;
            return true;
        }
        // form:0 same as noform
        if (c == "form:0" || c == "stance:0") {
            for (const auto& a : gameHandler.getPlayerAuras())
                if (!a.isEmpty() && a.maxDurationMs == -1) return false;
            return true;
        }

        // buff:SpellName / nobuff:SpellName — check if the effective target (or player
        // if no target specified) has a buff with the given name.
        // debuff:SpellName / nodebuff:SpellName — same for debuffs (harmful auras).
        auto checkAuraByName = [&](const std::string& spellName, bool wantDebuff,
                                   bool negate) -> bool {
            // Determine which aura list to check: effective target or player
            const std::vector<game::AuraSlot>* auras = nullptr;
            if (tgt != static_cast<uint64_t>(-1) && tgt != 0 && tgt != gameHandler.getPlayerGuid()) {
                // Check target's auras
                auras = &gameHandler.getTargetAuras();
            } else {
                auras = &gameHandler.getPlayerAuras();
            }
            std::string nameLow = spellName;
            for (char& ch : nameLow) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (const auto& a : *auras) {
                if (a.isEmpty() || a.spellId == 0) continue;
                // Filter: debuffs have the HARMFUL flag (0x80) or spell has a dispel type
                bool isDebuff = (a.flags & 0x80) != 0;
                if (wantDebuff ? !isDebuff : isDebuff) continue;
                std::string sn = gameHandler.getSpellName(a.spellId);
                for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (sn == nameLow) return !negate;
            }
            return negate;
        };
        if (c.rfind("buff:", 0) == 0 && c.size() > 5)
            return checkAuraByName(c.substr(5), false, false);
        if (c.rfind("nobuff:", 0) == 0 && c.size() > 7)
            return checkAuraByName(c.substr(7), false, true);
        if (c.rfind("debuff:", 0) == 0 && c.size() > 7)
            return checkAuraByName(c.substr(7), true, false);
        if (c.rfind("nodebuff:", 0) == 0 && c.size() > 9)
            return checkAuraByName(c.substr(9), true, true);

        // mounted / nomounted
        if (c == "mounted")   return gameHandler.isMounted();
        if (c == "nomounted") return !gameHandler.isMounted();

        // group (any group) / nogroup / raid
        if (c == "group")  return !gameHandler.getPartyData().isEmpty();
        if (c == "nogroup") return gameHandler.getPartyData().isEmpty();
        if (c == "raid")   {
            const auto& pd = gameHandler.getPartyData();
            return pd.groupType >= 1;  // groupType 1 = raid, 0 = party
        }

        // channeling:SpellName — player is currently channeling that spell
        if (c.rfind("channeling:", 0) == 0 && c.size() > 11) {
            if (!gameHandler.isChanneling()) return false;
            std::string want = c.substr(11);
            for (char& ch : want) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            uint32_t castSpellId = gameHandler.getCurrentCastSpellId();
            std::string sn = gameHandler.getSpellName(castSpellId);
            for (char& ch : sn) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return sn == want;
        }
        if (c == "channeling") return gameHandler.isChanneling();
        if (c == "nochanneling") return !gameHandler.isChanneling();

        // casting (any active cast or channel)
        if (c == "casting")   return gameHandler.isCasting();
        if (c == "nocasting") return !gameHandler.isCasting();

        // vehicle / novehicle (WotLK)
        if (c == "vehicle")   return gameHandler.getVehicleId() != 0;
        if (c == "novehicle") return gameHandler.getVehicleId() == 0;

        // Unknown → permissive (don't block)
        return true;
    };

    for (auto& alt : alts) {
        // trim
        size_t fs = alt.find_first_not_of(" \t");
        if (fs == std::string::npos) continue;
        alt = alt.substr(fs);
        size_t ls = alt.find_last_not_of(" \t");
        if (ls != std::string::npos) alt.resize(ls + 1);

        if (!alt.empty() && alt[0] == '[') {
            size_t close = alt.find(']');
            if (close == std::string::npos) continue;
            std::string condStr  = alt.substr(1, close - 1);
            std::string argPart  = alt.substr(close + 1);
            // Trim argPart
            size_t as = argPart.find_first_not_of(" \t");
            argPart = (as != std::string::npos) ? argPart.substr(as) : "";

            // Evaluate comma-separated conditions
            uint64_t tgt = static_cast<uint64_t>(-1);
            bool pass = true;
            size_t cp = 0;
            while (pass) {
                size_t comma = condStr.find(',', cp);
                std::string tok = condStr.substr(cp, comma == std::string::npos ? std::string::npos : comma - cp);
                if (!evalCond(tok, tgt)) { pass = false; break; }
                if (comma == std::string::npos) break;
                cp = comma + 1;
            }
            if (pass) {
                if (tgt != static_cast<uint64_t>(-1)) targetOverride = tgt;
                return argPart;
            }
        } else {
            // No condition block — default fallback always matches
            return alt;
        }
    }
    return {};
}

// Execute all non-comment lines of a macro body in sequence.
// In WoW, every line executes per click; the server enforces spell-cast limits.
// /stopmacro (with optional conditionals) halts the remaining commands early.

void ChatPanel::executeMacroText(game::GameHandler& gameHandler,
                                  InventoryScreen& inventoryScreen,
                                  SpellbookScreen& spellbookScreen,
                                  QuestLogScreen& questLogScreen,
                                  const std::string& macroText) {
    macroStopped_ = false;
    for (const auto& cmd : allMacroCommands(macroText)) {
        strncpy(chatInputBuffer_, cmd.c_str(), sizeof(chatInputBuffer_) - 1);
        chatInputBuffer_[sizeof(chatInputBuffer_) - 1] = '\0';
        sendChatMessage(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        if (macroStopped_) break;
    }
    macroStopped_ = false;
}

// /castsequence persistent state — shared across all macros using the same spell list.
// Keyed by the normalized (lowercase, comma-joined) spell sequence string.
namespace {
struct CastSeqState {
    size_t   index = 0;
    float    lastPressSec = 0.0f;
    uint64_t lastTargetGuid = 0;
    bool     lastInCombat = false;
};
std::unordered_map<std::string, CastSeqState> s_castSeqStates;
}  // namespace


void ChatPanel::sendChatMessage(game::GameHandler& gameHandler,
                                 InventoryScreen& /*inventoryScreen*/,
                                 SpellbookScreen& /*spellbookScreen*/,
                                 QuestLogScreen& /*questLogScreen*/) {
    if (strlen(chatInputBuffer_) > 0) {
        std::string input(chatInputBuffer_);

        // Save to sent-message history (skip pure whitespace, cap at 50 entries)
        {
            bool allSpace = true;
            for (char c : input) { if (!std::isspace(static_cast<unsigned char>(c))) { allSpace = false; break; } }
            if (!allSpace) {
                // Remove duplicate of last entry if identical
                if (chatSentHistory_.empty() || chatSentHistory_.back() != input) {
                    chatSentHistory_.push_back(input);
                    if (chatSentHistory_.size() > 50)
                        chatSentHistory_.erase(chatSentHistory_.begin());
                }
            }
        }
        chatHistoryIdx_ = -1;  // reset browsing position after send

        game::ChatType type = game::ChatType::SAY;
        std::string message = input;
        std::string target;

        // Track if a channel shortcut should change the chat type dropdown
        int switchChatType = -1;

        // Check for slash commands
        if (input.size() > 1 && input[0] == '/') {
            std::string command = input.substr(1);
            size_t spacePos = command.find(' ');
            std::string cmd = (spacePos != std::string::npos) ? command.substr(0, spacePos) : command;

            // Convert command to lowercase for comparison
            std::string cmdLower = cmd;
            for (char& c : cmdLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // /run <lua code> — execute Lua script via addon system
            if ((cmdLower == "run" || cmdLower == "script") && spacePos != std::string::npos) {
                std::string luaCode = command.substr(spacePos + 1);
                auto* am = services_.addonManager;
                if (am) {
                    am->runScript(luaCode);
                } else {
                    gameHandler.addUIError("Addon system not initialized.");
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /dump <expression> — evaluate Lua expression and print result
            if ((cmdLower == "dump" || cmdLower == "print") && spacePos != std::string::npos) {
                std::string expr = command.substr(spacePos + 1);
                auto* am = services_.addonManager;
                if (am && am->isInitialized()) {
                    // Wrap expression in print(tostring(...)) to display the value
                    std::string wrapped = "local __v = " + expr +
                        "; if type(__v) == 'table' then "
                        "  local parts = {} "
                        "  for k,v in pairs(__v) do parts[#parts+1] = tostring(k)..'='..tostring(v) end "
                        "  print('{' .. table.concat(parts, ', ') .. '}') "
                        "else print(tostring(__v)) end";
                    am->runScript(wrapped);
                } else {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "Addon system not initialized.";
                    gameHandler.addLocalChatMessage(errMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Check addon slash commands (SlashCmdList) before built-in commands
            {
                auto* am = services_.addonManager;
                if (am && am->isInitialized()) {
                    std::string slashCmd = "/" + cmdLower;
                    std::string slashArgs;
                    if (spacePos != std::string::npos) slashArgs = command.substr(spacePos + 1);
                    if (am->getLuaEngine()->dispatchSlashCommand(slashCmd, slashArgs)) {
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
            }

            // Special commands
            if (cmdLower == "logout") {
                core::Application::getInstance().logoutToLogin();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clear") {
                gameHandler.clearChatHistory();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /reload or /reloadui — reload all addons (save variables, re-init Lua, re-scan .toc files)
            if (cmdLower == "reload" || cmdLower == "reloadui" || cmdLower == "rl") {
                auto* am = services_.addonManager;
                if (am) {
                    am->reload();
                    am->fireEvent("VARIABLES_LOADED");
                    am->fireEvent("PLAYER_LOGIN");
                    am->fireEvent("PLAYER_ENTERING_WORLD");
                    game::MessageChatData rlMsg;
                    rlMsg.type = game::ChatType::SYSTEM;
                    rlMsg.language = game::ChatLanguage::UNIVERSAL;
                    rlMsg.message = "Interface reloaded.";
                    gameHandler.addLocalChatMessage(rlMsg);
                } else {
                    game::MessageChatData rlMsg;
                    rlMsg.type = game::ChatType::SYSTEM;
                    rlMsg.language = game::ChatLanguage::UNIVERSAL;
                    rlMsg.message = "Addon system not available.";
                    gameHandler.addLocalChatMessage(rlMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stopmacro [conditions]
            // Halts execution of the current macro (remaining lines are skipped).
            // With a condition block, only stops if the conditions evaluate to true.
            //   /stopmacro            → always stops
            //   /stopmacro [combat]   → stops only while in combat
            //   /stopmacro [nocombat] → stops only when not in combat
            if (cmdLower == "stopmacro") {
                bool shouldStop = true;
                if (spacePos != std::string::npos) {
                    std::string condArg = command.substr(spacePos + 1);
                    while (!condArg.empty() && condArg.front() == ' ') condArg.erase(condArg.begin());
                    if (!condArg.empty() && condArg.front() == '[') {
                        // Append a sentinel action so evaluateMacroConditionals can signal a match.
                        uint64_t tgtOver = static_cast<uint64_t>(-1);
                        std::string hit = evaluateMacroConditionals(condArg + " __stop__", gameHandler, tgtOver);
                        shouldStop = !hit.empty();
                    }
                }
                if (shouldStop) macroStopped_ = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /invite command
            if (cmdLower == "invite" && spacePos != std::string::npos) {
                std::string targetName = command.substr(spacePos + 1);
                gameHandler.inviteToGroup(targetName);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /inspect command
            if (cmdLower == "inspect") {
                gameHandler.inspectTarget();
                slashCmds_.showInspect = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /threat command
            if (cmdLower == "threat") {
                slashCmds_.toggleThreat = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /score command — BG scoreboard
            if (cmdLower == "score") {
                gameHandler.requestPvpLog();
                slashCmds_.showBgScore = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /time command
            if (cmdLower == "time") {
                gameHandler.queryServerTime();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /loc command — print player coordinates and zone name
            if (cmdLower == "loc" || cmdLower == "coords" || cmdLower == "whereami") {
                const auto& pmi = gameHandler.getMovementInfo();
                std::string zoneName;
                if (auto* rend = services_.renderer)
                    zoneName = rend->getCurrentZoneName();
                char buf[256];
                snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f%s%s",
                         pmi.x, pmi.y, pmi.z,
                         zoneName.empty() ? "" : " — ",
                         zoneName.c_str());
                game::MessageChatData sysMsg;
                sysMsg.type = game::ChatType::SYSTEM;
                sysMsg.language = game::ChatLanguage::UNIVERSAL;
                sysMsg.message = buf;
                gameHandler.addLocalChatMessage(sysMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /screenshot command — capture current frame to PNG
            if (cmdLower == "screenshot" || cmdLower == "ss") {
                slashCmds_.takeScreenshot = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /zone command — print current zone name
            if (cmdLower == "zone") {
                std::string zoneName;
                if (auto* rend = services_.renderer)
                    zoneName = rend->getCurrentZoneName();
                game::MessageChatData sysMsg;
                sysMsg.type = game::ChatType::SYSTEM;
                sysMsg.language = game::ChatLanguage::UNIVERSAL;
                sysMsg.message = zoneName.empty() ? "You are not in a known zone." : "You are in: " + zoneName;
                gameHandler.addLocalChatMessage(sysMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /played command
            if (cmdLower == "played") {
                gameHandler.requestPlayedTime();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ticket command — open GM ticket window
            if (cmdLower == "ticket" || cmdLower == "gmticket" || cmdLower == "gm") {
                slashCmds_.showGmTicket = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /chathelp command — list chat-channel slash commands
            if (cmdLower == "chathelp") {
                static constexpr const char* kChatHelp[] = {
                    "--- Chat Channel Commands ---",
                    "/s [msg]          Say to nearby players",
                    "/y [msg]          Yell to a wider area",
                    "/w <name> [msg]   Whisper to player",
                    "/r [msg]          Reply to last whisper",
                    "/p [msg]          Party chat",
                    "/g [msg]          Guild chat",
                    "/o [msg]          Guild officer chat",
                    "/raid [msg]       Raid chat",
                    "/rw [msg]         Raid warning",
                    "/bg [msg]         Battleground chat",
                    "/1 [msg]          General channel",
                    "/2 [msg]          Trade channel  (also /wts /wtb)",
                    "/<N> [msg]        Channel by number",
                    "/join <chan>      Join a channel",
                    "/leave <chan>     Leave a channel",
                    "/afk [msg]        Set AFK status",
                    "/dnd [msg]        Set Do Not Disturb",
                };
                for (const char* line : kChatHelp) {
                    game::MessageChatData helpMsg;
                    helpMsg.type = game::ChatType::SYSTEM;
                    helpMsg.language = game::ChatLanguage::UNIVERSAL;
                    helpMsg.message = line;
                    gameHandler.addLocalChatMessage(helpMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /macrohelp command — list available macro conditionals
            if (cmdLower == "macrohelp") {
                static constexpr const char* kMacroHelp[] = {
                    "--- Macro Conditionals ---",
                    "Usage: /cast [cond1,cond2] Spell1; [cond3] Spell2; Default",
                    "State:   [combat] [mounted] [swimming] [flying] [stealthed]",
                    "         [channeling] [pet] [group] [raid] [indoors] [outdoors]",
                    "Spec:    [spec:1] [spec:2]  (active talent spec, 1-based)",
                    "         (prefix no- to negate any condition)",
                    "Target:  [harm] [help] [exists] [noexists] [dead] [nodead]",
                    "         [target=focus] [target=pet] [target=mouseover] [target=player]",
                    "         (also: @focus, @pet, @mouseover, @player, @target)",
                    "Form:    [noform] [nostance] [form:0]",
                    "Keys:    [mod:shift] [mod:ctrl] [mod:alt]",
                    "Aura:    [buff:Name] [nobuff:Name] [debuff:Name] [nodebuff:Name]",
                    "Other:   #showtooltip, /stopmacro [cond], /castsequence",
                };
                for (const char* line : kMacroHelp) {
                    game::MessageChatData m;
                    m.type = game::ChatType::SYSTEM;
                    m.language = game::ChatLanguage::UNIVERSAL;
                    m.message = line;
                    gameHandler.addLocalChatMessage(m);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /help command — list available slash commands
            if (cmdLower == "help" || cmdLower == "?") {
                static constexpr const char* kHelpLines[] = {
                    "--- Wowee Slash Commands ---",
                    "Chat: /s /y /p /g /raid /rw /o /bg /w <name> /r  /join /leave",
                    "Social: /who  /friend add/remove  /ignore  /unignore",
                    "Party: /invite  /uninvite  /leave  /readycheck  /mark  /roll",
                    "       /maintank  /mainassist  /raidconvert  /raidinfo",
                    "       /lootmethod  /lootthreshold",
                    "Guild: /ginvite  /gkick  /gquit  /gpromote  /gdemote  /gmotd",
                    "       /gleader  /groster  /ginfo  /gcreate  /gdisband",
                    "Combat: /cast  /castsequence  /use  /startattack  /stopattack",
                    "        /stopcasting  /duel  /forfeit  /pvp  /assist",
                    "        /follow  /stopfollow  /threat  /combatlog",
                    "Items: /use <item>  /equip <item>  /equipset [name]",
                    "Target: /target  /cleartarget  /focus  /clearfocus  /inspect",
                    "Movement: /sit  /stand  /kneel  /dismount",
                    "Misc: /played  /time  /zone  /loc  /afk  /dnd  /helm  /cloak",
                    "      /trade  /score  /unstuck  /logout  /quit  /exit  /ticket",
                    "      /screenshot  /difficulty",
                    "      /macrohelp  /chathelp  /help",
                };
                for (const char* line : kHelpLines) {
                    game::MessageChatData helpMsg;
                    helpMsg.type = game::ChatType::SYSTEM;
                    helpMsg.language = game::ChatLanguage::UNIVERSAL;
                    helpMsg.message = line;
                    gameHandler.addLocalChatMessage(helpMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /who commands
            if (cmdLower == "who" || cmdLower == "whois" || cmdLower == "online" || cmdLower == "players") {
                std::string query;
                if (spacePos != std::string::npos) {
                    query = command.substr(spacePos + 1);
                    // Trim leading/trailing whitespace
                    size_t first = query.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) {
                        query.clear();
                    } else {
                        size_t last = query.find_last_not_of(" \t\r\n");
                        query = query.substr(first, last - first + 1);
                    }
                }

                if ((cmdLower == "whois") && query.empty()) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /whois <playerName>";
                    gameHandler.addLocalChatMessage(msg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                if (cmdLower == "who" && (query == "help" || query == "?")) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Who commands: /who [name/filter], /whois <name>, /online";
                    gameHandler.addLocalChatMessage(msg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                gameHandler.queryWho(query);
                slashCmds_.showWho = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /combatlog command
            if (cmdLower == "combatlog" || cmdLower == "cl") {
                slashCmds_.toggleCombatLog = true;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /roll command
            if (cmdLower == "roll" || cmdLower == "random" || cmdLower == "rnd") {
                uint32_t minRoll = 1;
                uint32_t maxRoll = 100;

                if (spacePos != std::string::npos) {
                    std::string args = command.substr(spacePos + 1);
                    size_t dashPos = args.find('-');
                    size_t spacePos2 = args.find(' ');

                    if (dashPos != std::string::npos) {
                        // Format: /roll 1-100
                        try {
                            minRoll = std::stoul(args.substr(0, dashPos));
                            maxRoll = std::stoul(args.substr(dashPos + 1));
                        } catch (...) {}
                    } else if (spacePos2 != std::string::npos) {
                        // Format: /roll 1 100
                        try {
                            minRoll = std::stoul(args.substr(0, spacePos2));
                            maxRoll = std::stoul(args.substr(spacePos2 + 1));
                        } catch (...) {}
                    } else {
                        // Format: /roll 100 (means 1-100)
                        try {
                            maxRoll = std::stoul(args);
                        } catch (...) {}
                    }
                }

                gameHandler.randomRoll(minRoll, maxRoll);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /friend or /addfriend command
            if (cmdLower == "friend" || cmdLower == "addfriend") {
                if (spacePos != std::string::npos) {
                    std::string args = command.substr(spacePos + 1);
                    size_t subCmdSpace = args.find(' ');

                    if (cmdLower == "friend" && subCmdSpace != std::string::npos) {
                        std::string subCmd = args.substr(0, subCmdSpace);
                        std::transform(subCmd.begin(), subCmd.end(), subCmd.begin(), ::tolower);

                        if (subCmd == "add") {
                            std::string playerName = args.substr(subCmdSpace + 1);
                            gameHandler.addFriend(playerName);
                            chatInputBuffer_[0] = '\0';
                            return;
                        } else if (subCmd == "remove" || subCmd == "delete" || subCmd == "rem") {
                            std::string playerName = args.substr(subCmdSpace + 1);
                            gameHandler.removeFriend(playerName);
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                    } else {
                        // /addfriend name or /friend name (assume add)
                        gameHandler.addFriend(args);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /friend add <name> or /friend remove <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /removefriend or /delfriend command
            if (cmdLower == "removefriend" || cmdLower == "delfriend" || cmdLower == "remfriend") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeFriend(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /removefriend <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ignore command
            if (cmdLower == "ignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.addIgnore(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /unignore command
            if (cmdLower == "unignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeIgnore(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /unignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /dismount command
            if (cmdLower == "dismount") {
                gameHandler.dismount();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Pet control commands (common macro use)
            // Action IDs: 1=passive, 2=follow, 3=stay, 4=defensive, 5=attack, 6=aggressive
            if (cmdLower == "petattack") {
                uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                gameHandler.sendPetAction(5, target);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petfollow") {
                gameHandler.sendPetAction(2, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petstay" || cmdLower == "pethalt") {
                gameHandler.sendPetAction(3, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petpassive") {
                gameHandler.sendPetAction(1, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petdefensive") {
                gameHandler.sendPetAction(4, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petaggressive") {
                gameHandler.sendPetAction(6, 0);
                chatInputBuffer_[0] = '\0';
                return;
            }
            if (cmdLower == "petdismiss") {
                gameHandler.dismissPet();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancelform / /cancelshapeshift — leave current shapeshift/stance
            if (cmdLower == "cancelform" || cmdLower == "cancelshapeshift") {
                // Cancel the first permanent shapeshift aura the player has
                for (const auto& aura : gameHandler.getPlayerAuras()) {
                    if (aura.spellId == 0) continue;
                    // Permanent shapeshift auras have the permanent flag (0x20) set
                    if (aura.flags & 0x20) {
                        gameHandler.cancelAura(aura.spellId);
                        break;
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancelaura <spell name|#id> — cancel a specific buff by name or ID
            if (cmdLower == "cancelaura" && spacePos != std::string::npos) {
                std::string auraArg = command.substr(spacePos + 1);
                while (!auraArg.empty() && auraArg.front() == ' ') auraArg.erase(auraArg.begin());
                while (!auraArg.empty() && auraArg.back()  == ' ') auraArg.pop_back();
                // Try numeric ID first
                {
                    std::string numStr = auraArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNum = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNum) {
                        uint32_t spellId = 0;
                        try { spellId = static_cast<uint32_t>(std::stoul(numStr)); } catch (...) {}
                        if (spellId) gameHandler.cancelAura(spellId);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
                // Name match against player auras
                std::string argLow = auraArg;
                for (char& c : argLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const auto& aura : gameHandler.getPlayerAuras()) {
                    if (aura.spellId == 0) continue;
                    std::string sn = gameHandler.getSpellName(aura.spellId);
                    for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (sn == argLow) {
                        gameHandler.cancelAura(aura.spellId);
                        break;
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /sit command
            if (cmdLower == "sit") {
                gameHandler.setStandState(1);  // 1 = sit
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stand command
            if (cmdLower == "stand") {
                gameHandler.setStandState(0);  // 0 = stand
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /kneel command
            if (cmdLower == "kneel") {
                gameHandler.setStandState(8);  // 8 = kneel
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /logout command (also /camp, /quit, /exit)
            if (cmdLower == "logout" || cmdLower == "camp" || cmdLower == "quit" || cmdLower == "exit") {
                gameHandler.requestLogout();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cancellogout command
            if (cmdLower == "cancellogout") {
                gameHandler.cancelLogout();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /difficulty command — set dungeon/raid difficulty (WotLK)
            if (cmdLower == "difficulty") {
                std::string arg;
                if (spacePos != std::string::npos) {
                    arg = command.substr(spacePos + 1);
                    // Trim whitespace
                    size_t first = arg.find_first_not_of(" \t");
                    size_t last  = arg.find_last_not_of(" \t");
                    if (first != std::string::npos)
                        arg = arg.substr(first, last - first + 1);
                    else
                        arg.clear();
                    for (auto& ch : arg) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }

                uint32_t diff = 0;
                bool valid = true;
                if (arg == "normal" || arg == "0")         diff = 0;
                else if (arg == "heroic" || arg == "1")    diff = 1;
                else if (arg == "25" || arg == "25normal" || arg == "25man" || arg == "2")
                    diff = 2;
                else if (arg == "25heroic" || arg == "25manheroic" || arg == "3")
                    diff = 3;
                else valid = false;

                if (!valid || arg.empty()) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /difficulty normal|heroic|25|25heroic  (0-3)";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    static constexpr const char* kDiffNames[] = {
                        "Normal (5-man)", "Heroic (5-man)", "Normal (25-man)", "Heroic (25-man)"
                    };
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Setting difficulty to: ") + kDiffNames[diff];
                    gameHandler.addLocalChatMessage(msg);
                    gameHandler.sendSetDifficulty(diff);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /helm command
            if (cmdLower == "helm" || cmdLower == "helmet" || cmdLower == "showhelm") {
                gameHandler.toggleHelm();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /cloak command
            if (cmdLower == "cloak" || cmdLower == "showcloak") {
                gameHandler.toggleCloak();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /follow command
            if (cmdLower == "follow" || cmdLower == "f") {
                gameHandler.followTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /stopfollow command
            if (cmdLower == "stopfollow") {
                gameHandler.cancelFollow();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /assist command
            if (cmdLower == "assist") {
                // /assist              → assist current target (use their target)
                // /assist PlayerName   → find PlayerName, target their target
                // /assist [target=X]   → evaluate conditional, target that entity's target
                auto assistEntityTarget = [&](uint64_t srcGuid) {
                    auto srcEnt = gameHandler.getEntityManager().getEntity(srcGuid);
                    if (!srcEnt) { gameHandler.assistTarget(); return; }
                    uint64_t atkGuid = 0;
                    const auto& flds = srcEnt->getFields();
                    auto iLo = flds.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
                    if (iLo != flds.end()) {
                        atkGuid = iLo->second;
                        auto iHi = flds.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                        if (iHi != flds.end()) atkGuid |= (static_cast<uint64_t>(iHi->second) << 32);
                    }
                    if (atkGuid != 0) {
                        gameHandler.setTarget(atkGuid);
                    } else {
                        std::string sn = getEntityName(srcEnt);
                        game::MessageChatData msg;
                        msg.type = game::ChatType::SYSTEM;
                        msg.language = game::ChatLanguage::UNIVERSAL;
                        msg.message = (sn.empty() ? "Target" : sn) + " has no target.";
                        gameHandler.addLocalChatMessage(msg);
                    }
                };

                if (spacePos != std::string::npos) {
                    std::string assistArg = command.substr(spacePos + 1);
                    while (!assistArg.empty() && assistArg.front() == ' ') assistArg.erase(assistArg.begin());

                    // Evaluate conditionals if present
                    uint64_t assistOver = static_cast<uint64_t>(-1);
                    if (!assistArg.empty() && assistArg.front() == '[') {
                        assistArg = evaluateMacroConditionals(assistArg, gameHandler, assistOver);
                        if (assistArg.empty() && assistOver == static_cast<uint64_t>(-1)) {
                            chatInputBuffer_[0] = '\0'; return;  // no condition matched
                        }
                        while (!assistArg.empty() && assistArg.front() == ' ') assistArg.erase(assistArg.begin());
                        while (!assistArg.empty() && assistArg.back()  == ' ') assistArg.pop_back();
                    }

                    if (assistOver != static_cast<uint64_t>(-1) && assistOver != 0) {
                        assistEntityTarget(assistOver);
                    } else if (!assistArg.empty()) {
                        // Name search
                        std::string argLow = assistArg;
                        for (char& c : argLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        uint64_t bestGuid = 0; float bestDist = std::numeric_limits<float>::max();
                        const auto& pmi = gameHandler.getMovementInfo();
                        for (const auto& [guid, ent] : gameHandler.getEntityManager().getEntities()) {
                            if (!ent || ent->getType() == game::ObjectType::OBJECT) continue;
                            std::string nm = getEntityName(ent);
                            std::string nml = nm;
                            for (char& c : nml) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (nml.find(argLow) != 0) continue;
                            float d2 = (ent->getX()-pmi.x)*(ent->getX()-pmi.x)
                                     + (ent->getY()-pmi.y)*(ent->getY()-pmi.y);
                            if (d2 < bestDist) { bestDist = d2; bestGuid = guid; }
                        }
                        if (bestGuid) assistEntityTarget(bestGuid);
                        else {
                            game::MessageChatData msg;
                            msg.type = game::ChatType::SYSTEM;
                            msg.language = game::ChatLanguage::UNIVERSAL;
                            msg.message = "No unit matching '" + assistArg + "' found.";
                            gameHandler.addLocalChatMessage(msg);
                        }
                    } else {
                        gameHandler.assistTarget();
                    }
                } else {
                    gameHandler.assistTarget();
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /pvp command
            if (cmdLower == "pvp") {
                gameHandler.togglePvp();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ginfo command
            if (cmdLower == "ginfo" || cmdLower == "guildinfo") {
                gameHandler.requestGuildInfo();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /groster command
            if (cmdLower == "groster" || cmdLower == "guildroster") {
                gameHandler.requestGuildRoster();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gmotd command
            if (cmdLower == "gmotd" || cmdLower == "guildmotd") {
                if (spacePos != std::string::npos) {
                    std::string motd = command.substr(spacePos + 1);
                    gameHandler.setGuildMotd(motd);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gmotd <message>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gpromote command
            if (cmdLower == "gpromote" || cmdLower == "guildpromote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.promoteGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gpromote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gdemote command
            if (cmdLower == "gdemote" || cmdLower == "guilddemote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.demoteGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gdemote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gquit command
            if (cmdLower == "gquit" || cmdLower == "guildquit" || cmdLower == "leaveguild") {
                gameHandler.leaveGuild();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ginvite command
            if (cmdLower == "ginvite" || cmdLower == "guildinvite") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.inviteToGuild(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ginvite <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gkick command
            if (cmdLower == "gkick" || cmdLower == "guildkick") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.kickGuildMember(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gkick <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gcreate command
            if (cmdLower == "gcreate" || cmdLower == "guildcreate") {
                if (spacePos != std::string::npos) {
                    std::string guildName = command.substr(spacePos + 1);
                    gameHandler.createGuild(guildName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gcreate <guild name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gdisband command
            if (cmdLower == "gdisband" || cmdLower == "guilddisband") {
                gameHandler.disbandGuild();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /gleader command
            if (cmdLower == "gleader" || cmdLower == "guildleader") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.setGuildLeader(playerName);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gleader <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /readycheck command
            if (cmdLower == "readycheck" || cmdLower == "rc") {
                gameHandler.initiateReadyCheck();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /ready command (respond yes to ready check)
            if (cmdLower == "ready") {
                gameHandler.respondToReadyCheck(true);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /notready command (respond no to ready check)
            if (cmdLower == "notready" || cmdLower == "nr") {
                gameHandler.respondToReadyCheck(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /yield or /forfeit command
            if (cmdLower == "yield" || cmdLower == "forfeit" || cmdLower == "surrender") {
                gameHandler.forfeitDuel();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // AFK command
            if (cmdLower == "afk" || cmdLower == "away") {
                std::string afkMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleAfk(afkMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // DND command
            if (cmdLower == "dnd" || cmdLower == "busy") {
                std::string dndMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleDnd(dndMsg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Reply command
            if (cmdLower == "r" || cmdLower == "reply") {
                std::string lastSender = gameHandler.getLastWhisperSender();
                if (lastSender.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "No one has whispered you yet.";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                // Set whisper target to last whisper sender
                strncpy(whisperTargetBuffer_, lastSender.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                if (spacePos != std::string::npos) {
                    // /r message — send reply immediately
                    std::string replyMsg = command.substr(spacePos + 1);
                    gameHandler.sendChatMessage(game::ChatType::WHISPER, replyMsg, lastSender);
                }
                // Switch to whisper tab
                selectedChatType_ = 4;
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Party/Raid management commands
            if (cmdLower == "uninvite" || cmdLower == "kick") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.uninvitePlayer(playerName);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Usage: /uninvite <player name>";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "leave" || cmdLower == "leaveparty") {
                gameHandler.leaveParty();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "maintank" || cmdLower == "mt") {
                if (gameHandler.hasTarget()) {
                    gameHandler.setMainTank(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to set as main tank.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "mainassist" || cmdLower == "ma") {
                if (gameHandler.hasTarget()) {
                    gameHandler.setMainAssist(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to set as main assist.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearmaintank") {
                gameHandler.clearMainTank();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearmainassist") {
                gameHandler.clearMainAssist();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "raidinfo") {
                gameHandler.requestRaidInfo();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "raidconvert") {
                gameHandler.convertToRaid();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /lootmethod (or /grouploot, /setloot) — set party/raid loot method
            if (cmdLower == "lootmethod" || cmdLower == "grouploot" || cmdLower == "setloot") {
                if (!gameHandler.isInGroup()) {
                    gameHandler.addUIError("You are not in a group.");
                } else if (spacePos == std::string::npos) {
                    // No argument — show current method and usage
                    static constexpr const char* kMethodNames[] = {
                        "Free for All", "Round Robin", "Master Looter", "Group Loot", "Need Before Greed"
                    };
                    const auto& pd = gameHandler.getPartyData();
                    const char* cur = (pd.lootMethod < 5) ? kMethodNames[pd.lootMethod] : "Unknown";
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Current loot method: ") + cur;
                    gameHandler.addLocalChatMessage(msg);
                    msg.message = "Usage: /lootmethod ffa|roundrobin|master|group|needbeforegreed";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    std::string arg = command.substr(spacePos + 1);
                    // Lowercase the argument
                    for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    uint32_t method = 0xFFFFFFFF;
                    if (arg == "ffa" || arg == "freeforall")         method = 0;
                    else if (arg == "roundrobin" || arg == "rr")     method = 1;
                    else if (arg == "master" || arg == "masterloot") method = 2;
                    else if (arg == "group" || arg == "grouploot")   method = 3;
                    else if (arg == "needbeforegreed" || arg == "nbg" || arg == "need") method = 4;

                    if (method == 0xFFFFFFFF) {
                        gameHandler.addUIError("Unknown loot method. Use: ffa, roundrobin, master, group, needbeforegreed");
                    } else {
                        const auto& pd = gameHandler.getPartyData();
                        // Master loot uses player guid as master looter; otherwise 0
                        uint64_t masterGuid = (method == 2) ? gameHandler.getPlayerGuid() : 0;
                        gameHandler.sendSetLootMethod(method, pd.lootThreshold, masterGuid);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /lootthreshold — set minimum item quality for group loot rolls
            if (cmdLower == "lootthreshold") {
                if (!gameHandler.isInGroup()) {
                    gameHandler.addUIError("You are not in a group.");
                } else if (spacePos == std::string::npos) {
                    const auto& pd = gameHandler.getPartyData();
                    static constexpr const char* kQualityNames[] = {
                        "Poor (grey)", "Common (white)", "Uncommon (green)",
                        "Rare (blue)", "Epic (purple)", "Legendary (orange)"
                    };
                    const char* cur = (pd.lootThreshold < 6) ? kQualityNames[pd.lootThreshold] : "Unknown";
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = std::string("Current loot threshold: ") + cur;
                    gameHandler.addLocalChatMessage(msg);
                    msg.message = "Usage: /lootthreshold <0-5> (0=Poor, 1=Common, 2=Uncommon, 3=Rare, 4=Epic, 5=Legendary)";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    std::string arg = command.substr(spacePos + 1);
                    // Trim whitespace
                    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
                    uint32_t threshold = 0xFFFFFFFF;
                    if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '5') {
                        threshold = static_cast<uint32_t>(arg[0] - '0');
                    } else {
                        // Accept quality names
                        for (auto& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (arg == "poor" || arg == "grey" || arg == "gray") threshold = 0;
                        else if (arg == "common" || arg == "white")          threshold = 1;
                        else if (arg == "uncommon" || arg == "green")        threshold = 2;
                        else if (arg == "rare" || arg == "blue")             threshold = 3;
                        else if (arg == "epic" || arg == "purple")           threshold = 4;
                        else if (arg == "legendary" || arg == "orange")      threshold = 5;
                    }

                    if (threshold == 0xFFFFFFFF) {
                        gameHandler.addUIError("Invalid threshold. Use 0-5 or: poor, common, uncommon, rare, epic, legendary");
                    } else {
                        const auto& pd = gameHandler.getPartyData();
                        uint64_t masterGuid = (pd.lootMethod == 2) ? gameHandler.getPlayerGuid() : 0;
                        gameHandler.sendSetLootMethod(pd.lootMethod, threshold, masterGuid);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /mark [icon] — set or clear a raid target mark on the current target.
            // Icon names (case-insensitive): star, circle, diamond, triangle, moon, square, cross, skull
            // /mark clear | /mark 0 — remove all marks (sets icon 0xFF = clear)
            // /mark — no arg marks with skull (icon 7)
            if (cmdLower == "mark" || cmdLower == "marktarget" || cmdLower == "raidtarget") {
                if (!gameHandler.hasTarget()) {
                    game::MessageChatData noTgt;
                    noTgt.type = game::ChatType::SYSTEM;
                    noTgt.language = game::ChatLanguage::UNIVERSAL;
                    noTgt.message = "No target selected.";
                    gameHandler.addLocalChatMessage(noTgt);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                static constexpr const char* kMarkWords[] = {
                    "star", "circle", "diamond", "triangle", "moon", "square", "cross", "skull"
                };
                uint8_t icon = 7; // default: skull
                if (spacePos != std::string::npos) {
                    std::string arg = command.substr(spacePos + 1);
                    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
                    std::string argLow = arg;
                    for (auto& c : argLow) c = static_cast<char>(std::tolower(c));
                    if (argLow == "clear" || argLow == "0" || argLow == "none") {
                        gameHandler.setRaidMark(gameHandler.getTargetGuid(), 0xFF);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                    bool found = false;
                    for (int mi = 0; mi < 8; ++mi) {
                        if (argLow == kMarkWords[mi]) { icon = static_cast<uint8_t>(mi); found = true; break; }
                    }
                    if (!found && !argLow.empty() && argLow[0] >= '1' && argLow[0] <= '8') {
                        icon = static_cast<uint8_t>(argLow[0] - '1');
                        found = true;
                    }
                    if (!found) {
                        game::MessageChatData badArg;
                        badArg.type = game::ChatType::SYSTEM;
                        badArg.language = game::ChatLanguage::UNIVERSAL;
                        badArg.message = "Unknown mark. Use: star circle diamond triangle moon square cross skull";
                        gameHandler.addLocalChatMessage(badArg);
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }
                gameHandler.setRaidMark(gameHandler.getTargetGuid(), icon);
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Combat and Trade commands
            if (cmdLower == "duel") {
                if (gameHandler.hasTarget()) {
                    gameHandler.proposeDuel(gameHandler.getTargetGuid());
                } else if (spacePos != std::string::npos) {
                    // Target player by name (would need name-to-GUID lookup)
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to challenge to a duel.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to challenge to a duel.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "trade") {
                if (gameHandler.hasTarget()) {
                    gameHandler.initiateTrade(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a player to trade with.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "startattack") {
                // Support macro conditionals: /startattack [harm,nodead]
                bool condPass = true;
                uint64_t saOverride = static_cast<uint64_t>(-1);
                if (spacePos != std::string::npos) {
                    std::string saArg = command.substr(spacePos + 1);
                    while (!saArg.empty() && saArg.front() == ' ') saArg.erase(saArg.begin());
                    if (!saArg.empty() && saArg.front() == '[') {
                        std::string result = evaluateMacroConditionals(saArg, gameHandler, saOverride);
                        condPass = !(result.empty() && saOverride == static_cast<uint64_t>(-1));
                    }
                }
                if (condPass) {
                    uint64_t atkTarget = (saOverride != static_cast<uint64_t>(-1) && saOverride != 0)
                        ? saOverride : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                    if (atkTarget != 0) {
                        gameHandler.startAutoAttack(atkTarget);
                    } else {
                        game::MessageChatData msg;
                        msg.type = game::ChatType::SYSTEM;
                        msg.language = game::ChatLanguage::UNIVERSAL;
                        msg.message = "You have no target.";
                        gameHandler.addLocalChatMessage(msg);
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "stopattack") {
                gameHandler.stopAutoAttack();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "stopcasting") {
                gameHandler.stopCasting();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "cancelqueuedspell" || cmdLower == "stopspellqueue") {
                gameHandler.cancelQueuedSpell();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /equipset [name] — equip a saved equipment set by name (partial match, case-insensitive)
            // /equipset          — list available sets in chat
            if (cmdLower == "equipset") {
                const auto& sets = gameHandler.getEquipmentSets();
                auto sysSay = [&](const std::string& msg) {
                    game::MessageChatData m;
                    m.type = game::ChatType::SYSTEM;
                    m.language = game::ChatLanguage::UNIVERSAL;
                    m.message = msg;
                    gameHandler.addLocalChatMessage(m);
                };
                if (spacePos == std::string::npos) {
                    // No argument: list available sets
                    if (sets.empty()) {
                        sysSay("[System] No equipment sets saved.");
                    } else {
                        sysSay("[System] Equipment sets:");
                        for (const auto& es : sets)
                            sysSay("  " + es.name);
                    }
                } else {
                    std::string setName = command.substr(spacePos + 1);
                    while (!setName.empty() && setName.front() == ' ') setName.erase(setName.begin());
                    while (!setName.empty() && setName.back()  == ' ') setName.pop_back();
                    // Case-insensitive prefix match
                    std::string setLower = setName;
                    std::transform(setLower.begin(), setLower.end(), setLower.begin(), ::tolower);
                    const game::GameHandler::EquipmentSetInfo* found = nullptr;
                    for (const auto& es : sets) {
                        std::string nameLow = es.name;
                        std::transform(nameLow.begin(), nameLow.end(), nameLow.begin(), ::tolower);
                        if (nameLow == setLower || nameLow.find(setLower) == 0) {
                            found = &es;
                            break;
                        }
                    }
                    if (found) {
                        gameHandler.useEquipmentSet(found->setId);
                    } else {
                        sysSay("[System] No equipment set matching '" + setName + "'.");
                    }
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /castsequence [conds] [reset=N/target/combat] Spell1, Spell2, ...
            // Cycles through the spell list on successive presses; resets per the reset= spec.
            if (cmdLower == "castsequence" && spacePos != std::string::npos) {
                std::string seqArg = command.substr(spacePos + 1);
                while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());

                // Macro conditionals
                uint64_t seqTgtOver = static_cast<uint64_t>(-1);
                if (!seqArg.empty() && seqArg.front() == '[') {
                    seqArg = evaluateMacroConditionals(seqArg, gameHandler, seqTgtOver);
                    if (seqArg.empty() && seqTgtOver == static_cast<uint64_t>(-1)) {
                        chatInputBuffer_[0] = '\0'; return;
                    }
                    while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());
                    while (!seqArg.empty() && seqArg.back()  == ' ') seqArg.pop_back();
                }

                // Optional reset= spec (may contain slash-separated conditions: reset=5/target)
                std::string resetSpec;
                if (seqArg.rfind("reset=", 0) == 0) {
                    size_t spAfter = seqArg.find(' ');
                    if (spAfter != std::string::npos) {
                        resetSpec = seqArg.substr(6, spAfter - 6);
                        seqArg = seqArg.substr(spAfter + 1);
                        while (!seqArg.empty() && seqArg.front() == ' ') seqArg.erase(seqArg.begin());
                    }
                }

                // Parse comma-separated spell list
                std::vector<std::string> seqSpells;
                {
                    std::string cur;
                    for (char c : seqArg) {
                        if (c == ',') {
                            while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                            while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                            if (!cur.empty()) seqSpells.push_back(cur);
                            cur.clear();
                        } else { cur += c; }
                    }
                    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                    while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                    if (!cur.empty()) seqSpells.push_back(cur);
                }
                if (seqSpells.empty()) { chatInputBuffer_[0] = '\0'; return; }

                // Build stable key from lowercase spell list
                std::string seqKey;
                for (size_t k = 0; k < seqSpells.size(); ++k) {
                    if (k) seqKey += ',';
                    std::string sl = seqSpells[k];
                    for (char& c : sl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    seqKey += sl;
                }

                auto& seqState = s_castSeqStates[seqKey];

                // Check reset conditions (slash-separated: e.g. "5/target")
                float nowSec = static_cast<float>(ImGui::GetTime());
                bool shouldReset = false;
                if (!resetSpec.empty()) {
                    size_t rpos = 0;
                    while (rpos <= resetSpec.size()) {
                        size_t slash = resetSpec.find('/', rpos);
                        std::string part = (slash != std::string::npos)
                            ? resetSpec.substr(rpos, slash - rpos)
                            : resetSpec.substr(rpos);
                        std::string plow = part;
                        for (char& c : plow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        bool isNum = !plow.empty() && std::all_of(plow.begin(), plow.end(),
                            [](unsigned char c){ return std::isdigit(c) || c == '.'; });
                        if (isNum) {
                            float rSec = 0.0f;
                            try { rSec = std::stof(plow); } catch (...) {}
                            if (rSec > 0.0f && nowSec - seqState.lastPressSec > rSec) shouldReset = true;
                        } else if (plow == "target") {
                            if (gameHandler.getTargetGuid() != seqState.lastTargetGuid) shouldReset = true;
                        } else if (plow == "combat") {
                            if (gameHandler.isInCombat() != seqState.lastInCombat) shouldReset = true;
                        }
                        if (slash == std::string::npos) break;
                        rpos = slash + 1;
                    }
                }
                if (shouldReset || seqState.index >= seqSpells.size()) seqState.index = 0;

                const std::string& seqSpell = seqSpells[seqState.index];
                seqState.index = (seqState.index + 1) % seqSpells.size();
                seqState.lastPressSec  = nowSec;
                seqState.lastTargetGuid = gameHandler.getTargetGuid();
                seqState.lastInCombat   = gameHandler.isInCombat();

                // Cast the selected spell — mirrors /cast spell lookup
                std::string ssLow = seqSpell;
                for (char& c : ssLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!ssLow.empty() && ssLow.front() == '!') ssLow.erase(ssLow.begin());

                uint64_t seqTargetGuid = (seqTgtOver != static_cast<uint64_t>(-1) && seqTgtOver != 0)
                    ? seqTgtOver : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);

                // Numeric ID
                if (!ssLow.empty() && ssLow.front() == '#') ssLow.erase(ssLow.begin());
                bool ssNumeric = !ssLow.empty() && std::all_of(ssLow.begin(), ssLow.end(),
                    [](unsigned char c){ return std::isdigit(c); });
                if (ssNumeric) {
                    uint32_t ssId = 0;
                    try { ssId = static_cast<uint32_t>(std::stoul(ssLow)); } catch (...) {}
                    if (ssId) gameHandler.castSpell(ssId, seqTargetGuid);
                } else {
                    uint32_t ssBest = 0; int ssBestRank = -1;
                    for (uint32_t sid : gameHandler.getKnownSpells()) {
                        const std::string& sn = gameHandler.getSpellName(sid);
                        if (sn.empty()) continue;
                        std::string snl = sn;
                        for (char& c : snl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (snl != ssLow) continue;
                        int sRnk = 0;
                        const std::string& rk = gameHandler.getSpellRank(sid);
                        if (!rk.empty()) {
                            std::string rkl = rk;
                            for (char& c : rkl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (rkl.rfind("rank ", 0) == 0) { try { sRnk = std::stoi(rkl.substr(5)); } catch (...) {} }
                        }
                        if (sRnk > ssBestRank) { ssBestRank = sRnk; ssBest = sid; }
                    }
                    if (ssBest) gameHandler.castSpell(ssBest, seqTargetGuid);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "cast" && spacePos != std::string::npos) {
                std::string spellArg = command.substr(spacePos + 1);
                // Trim leading/trailing whitespace
                while (!spellArg.empty() && spellArg.front() == ' ') spellArg.erase(spellArg.begin());
                while (!spellArg.empty() && spellArg.back()  == ' ') spellArg.pop_back();

                // Evaluate WoW macro conditionals: /cast [mod:shift] Greater Heal; Flash Heal
                uint64_t castTargetOverride = static_cast<uint64_t>(-1);
                if (!spellArg.empty() && spellArg.front() == '[') {
                    spellArg = evaluateMacroConditionals(spellArg, gameHandler, castTargetOverride);
                    if (spellArg.empty()) {
                        chatInputBuffer_[0] = '\0';
                        return;  // No conditional matched — skip cast
                    }
                    while (!spellArg.empty() && spellArg.front() == ' ') spellArg.erase(spellArg.begin());
                    while (!spellArg.empty() && spellArg.back()  == ' ') spellArg.pop_back();
                }

                // Strip leading '!' (WoW /cast !Spell forces recast without toggling off)
                if (!spellArg.empty() && spellArg.front() == '!') spellArg.erase(spellArg.begin());

                // Support numeric spell ID: /cast 133 or /cast #133
                {
                    std::string numStr = spellArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNumeric = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNumeric) {
                        uint32_t spellId = 0;
                        try { spellId = static_cast<uint32_t>(std::stoul(numStr)); } catch (...) {}
                        if (spellId != 0) {
                            uint64_t targetGuid = (castTargetOverride != static_cast<uint64_t>(-1))
                                ? castTargetOverride
                                : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                            gameHandler.castSpell(spellId, targetGuid);
                        }
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                // Parse optional "(Rank N)" suffix: "Fireball(Rank 3)" or "Fireball (Rank 3)"
                int requestedRank = -1;  // -1 = highest rank
                std::string spellName = spellArg;
                {
                    auto rankPos = spellArg.find('(');
                    if (rankPos != std::string::npos) {
                        std::string rankStr = spellArg.substr(rankPos + 1);
                        // Strip closing paren and whitespace
                        auto closePos = rankStr.find(')');
                        if (closePos != std::string::npos) rankStr = rankStr.substr(0, closePos);
                        for (char& c : rankStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        // Expect "rank N"
                        if (rankStr.rfind("rank ", 0) == 0) {
                            try { requestedRank = std::stoi(rankStr.substr(5)); } catch (...) {}
                        }
                        spellName = spellArg.substr(0, rankPos);
                        while (!spellName.empty() && spellName.back() == ' ') spellName.pop_back();
                    }
                }

                std::string spellNameLower = spellName;
                for (char& c : spellNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                // Search known spells for a name match; pick highest rank (or specific rank)
                uint32_t bestSpellId = 0;
                int bestRank = -1;
                for (uint32_t sid : gameHandler.getKnownSpells()) {
                    const std::string& sName = gameHandler.getSpellName(sid);
                    if (sName.empty()) continue;
                    std::string sNameLower = sName;
                    for (char& c : sNameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (sNameLower != spellNameLower) continue;

                    // Parse numeric rank from rank string ("Rank 3" → 3, "" → 0)
                    int sRank = 0;
                    const std::string& rankStr = gameHandler.getSpellRank(sid);
                    if (!rankStr.empty()) {
                        std::string rLow = rankStr;
                        for (char& c : rLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (rLow.rfind("rank ", 0) == 0) {
                            try { sRank = std::stoi(rLow.substr(5)); } catch (...) {}
                        }
                    }

                    if (requestedRank >= 0) {
                        if (sRank == requestedRank) { bestSpellId = sid; break; }
                    } else {
                        if (sRank > bestRank) { bestRank = sRank; bestSpellId = sid; }
                    }
                }

                if (bestSpellId) {
                    uint64_t targetGuid = (castTargetOverride != static_cast<uint64_t>(-1))
                        ? castTargetOverride
                        : (gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0);
                    gameHandler.castSpell(bestSpellId, targetGuid);
                } else {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = requestedRank >= 0
                        ? "You don't know '" + spellName + "' (Rank " + std::to_string(requestedRank) + ")."
                        : "Unknown spell: '" + spellName + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /use <item name|#id|bag slot|equip slot>
            // Supports: item name, numeric item ID (#N or N), bag/slot (/use 0 1 = backpack slot 1,
            //           /use 1-4 slot = bag slot), equipment slot number (/use 16 = main hand)
            if (cmdLower == "use" && spacePos != std::string::npos) {
                std::string useArg = command.substr(spacePos + 1);
                while (!useArg.empty() && useArg.front() == ' ') useArg.erase(useArg.begin());
                while (!useArg.empty() && useArg.back()  == ' ') useArg.pop_back();

                // Handle macro conditionals: /use [mod:shift] ItemName; OtherItem
                if (!useArg.empty() && useArg.front() == '[') {
                    uint64_t dummy = static_cast<uint64_t>(-1);
                    useArg = evaluateMacroConditionals(useArg, gameHandler, dummy);
                    if (useArg.empty()) { chatInputBuffer_[0] = '\0'; return; }
                    while (!useArg.empty() && useArg.front() == ' ') useArg.erase(useArg.begin());
                    while (!useArg.empty() && useArg.back()  == ' ') useArg.pop_back();
                }

                // Check for bag/slot notation: two numbers separated by whitespace
                {
                    std::istringstream iss(useArg);
                    int bagNum = -1, slotNum = -1;
                    iss >> bagNum >> slotNum;
                    if (!iss.fail() && slotNum >= 1) {
                        if (bagNum == 0) {
                            // Backpack: bag=0, slot 1-based → 0-based
                            gameHandler.useItemBySlot(slotNum - 1);
                            chatInputBuffer_[0] = '\0';
                            return;
                        } else if (bagNum >= 1 && bagNum <= game::Inventory::NUM_BAG_SLOTS) {
                            // Equip bag: bags are 1-indexed (bag 1 = bagIndex 0)
                            gameHandler.useItemInBag(bagNum - 1, slotNum - 1);
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                    }
                }

                // Numeric equip slot: /use 16 = slot 16 (1-based, WoW equip slot enum)
                {
                    std::string numStr = useArg;
                    if (!numStr.empty() && numStr.front() == '#') numStr.erase(numStr.begin());
                    bool isNumeric = !numStr.empty() &&
                        std::all_of(numStr.begin(), numStr.end(),
                                    [](unsigned char c){ return std::isdigit(c); });
                    if (isNumeric) {
                        // Treat as equip slot (1-based, maps to EquipSlot enum 0-based)
                        int slotNum = 0;
                        try { slotNum = std::stoi(numStr); } catch (...) {}
                        if (slotNum >= 1 && slotNum <= static_cast<int>(game::EquipSlot::BAG4) + 1) {
                            auto eslot = static_cast<game::EquipSlot>(slotNum - 1);
                            const auto& esl = gameHandler.getInventory().getEquipSlot(eslot);
                            if (!esl.empty())
                                gameHandler.useItemById(esl.item.itemId);
                        }
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                }

                std::string useArgLower = useArg;
                for (char& c : useArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                bool found = false;
                const auto& inv = gameHandler.getInventory();
                // Search backpack
                for (int s = 0; s < inv.getBackpackSize() && !found; ++s) {
                    const auto& slot = inv.getBackpackSlot(s);
                    if (slot.empty()) continue;
                    const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                    if (!info) continue;
                    std::string nameLow = info->name;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLow == useArgLower) {
                        gameHandler.useItemBySlot(s);
                        found = true;
                    }
                }
                // Search bags
                for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS && !found; ++b) {
                    for (int s = 0; s < inv.getBagSize(b) && !found; ++s) {
                        const auto& slot = inv.getBagSlot(b, s);
                        if (slot.empty()) continue;
                        const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                        if (!info) continue;
                        std::string nameLow = info->name;
                        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (nameLow == useArgLower) {
                            gameHandler.useItemInBag(b, s);
                            found = true;
                        }
                    }
                }
                if (!found) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "Item not found: '" + useArg + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /equip <item name> — auto-equip an item from backpack/bags by name
            if (cmdLower == "equip" && spacePos != std::string::npos) {
                std::string equipArg = command.substr(spacePos + 1);
                while (!equipArg.empty() && equipArg.front() == ' ') equipArg.erase(equipArg.begin());
                while (!equipArg.empty() && equipArg.back()  == ' ') equipArg.pop_back();
                std::string equipArgLower = equipArg;
                for (char& c : equipArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                bool found = false;
                const auto& inv = gameHandler.getInventory();
                // Search backpack
                for (int s = 0; s < inv.getBackpackSize() && !found; ++s) {
                    const auto& slot = inv.getBackpackSlot(s);
                    if (slot.empty()) continue;
                    const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                    if (!info) continue;
                    std::string nameLow = info->name;
                    for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLow == equipArgLower) {
                        gameHandler.autoEquipItemBySlot(s);
                        found = true;
                    }
                }
                // Search bags
                for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS && !found; ++b) {
                    for (int s = 0; s < inv.getBagSize(b) && !found; ++s) {
                        const auto& slot = inv.getBagSlot(b, s);
                        if (slot.empty()) continue;
                        const auto* info = gameHandler.getItemInfo(slot.item.itemId);
                        if (!info) continue;
                        std::string nameLow = info->name;
                        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (nameLow == equipArgLower) {
                            gameHandler.autoEquipItemInBag(b, s);
                            found = true;
                        }
                    }
                }
                if (!found) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "Item not found: '" + equipArg + "'.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Targeting commands
            if (cmdLower == "cleartarget") {
                // Support macro conditionals: /cleartarget [dead] clears only if target is dead
                bool ctCondPass = true;
                if (spacePos != std::string::npos) {
                    std::string ctArg = command.substr(spacePos + 1);
                    while (!ctArg.empty() && ctArg.front() == ' ') ctArg.erase(ctArg.begin());
                    if (!ctArg.empty() && ctArg.front() == '[') {
                        uint64_t ctOver = static_cast<uint64_t>(-1);
                        std::string res = evaluateMacroConditionals(ctArg, gameHandler, ctOver);
                        ctCondPass = !(res.empty() && ctOver == static_cast<uint64_t>(-1));
                    }
                }
                if (ctCondPass) gameHandler.clearTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "target" && spacePos != std::string::npos) {
                // Search visible entities for name match (case-insensitive prefix).
                // Among all matches, pick the nearest living unit to the player.
                // Supports WoW macro conditionals: /target [target=mouseover]; /target [mod:shift] Boss
                std::string targetArg = command.substr(spacePos + 1);

                // Evaluate conditionals if present
                uint64_t targetCmdOverride = static_cast<uint64_t>(-1);
                if (!targetArg.empty() && targetArg.front() == '[') {
                    targetArg = evaluateMacroConditionals(targetArg, gameHandler, targetCmdOverride);
                    if (targetArg.empty() && targetCmdOverride == static_cast<uint64_t>(-1)) {
                        // No condition matched — silently skip (macro fallthrough)
                        chatInputBuffer_[0] = '\0';
                        return;
                    }
                    while (!targetArg.empty() && targetArg.front() == ' ') targetArg.erase(targetArg.begin());
                    while (!targetArg.empty() && targetArg.back()  == ' ') targetArg.pop_back();
                }

                // If conditionals resolved to a specific GUID, target it directly
                if (targetCmdOverride != static_cast<uint64_t>(-1) && targetCmdOverride != 0) {
                    gameHandler.setTarget(targetCmdOverride);
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                // If no name remains (bare conditional like [target=mouseover] with 0 guid), skip silently
                if (targetArg.empty()) {
                    chatInputBuffer_[0] = '\0';
                    return;
                }

                std::string targetArgLower = targetArg;
                for (char& c : targetArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                uint64_t bestGuid = 0;
                float    bestDist = std::numeric_limits<float>::max();
                const auto& pmi = gameHandler.getMovementInfo();
                const float playerX = pmi.x;
                const float playerY = pmi.y;
                const float playerZ = pmi.z;
                for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                    if (!entity || entity->getType() == game::ObjectType::OBJECT) continue;
                    std::string name;
                    if (entity->getType() == game::ObjectType::PLAYER ||
                        entity->getType() == game::ObjectType::UNIT) {
                        auto unit = std::static_pointer_cast<game::Unit>(entity);
                        name = unit->getName();
                    }
                    if (name.empty()) continue;
                    std::string nameLower = name;
                    for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (nameLower.find(targetArgLower) == 0) {
                        float dx = entity->getX() - playerX;
                        float dy = entity->getY() - playerY;
                        float dz = entity->getZ() - playerZ;
                        float dist = dx*dx + dy*dy + dz*dz;
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestGuid = guid;
                        }
                    }
                }
                if (bestGuid) {
                    gameHandler.setTarget(bestGuid);
                } else {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "No target matching '" + targetArg + "' found.";
                    gameHandler.addLocalChatMessage(sysMsg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetenemy") {
                gameHandler.targetEnemy(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetfriend") {
                gameHandler.targetFriend(false);
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlasttarget" || cmdLower == "targetlast") {
                gameHandler.targetLastTarget();
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastenemy") {
                gameHandler.targetEnemy(true);  // Reverse direction
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastfriend") {
                gameHandler.targetFriend(true);  // Reverse direction
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "focus") {
                // /focus                  → set current target as focus
                // /focus PlayerName       → search for entity by name and set as focus
                // /focus [target=X] Name  → macro conditional: set focus to resolved target
                if (spacePos != std::string::npos) {
                    std::string focusArg = command.substr(spacePos + 1);

                    // Evaluate conditionals if present
                    uint64_t focusCmdOverride = static_cast<uint64_t>(-1);
                    if (!focusArg.empty() && focusArg.front() == '[') {
                        focusArg = evaluateMacroConditionals(focusArg, gameHandler, focusCmdOverride);
                        if (focusArg.empty() && focusCmdOverride == static_cast<uint64_t>(-1)) {
                            chatInputBuffer_[0] = '\0';
                            return;
                        }
                        while (!focusArg.empty() && focusArg.front() == ' ') focusArg.erase(focusArg.begin());
                        while (!focusArg.empty() && focusArg.back()  == ' ') focusArg.pop_back();
                    }

                    if (focusCmdOverride != static_cast<uint64_t>(-1) && focusCmdOverride != 0) {
                        // Conditional resolved to a specific GUID (e.g. [target=mouseover])
                        gameHandler.setFocus(focusCmdOverride);
                    } else if (!focusArg.empty()) {
                        // Name search — same logic as /target
                        std::string focusArgLower = focusArg;
                        for (char& c : focusArgLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        uint64_t bestGuid = 0;
                        float    bestDist = std::numeric_limits<float>::max();
                        const auto& pmi = gameHandler.getMovementInfo();
                        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                            if (!entity || entity->getType() == game::ObjectType::OBJECT) continue;
                            std::string name;
                            if (entity->getType() == game::ObjectType::PLAYER ||
                                entity->getType() == game::ObjectType::UNIT) {
                                auto unit = std::static_pointer_cast<game::Unit>(entity);
                                name = unit->getName();
                            }
                            if (name.empty()) continue;
                            std::string nameLower = name;
                            for (char& c : nameLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            if (nameLower.find(focusArgLower) == 0) {
                                float dx = entity->getX() - pmi.x;
                                float dy = entity->getY() - pmi.y;
                                float dz = entity->getZ() - pmi.z;
                                float dist = dx*dx + dy*dy + dz*dz;
                                if (dist < bestDist) { bestDist = dist; bestGuid = guid; }
                            }
                        }
                        if (bestGuid) {
                            gameHandler.setFocus(bestGuid);
                        } else {
                            game::MessageChatData msg;
                            msg.type = game::ChatType::SYSTEM;
                            msg.language = game::ChatLanguage::UNIVERSAL;
                            msg.message = "No unit matching '" + focusArg + "' found.";
                            gameHandler.addLocalChatMessage(msg);
                        }
                    }
                } else if (gameHandler.hasTarget()) {
                    gameHandler.setFocus(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a unit to set as focus.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            if (cmdLower == "clearfocus") {
                gameHandler.clearFocus();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /unstuck command — resets player position to floor height
            if (cmdLower == "unstuck") {
                gameHandler.unstuck();
                chatInputBuffer_[0] = '\0';
                return;
            }
            // /unstuckgy command — move to nearest graveyard
            if (cmdLower == "unstuckgy") {
                gameHandler.unstuckGy();
                chatInputBuffer_[0] = '\0';
                return;
            }
            // /unstuckhearth command — teleport to hearthstone bind point
            if (cmdLower == "unstuckhearth") {
                gameHandler.unstuckHearth();
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /transport board — board test transport
            if (cmdLower == "transport board") {
                auto* tm = gameHandler.getTransportManager();
                if (tm) {
                    // Test transport GUID
                    uint64_t testTransportGuid = 0x1000000000000001ULL;
                    // Place player at center of deck (rough estimate)
                    glm::vec3 deckCenter(0.0f, 0.0f, 5.0f);
                    gameHandler.setPlayerOnTransport(testTransportGuid, deckCenter);
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Boarded test transport. Use '/transport leave' to disembark.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Transport system not available.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // /transport leave — disembark from transport
            if (cmdLower == "transport leave") {
                if (gameHandler.isOnTransport()) {
                    gameHandler.clearPlayerTransport();
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Disembarked from transport.";
                    gameHandler.addLocalChatMessage(msg);
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You are not on a transport.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer_[0] = '\0';
                return;
            }

            // Chat channel slash commands
            // If used without a message (e.g. just "/s"), switch the chat type dropdown
            bool isChannelCommand = false;
            if (cmdLower == "s" || cmdLower == "say") {
                type = game::ChatType::SAY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 0;
            } else if (cmdLower == "y" || cmdLower == "yell" || cmdLower == "shout") {
                type = game::ChatType::YELL;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 1;
            } else if (cmdLower == "p" || cmdLower == "party") {
                type = game::ChatType::PARTY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 2;
            } else if (cmdLower == "g" || cmdLower == "guild") {
                type = game::ChatType::GUILD;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 3;
            } else if (cmdLower == "raid" || cmdLower == "rsay" || cmdLower == "ra") {
                type = game::ChatType::RAID;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 5;
            } else if (cmdLower == "raidwarning" || cmdLower == "rw") {
                type = game::ChatType::RAID_WARNING;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 8;
            } else if (cmdLower == "officer" || cmdLower == "o" || cmdLower == "osay") {
                type = game::ChatType::OFFICER;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 6;
            } else if (cmdLower == "battleground" || cmdLower == "bg") {
                type = game::ChatType::BATTLEGROUND;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 7;
            } else if (cmdLower == "instance" || cmdLower == "i") {
                // Instance chat uses PARTY chat type
                type = game::ChatType::PARTY;
                message = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                isChannelCommand = true;
                switchChatType = 9;
            } else if (cmdLower == "join") {
                // /join with no args: accept pending BG invite if any
                if (spacePos == std::string::npos && gameHandler.hasPendingBgInvite()) {
                    gameHandler.acceptBattlefield();
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                // /join ChannelName [password]
                if (spacePos != std::string::npos) {
                    std::string rest = command.substr(spacePos + 1);
                    size_t pwStart = rest.find(' ');
                    std::string channelName = (pwStart != std::string::npos) ? rest.substr(0, pwStart) : rest;
                    std::string password = (pwStart != std::string::npos) ? rest.substr(pwStart + 1) : "";
                    gameHandler.joinChannel(channelName, password);
                }
                chatInputBuffer_[0] = '\0';
                return;
            } else if (cmdLower == "leave") {
                // /leave ChannelName
                if (spacePos != std::string::npos) {
                    std::string channelName = command.substr(spacePos + 1);
                    gameHandler.leaveChannel(channelName);
                }
                chatInputBuffer_[0] = '\0';
                return;
            } else if ((cmdLower == "wts" || cmdLower == "wtb") && spacePos != std::string::npos) {
                // /wts and /wtb — send to Trade channel
                // Prefix with [WTS] / [WTB] and route to the Trade channel
                const std::string tag = (cmdLower == "wts") ? "[WTS] " : "[WTB] ";
                const std::string body = command.substr(spacePos + 1);
                // Find the Trade channel among joined channels (case-insensitive prefix match)
                std::string tradeChan;
                for (const auto& ch : gameHandler.getJoinedChannels()) {
                    std::string chLow = ch;
                    for (char& c : chLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (chLow.rfind("trade", 0) == 0) { tradeChan = ch; break; }
                }
                if (tradeChan.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.language = game::ChatLanguage::UNIVERSAL;
                    errMsg.message = "You are not in the Trade channel.";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                message = tag + body;
                type = game::ChatType::CHANNEL;
                target = tradeChan;
                isChannelCommand = true;
            } else if (cmdLower.size() == 1 && cmdLower[0] >= '1' && cmdLower[0] <= '9') {
                // /1 msg, /2 msg — channel shortcuts
                int channelIdx = cmdLower[0] - '0';
                std::string channelName = gameHandler.getChannelByIndex(channelIdx);
                if (!channelName.empty() && spacePos != std::string::npos) {
                    message = command.substr(spacePos + 1);
                    type = game::ChatType::CHANNEL;
                    target = channelName;
                    isChannelCommand = true;
                } else if (channelName.empty()) {
                    game::MessageChatData errMsg;
                    errMsg.type = game::ChatType::SYSTEM;
                    errMsg.message = "You are not in channel " + std::to_string(channelIdx) + ".";
                    gameHandler.addLocalChatMessage(errMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                } else {
                    chatInputBuffer_[0] = '\0';
                    return;
                }
            } else if (cmdLower == "w" || cmdLower == "whisper" || cmdLower == "tell" || cmdLower == "t") {
                switchChatType = 4;
                if (spacePos != std::string::npos) {
                    std::string rest = command.substr(spacePos + 1);
                    size_t msgStart = rest.find(' ');
                    if (msgStart != std::string::npos) {
                        // /w PlayerName message — send whisper immediately
                        target = rest.substr(0, msgStart);
                        message = rest.substr(msgStart + 1);
                        type = game::ChatType::WHISPER;
                        isChannelCommand = true;
                        // Set whisper target for future messages
                        strncpy(whisperTargetBuffer_, target.c_str(), sizeof(whisperTargetBuffer_) - 1);
                        whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                    } else {
                        // /w PlayerName — switch to whisper mode with target set
                        strncpy(whisperTargetBuffer_, rest.c_str(), sizeof(whisperTargetBuffer_) - 1);
                        whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                        message = "";
                        isChannelCommand = true;
                    }
                } else {
                    // Just "/w" — switch to whisper mode
                    message = "";
                    isChannelCommand = true;
                }
            } else if (cmdLower == "r" || cmdLower == "reply") {
                switchChatType = 4;
                std::string lastSender = gameHandler.getLastWhisperSender();
                if (lastSender.empty()) {
                    game::MessageChatData sysMsg;
                    sysMsg.type = game::ChatType::SYSTEM;
                    sysMsg.language = game::ChatLanguage::UNIVERSAL;
                    sysMsg.message = "No one has whispered you yet.";
                    gameHandler.addLocalChatMessage(sysMsg);
                    chatInputBuffer_[0] = '\0';
                    return;
                }
                target = lastSender;
                strncpy(whisperTargetBuffer_, target.c_str(), sizeof(whisperTargetBuffer_) - 1);
                whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
                if (spacePos != std::string::npos) {
                    message = command.substr(spacePos + 1);
                    type = game::ChatType::WHISPER;
                } else {
                    message = "";
                }
                isChannelCommand = true;
            }

            // Check for emote commands
            if (!isChannelCommand) {
                std::string targetName;
                const std::string* targetNamePtr = nullptr;
                if (gameHandler.hasTarget()) {
                    auto targetEntity = gameHandler.getTarget();
                    if (targetEntity) {
                        targetName = getEntityName(targetEntity);
                        if (!targetName.empty()) targetNamePtr = &targetName;
                    }
                }

                std::string emoteText = rendering::Renderer::getEmoteText(cmdLower, targetNamePtr);
                if (!emoteText.empty()) {
                    // Play the emote animation
                    auto* renderer = services_.renderer;
                    if (renderer) {
                        renderer->playEmote(cmdLower);
                    }

                    // Send CMSG_TEXT_EMOTE to server
                    uint32_t dbcId = rendering::Renderer::getEmoteDbcId(cmdLower);
                    if (dbcId != 0) {
                        uint64_t targetGuid = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                        gameHandler.sendTextEmote(dbcId, targetGuid);
                    }

                    // Add local chat message
                    game::MessageChatData msg;
                    msg.type = game::ChatType::TEXT_EMOTE;
                    msg.language = game::ChatLanguage::COMMON;
                    msg.message = emoteText;
                    gameHandler.addLocalChatMessage(msg);

                    chatInputBuffer_[0] = '\0';
                    return;
                }

                // Not a recognized command — fall through and send as normal chat
                if (!isChannelCommand) {
                    message = input;
                }
            }

            // If no valid command found and starts with /, just send as-is
            if (!isChannelCommand && message == input) {
                // Use the selected chat type from dropdown
                switch (selectedChatType_) {
                    case 0: type = game::ChatType::SAY; break;
                    case 1: type = game::ChatType::YELL; break;
                    case 2: type = game::ChatType::PARTY; break;
                    case 3: type = game::ChatType::GUILD; break;
                    case 4: type = game::ChatType::WHISPER; target = whisperTargetBuffer_; break;
                    case 5: type = game::ChatType::RAID; break;
                    case 6: type = game::ChatType::OFFICER; break;
                    case 7: type = game::ChatType::BATTLEGROUND; break;
                    case 8: type = game::ChatType::RAID_WARNING; break;
                    case 9: type = game::ChatType::PARTY; break; // INSTANCE uses PARTY
                    case 10: { // CHANNEL
                        const auto& chans = gameHandler.getJoinedChannels();
                        if (!chans.empty() && selectedChannelIdx_ < static_cast<int>(chans.size())) {
                            type = game::ChatType::CHANNEL;
                            target = chans[selectedChannelIdx_];
                        } else { type = game::ChatType::SAY; }
                        break;
                    }
                    default: type = game::ChatType::SAY; break;
                }
            }
        } else {
            // No slash command, use the selected chat type from dropdown
            switch (selectedChatType_) {
                case 0: type = game::ChatType::SAY; break;
                case 1: type = game::ChatType::YELL; break;
                case 2: type = game::ChatType::PARTY; break;
                case 3: type = game::ChatType::GUILD; break;
                case 4: type = game::ChatType::WHISPER; target = whisperTargetBuffer_; break;
                case 5: type = game::ChatType::RAID; break;
                case 6: type = game::ChatType::OFFICER; break;
                case 7: type = game::ChatType::BATTLEGROUND; break;
                case 8: type = game::ChatType::RAID_WARNING; break;
                case 9: type = game::ChatType::PARTY; break; // INSTANCE uses PARTY
                case 10: { // CHANNEL
                    const auto& chans = gameHandler.getJoinedChannels();
                    if (!chans.empty() && selectedChannelIdx_ < static_cast<int>(chans.size())) {
                        type = game::ChatType::CHANNEL;
                        target = chans[selectedChannelIdx_];
                    } else { type = game::ChatType::SAY; }
                    break;
                }
                default: type = game::ChatType::SAY; break;
            }
        }

        // Whisper shortcuts to PortBot/GMBot: translate to GM teleport commands.
        if (type == game::ChatType::WHISPER && isPortBotTarget(target)) {
            std::string cmd = buildPortBotCommand(message);
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            if (cmd.empty() || cmd == "__help__") {
                msg.message = "PortBot: /w PortBot <dest>. Aliases: sw if darn org tb uc shatt dal. Also supports '.tele ...' or 'xyz x y z [map [o]]'.";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer_[0] = '\0';
                return;
            }

            gameHandler.sendChatMessage(game::ChatType::SAY, cmd, "");
            msg.message = "PortBot executed: " + cmd;
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer_[0] = '\0';
            return;
        }

        // Validate whisper has a target
        if (type == game::ChatType::WHISPER && target.empty()) {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "You must specify a player name for whisper.";
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer_[0] = '\0';
            return;
        }

        // Don't send empty messages — but switch chat type if a channel shortcut was used
        if (!message.empty()) {
            gameHandler.sendChatMessage(type, message, target);
        }

        // Switch chat type dropdown when channel shortcut used (with or without message)
        if (switchChatType >= 0) {
            selectedChatType_ = switchChatType;
        }

        // Clear input
        chatInputBuffer_[0] = '\0';
    }
}


const char* ChatPanel::getChatTypeName(game::ChatType type) const {
    switch (type) {
        case game::ChatType::SAY: return "Say";
        case game::ChatType::YELL: return "Yell";
        case game::ChatType::EMOTE: return "Emote";
        case game::ChatType::TEXT_EMOTE: return "Emote";
        case game::ChatType::PARTY: return "Party";
        case game::ChatType::GUILD: return "Guild";
        case game::ChatType::OFFICER: return "Officer";
        case game::ChatType::RAID: return "Raid";
        case game::ChatType::RAID_LEADER: return "Raid Leader";
        case game::ChatType::RAID_WARNING: return "Raid Warning";
        case game::ChatType::BATTLEGROUND: return "Battleground";
        case game::ChatType::BATTLEGROUND_LEADER: return "Battleground Leader";
        case game::ChatType::WHISPER: return "Whisper";
        case game::ChatType::WHISPER_INFORM: return "To";
        case game::ChatType::SYSTEM: return "System";
        case game::ChatType::MONSTER_SAY: return "Say";
        case game::ChatType::MONSTER_YELL: return "Yell";
        case game::ChatType::MONSTER_EMOTE: return "Emote";
        case game::ChatType::CHANNEL: return "Channel";
        case game::ChatType::ACHIEVEMENT: return "Achievement";
        case game::ChatType::DND: return "DND";
        case game::ChatType::AFK: return "AFK";
        case game::ChatType::BG_SYSTEM_NEUTRAL:
        case game::ChatType::BG_SYSTEM_ALLIANCE:
        case game::ChatType::BG_SYSTEM_HORDE: return "System";
        default: return "Unknown";
    }
}


ImVec4 ChatPanel::getChatTypeColor(game::ChatType type) const {
    switch (type) {
        case game::ChatType::SAY:
            return ui::colors::kWhite;  // White
        case game::ChatType::YELL:
            return kColorRed;  // Red
        case game::ChatType::EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::TEXT_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::PARTY:
            return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue
        case game::ChatType::GUILD:
            return kColorBrightGreen;  // Green
        case game::ChatType::OFFICER:
            return ImVec4(0.3f, 0.8f, 0.3f, 1.0f);  // Dark green
        case game::ChatType::RAID:
            return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
        case game::ChatType::RAID_LEADER:
            return ImVec4(1.0f, 0.4f, 0.0f, 1.0f);  // Darker orange
        case game::ChatType::RAID_WARNING:
            return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Red
        case game::ChatType::BATTLEGROUND:
            return ImVec4(1.0f, 0.6f, 0.0f, 1.0f);  // Orange-gold
        case game::ChatType::BATTLEGROUND_LEADER:
            return ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
        case game::ChatType::WHISPER:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink
        case game::ChatType::WHISPER_INFORM:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink
        case game::ChatType::SYSTEM:
            return kColorYellow;  // Yellow
        case game::ChatType::MONSTER_SAY:
            return ui::colors::kWhite;  // White (same as SAY)
        case game::ChatType::MONSTER_YELL:
            return kColorRed;  // Red (same as YELL)
        case game::ChatType::MONSTER_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange (same as EMOTE)
        case game::ChatType::CHANNEL:
            return ImVec4(1.0f, 0.7f, 0.7f, 1.0f);  // Light pink
        case game::ChatType::ACHIEVEMENT:
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Bright yellow
        case game::ChatType::GUILD_ACHIEVEMENT:
            return colors::kWarmGold; // Gold
        case game::ChatType::SKILL:
            return colors::kCyan;  // Cyan
        case game::ChatType::LOOT:
            return ImVec4(0.8f, 0.5f, 1.0f, 1.0f);  // Light purple
        case game::ChatType::MONSTER_WHISPER:
        case game::ChatType::RAID_BOSS_WHISPER:
            return ImVec4(1.0f, 0.5f, 1.0f, 1.0f);  // Pink (same as WHISPER)
        case game::ChatType::RAID_BOSS_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange (same as EMOTE)
        case game::ChatType::MONSTER_PARTY:
            return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue (same as PARTY)
        case game::ChatType::BG_SYSTEM_NEUTRAL:
            return colors::kWarmGold; // Gold
        case game::ChatType::BG_SYSTEM_ALLIANCE:
            return ImVec4(0.3f, 0.6f, 1.0f, 1.0f);  // Blue
        case game::ChatType::BG_SYSTEM_HORDE:
            return kColorRed;  // Red
        case game::ChatType::AFK:
        case game::ChatType::DND:
            return ImVec4(0.85f, 0.85f, 0.85f, 0.8f); // Light gray
        default:
            return ui::colors::kLightGray;  // Gray
    }
}


std::string ChatPanel::replaceGenderPlaceholders(const std::string& text, game::GameHandler& gameHandler) {
    // Get player gender, pronouns, and name
    game::Gender gender = game::Gender::NONBINARY;
    std::string playerName = "Adventurer";
    const auto* character = gameHandler.getActiveCharacter();
    if (character) {
        gender = character->gender;
        if (!character->name.empty()) {
            playerName = character->name;
        }
    }
    game::Pronouns pronouns = game::Pronouns::forGender(gender);

    std::string result = text;

    // Helper to trim whitespace
    auto trim = [](std::string& s) {
        const char* ws = " \t\n\r";
        size_t start = s.find_first_not_of(ws);
        if (start == std::string::npos) { s.clear(); return; }
        size_t end = s.find_last_not_of(ws);
        s = s.substr(start, end - start + 1);
    };

    // Replace $g/$G placeholders first.
    size_t pos = 0;
    while ((pos = result.find('$', pos)) != std::string::npos) {
        if (pos + 1 >= result.length()) break;
        char marker = result[pos + 1];
        if (marker != 'g' && marker != 'G') { pos++; continue; }

        size_t endPos = result.find(';', pos);
        if (endPos == std::string::npos) { pos += 2; continue; }

        std::string placeholder = result.substr(pos + 2, endPos - pos - 2);

        // Split by colons
        std::vector<std::string> parts;
        size_t start = 0;
        size_t colonPos;
        while ((colonPos = placeholder.find(':', start)) != std::string::npos) {
            std::string part = placeholder.substr(start, colonPos - start);
            trim(part);
            parts.push_back(part);
            start = colonPos + 1;
        }
        // Add the last part
        std::string lastPart = placeholder.substr(start);
        trim(lastPart);
        parts.push_back(lastPart);

        // Select appropriate text based on gender
        std::string replacement;
        if (parts.size() >= 3) {
            // Three options: male, female, nonbinary
            switch (gender) {
                case game::Gender::MALE:
                    replacement = parts[0];
                    break;
                case game::Gender::FEMALE:
                    replacement = parts[1];
                    break;
                case game::Gender::NONBINARY:
                    replacement = parts[2];
                    break;
            }
        } else if (parts.size() >= 2) {
            // Two options: male, female (use first for nonbinary)
            switch (gender) {
                case game::Gender::MALE:
                    replacement = parts[0];
                    break;
                case game::Gender::FEMALE:
                    replacement = parts[1];
                    break;
                case game::Gender::NONBINARY:
                    // Default to gender-neutral: use the shorter/simpler option
                    replacement = parts[0].length() <= parts[1].length() ? parts[0] : parts[1];
                    break;
            }
        } else {
            // Malformed placeholder
            pos = endPos + 1;
            continue;
        }

        result.replace(pos, endPos - pos + 1, replacement);
        pos += replacement.length();
    }

    // Resolve class and race names for $C and $R placeholders
    std::string className = "Adventurer";
    std::string raceName = "Unknown";
    if (character) {
        className = game::getClassName(character->characterClass);
        raceName = game::getRaceName(character->race);
    }

    // Replace simple placeholders.
    // $n/$N = player name, $c/$C = class name, $r/$R = race name
    // $p = subject pronoun (he/she/they)
    // $o = object pronoun (him/her/them)
    // $s = possessive adjective (his/her/their)
    // $S = possessive pronoun (his/hers/theirs)
    // $b/$B = line break
    pos = 0;
    while ((pos = result.find('$', pos)) != std::string::npos) {
        if (pos + 1 >= result.length()) break;

        char code = result[pos + 1];
        std::string replacement;
        switch (code) {
            case 'n': case 'N': replacement = playerName; break;
            case 'c': case 'C': replacement = className; break;
            case 'r': case 'R': replacement = raceName; break;
            case 'p': replacement = pronouns.subject; break;
            case 'o': replacement = pronouns.object; break;
            case 's': replacement = pronouns.possessive; break;
            case 'S': replacement = pronouns.possessiveP; break;
            case 'b': case 'B': replacement = "\n"; break;
            case 'g': case 'G': pos++; continue;
            default: pos++; continue;
        }

        result.replace(pos, 2, replacement);
        pos += replacement.length();
    }

    // WoW markup linebreak token.
    pos = 0;
    while ((pos = result.find("|n", pos)) != std::string::npos) {
        result.replace(pos, 2, "\n");
        pos += 1;
    }
    pos = 0;
    while ((pos = result.find("|N", pos)) != std::string::npos) {
        result.replace(pos, 2, "\n");
        pos += 1;
    }

    return result;
}

void ChatPanel::renderBubbles(game::GameHandler& gameHandler) {
    if (chatBubbles_.empty()) return;

    auto* renderer = services_.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Get delta time from ImGui
    float dt = ImGui::GetIO().DeltaTime;

    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Update and render bubbles
    for (int i = static_cast<int>(chatBubbles_.size()) - 1; i >= 0; --i) {
        auto& bubble = chatBubbles_[i];
        bubble.timeRemaining -= dt;
        if (bubble.timeRemaining <= 0.0f) {
            chatBubbles_.erase(chatBubbles_.begin() + i);
            continue;
        }

        // Get entity position
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        if (!entity) continue;

        // Convert canonical → render coordinates, offset up by 2.5 units for bubble above head
        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ() + 2.5f);
        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;  // Behind camera

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float screenX = (ndc.x * 0.5f + 0.5f) * screenW;
        // Camera bakes the Vulkan Y-flip into the projection matrix:
        // NDC y=-1 is top, y=1 is bottom — same convention as nameplate/minimap projection.
        float screenY = (ndc.y * 0.5f + 0.5f) * screenH;

        // Skip if off-screen
        if (screenX < -200.0f || screenX > screenW + 200.0f ||
            screenY < -100.0f || screenY > screenH + 100.0f) continue;

        // Fade alpha over last 2 seconds
        float alpha = 1.0f;
        if (bubble.timeRemaining < 2.0f) {
            alpha = bubble.timeRemaining / 2.0f;
        }

        // Draw bubble window
        std::string winId = "##ChatBubble" + std::to_string(bubble.senderGuid);
        ImGui::SetNextWindowPos(ImVec2(screenX, screenY), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.7f * alpha);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));

        ImGui::Begin(winId.c_str(), nullptr, flags);

        ImVec4 textColor = bubble.isYell
            ? ImVec4(1.0f, 0.2f, 0.2f, alpha)
            : ImVec4(1.0f, 1.0f, 1.0f, alpha);

        ImGui::PushStyleColor(ImGuiCol_Text, textColor);
        ImGui::PushTextWrapPos(200.0f);
        ImGui::TextWrapped("%s", bubble.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}


// ---- Public interface methods ----

void ChatPanel::setupCallbacks(game::GameHandler& gameHandler) {
    if (!chatBubbleCallbackSet_) {
        gameHandler.setChatBubbleCallback([this](uint64_t guid, const std::string& msg, bool isYell) {
            float duration = 8.0f + static_cast<float>(msg.size()) * 0.06f;
            if (isYell) duration += 2.0f;
            if (duration > 15.0f) duration = 15.0f;

            // Replace existing bubble for same sender
            for (auto& b : chatBubbles_) {
                if (b.senderGuid == guid) {
                    b.message = msg;
                    b.timeRemaining = duration;
                    b.totalDuration = duration;
                    b.isYell = isYell;
                    return;
                }
            }
            // Evict oldest if too many
            if (chatBubbles_.size() >= 10) {
                chatBubbles_.erase(chatBubbles_.begin());
            }
            chatBubbles_.push_back({guid, msg, duration, duration, isYell});
        });
        chatBubbleCallbackSet_ = true;
    }
}

void ChatPanel::insertChatLink(const std::string& link) {
    if (link.empty()) return;
    size_t curLen = strlen(chatInputBuffer_);
    if (curLen + link.size() + 1 < sizeof(chatInputBuffer_)) {
        strncat(chatInputBuffer_, link.c_str(), sizeof(chatInputBuffer_) - curLen - 1);
        chatInputMoveCursorToEnd_ = true;
        refocusChatInput_ = true;
    }
}

void ChatPanel::activateSlashInput() {
    refocusChatInput_ = true;
    chatInputBuffer_[0] = '/';
    chatInputBuffer_[1] = '\0';
    chatInputMoveCursorToEnd_ = true;
}

void ChatPanel::activateInput() {
    refocusChatInput_ = true;
}

void ChatPanel::setWhisperTarget(const std::string& name) {
    selectedChatType_ = 4;  // WHISPER
    strncpy(whisperTargetBuffer_, name.c_str(), sizeof(whisperTargetBuffer_) - 1);
    whisperTargetBuffer_[sizeof(whisperTargetBuffer_) - 1] = '\0';
    refocusChatInput_ = true;
}

ChatPanel::SlashCommands ChatPanel::consumeSlashCommands() {
    SlashCommands result = slashCmds_;
    slashCmds_ = {};
    return result;
}

void ChatPanel::renderSettingsTab(std::function<void()> saveSettingsFn) {
    ImGui::Spacing();

    ImGui::Text("Appearance");
    ImGui::Separator();

    if (ImGui::Checkbox("Show Timestamps", &chatShowTimestamps)) {
        saveSettingsFn();
    }
    ImGui::SetItemTooltip("Show [HH:MM] before each chat message");

    const char* fontSizes[] = { "Small", "Medium", "Large" };
    if (ImGui::Combo("Chat Font Size", &chatFontSize, fontSizes, 3)) {
        saveSettingsFn();
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Auto-Join Channels");
    ImGui::Separator();

    if (ImGui::Checkbox("General", &chatAutoJoinGeneral)) saveSettingsFn();
    if (ImGui::Checkbox("Trade", &chatAutoJoinTrade)) saveSettingsFn();
    if (ImGui::Checkbox("LocalDefense", &chatAutoJoinLocalDefense)) saveSettingsFn();
    if (ImGui::Checkbox("LookingForGroup", &chatAutoJoinLFG)) saveSettingsFn();
    if (ImGui::Checkbox("Local", &chatAutoJoinLocal)) saveSettingsFn();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Text("Joined Channels");
    ImGui::Separator();

    ImGui::TextDisabled("Use /join and /leave commands in chat to manage channels.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Restore Chat Defaults", ImVec2(-1, 0))) {
        restoreDefaults();
        saveSettingsFn();
    }
}

void ChatPanel::restoreDefaults() {
    chatShowTimestamps = false;
    chatFontSize = 1;
    chatAutoJoinGeneral = true;
    chatAutoJoinTrade = true;
    chatAutoJoinLocalDefense = true;
    chatAutoJoinLFG = true;
    chatAutoJoinLocal = true;
}

} // namespace ui
} // namespace wowee
