#include "ui/game_screen.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
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
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>

#include <unordered_set>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;
    constexpr auto& kColorRed        = kRed;
    constexpr auto& kColorGreen      = kGreen;
    constexpr auto& kColorBrightGreen= kBrightGreen;
    constexpr auto& kColorYellow     = kYellow;
    constexpr auto& kColorGray       = kGray;
    constexpr auto& kColorDarkGray   = kDarkGray;

    // Abbreviated month names (indexed 0-11)
    constexpr const char* kMonthAbbrev[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    // Common ImGui window flags for popup dialogs
    const ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;

    bool raySphereIntersect(const wowee::rendering::Ray& ray, const glm::vec3& center, float radius, float& tOut) {
        glm::vec3 oc = ray.origin - center;
        float b = glm::dot(oc, ray.direction);
        float c = glm::dot(oc, oc) - radius * radius;
        float discriminant = b * b - c;
        if (discriminant < 0.0f) return false;
        float t = -b - std::sqrt(discriminant);
        if (t < 0.0f) t = -b + std::sqrt(discriminant);
        if (t < 0.0f) return false;
        tOut = t;
        return true;
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

    // Draw a four-edge screen vignette (gradient overlay along each edge).
    // Used for damage flash, low-health pulse, and level-up golden burst.
    void drawScreenEdgeVignette(uint8_t r, uint8_t g, uint8_t b,
                                int alpha, float thicknessRatio) {
        if (alpha <= 0) return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float W = ImGui::GetIO().DisplaySize.x;
        const float H = ImGui::GetIO().DisplaySize.y;
        const float thickness = std::min(W, H) * thicknessRatio;
        const ImU32 edgeCol = IM_COL32(r, g, b, alpha);
        const ImU32 fadeCol = IM_COL32(r, g, b, 0);
        // Top
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, thickness),
                                    edgeCol, edgeCol, fadeCol, fadeCol);
        // Bottom
        fg->AddRectFilledMultiColor(ImVec2(0, H - thickness), ImVec2(W, H),
                                    fadeCol, fadeCol, edgeCol, edgeCol);
        // Left
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(thickness, H),
                                    edgeCol, fadeCol, fadeCol, edgeCol);
        // Right
        fg->AddRectFilledMultiColor(ImVec2(W - thickness, 0), ImVec2(W, H),
                                    fadeCol, edgeCol, edgeCol, fadeCol);
    }

}

namespace wowee { namespace ui {

GameScreen::GameScreen() {
    loadSettings();
}

// Set UI services and propagate to child components
void GameScreen::setServices(const UIServices& services) {
    services_ = services;
    // Update legacy pointer for compatibility
    appearanceComposer_ = services.appearanceComposer;
    // Propagate to child panels
    chatPanel_.setServices(services);
    toastManager_.setServices(services);
    dialogManager_.setServices(services);
    settingsPanel_.setServices(services);
    combatUI_.setServices(services);
    socialPanel_.setServices(services);
    actionBarPanel_.setServices(services);
    windowManager_.setServices(services);
}

void GameScreen::render(game::GameHandler& gameHandler) {
    // Set up chat bubble callback (once) and cache game handler in ChatPanel
    chatPanel_.setupCallbacks(gameHandler);
    toastManager_.setupCallbacks(gameHandler);

    // Set up appearance-changed callback to refresh inventory preview (barber shop, etc.)
    if (!appearanceCallbackSet_) {
        gameHandler.setAppearanceChangedCallback([this]() {
            inventoryScreenCharGuid_ = 0;  // force preview re-sync on next frame
        });
        appearanceCallbackSet_ = true;
    }

    // Set up UI error frame callback (once)
    if (!uiErrorCallbackSet_) {
        gameHandler.setUIErrorCallback([this](const std::string& msg) {
            uiErrors_.push_back({msg, 0.0f});
            if (uiErrors_.size() > 5) uiErrors_.erase(uiErrors_.begin());
            // Play error sound for each new error (rate-limited by deque cap of 5)
            if (auto* ac = services_.audioCoordinator) {
                if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
            }
        });
        uiErrorCallbackSet_ = true;
    }

    // Flash the action bar button whose spell just failed (0.5 s red overlay).
    if (!castFailedCallbackSet_) {
        gameHandler.setSpellCastFailedCallback([this](uint32_t spellId) {
            if (spellId == 0) return;
            float now = static_cast<float>(ImGui::GetTime());
            actionBarPanel_.actionFlashEndTimes_[spellId] = now + actionBarPanel_.kActionFlashDuration;
        });
        castFailedCallbackSet_ = true;
    }

    // Apply UI transparency setting
    float prevAlpha = ImGui::GetStyle().Alpha;
    ImGui::GetStyle().Alpha = settingsPanel_.uiOpacity_;

    // Sync minimap opacity with UI opacity
    {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setOpacity(settingsPanel_.uiOpacity_);
            }
        }
    }

    // Apply initial settings when renderer becomes available
    if (!settingsPanel_.minimapSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                settingsPanel_.minimapRotate_ = false;
                settingsPanel_.pendingMinimapRotate = false;
                minimap->setRotateWithCamera(false);
                minimap->setSquareShape(settingsPanel_.minimapSquare_);
                settingsPanel_.minimapSettingsApplied_ = true;
            }
            if (auto* zm = renderer->getZoneManager()) {
                zm->setUseOriginalSoundtrack(settingsPanel_.pendingUseOriginalSoundtrack);
            }
            if (auto* tm = renderer->getTerrainManager()) {
                tm->setGroundClutterDensityScale(static_cast<float>(settingsPanel_.pendingGroundClutterDensity) / 100.0f);
            }
            // Restore mute state: save actual master volume first, then apply mute
            if (settingsPanel_.soundMuted_) {
                float actual = audio::AudioEngine::instance().getMasterVolume();
                settingsPanel_.preMuteVolume_ = (actual > 0.0f) ? actual
                    : static_cast<float>(settingsPanel_.pendingMasterVolume) / 100.0f;
                audio::AudioEngine::instance().setMasterVolume(0.0f);
            }
        }
    }

    // Apply saved volume settings once when audio managers first become available
    if (!settingsPanel_.volumeSettingsApplied_) {
        auto* ac = services_.audioCoordinator;
        if (ac && ac->getUiSoundManager()) {
            settingsPanel_.applyAudioVolumes(ac);
            settingsPanel_.volumeSettingsApplied_ = true;
        }
    }

    // Apply saved MSAA setting once when renderer is available
    if (!settingsPanel_.msaaSettingsApplied_ && settingsPanel_.pendingAntiAliasing > 0) {
        auto* renderer = services_.renderer;
        if (renderer) {
            static const VkSampleCountFlagBits aaSamples[] = {
                VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
            };
            renderer->setMsaaSamples(aaSamples[settingsPanel_.pendingAntiAliasing]);
            settingsPanel_.msaaSettingsApplied_ = true;
        }
    } else {
        settingsPanel_.msaaSettingsApplied_ = true;
    }

    // Apply saved FXAA setting once when renderer is available
    if (!settingsPanel_.fxaaSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            renderer->getPostProcessPipeline()->setFXAAEnabled(settingsPanel_.pendingFXAA);
            settingsPanel_.fxaaSettingsApplied_ = true;
        }
    }

    // Apply saved water refraction setting once when renderer is available
    if (!settingsPanel_.waterRefractionApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            renderer->setWaterRefractionEnabled(settingsPanel_.pendingWaterRefraction);
            settingsPanel_.waterRefractionApplied_ = true;
        }
    }

    // Apply saved normal mapping / POM settings once when WMO renderer is available
    if (!settingsPanel_.normalMapSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* wr = renderer->getWMORenderer()) {
                wr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                wr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                wr->setPOMEnabled(settingsPanel_.pendingPOM);
                wr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                    cr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                    cr->setPOMEnabled(settingsPanel_.pendingPOM);
                    cr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                }
                settingsPanel_.normalMapSettingsApplied_ = true;
            }
        }
    }

    // Apply saved upscaling setting once when renderer is available
    if (!settingsPanel_.fsrSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            static constexpr float fsrScales[] = { 0.77f, 0.67f, 0.59f, 1.00f };
            settingsPanel_.pendingFSRQuality = std::clamp(settingsPanel_.pendingFSRQuality, 0, 3);
            renderer->getPostProcessPipeline()->setFSRQuality(fsrScales[settingsPanel_.pendingFSRQuality]);
            renderer->getPostProcessPipeline()->setFSRSharpness(settingsPanel_.pendingFSRSharpness);
            renderer->getPostProcessPipeline()->setFSR2DebugTuning(settingsPanel_.pendingFSR2JitterSign, settingsPanel_.pendingFSR2MotionVecScaleX, settingsPanel_.pendingFSR2MotionVecScaleY);
            renderer->getPostProcessPipeline()->setAmdFsr3FramegenEnabled(settingsPanel_.pendingAMDFramegen);
            int effectiveMode = settingsPanel_.pendingUpscalingMode;

            // Defer FSR2/FSR3 activation until fully in-world to avoid
            // init issues during login/character selection screens.
            if (effectiveMode == 2 && gameHandler.getState() != game::WorldState::IN_WORLD) {
                renderer->setFSREnabled(false);
                renderer->setFSR2Enabled(false);
            } else {
                renderer->setFSREnabled(effectiveMode == 1);
                renderer->setFSR2Enabled(effectiveMode == 2);
                settingsPanel_.fsrSettingsApplied_ = true;
            }
        }
    }

    // Apply auto-loot / auto-sell settings to GameHandler every frame (cheap bool sync)
    gameHandler.setAutoLoot(settingsPanel_.pendingAutoLoot);
    gameHandler.setAutoSellGrey(settingsPanel_.pendingAutoSellGrey);
    gameHandler.setAutoRepair(settingsPanel_.pendingAutoRepair);

    // Sync chat auto-join settings to GameHandler
    gameHandler.chatAutoJoin.general = chatPanel_.chatAutoJoinGeneral;
    gameHandler.chatAutoJoin.trade = chatPanel_.chatAutoJoinTrade;
    gameHandler.chatAutoJoin.localDefense = chatPanel_.chatAutoJoinLocalDefense;
    gameHandler.chatAutoJoin.lfg = chatPanel_.chatAutoJoinLFG;
    gameHandler.chatAutoJoin.local = chatPanel_.chatAutoJoinLocal;

    // Process targeting input before UI windows
    processTargetInput(gameHandler);

    renderPlayerFrame(gameHandler);

    // Pet frame (below player frame, only when player has an active pet)
    if (gameHandler.hasPet()) {
        renderPetFrame(gameHandler);
    }

    // Auto-open pet rename modal when server signals the pet is renameable (first tame)
    if (gameHandler.consumePetRenameablePending()) {
        petRenameOpen_ = true;
        petRenameBuf_[0] = '\0';
    }

    // Totem frame (Shaman only, when any totem is active)
    if (gameHandler.getPlayerClass() == 7) {
        renderTotemFrame(gameHandler);
    }

    // Target frame (only when we have a target)
    if (gameHandler.hasTarget()) {
        renderTargetFrame(gameHandler);
    }

    // Focus target frame (only when we have a focus)
    if (gameHandler.hasFocus()) {
        renderFocusFrame(gameHandler);
    }

    // Render windows
    if (showPlayerInfo) {
        renderPlayerInfo(gameHandler);
    }

    if (showEntityWindow) {
        renderEntityList(gameHandler);
    }

    if (showChatWindow) {
        chatPanel_.getSpellIcon = [this](uint32_t id, pipeline::AssetManager* am) {
            return getSpellIcon(id, am);
        };
        chatPanel_.render(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        // Process slash commands that affect GameScreen state
        auto cmds = chatPanel_.consumeSlashCommands();
        if (cmds.showInspect) socialPanel_.showInspectWindow_ = true;
        if (cmds.toggleThreat) combatUI_.showThreatWindow_ = !combatUI_.showThreatWindow_;
        if (cmds.showBgScore) combatUI_.showBgScoreboard_ = !combatUI_.showBgScoreboard_;
        if (cmds.showGmTicket) windowManager_.showGmTicketWindow_ = true;
        if (cmds.showWho) socialPanel_.showWhoWindow_ = true;
        if (cmds.toggleCombatLog) combatUI_.showCombatLog_ = !combatUI_.showCombatLog_;
        if (cmds.takeScreenshot) takeScreenshot(gameHandler);
    }

    // ---- New UI elements ----
    actionBarPanel_.renderActionBar(gameHandler, settingsPanel_, chatPanel_,
        inventoryScreen, spellbookScreen, questLogScreen,
        [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); });
    actionBarPanel_.renderStanceBar(gameHandler, settingsPanel_, spellbookScreen,
        [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); });
    actionBarPanel_.renderBagBar(gameHandler, settingsPanel_, inventoryScreen);
    actionBarPanel_.renderXpBar(gameHandler, settingsPanel_);
    actionBarPanel_.renderRepBar(gameHandler, settingsPanel_);
    auto spellIconFn = [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); };
    combatUI_.renderCastBar(gameHandler, spellIconFn);
    renderMirrorTimers(gameHandler);
    combatUI_.renderCooldownTracker(gameHandler, settingsPanel_, spellIconFn);
    renderQuestObjectiveTracker(gameHandler);
    renderNameplates(gameHandler);  // player names always shown; NPC plates gated by showNameplates_
    combatUI_.renderBattlegroundScore(gameHandler);
    combatUI_.renderRaidWarningOverlay(gameHandler);
    combatUI_.renderCombatText(gameHandler);
    combatUI_.renderDPSMeter(gameHandler, settingsPanel_);
    renderDurabilityWarning(gameHandler);
    renderUIErrors(gameHandler, ImGui::GetIO().DeltaTime);
    toastManager_.renderEarlyToasts(ImGui::GetIO().DeltaTime, gameHandler);
    if (socialPanel_.showRaidFrames_) {
        socialPanel_.renderPartyFrames(gameHandler, chatPanel_, spellIconFn);
    }
    socialPanel_.renderBossFrames(gameHandler, spellbookScreen, spellIconFn);
    dialogManager_.renderDialogs(gameHandler, inventoryScreen, chatPanel_);
    socialPanel_.renderGuildRoster(gameHandler, chatPanel_);
    socialPanel_.renderSocialFrame(gameHandler, chatPanel_);
    combatUI_.renderBuffBar(gameHandler, spellbookScreen, spellIconFn);
    windowManager_.renderLootWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderGossipWindow(gameHandler, chatPanel_);
    windowManager_.renderQuestDetailsWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderQuestRequestItemsWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderQuestOfferRewardWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderVendorWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderTrainerWindow(gameHandler,
        [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); });
    windowManager_.renderBarberShopWindow(gameHandler);
    windowManager_.renderStableWindow(gameHandler);
    windowManager_.renderTaxiWindow(gameHandler);
    windowManager_.renderMailWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderMailComposeWindow(gameHandler, inventoryScreen);
    windowManager_.renderBankWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderGuildBankWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderAuctionHouseWindow(gameHandler, inventoryScreen, chatPanel_);
    socialPanel_.renderDungeonFinderWindow(gameHandler, chatPanel_);
    windowManager_.renderInstanceLockouts(gameHandler);
    socialPanel_.renderWhoWindow(gameHandler, chatPanel_);
    combatUI_.renderCombatLog(gameHandler, spellbookScreen);
    windowManager_.renderAchievementWindow(gameHandler);
    windowManager_.renderSkillsWindow(gameHandler);
    windowManager_.renderTitlesWindow(gameHandler);
    windowManager_.renderEquipSetWindow(gameHandler);
    windowManager_.renderGmTicketWindow(gameHandler);
    socialPanel_.renderInspectWindow(gameHandler, inventoryScreen);
    windowManager_.renderBookWindow(gameHandler);
    combatUI_.renderThreatWindow(gameHandler);
    combatUI_.renderBgScoreboard(gameHandler);
    if (showMinimap_) {
        renderMinimapMarkers(gameHandler);
    }
    windowManager_.renderLogoutCountdown(gameHandler);
    windowManager_.renderDeathScreen(gameHandler);
    windowManager_.renderReclaimCorpseButton(gameHandler);
    dialogManager_.renderLateDialogs(gameHandler);
    chatPanel_.renderBubbles(gameHandler);
    windowManager_.renderEscapeMenu(settingsPanel_);
    settingsPanel_.renderSettingsWindow(inventoryScreen, chatPanel_, [this]() { saveSettings(); });
    toastManager_.renderLateToasts(gameHandler);
    renderWeatherOverlay(gameHandler);

    renderWorldMap(gameHandler);

    questLogScreen.render(gameHandler, inventoryScreen);

    spellbookScreen.render(gameHandler, services_.assetManager);

    // Insert spell link into chat if player shift-clicked a spellbook entry
    {
        std::string pendingSpellLink = spellbookScreen.getAndClearPendingChatLink();
        if (!pendingSpellLink.empty()) {
            chatPanel_.insertChatLink(pendingSpellLink);
        }
    }

    // Talents (N key toggle handled inside)
    talentScreen.render(gameHandler);

    // Set up inventory screen asset manager + player appearance (re-init on character switch)
    {
        uint64_t activeGuid = gameHandler.getActiveCharacterGuid();
        if (activeGuid != 0 && activeGuid != inventoryScreenCharGuid_) {
            auto* am = services_.assetManager;
            if (am) {
                inventoryScreen.setAssetManager(am);
                const auto* ch = gameHandler.getActiveCharacter();
                if (ch) {
                    uint8_t skin = ch->appearanceBytes & 0xFF;
                    uint8_t face = (ch->appearanceBytes >> 8) & 0xFF;
                    uint8_t hairStyle = (ch->appearanceBytes >> 16) & 0xFF;
                    uint8_t hairColor = (ch->appearanceBytes >> 24) & 0xFF;
                    inventoryScreen.setPlayerAppearance(
                        ch->race, ch->gender, skin, face,
                        hairStyle, hairColor, ch->facialFeatures);
                    inventoryScreenCharGuid_ = activeGuid;
                }
            }
        }
    }

    // Set vendor mode before rendering inventory
    inventoryScreen.setVendorMode(gameHandler.isVendorWindowOpen(), &gameHandler);

    // Auto-open bags once when vendor window first opens
    if (gameHandler.isVendorWindowOpen()) {
        if (!windowManager_.vendorBagsOpened_) {
            windowManager_.vendorBagsOpened_ = true;
            if (inventoryScreen.isSeparateBags()) {
                inventoryScreen.openAllBags();
            } else if (!inventoryScreen.isOpen()) {
                inventoryScreen.setOpen(true);
            }
        }
    } else {
        windowManager_.vendorBagsOpened_ = false;
    }

    inventoryScreen.setGameHandler(&gameHandler);
    inventoryScreen.render(gameHandler.getInventory(), gameHandler.getMoneyCopper());

    // Character screen (C key toggle handled inside render())
    inventoryScreen.renderCharacterScreen(gameHandler);

    // Insert item link into chat if player shift-clicked any inventory/equipment slot
    {
        std::string pendingLink = inventoryScreen.getAndClearPendingChatLink();
        if (!pendingLink.empty()) {
            chatPanel_.insertChatLink(pendingLink);
        }
    }

    if (inventoryScreen.consumeEquipmentDirty() || gameHandler.consumeOnlineEquipmentDirty()) {
        updateCharacterGeosets(gameHandler.getInventory());
        updateCharacterTextures(gameHandler.getInventory());
        if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons();
        inventoryScreen.markPreviewDirty();
        // Update renderer weapon type for animation selection
        auto* r = services_.renderer;
        if (r) {
            const auto& mh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            const auto& oh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (mh.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(0, false);
            } else {
                // Polearms and staves use ATTACK_2H_LOOSE instead of ATTACK_2H
                bool is2HLoose = (mh.item.subclassName == "Polearm" || mh.item.subclassName == "Staff");
                bool isFist = (mh.item.subclassName == "Fist Weapon");
                bool isDagger = (mh.item.subclassName == "Dagger");
                bool hasOffHand = !oh.empty() &&
                    (oh.item.inventoryType == game::InvType::ONE_HAND ||
                     oh.item.subclassName == "Fist Weapon");
                bool hasShield = !oh.empty() && oh.item.inventoryType == game::InvType::SHIELD;
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(mh.item.inventoryType, is2HLoose, isFist, isDagger, hasOffHand, hasShield);
            }
            // Detect ranged weapon type from RANGED slot
            const auto& rangedSlot = gameHandler.getInventory().getEquipSlot(game::EquipSlot::RANGED);
            if (rangedSlot.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_BOW) {
                // subclassName distinguishes Bow vs Crossbow
                if (rangedSlot.item.subclassName == "Crossbow") {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::CROSSBOW);
                } else {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::BOW);
                }
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_GUN) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::GUN);
            } else if (rangedSlot.item.inventoryType == game::InvType::THROWN) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::THROWN);
            } else {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            }
        }
    }

    // Update renderer face-target position and selection circle
    auto* renderer = services_.renderer;
    if (renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->setInCombat(gameHandler.isInCombat() &&
                              !gameHandler.isPlayerDead() &&
                              !gameHandler.isPlayerGhost());
        if (auto* cr = renderer->getCharacterRenderer()) {
            uint32_t charInstId = renderer->getCharacterInstanceId();
            if (charInstId != 0) {
                const bool isGhost = gameHandler.isPlayerGhost();
                if (!ghostOpacityStateKnown_ ||
                    ghostOpacityLastState_ != isGhost ||
                    ghostOpacityLastInstanceId_ != charInstId) {
                    cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
                    ghostOpacityStateKnown_ = true;
                    ghostOpacityLastState_ = isGhost;
                    ghostOpacityLastInstanceId_ = charInstId;
                }
            }
        }
        static glm::vec3 targetGLPos;
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                // Prefer the renderer's actual instance position so the selection
                // circle tracks the rendered model (not a parallel entity-space
                // interpolator that can drift from the visual position).
                glm::vec3 instPos;
                if (core::Application::getInstance().getRenderPositionForGuid(target->getGuid(), instPos)) {
                    targetGLPos = instPos;
                    // Override Z with foot position to sit the circle on the ground.
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                } else {
                    // Fallback: entity game-logic position (no CharacterRenderer instance yet)
                    targetGLPos = core::coords::canonicalToRender(
                        glm::vec3(target->getX(), target->getY(), target->getZ()));
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                }
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(&targetGLPos);

                // Selection circle color: WoW-canonical level-based colors
                bool showSelectionCircle = false;
                glm::vec3 circleColor(1.0f, 1.0f, 0.3f); // default yellow
                float circleRadius = 1.5f;
                {
                    glm::vec3 boundsCenter;
                    float boundsRadius = 0.0f;
                    if (core::Application::getInstance().getRenderBoundsForGuid(target->getGuid(), boundsCenter, boundsRadius)) {
                        float r = boundsRadius * 1.1f;
                        circleRadius = std::min(std::max(r, 0.8f), 8.0f);
                    }
                }
                if (target->getType() == game::ObjectType::UNIT) {
                    showSelectionCircle = true;
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        circleColor = glm::vec3(0.5f, 0.5f, 0.5f); // gray (dead)
                    } else if (unit->isHostile() || gameHandler.isAggressiveTowardPlayer(target->getGuid())) {
                        uint32_t playerLv = gameHandler.getPlayerLevel();
                        uint32_t mobLv = unit->getLevel();
                        int32_t diff = static_cast<int32_t>(mobLv) - static_cast<int32_t>(playerLv);
                        if (game::GameHandler::killXp(playerLv, mobLv) == 0) {
                            circleColor = glm::vec3(0.6f, 0.6f, 0.6f); // grey
                        } else if (diff >= 10) {
                            circleColor = glm::vec3(1.0f, 0.1f, 0.1f); // red
                        } else if (diff >= 5) {
                            circleColor = glm::vec3(1.0f, 0.5f, 0.1f); // orange
                        } else if (diff >= -2) {
                            circleColor = glm::vec3(1.0f, 1.0f, 0.1f); // yellow
                        } else {
                            circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green
                        }
                    } else {
                        circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green (friendly)
                    }
                } else if (target->getType() == game::ObjectType::PLAYER) {
                    showSelectionCircle = true;
                    circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green (player)
                }
                if (showSelectionCircle) {
                    renderer->setSelectionCircle(targetGLPos, circleRadius, circleColor);
                } else {
                    renderer->clearSelectionCircle();
                }
            } else {
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
                renderer->clearSelectionCircle();
            }
        } else {
            if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
            renderer->clearSelectionCircle();
        }
    }

    // Screen edge damage flash — red vignette that fires on HP decrease
    {
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        uint32_t currentHp = 0;
        if (playerEntity && (playerEntity->getType() == game::ObjectType::PLAYER ||
                             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                currentHp = unit->getHealth();
        }

        // Detect HP drop (ignore transitions from 0 — entity just spawned or uninitialized)
        if (settingsPanel_.damageFlashEnabled_ && lastPlayerHp_ > 0 && currentHp < lastPlayerHp_ && currentHp > 0)
            damageFlashAlpha_ = 1.0f;
        lastPlayerHp_ = currentHp;

        // Fade out over ~0.5 seconds
        if (damageFlashAlpha_ > 0.0f) {
            damageFlashAlpha_ -= ImGui::GetIO().DeltaTime * 2.0f;
            if (damageFlashAlpha_ < 0.0f) damageFlashAlpha_ = 0.0f;
            drawScreenEdgeVignette(200, 0, 0,
                                   static_cast<int>(damageFlashAlpha_ * 100.0f), 0.12f);
        }
    }

    // Persistent low-health vignette — pulsing red edges when HP < 20%
    {
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        bool isDead = gameHandler.isPlayerDead();
        float hpPct = 1.0f;
        if (!isDead && playerEntity &&
            (playerEntity->getType() == game::ObjectType::PLAYER ||
             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                hpPct = static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth());
        }

        // Only show when alive and below 20% HP; intensity increases as HP drops
        if (settingsPanel_.lowHealthVignetteEnabled_ && !isDead && hpPct < 0.20f && hpPct > 0.0f) {
            // Base intensity from HP deficit (0 at 20%, 1 at 0%); pulse at ~1.5 Hz
            float danger = (0.20f - hpPct) / 0.20f;
            float pulse  = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.4f);
            int   alpha  = static_cast<int>(danger * pulse * 90.0f);  // max ~90 alpha, subtle
            drawScreenEdgeVignette(200, 0, 0, alpha, 0.15f);
        }
    }

    // Level-up golden burst overlay
    if (toastManager_.levelUpFlashAlpha > 0.0f) {
        toastManager_.levelUpFlashAlpha -= ImGui::GetIO().DeltaTime * 1.0f;  // fade over ~1 second
        if (toastManager_.levelUpFlashAlpha < 0.0f) toastManager_.levelUpFlashAlpha = 0.0f;

        const int alpha = static_cast<int>(toastManager_.levelUpFlashAlpha * 160.0f);
        drawScreenEdgeVignette(255, 210, 50, alpha, 0.18f);

        // "Level X!" text in the center during the first half of the animation
        if (toastManager_.levelUpFlashAlpha > 0.5f && toastManager_.levelUpDisplayLevel > 0) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const float W = ImGui::GetIO().DisplaySize.x;
            const float H = ImGui::GetIO().DisplaySize.y;
            char lvlText[32];
            snprintf(lvlText, sizeof(lvlText), "Level %u!", toastManager_.levelUpDisplayLevel);
            ImVec2 ts = ImGui::CalcTextSize(lvlText);
            float tx = (W - ts.x) * 0.5f;
            float ty = H * 0.35f;
            // Large shadow + bright gold text
            fg->AddText(nullptr, 28.0f, ImVec2(tx + 2, ty + 2), IM_COL32(0, 0, 0, alpha), lvlText);
            fg->AddText(nullptr, 28.0f, ImVec2(tx, ty), IM_COL32(255, 230, 80, alpha), lvlText);
        }
    }

    // Restore previous alpha
    ImGui::GetStyle().Alpha = prevAlpha;
}

void GameScreen::renderPlayerInfo(game::GameHandler& gameHandler) {
    ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::Begin("Player Info", &showPlayerInfo);

    const auto& movement = gameHandler.getMovementInfo();

    ImGui::Text("Position & Movement");
    ImGui::Separator();
    ImGui::Spacing();

    // Position
    ImGui::Text("Position:");
    ImGui::Indent();
    ImGui::Text("X: %.2f", movement.x);
    ImGui::Text("Y: %.2f", movement.y);
    ImGui::Text("Z: %.2f", movement.z);
    ImGui::Text("Orientation: %.2f rad (%.1f deg)", movement.orientation, movement.orientation * 180.0f / 3.14159f);
    ImGui::Unindent();

    ImGui::Spacing();

    // Movement flags
    ImGui::Text("Movement Flags: 0x%08X", movement.flags);
    ImGui::Text("Time: %u ms", movement.time);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Connection state
    ImGui::Text("Connection State:");
    ImGui::Indent();
    auto state = gameHandler.getState();
    switch (state) {
        case game::WorldState::IN_WORLD:
            ImGui::TextColored(kColorBrightGreen, "In World");
            break;
        case game::WorldState::AUTHENTICATED:
            ImGui::TextColored(kColorYellow, "Authenticated");
            break;
        case game::WorldState::ENTERING_WORLD:
            ImGui::TextColored(kColorYellow, "Entering World...");
            break;
        default:
            ImGui::TextColored(kColorRed, "State: %d", static_cast<int>(state));
            break;
    }
    ImGui::Unindent();

    ImGui::End();
}

void GameScreen::renderEntityList(game::GameHandler& gameHandler) {
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 290), ImGuiCond_FirstUseEver);
    ImGui::Begin("Entities", &showEntityWindow);

    const auto& entityManager = gameHandler.getEntityManager();
    const auto& entities = entityManager.getEntities();

    ImGui::Text("Entities in View: %zu", entities.size());
    ImGui::Separator();
    ImGui::Spacing();

    if (entities.empty()) {
        ImGui::TextDisabled("No entities in view");
    } else {
        // Entity table
        if (ImGui::BeginTable("EntitiesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            const auto& playerMovement = gameHandler.getMovementInfo();
            float playerX = playerMovement.x;
            float playerY = playerMovement.y;
            float playerZ = playerMovement.z;

            for (const auto& [guid, entity] : entities) {
                ImGui::TableNextRow();

                // GUID
                ImGui::TableSetColumnIndex(0);
                char guidStr[24];
                snprintf(guidStr, sizeof(guidStr), "0x%016llX", (unsigned long long)guid);
                ImGui::Text("%s", guidStr);

                // Type
                ImGui::TableSetColumnIndex(1);
                switch (entity->getType()) {
                    case game::ObjectType::PLAYER:
                        ImGui::TextColored(kColorBrightGreen, "Player");
                        break;
                    case game::ObjectType::UNIT:
                        ImGui::TextColored(kColorYellow, "Unit");
                        break;
                    case game::ObjectType::GAMEOBJECT:
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "GameObject");
                        break;
                    default:
                        ImGui::Text("Object");
                        break;
                }

                // Name (for players and units)
                ImGui::TableSetColumnIndex(2);
                if (entity->getType() == game::ObjectType::PLAYER) {
                    auto player = std::static_pointer_cast<game::Player>(entity);
                    ImGui::Text("%s", player->getName().c_str());
                } else if (entity->getType() == game::ObjectType::UNIT) {
                    auto unit = std::static_pointer_cast<game::Unit>(entity);
                    if (!unit->getName().empty()) {
                        ImGui::Text("%s", unit->getName().c_str());
                    } else {
                        ImGui::TextDisabled("--");
                    }
                } else {
                    ImGui::TextDisabled("--");
                }

                // Position
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.1f, %.1f, %.1f", entity->getX(), entity->getY(), entity->getZ());

                // Distance from player
                ImGui::TableSetColumnIndex(4);
                float dx = entity->getX() - playerX;
                float dy = entity->getY() - playerY;
                float dz = entity->getZ() - playerZ;
                float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                ImGui::Text("%.1f", distance);
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();
}

void GameScreen::processTargetInput(game::GameHandler& gameHandler) {
    auto& io = ImGui::GetIO();
    auto& input = core::Input::getInstance();

    // If the user is typing (or about to focus chat this frame), do not allow
    // A-Z or 1-0 shortcuts to fire.
    if (!io.WantTextInput && !chatPanel_.isChatInputActive() && input.isKeyJustPressed(SDL_SCANCODE_SLASH)) {
        chatPanel_.activateSlashInput();
    }
    if (!io.WantTextInput && !chatPanel_.isChatInputActive() &&
        KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_CHAT, true)) {
        chatPanel_.activateInput();
    }

    const bool textFocus = chatPanel_.isChatInputActive() || io.WantTextInput;

    // Tab targeting (when keyboard not captured by UI)
    if (!io.WantCaptureKeyboard) {
        // When typing in chat (or any text input), never treat keys as gameplay/UI shortcuts.
        if (!textFocus && input.isKeyJustPressed(SDL_SCANCODE_TAB)) {
            const auto& movement = gameHandler.getMovementInfo();
            gameHandler.tabTarget(movement.x, movement.y, movement.z);
        }

        if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SETTINGS, true)) {
            if (settingsPanel_.showSettingsWindow) {
                settingsPanel_.showSettingsWindow = false;
            } else if (windowManager_.showEscapeMenu) {
                windowManager_.showEscapeMenu = false;
                settingsPanel_.showEscapeSettingsNotice = false;
            } else if (gameHandler.isCasting()) {
                gameHandler.cancelCast();
            } else if (gameHandler.isLootWindowOpen()) {
                gameHandler.closeLoot();
            } else if (gameHandler.isGossipWindowOpen()) {
                gameHandler.closeGossip();
            } else if (gameHandler.isVendorWindowOpen()) {
                gameHandler.closeVendor();
            } else if (gameHandler.isBarberShopOpen()) {
                gameHandler.closeBarberShop();
            } else if (gameHandler.isBankOpen()) {
                gameHandler.closeBank();
            } else if (gameHandler.isTrainerWindowOpen()) {
                gameHandler.closeTrainer();
            } else if (gameHandler.isMailboxOpen()) {
                gameHandler.closeMailbox();
            } else if (gameHandler.isAuctionHouseOpen()) {
                gameHandler.closeAuctionHouse();
            } else if (gameHandler.isQuestDetailsOpen()) {
                gameHandler.declineQuest();
            } else if (gameHandler.isQuestOfferRewardOpen()) {
                gameHandler.closeQuestOfferReward();
            } else if (gameHandler.isQuestRequestItemsOpen()) {
                gameHandler.closeQuestRequestItems();
            } else if (gameHandler.isTradeOpen()) {
                gameHandler.cancelTrade();
            } else if (socialPanel_.showWhoWindow_) {
                socialPanel_.showWhoWindow_ = false;
            } else if (combatUI_.showCombatLog_) {
                combatUI_.showCombatLog_ = false;
            } else if (socialPanel_.showSocialFrame_) {
                socialPanel_.showSocialFrame_ = false;
            } else if (talentScreen.isOpen()) {
                talentScreen.setOpen(false);
            } else if (spellbookScreen.isOpen()) {
                spellbookScreen.setOpen(false);
            } else if (questLogScreen.isOpen()) {
                questLogScreen.setOpen(false);
            } else if (inventoryScreen.isCharacterOpen()) {
                inventoryScreen.toggleCharacter();
            } else if (inventoryScreen.isOpen()) {
                inventoryScreen.setOpen(false);
            } else if (showWorldMap_) {
                showWorldMap_ = false;
            } else {
                windowManager_.showEscapeMenu = true;
            }
        }

        if (!textFocus) {
            // Toggle character screen (C) and inventory/bags (I)
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_CHARACTER_SCREEN)) {
                const bool wasOpen = inventoryScreen.isCharacterOpen();
                inventoryScreen.toggleCharacter();
                if (!wasOpen && gameHandler.isConnected()) {
                    gameHandler.requestPlayedTime();
                }
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_INVENTORY)) {
                inventoryScreen.toggle();
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_NAMEPLATES)) {
                if (ImGui::GetIO().KeyShift)
                    settingsPanel_.showFriendlyNameplates_ = !settingsPanel_.showFriendlyNameplates_;
                else
                    showNameplates_ = !showNameplates_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_WORLD_MAP)) {
                showWorldMap_ = !showWorldMap_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_MINIMAP)) {
                showMinimap_ = !showMinimap_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_RAID_FRAMES)) {
                socialPanel_.showRaidFrames_ = !socialPanel_.showRaidFrames_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_ACHIEVEMENTS)) {
                windowManager_.showAchievementWindow_ = !windowManager_.showAchievementWindow_;
            }
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SKILLS)) {
                windowManager_.showSkillsWindow_ = !windowManager_.showSkillsWindow_;
            }

            // Toggle Titles window with H (hero/title screen — no conflicting keybinding)
            if (input.isKeyJustPressed(SDL_SCANCODE_H) && !ImGui::GetIO().WantCaptureKeyboard) {
                windowManager_.showTitlesWindow_ = !windowManager_.showTitlesWindow_;
            }

            // Screenshot (PrintScreen key)
            if (input.isKeyJustPressed(SDL_SCANCODE_PRINTSCREEN)) {
                takeScreenshot(gameHandler);
            }

            // Action bar keys (1-9, 0, -, =)
            static const SDL_Scancode actionBarKeys[] = {
                SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
                SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
                SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS
            };
            const bool shiftDown = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
            const bool ctrlDown  = input.isKeyPressed(SDL_SCANCODE_LCTRL)  || input.isKeyPressed(SDL_SCANCODE_RCTRL);
            const auto& bar = gameHandler.getActionBar();

            // Ctrl+1..Ctrl+8 → switch stance/form/presence (WoW default bindings).
            // Only fires for classes that use a stance bar; same slot ordering as
            // renderStanceBar: Warrior, DK, Druid, Rogue, Priest.
            if (ctrlDown) {
                static constexpr uint32_t warriorStances[]  = { 2457, 71, 2458 };
                static constexpr uint32_t dkPresences[]     = { 48266, 48263, 48265 };
                static constexpr uint32_t druidForms[]      = { 5487, 9634, 768, 783, 1066, 24858, 33891, 33943, 40120 };
                static constexpr uint32_t rogueForms[]      = { 1784 };
                static constexpr uint32_t priestForms[]     = { 15473 };
                const uint32_t* stArr = nullptr; int stCnt = 0;
                switch (gameHandler.getPlayerClass()) {
                    case 1:  stArr = warriorStances; stCnt = 3; break;
                    case 6:  stArr = dkPresences;    stCnt = 3; break;
                    case 11: stArr = druidForms;     stCnt = 9; break;
                    case 4:  stArr = rogueForms;     stCnt = 1; break;
                    case 5:  stArr = priestForms;    stCnt = 1; break;
                }
                if (stArr) {
                    const auto& known = gameHandler.getKnownSpells();
                    // Build available list (same order as UI)
                    std::vector<uint32_t> avail;
                    avail.reserve(stCnt);
                    for (int i = 0; i < stCnt; ++i)
                        if (known.count(stArr[i])) avail.push_back(stArr[i]);
                    // Ctrl+1 = first stance, Ctrl+2 = second, …
                    for (int i = 0; i < static_cast<int>(avail.size()) && i < 8; ++i) {
                        if (input.isKeyJustPressed(actionBarKeys[i]))
                            gameHandler.castSpell(avail[i]);
                    }
                }
            }

            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                if (!ctrlDown && input.isKeyJustPressed(actionBarKeys[i])) {
                    int slotIdx = shiftDown ? (game::GameHandler::SLOTS_PER_BAR + i) : i;
                    if (bar[slotIdx].type == game::ActionBarSlot::SPELL && bar[slotIdx].isReady()) {
                        uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                        gameHandler.castSpell(bar[slotIdx].id, target);
                    } else if (bar[slotIdx].type == game::ActionBarSlot::ITEM && bar[slotIdx].id != 0) {
                        gameHandler.useItemById(bar[slotIdx].id);
                    } else if (bar[slotIdx].type == game::ActionBarSlot::MACRO) {
                        chatPanel_.executeMacroText(gameHandler, inventoryScreen, spellbookScreen, questLogScreen, gameHandler.getMacroText(bar[slotIdx].id));
                    }
                }
            }
        }

    }

    // Cursor affordance: show hand cursor over interactable entities.
    if (!io.WantCaptureMouse) {
        auto* renderer = services_.renderer;
        auto* camera = renderer ? renderer->getCamera() : nullptr;
        auto* window = services_.window;
        if (camera && window) {
            glm::vec2 mousePos = input.getMousePosition();
            float screenW = static_cast<float>(window->getWidth());
            float screenH = static_cast<float>(window->getHeight());
            rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
            float closestT = 1e30f;
            bool hoverInteractable = false;
            for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                bool isGo   = (entity->getType() == game::ObjectType::GAMEOBJECT);
                bool isUnit = (entity->getType() == game::ObjectType::UNIT);
                bool isPlayer = (entity->getType() == game::ObjectType::PLAYER);
                if (!isGo && !isUnit && !isPlayer) continue;
                if (guid == gameHandler.getPlayerGuid()) continue; // skip self

                glm::vec3 hitCenter;
                float hitRadius = 0.0f;
                bool hasBounds = core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
                if (!hasBounds) {
                    hitRadius = isGo ? 2.5f : 1.8f;
                    hitCenter = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                    hitCenter.z += isGo ? 1.2f : 1.0f;
                } else {
                    hitRadius = std::max(hitRadius * 1.1f, 0.8f);
                }

                float hitT;
                if (raySphereIntersect(ray, hitCenter, hitRadius, hitT) && hitT < closestT) {
                    closestT = hitT;
                    hoverInteractable = true;
                }
            }
            if (hoverInteractable) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
    }

    // Left-click targeting: only on mouse-up if the mouse didn't drag (camera rotate)
    // Record press position on mouse-down
    if (!io.WantCaptureMouse && input.isMouseButtonJustPressed(SDL_BUTTON_LEFT) && !input.isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
        leftClickPressPos_ = input.getMousePosition();
        leftClickWasPress_ = true;
    }

    // On mouse-up, check if it was a click (not a drag)
    if (leftClickWasPress_ && input.isMouseButtonJustReleased(SDL_BUTTON_LEFT)) {
        leftClickWasPress_ = false;
        glm::vec2 releasePos = input.getMousePosition();
        glm::vec2 dragDelta = releasePos - leftClickPressPos_;
        float dragDistSq = glm::dot(dragDelta, dragDelta);
        constexpr float CLICK_THRESHOLD = 5.0f;  // pixels

        if (dragDistSq < CLICK_THRESHOLD * CLICK_THRESHOLD) {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;

            if (camera && window) {
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());

                rendering::Ray ray = camera->screenToWorldRay(leftClickPressPos_.x, leftClickPressPos_.y, screenW, screenH);

                float closestT = 1e30f;
                uint64_t closestGuid = 0;
                float closestHostileUnitT = 1e30f;
                uint64_t closestHostileUnitGuid = 0;

                const uint64_t myGuid = gameHandler.getPlayerGuid();
                for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                    auto t = entity->getType();
                    if (t != game::ObjectType::UNIT &&
                        t != game::ObjectType::PLAYER &&
                        t != game::ObjectType::GAMEOBJECT) continue;
                    if (guid == myGuid) continue;  // Don't target self

                    glm::vec3 hitCenter;
                    float hitRadius = 0.0f;
                    bool hasBounds = core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
                    if (!hasBounds) {
                        // Fallback hitbox based on entity type
                        float heightOffset = 1.5f;
                        hitRadius = 1.5f;
                        if (t == game::ObjectType::UNIT) {
                            auto unit = std::static_pointer_cast<game::Unit>(entity);
                            // Critters have very low max health (< 100)
                            if (unit->getMaxHealth() > 0 && unit->getMaxHealth() < 100) {
                                hitRadius = 0.5f;
                                heightOffset = 0.3f;
                            }
                        } else if (t == game::ObjectType::GAMEOBJECT) {
                            hitRadius = 2.5f;
                            heightOffset = 1.2f;
                        }
                        hitCenter = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                        hitCenter.z += heightOffset;
                    } else {
                        hitRadius = std::max(hitRadius * 1.1f, 0.6f);
                    }

                    float hitT;
                    if (raySphereIntersect(ray, hitCenter, hitRadius, hitT)) {
                        if (t == game::ObjectType::UNIT) {
                            auto unit = std::static_pointer_cast<game::Unit>(entity);
                            bool hostileUnit = unit->isHostile() || gameHandler.isAggressiveTowardPlayer(guid);
                            if (hostileUnit && hitT < closestHostileUnitT) {
                                closestHostileUnitT = hitT;
                                closestHostileUnitGuid = guid;
                            }
                        }
                        if (hitT < closestT) {
                            closestT = hitT;
                            closestGuid = guid;
                        }
                    }
                }

                // Prefer hostile monsters over nearby gameobjects/others when both are hittable.
                if (closestHostileUnitGuid != 0) {
                    closestGuid = closestHostileUnitGuid;
                }

                if (closestGuid != 0) {
                    gameHandler.setTarget(closestGuid);
                } else {
                    // Clicked empty space — deselect current target
                    gameHandler.clearTarget();
                }
            }
        }
    }

    // Right-click: select NPC (if needed) then interact / loot / auto-attack
    // Suppress when left button is held (both-button run)
    if (!io.WantCaptureMouse && input.isMouseButtonJustPressed(SDL_BUTTON_RIGHT) && !input.isMouseButtonPressed(SDL_BUTTON_LEFT)) {
        // If a gameobject is already targeted, prioritize interacting with that target
        // instead of re-picking under cursor (which can hit nearby decorative GOs).
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::GAMEOBJECT) {
                LOG_DEBUG("[GO-DIAG] Right-click: re-interacting with targeted GO 0x",
                            std::hex, target->getGuid(), std::dec);
                gameHandler.setTarget(target->getGuid());
                gameHandler.interactWithGameObject(target->getGuid());
                return;
            }
        }

        // If no target or right-clicking in world, try to pick one under cursor
        {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;
            if (camera && window) {
                // If a quest objective gameobject is under the cursor, prefer it over
                // hostile units so quest pickups (e.g. "Bundle of Wood") are reliable.
                std::unordered_set<uint32_t> questObjectiveGoEntries;
                {
                    const auto& ql = gameHandler.getQuestLog();
                    questObjectiveGoEntries.reserve(32);
                    for (const auto& q : ql) {
                        if (q.complete) continue;
                        for (const auto& obj : q.killObjectives) {
                            if (obj.npcOrGoId >= 0 || obj.required == 0) continue;
                            uint32_t entry = static_cast<uint32_t>(-obj.npcOrGoId);
                            uint32_t cur = 0;
                            auto it = q.killCounts.find(entry);
                            if (it != q.killCounts.end()) cur = it->second.first;
                            if (cur < obj.required) questObjectiveGoEntries.insert(entry);
                        }
                    }
                }

                glm::vec2 mousePos = input.getMousePosition();
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());
                rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
                float closestT = 1e30f;
                uint64_t closestGuid = 0;
                game::ObjectType closestType = game::ObjectType::OBJECT;
                float closestHostileUnitT = 1e30f;
                uint64_t closestHostileUnitGuid = 0;
                float closestQuestGoT = 1e30f;
                uint64_t closestQuestGoGuid = 0;
                float closestGoT = 1e30f;
                uint64_t closestGoGuid = 0;
                const uint64_t myGuid = gameHandler.getPlayerGuid();
                for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                    auto t = entity->getType();
                    if (t != game::ObjectType::UNIT &&
                        t != game::ObjectType::PLAYER &&
                        t != game::ObjectType::GAMEOBJECT)
                        continue;
                    if (guid == myGuid) continue;

                    glm::vec3 hitCenter;
                    float hitRadius = 0.0f;
                    bool hasBounds = core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
                    if (!hasBounds) {
                        float heightOffset = 1.5f;
                        hitRadius = 1.5f;
                        if (t == game::ObjectType::UNIT) {
                            auto unit = std::static_pointer_cast<game::Unit>(entity);
                            if (unit->getMaxHealth() > 0 && unit->getMaxHealth() < 100) {
                                hitRadius = 0.5f;
                                heightOffset = 0.3f;
                            }
                        } else if (t == game::ObjectType::GAMEOBJECT) {
                            hitRadius = 2.5f;
                            heightOffset = 1.2f;
                        }
                        hitCenter = core::coords::canonicalToRender(
                            glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                        hitCenter.z += heightOffset;
                        // Log each unique GO's raypick position once
                        if (t == game::ObjectType::GAMEOBJECT) {
                            static std::unordered_set<uint64_t> goPickLog;
                            if (goPickLog.insert(guid).second) {
                                auto go = std::static_pointer_cast<game::GameObject>(entity);
                                LOG_DEBUG("[GO-DIAG] Raypick GO: guid=0x", std::hex, guid, std::dec,
                                            " entry=", go->getEntry(), " name='", go->getName(),
                                            "' pos=(", entity->getX(), ",", entity->getY(), ",", entity->getZ(),
                                            ") center=(", hitCenter.x, ",", hitCenter.y, ",", hitCenter.z,
                                            ") r=", hitRadius);
                            }
                        }
                    } else {
                        hitRadius = std::max(hitRadius * 1.1f, 0.6f);
                    }

                    float hitT;
                    if (raySphereIntersect(ray, hitCenter, hitRadius, hitT)) {
                        if (t == game::ObjectType::UNIT) {
                            auto unit = std::static_pointer_cast<game::Unit>(entity);
                            bool hostileUnit = unit->isHostile() || gameHandler.isAggressiveTowardPlayer(guid);
                            if (hostileUnit && hitT < closestHostileUnitT) {
                                closestHostileUnitT = hitT;
                                closestHostileUnitGuid = guid;
                            }
                        }
                        if (t == game::ObjectType::GAMEOBJECT) {
                            if (hitT < closestGoT) {
                                closestGoT = hitT;
                                closestGoGuid = guid;
                            }
                            if (!questObjectiveGoEntries.empty()) {
                                auto go = std::static_pointer_cast<game::GameObject>(entity);
                                if (questObjectiveGoEntries.count(go->getEntry())) {
                                    if (hitT < closestQuestGoT) {
                                        closestQuestGoT = hitT;
                                        closestQuestGoGuid = guid;
                                    }
                                }
                            }
                        }
                        if (hitT < closestT) {
                            closestT = hitT;
                            closestGuid = guid;
                            closestType = t;
                        }
                    }
                }

                // Priority: quest GO > closer of (GO, hostile unit) > closest anything.
                if (closestQuestGoGuid != 0) {
                    closestGuid = closestQuestGoGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else if (closestGoGuid != 0 && closestHostileUnitGuid != 0) {
                    // Both a GO and hostile unit were hit — prefer whichever is closer.
                    if (closestGoT <= closestHostileUnitT) {
                        closestGuid = closestGoGuid;
                        closestType = game::ObjectType::GAMEOBJECT;
                    } else {
                        closestGuid = closestHostileUnitGuid;
                        closestType = game::ObjectType::UNIT;
                    }
                } else if (closestGoGuid != 0) {
                    closestGuid = closestGoGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else if (closestHostileUnitGuid != 0) {
                    closestGuid = closestHostileUnitGuid;
                    closestType = game::ObjectType::UNIT;
                }

                if (closestGuid != 0) {
                    if (closestType == game::ObjectType::GAMEOBJECT) {
                        LOG_DEBUG("[GO-DIAG] Right-click: raypick hit GO 0x",
                                    std::hex, closestGuid, std::dec);
                        gameHandler.setTarget(closestGuid);
                        gameHandler.interactWithGameObject(closestGuid);
                        return;
                    }
                    gameHandler.setTarget(closestGuid);
                }
            }
        }
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                if (target->getType() == game::ObjectType::UNIT) {
                    // Check if unit is dead (health == 0) → loot, otherwise interact/attack
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        gameHandler.lootTarget(target->getGuid());
                    } else {
                        // Interact with service NPCs; otherwise treat non-interactable living units
                        // as attackable fallback (covers bad faction-template classification).
                        auto isSpiritNpc = [&]() -> bool {
                            constexpr uint32_t NPC_FLAG_SPIRIT_GUIDE = 0x00004000;
                            constexpr uint32_t NPC_FLAG_SPIRIT_HEALER = 0x00008000;
                            if (unit->getNpcFlags() & (NPC_FLAG_SPIRIT_GUIDE | NPC_FLAG_SPIRIT_HEALER)) {
                                return true;
                            }
                            std::string name = unit->getName();
                            std::transform(name.begin(), name.end(), name.begin(),
                                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            return (name.find("spirit healer") != std::string::npos) ||
                                   (name.find("spirit guide") != std::string::npos);
                        };
                        bool allowSpiritInteract = (gameHandler.isPlayerDead() || gameHandler.isPlayerGhost()) && isSpiritNpc();
                        bool canInteractNpc = unit->isInteractable() || allowSpiritInteract;
                        bool shouldAttackByFallback = !canInteractNpc;
                        if (!unit->isHostile() && canInteractNpc) {
                            gameHandler.interactWithNpc(target->getGuid());
                        } else if (unit->isHostile() || shouldAttackByFallback) {
                            gameHandler.startAutoAttack(target->getGuid());
                        }
                    }
                } else if (target->getType() == game::ObjectType::GAMEOBJECT) {
                    gameHandler.interactWithGameObject(target->getGuid());
                } else if (target->getType() == game::ObjectType::PLAYER) {
                    // Right-click another player could start attack in PvP context
                }
            }
        }
    }
}


}} // namespace wowee::ui
