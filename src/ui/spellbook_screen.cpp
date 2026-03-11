#include "ui/spellbook_screen.hpp"
#include "core/input.hpp"
#include "core/application.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <map>
#include <cctype>

namespace wowee { namespace ui {

// Case-insensitive substring match
static bool containsCI(const std::string& haystack, const char* needle) {
    if (!needle || !needle[0]) return true;
    size_t needleLen = strlen(needle);
    if (needleLen > haystack.size()) return false;
    for (size_t i = 0; i <= haystack.size() - needleLen; i++) {
        bool match = true;
        for (size_t j = 0; j < needleLen; j++) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void SpellbookScreen::loadSpellDBC(pipeline::AssetManager* assetManager) {
    if (dbcLoadAttempted) return;
    dbcLoadAttempted = true;

    if (!assetManager || !assetManager->isInitialized()) return;

    auto dbc = assetManager->loadDBC("Spell.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("Spellbook: Could not load Spell.dbc");
        return;
    }

    uint32_t fieldCount = dbc->getFieldCount();
    if (fieldCount < 154) {
        LOG_WARNING("Spellbook: Spell.dbc has ", fieldCount, " fields, expected 234+");
        return;
    }

    const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;

    // Load SpellCastTimes.dbc: field 0=ID, field 1=Base(ms), field 2=PerLevel, field 3=Minimum
    std::unordered_map<uint32_t, uint32_t> castTimeMap;  // index → base ms
    auto castTimeDbc = assetManager->loadDBC("SpellCastTimes.dbc");
    if (castTimeDbc && castTimeDbc->isLoaded()) {
        for (uint32_t i = 0; i < castTimeDbc->getRecordCount(); ++i) {
            uint32_t id   = castTimeDbc->getUInt32(i, 0);
            int32_t  base = static_cast<int32_t>(castTimeDbc->getUInt32(i, 1));
            if (id > 0 && base > 0)
                castTimeMap[id] = static_cast<uint32_t>(base);
        }
    }

    // Load SpellRange.dbc: field 0=ID, field 5=MaxRangeHostile (float)
    std::unordered_map<uint32_t, float> rangeMap;  // index → max yards
    auto rangeDbc = assetManager->loadDBC("SpellRange.dbc");
    if (rangeDbc && rangeDbc->isLoaded()) {
        uint32_t rangeFieldCount = rangeDbc->getFieldCount();
        if (rangeFieldCount >= 6) {
            for (uint32_t i = 0; i < rangeDbc->getRecordCount(); ++i) {
                uint32_t id = rangeDbc->getUInt32(i, 0);
                float maxRange = rangeDbc->getFloat(i, 5);
                if (id > 0 && maxRange > 0.0f)
                    rangeMap[id] = maxRange;
            }
        }
    }

    auto tryLoad = [&](uint32_t idField, uint32_t attrField, uint32_t iconField,
                       uint32_t nameField, uint32_t rankField, uint32_t tooltipField,
                       uint32_t powerTypeField, uint32_t manaCostField,
                       uint32_t castTimeIndexField, uint32_t rangeIndexField,
                       const char* label) {
        spellData.clear();
        uint32_t count = dbc->getRecordCount();
        const uint32_t fc = dbc->getFieldCount();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t spellId = dbc->getUInt32(i, idField);
            if (spellId == 0) continue;

            SpellInfo info;
            info.spellId = spellId;
            info.attributes = dbc->getUInt32(i, attrField);
            info.iconId = dbc->getUInt32(i, iconField);
            info.name = dbc->getString(i, nameField);
            if (rankField < fc)    info.rank = dbc->getString(i, rankField);
            if (tooltipField < fc) info.description = dbc->getString(i, tooltipField);
            // Optional fields: only read if field index is valid for this DBC version
            if (powerTypeField < fc)   info.powerType = dbc->getUInt32(i, powerTypeField);
            if (manaCostField  < fc)   info.manaCost  = dbc->getUInt32(i, manaCostField);
            if (castTimeIndexField < fc) {
                uint32_t ctIdx = dbc->getUInt32(i, castTimeIndexField);
                if (ctIdx > 0) {
                    auto ctIt = castTimeMap.find(ctIdx);
                    if (ctIt != castTimeMap.end()) info.castTimeMs = ctIt->second;
                }
            }
            if (rangeIndexField < fc) {
                uint32_t rangeIdx = dbc->getUInt32(i, rangeIndexField);
                if (rangeIdx > 0) {
                    auto rangeIt = rangeMap.find(rangeIdx);
                    if (rangeIt != rangeMap.end()) info.rangeIndex = static_cast<uint32_t>(rangeIt->second);
                }
            }

            if (!info.name.empty()) {
                spellData[spellId] = std::move(info);
            }
        }
        LOG_INFO("Spellbook: Loaded ", spellData.size(), " spells from Spell.dbc (", label, ")");
    };

    if (spellL) {
        // Default to UINT32_MAX for optional fields; tryLoad will skip them if >= fieldCount.
        // Avoids reading wrong data from expansion DBCs that lack these fields (e.g. Classic/TBC).
        uint32_t tooltipField      = UINT32_MAX;
        uint32_t powerTypeField    = UINT32_MAX;
        uint32_t manaCostField     = UINT32_MAX;
        uint32_t castTimeIdxField  = UINT32_MAX;
        uint32_t rangeIdxField     = UINT32_MAX;
        try { tooltipField     = (*spellL)["Tooltip"]; } catch (...) {}
        try { powerTypeField   = (*spellL)["PowerType"]; } catch (...) {}
        try { manaCostField    = (*spellL)["ManaCost"]; } catch (...) {}
        try { castTimeIdxField = (*spellL)["CastingTimeIndex"]; } catch (...) {}
        try { rangeIdxField    = (*spellL)["RangeIndex"]; } catch (...) {}
        tryLoad((*spellL)["ID"], (*spellL)["Attributes"], (*spellL)["IconID"],
                (*spellL)["Name"], (*spellL)["Rank"], tooltipField,
                powerTypeField, manaCostField, castTimeIdxField, rangeIdxField,
                "expansion layout");
    }

    if (spellData.empty() && fieldCount >= 200) {
        LOG_INFO("Spellbook: Retrying with WotLK field indices (DBC has ", fieldCount, " fields)");
        // WotLK Spell.dbc field indices (verified against 3.3.5a schema)
        tryLoad(0, 4, 133, 136, 153, 139, 14, 39, 47, 49, "WotLK fallback");
    }

    dbcLoaded = !spellData.empty();
}

std::string SpellbookScreen::lookupSpellName(uint32_t spellId, pipeline::AssetManager* assetManager) {
    if (!dbcLoadAttempted) {
        loadSpellDBC(assetManager);
    }
    auto it = spellData.find(spellId);
    if (it != spellData.end()) return it->second.name;
    return {};
}

void SpellbookScreen::loadSpellIconDBC(pipeline::AssetManager* assetManager) {
    if (iconDbLoaded) return;
    iconDbLoaded = true;

    if (!assetManager || !assetManager->isInitialized()) return;

    auto dbc = assetManager->loadDBC("SpellIcon.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    const auto* iconL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellIcon") : nullptr;
    for (uint32_t i = 0; i < dbc->getRecordCount(); i++) {
        uint32_t id = dbc->getUInt32(i, iconL ? (*iconL)["ID"] : 0);
        std::string path = dbc->getString(i, iconL ? (*iconL)["Path"] : 1);
        if (!path.empty() && id > 0) {
            spellIconPaths[id] = path;
        }
    }
}

void SpellbookScreen::loadSkillLineDBCs(pipeline::AssetManager* assetManager) {
    if (skillLineDbLoaded) return;
    skillLineDbLoaded = true;

    if (!assetManager || !assetManager->isInitialized()) return;

    auto skillLineDbc = assetManager->loadDBC("SkillLine.dbc");
    const auto* slL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SkillLine") : nullptr;
    if (skillLineDbc && skillLineDbc->isLoaded()) {
        for (uint32_t i = 0; i < skillLineDbc->getRecordCount(); i++) {
            uint32_t id = skillLineDbc->getUInt32(i, slL ? (*slL)["ID"] : 0);
            uint32_t category = skillLineDbc->getUInt32(i, slL ? (*slL)["Category"] : 1);
            std::string name = skillLineDbc->getString(i, slL ? (*slL)["Name"] : 3);
            if (id > 0) {
                if (!name.empty()) {
                    skillLineNames[id] = name;
                }
                skillLineCategories[id] = category;
            }
        }
        LOG_INFO("Spellbook: Loaded ", skillLineNames.size(), " skill line names, ",
                 skillLineCategories.size(), " categories from SkillLine.dbc");
    } else {
        LOG_WARNING("Spellbook: Could not load SkillLine.dbc");
    }

    auto slaDbc = assetManager->loadDBC("SkillLineAbility.dbc");
    const auto* slaL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SkillLineAbility") : nullptr;
    if (slaDbc && slaDbc->isLoaded()) {
        for (uint32_t i = 0; i < slaDbc->getRecordCount(); i++) {
            uint32_t skillLineId = slaDbc->getUInt32(i, slaL ? (*slaL)["SkillLineID"] : 1);
            uint32_t spellId = slaDbc->getUInt32(i, slaL ? (*slaL)["SpellID"] : 2);
            if (spellId > 0 && skillLineId > 0) {
                spellToSkillLine.emplace(spellId, skillLineId);
            }
        }
        LOG_INFO("Spellbook: Loaded ", spellToSkillLine.size(), " spell-to-skillline mappings from SkillLineAbility.dbc");
    } else {
        LOG_WARNING("Spellbook: Could not load SkillLineAbility.dbc");
    }
}

void SpellbookScreen::categorizeSpells(const std::unordered_set<uint32_t>& knownSpells) {
    spellTabs.clear();

    // SkillLine.dbc category IDs
    static constexpr uint32_t CAT_CLASS       = 7;   // Class abilities (spec trees)
    static constexpr uint32_t CAT_PROFESSION  = 11;  // Primary professions
    static constexpr uint32_t CAT_SECONDARY   = 9;   // Secondary skills (Cooking, First Aid, Fishing, Riding, Companions)

    // Special skill line IDs that get their own tabs
    static constexpr uint32_t SKILLLINE_MOUNTS     = 777;  // Mount summon spells (category 7)
    static constexpr uint32_t SKILLLINE_RIDING     = 762;  // Riding skill ranks (category 9)
    static constexpr uint32_t SKILLLINE_COMPANIONS = 778;  // Vanity/companion pets (category 7)

    // Buckets
    std::map<uint32_t, std::vector<const SpellInfo*>> specSpells;  // class spec trees
    std::map<uint32_t, std::vector<const SpellInfo*>> profSpells;  // professions + secondary
    std::vector<const SpellInfo*> mountSpells;
    std::vector<const SpellInfo*> companionSpells;
    std::vector<const SpellInfo*> generalSpells;

    for (uint32_t spellId : knownSpells) {
        auto it = spellData.find(spellId);
        if (it == spellData.end()) continue;

        const SpellInfo* info = &it->second;

        // Check all skill lines this spell belongs to, prefer class (cat 7) > profession > secondary > special
        auto range = spellToSkillLine.equal_range(spellId);
        bool categorized = false;

        uint32_t bestSkillLine = 0;
        int bestPriority = -1; // 4=class, 3=profession, 2=secondary, 1=mount/companion

        for (auto slIt = range.first; slIt != range.second; ++slIt) {
            uint32_t skillLineId = slIt->second;

            if (skillLineId == SKILLLINE_MOUNTS || skillLineId == SKILLLINE_RIDING) {
                if (bestPriority < 1) { bestPriority = 1; bestSkillLine = SKILLLINE_MOUNTS; }
                continue;
            }
            if (skillLineId == SKILLLINE_COMPANIONS) {
                if (bestPriority < 1) { bestPriority = 1; bestSkillLine = skillLineId; }
                continue;
            }

            auto catIt = skillLineCategories.find(skillLineId);
            if (catIt != skillLineCategories.end()) {
                uint32_t cat = catIt->second;
                if (cat == CAT_CLASS && bestPriority < 4) {
                    bestPriority = 4; bestSkillLine = skillLineId;
                } else if (cat == CAT_PROFESSION && bestPriority < 3) {
                    bestPriority = 3; bestSkillLine = skillLineId;
                } else if (cat == CAT_SECONDARY && bestPriority < 2) {
                    bestPriority = 2; bestSkillLine = skillLineId;
                }
            }
        }

        if (bestSkillLine > 0) {
            if (bestSkillLine == SKILLLINE_MOUNTS) {
                mountSpells.push_back(info);
                categorized = true;
            } else if (bestSkillLine == SKILLLINE_COMPANIONS) {
                companionSpells.push_back(info);
                categorized = true;
            } else {
                auto catIt = skillLineCategories.find(bestSkillLine);
                if (catIt != skillLineCategories.end()) {
                    uint32_t cat = catIt->second;
                    if (cat == CAT_CLASS) {
                        specSpells[bestSkillLine].push_back(info);
                        categorized = true;
                    } else if (cat == CAT_PROFESSION || cat == CAT_SECONDARY) {
                        profSpells[bestSkillLine].push_back(info);
                        categorized = true;
                    }
                }
            }
        }

        if (!categorized) {
            generalSpells.push_back(info);
        }
    }

    LOG_INFO("Spellbook categorize: ", specSpells.size(), " spec groups, ",
             generalSpells.size(), " general, ", profSpells.size(), " prof groups, ",
             mountSpells.size(), " mounts, ", companionSpells.size(), " companions");
    for (const auto& [slId, spells] : specSpells) {
        auto nameIt = skillLineNames.find(slId);
        LOG_INFO("  Spec tab: skillLine=", slId, " name='",
                 (nameIt != skillLineNames.end() ? nameIt->second : "?"), "' spells=", spells.size());
    }

    auto byName = [](const SpellInfo* a, const SpellInfo* b) { return a->name < b->name; };

    // Helper: add sorted skill-line-grouped tabs
    auto addGroupedTabs = [&](std::map<uint32_t, std::vector<const SpellInfo*>>& groups,
                              const char* fallbackName) {
        std::vector<std::pair<std::string, std::vector<const SpellInfo*>>> named;
        for (auto& [skillLineId, spells] : groups) {
            auto nameIt = skillLineNames.find(skillLineId);
            std::string tabName = (nameIt != skillLineNames.end()) ? nameIt->second : fallbackName;
            std::sort(spells.begin(), spells.end(), byName);
            named.push_back({std::move(tabName), std::move(spells)});
        }
        std::sort(named.begin(), named.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [name, spells] : named) {
            spellTabs.push_back({std::move(name), std::move(spells)});
        }
    };

    // 1. Class spec tabs
    addGroupedTabs(specSpells, "Spec");

    // 2. General tab
    if (!generalSpells.empty()) {
        std::sort(generalSpells.begin(), generalSpells.end(), byName);
        spellTabs.push_back({"General", std::move(generalSpells)});
    }

    // 3. Professions tabs
    addGroupedTabs(profSpells, "Profession");

    // 4. Mounts tab
    if (!mountSpells.empty()) {
        std::sort(mountSpells.begin(), mountSpells.end(), byName);
        spellTabs.push_back({"Mounts", std::move(mountSpells)});
    }

    // 5. Companions tab
    if (!companionSpells.empty()) {
        std::sort(companionSpells.begin(), companionSpells.end(), byName);
        spellTabs.push_back({"Companions", std::move(companionSpells)});
    }

    lastKnownSpellCount = knownSpells.size();
    categorizedWithSkillLines = !spellToSkillLine.empty();
}

VkDescriptorSet SpellbookScreen::getSpellIcon(uint32_t iconId, pipeline::AssetManager* assetManager) {
    if (iconId == 0 || !assetManager) return VK_NULL_HANDLE;

    auto cit = spellIconCache.find(iconId);
    if (cit != spellIconCache.end()) return cit->second;

    auto pit = spellIconPaths.find(iconId);
    if (pit == spellIconPaths.end()) {
        spellIconCache[iconId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    std::string iconPath = pit->second + ".blp";
    auto blpData = assetManager->readFile(iconPath);
    if (blpData.empty()) {
        spellIconCache[iconId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto image = pipeline::BLPLoader::load(blpData);
    if (!image.isValid()) {
        spellIconCache[iconId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto* window = core::Application::getInstance().getWindow();
    auto* vkCtx = window ? window->getVkContext() : nullptr;
    if (!vkCtx) {
        spellIconCache[iconId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    VkDescriptorSet ds = vkCtx->uploadImGuiTexture(image.data.data(), image.width, image.height);
    spellIconCache[iconId] = ds;
    return ds;
}

const SpellInfo* SpellbookScreen::getSpellInfo(uint32_t spellId) const {
    auto it = spellData.find(spellId);
    return (it != spellData.end()) ? &it->second : nullptr;
}

void SpellbookScreen::renderSpellTooltip(const SpellInfo* info, game::GameHandler& gameHandler) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(320.0f);

    // Spell name in yellow
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "%s", info->name.c_str());

    // Rank in gray
    if (!info->rank.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%s)", info->rank.c_str());
    }

    // Passive indicator
    if (info->isPassive()) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Passive");
    }

    // Resource cost + cast time on same row (WoW style)
    if (!info->isPassive()) {
        // Left: resource cost
        char costBuf[64] = "";
        if (info->manaCost > 0) {
            const char* powerName = "Mana";
            switch (info->powerType) {
                case 1: powerName = "Rage";   break;
                case 3: powerName = "Energy"; break;
                case 4: powerName = "Focus";  break;
                default: break;
            }
            std::snprintf(costBuf, sizeof(costBuf), "%u %s", info->manaCost, powerName);
        }

        // Right: cast time
        char castBuf[32] = "";
        if (info->castTimeMs == 0) {
            std::snprintf(castBuf, sizeof(castBuf), "Instant cast");
        } else {
            float secs = info->castTimeMs / 1000.0f;
            std::snprintf(castBuf, sizeof(castBuf), "%.1f sec cast", secs);
        }

        if (costBuf[0] || castBuf[0]) {
            float wrapW = 320.0f;
            if (costBuf[0] && castBuf[0]) {
                float castW = ImGui::CalcTextSize(castBuf).x;
                ImGui::Text("%s", costBuf);
                ImGui::SameLine(wrapW - castW);
                ImGui::Text("%s", castBuf);
            } else if (castBuf[0]) {
                ImGui::Text("%s", castBuf);
            } else {
                ImGui::Text("%s", costBuf);
            }
        }

        // Range
        if (info->rangeIndex > 0) {
            char rangeBuf[32];
            if (info->rangeIndex <= 5)
                std::snprintf(rangeBuf, sizeof(rangeBuf), "Melee range");
            else
                std::snprintf(rangeBuf, sizeof(rangeBuf), "%u yd range", info->rangeIndex);
            ImGui::Text("%s", rangeBuf);
        }
    }

    // Cooldown if active
    float cd = gameHandler.getSpellCooldown(info->spellId);
    if (cd > 0.0f) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Cooldown: %.1fs", cd);
    }

    // Description
    if (!info->description.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", info->description.c_str());
    }

    // Usage hints
    if (!info->isPassive()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Drag to action bar");
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Double-click to cast");
    }

    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void SpellbookScreen::render(game::GameHandler& gameHandler, pipeline::AssetManager* assetManager) {
    // P key toggle (edge-triggered)
    bool wantsTextInput = ImGui::GetIO().WantTextInput;
    bool pDown = !wantsTextInput && core::Input::getInstance().isKeyPressed(SDL_SCANCODE_P);
    if (pDown && !pKeyWasDown) {
        open = !open;
    }
    pKeyWasDown = pDown;

    if (!open) return;

    // Lazy-load DBC data on first open
    if (!dbcLoadAttempted) {
        loadSpellDBC(assetManager);
    }
    if (!iconDbLoaded) {
        loadSpellIconDBC(assetManager);
    }
    if (!skillLineDbLoaded) {
        loadSkillLineDBCs(assetManager);
    }

    // Rebuild categories if spell list changed or skill line data became available
    const auto& spells = gameHandler.getKnownSpells();
    bool skillLinesNowAvailable = !spellToSkillLine.empty() && !categorizedWithSkillLines;
    if (spells.size() != lastKnownSpellCount || skillLinesNowAvailable) {
        categorizeSpells(spells);
    }

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    float bookW = 380.0f;
    float bookH = std::min(560.0f, screenH - 100.0f);
    float bookX = screenW - bookW - 10.0f;
    float bookY = 80.0f;

    ImGui::SetNextWindowPos(ImVec2(bookX, bookY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(bookW, bookH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 250), ImVec2(screenW, screenH));

    bool windowOpen = open;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    if (ImGui::Begin("Spellbook", &windowOpen)) {
        // Search bar
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search spells...", searchFilter_, sizeof(searchFilter_));

        ImGui::Spacing();

        // Tab bar
        if (ImGui::BeginTabBar("SpellbookTabs")) {
            for (size_t tabIdx = 0; tabIdx < spellTabs.size(); tabIdx++) {
                const auto& tab = spellTabs[tabIdx];

                // Count visible spells (respecting search filter)
                int visibleCount = 0;
                for (const SpellInfo* info : tab.spells) {
                    if (containsCI(info->name, searchFilter_)) visibleCount++;
                }

                char tabLabel[128];
                snprintf(tabLabel, sizeof(tabLabel), "%s (%d)###sbtab%zu",
                         tab.name.c_str(), visibleCount, tabIdx);

                if (ImGui::BeginTabItem(tabLabel)) {
                    if (visibleCount == 0) {
                        if (searchFilter_[0])
                            ImGui::TextDisabled("No matching spells.");
                        else
                            ImGui::TextDisabled("No spells in this category.");
                    }

                    ImGui::BeginChild("SpellList", ImVec2(0, 0), true);

                    const float iconSize = 36.0f;
                    const float rowHeight = iconSize + 4.0f;

                    for (const SpellInfo* info : tab.spells) {
                        // Apply search filter
                        if (!containsCI(info->name, searchFilter_)) continue;

                        ImGui::PushID(static_cast<int>(info->spellId));

                        float cd = gameHandler.getSpellCooldown(info->spellId);
                        bool onCooldown = cd > 0.0f;
                        bool isPassive = info->isPassive();

                        VkDescriptorSet iconTex = getSpellIcon(info->iconId, assetManager);

                        // Row selectable
                        ImGui::Selectable("##row", false,
                            ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight));
                        bool rowHovered = ImGui::IsItemHovered();
                        bool rowClicked = ImGui::IsItemClicked(0);
                        ImVec2 rMin = ImGui::GetItemRectMin();
                        ImVec2 rMax = ImGui::GetItemRectMax();
                        auto* dl = ImGui::GetWindowDrawList();

                        // Hover highlight
                        if (rowHovered) {
                            dl->AddRectFilled(rMin, rMax, IM_COL32(255, 255, 255, 15), 3.0f);
                        }

                        // Icon background
                        ImVec2 iconMin = rMin;
                        ImVec2 iconMax(rMin.x + iconSize, rMin.y + iconSize);
                        dl->AddRectFilled(iconMin, iconMax, IM_COL32(25, 25, 35, 200), 3.0f);

                        // Icon
                        if (iconTex) {
                            ImU32 tint = (isPassive || onCooldown) ? IM_COL32(150, 150, 150, 255) : IM_COL32(255, 255, 255, 255);
                            dl->AddImage((ImTextureID)(uintptr_t)iconTex,
                                ImVec2(iconMin.x + 1, iconMin.y + 1),
                                ImVec2(iconMax.x - 1, iconMax.y - 1),
                                ImVec2(0, 0), ImVec2(1, 1), tint);
                        }

                        // Icon border
                        ImU32 borderCol;
                        if (isPassive) {
                            borderCol = IM_COL32(180, 180, 50, 200);  // Yellow for passive
                        } else if (onCooldown) {
                            borderCol = IM_COL32(120, 40, 40, 200);   // Red for cooldown
                        } else {
                            borderCol = IM_COL32(100, 100, 120, 200); // Default border
                        }
                        dl->AddRect(iconMin, iconMax, borderCol, 3.0f, 0, 1.5f);

                        // Cooldown overlay on icon
                        if (onCooldown) {
                            // Darkened sweep
                            dl->AddRectFilled(iconMin, iconMax, IM_COL32(0, 0, 0, 120), 3.0f);
                            // Cooldown text centered on icon
                            char cdBuf[16];
                            snprintf(cdBuf, sizeof(cdBuf), "%.0f", cd);
                            ImVec2 cdSize = ImGui::CalcTextSize(cdBuf);
                            ImVec2 cdPos(iconMin.x + (iconSize - cdSize.x) * 0.5f,
                                         iconMin.y + (iconSize - cdSize.y) * 0.5f);
                            dl->AddText(ImVec2(cdPos.x + 1, cdPos.y + 1), IM_COL32(0, 0, 0, 255), cdBuf);
                            dl->AddText(cdPos, IM_COL32(255, 80, 80, 255), cdBuf);
                        }

                        // Spell name
                        float textX = rMin.x + iconSize + 8.0f;
                        float nameY = rMin.y + 2.0f;

                        ImU32 nameCol;
                        if (isPassive) {
                            nameCol = IM_COL32(255, 255, 130, 255);  // Yellow-ish for passive
                        } else if (onCooldown) {
                            nameCol = IM_COL32(150, 150, 150, 255);
                        } else {
                            nameCol = IM_COL32(255, 255, 255, 255);
                        }
                        dl->AddText(ImVec2(textX, nameY), nameCol, info->name.c_str());

                        // Second line: rank or passive/cooldown indicator
                        float subY = nameY + ImGui::GetTextLineHeight() + 1.0f;
                        if (!info->rank.empty()) {
                            dl->AddText(ImVec2(textX, subY),
                                IM_COL32(150, 150, 150, 255), info->rank.c_str());
                        }
                        if (isPassive) {
                            float afterRank = textX;
                            if (!info->rank.empty()) {
                                afterRank += ImGui::CalcTextSize(info->rank.c_str()).x + 8.0f;
                            }
                            dl->AddText(ImVec2(afterRank, subY),
                                IM_COL32(200, 200, 80, 200), "Passive");
                        } else if (onCooldown) {
                            float afterRank = textX;
                            if (!info->rank.empty()) {
                                afterRank += ImGui::CalcTextSize(info->rank.c_str()).x + 8.0f;
                            }
                            char cdText[32];
                            snprintf(cdText, sizeof(cdText), "%.1fs", cd);
                            dl->AddText(ImVec2(afterRank, subY),
                                IM_COL32(255, 100, 100, 200), cdText);
                        }

                        // Interaction
                        if (rowHovered) {
                            // Start drag on click (not passive)
                            if (rowClicked && !isPassive) {
                                draggingSpell_ = true;
                                dragSpellId_ = info->spellId;
                                dragSpellIconTex_ = iconTex;
                            }

                            // Double-click to cast
                            if (ImGui::IsMouseDoubleClicked(0) && !isPassive && !onCooldown) {
                                draggingSpell_ = false;
                                dragSpellId_ = 0;
                                dragSpellIconTex_ = VK_NULL_HANDLE;
                                uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                                gameHandler.castSpell(info->spellId, target);
                            }

                            // Tooltip (only when not dragging)
                            if (!draggingSpell_) {
                                renderSpellTooltip(info, gameHandler);
                            }
                        }

                        ImGui::PopID();
                    }

                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    if (!windowOpen) {
        open = false;
    }

    // Render dragged spell icon at cursor
    if (draggingSpell_ && dragSpellId_ != 0) {
        ImVec2 mousePos = ImGui::GetMousePos();
        float dragSize = 36.0f;
        if (dragSpellIconTex_) {
            ImGui::GetForegroundDrawList()->AddImage(
                (ImTextureID)(uintptr_t)dragSpellIconTex_,
                ImVec2(mousePos.x - dragSize * 0.5f, mousePos.y - dragSize * 0.5f),
                ImVec2(mousePos.x + dragSize * 0.5f, mousePos.y + dragSize * 0.5f));
        } else {
            ImGui::GetForegroundDrawList()->AddRectFilled(
                ImVec2(mousePos.x - dragSize * 0.5f, mousePos.y - dragSize * 0.5f),
                ImVec2(mousePos.x + dragSize * 0.5f, mousePos.y + dragSize * 0.5f),
                IM_COL32(80, 80, 120, 180), 3.0f);
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            draggingSpell_ = false;
            dragSpellId_ = 0;
            dragSpellIconTex_ = VK_NULL_HANDLE;
        }
    }
}

}} // namespace wowee::ui
