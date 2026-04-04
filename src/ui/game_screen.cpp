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
            renderer->setFXAAEnabled(settingsPanel_.pendingFXAA);
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
            renderer->setFSRQuality(fsrScales[settingsPanel_.pendingFSRQuality]);
            renderer->setFSRSharpness(settingsPanel_.pendingFSRSharpness);
            renderer->setFSR2DebugTuning(settingsPanel_.pendingFSR2JitterSign, settingsPanel_.pendingFSR2MotionVecScaleX, settingsPanel_.pendingFSR2MotionVecScaleY);
            renderer->setAmdFsr3FramegenEnabled(settingsPanel_.pendingAMDFramegen);
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
                r->setEquippedWeaponType(0, false);
            } else {
                // Polearms and staves use ATTACK_2H_LOOSE instead of ATTACK_2H
                bool is2HLoose = (mh.item.subclassName == "Polearm" || mh.item.subclassName == "Staff");
                bool isFist = (mh.item.subclassName == "Fist Weapon");
                bool isDagger = (mh.item.subclassName == "Dagger");
                bool hasOffHand = !oh.empty() &&
                    (oh.item.inventoryType == game::InvType::ONE_HAND ||
                     oh.item.subclassName == "Fist Weapon");
                bool hasShield = !oh.empty() && oh.item.inventoryType == game::InvType::SHIELD;
                r->setEquippedWeaponType(mh.item.inventoryType, is2HLoose, isFist, isDagger, hasOffHand, hasShield);
            }
            // Detect ranged weapon type from RANGED slot
            const auto& rangedSlot = gameHandler.getInventory().getEquipSlot(game::EquipSlot::RANGED);
            if (rangedSlot.empty()) {
                r->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_BOW) {
                // subclassName distinguishes Bow vs Crossbow
                if (rangedSlot.item.subclassName == "Crossbow")
                    r->setEquippedRangedType(rendering::RangedWeaponType::CROSSBOW);
                else
                    r->setEquippedRangedType(rendering::RangedWeaponType::BOW);
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_GUN) {
                r->setEquippedRangedType(rendering::RangedWeaponType::GUN);
            } else if (rangedSlot.item.inventoryType == game::InvType::THROWN) {
                r->setEquippedRangedType(rendering::RangedWeaponType::THROWN);
            } else {
                r->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            }
        }
    }

    // Update renderer face-target position and selection circle
    auto* renderer = services_.renderer;
    if (renderer) {
        renderer->setInCombat(gameHandler.isInCombat() &&
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
                renderer->setTargetPosition(&targetGLPos);

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
                renderer->setTargetPosition(nullptr);
                renderer->clearSelectionCircle();
            }
        } else {
            renderer->setTargetPosition(nullptr);
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

            // Draw four red gradient rectangles along each screen edge (vignette style)
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImGuiIO& io = ImGui::GetIO();
            const float W = io.DisplaySize.x;
            const float H = io.DisplaySize.y;
            const int alpha = static_cast<int>(damageFlashAlpha_ * 100.0f);
            const ImU32 edgeCol  = IM_COL32(200, 0, 0, alpha);
            const ImU32 fadeCol  = IM_COL32(200, 0, 0, 0);
            const float thickness = std::min(W, H) * 0.12f;

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
            if (alpha > 0) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                ImGuiIO& io = ImGui::GetIO();
                const float W = io.DisplaySize.x;
                const float H = io.DisplaySize.y;
                const float thickness = std::min(W, H) * 0.15f;
                const ImU32 edgeCol = IM_COL32(200, 0, 0, alpha);
                const ImU32 fadeCol = IM_COL32(200, 0, 0, 0);
                fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, thickness),
                                            edgeCol, edgeCol, fadeCol, fadeCol);
                fg->AddRectFilledMultiColor(ImVec2(0, H - thickness), ImVec2(W, H),
                                            fadeCol, fadeCol, edgeCol, edgeCol);
                fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(thickness, H),
                                            edgeCol, fadeCol, fadeCol, edgeCol);
                fg->AddRectFilledMultiColor(ImVec2(W - thickness, 0), ImVec2(W, H),
                                            fadeCol, edgeCol, edgeCol, fadeCol);
            }
        }
    }

    // Level-up golden burst overlay
    if (toastManager_.levelUpFlashAlpha > 0.0f) {
        toastManager_.levelUpFlashAlpha -= ImGui::GetIO().DeltaTime * 1.0f;  // fade over ~1 second
        if (toastManager_.levelUpFlashAlpha < 0.0f) toastManager_.levelUpFlashAlpha = 0.0f;

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x;
        const float H = io.DisplaySize.y;
        const int alpha = static_cast<int>(toastManager_.levelUpFlashAlpha * 160.0f);
        const ImU32 goldEdge = IM_COL32(255, 210, 50, alpha);
        const ImU32 goldFade = IM_COL32(255, 210, 50, 0);
        const float thickness = std::min(W, H) * 0.18f;

        // Four golden gradient edges
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, thickness),
                                    goldEdge, goldEdge, goldFade, goldFade);
        fg->AddRectFilledMultiColor(ImVec2(0, H - thickness), ImVec2(W, H),
                                    goldFade, goldFade, goldEdge, goldEdge);
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(thickness, H),
                                    goldEdge, goldFade, goldFade, goldEdge);
        fg->AddRectFilledMultiColor(ImVec2(W - thickness, 0), ImVec2(W, H),
                                    goldFade, goldEdge, goldEdge, goldFade);

        // "Level X!" text in the center during the first half of the animation
        if (toastManager_.levelUpFlashAlpha > 0.5f && toastManager_.levelUpDisplayLevel > 0) {
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
                static const uint32_t warriorStances[]  = { 2457, 71, 2458 };
                static const uint32_t dkPresences[]     = { 48266, 48263, 48265 };
                static const uint32_t druidForms[]      = { 5487, 9634, 768, 783, 1066, 24858, 33891, 33943, 40120 };
                static const uint32_t rogueForms[]      = { 1784 };
                static const uint32_t priestForms[]     = { 15473 };
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
                LOG_WARNING("[GO-DIAG] Right-click: re-interacting with targeted GO 0x",
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
                                LOG_WARNING("[GO-DIAG] Raypick GO: guid=0x", std::hex, guid, std::dec,
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
                        LOG_WARNING("[GO-DIAG] Right-click: raypick hit GO 0x",
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

void GameScreen::renderPlayerFrame(game::GameHandler& gameHandler) {
    bool isDead = gameHandler.isPlayerDead();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(250.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));
    const bool inCombatConfirmed = gameHandler.isInCombat();
    const bool attackIntentOnly = gameHandler.hasAutoAttackIntent() && !inCombatConfirmed;
    ImVec4 playerBorder = isDead
        ? kColorDarkGray
        : (inCombatConfirmed
            ? colors::kBrightRed
            : (attackIntentOnly
                ? ImVec4(1.0f, 0.7f, 0.2f, 1.0f)
                : ImVec4(0.4f, 0.4f, 0.4f, 1.0f)));
    ImGui::PushStyleColor(ImGuiCol_Border, playerBorder);

    if (ImGui::Begin("##PlayerFrame", nullptr, flags)) {
        // Use selected character info if available, otherwise defaults
        std::string playerName = "Adventurer";
        uint32_t playerLevel = 1;
        uint32_t playerHp = 100;
        uint32_t playerMaxHp = 100;

        const auto& characters = gameHandler.getCharacters();
        uint64_t activeGuid = gameHandler.getActiveCharacterGuid();
        const game::Character* activeChar = nullptr;
        for (const auto& c : characters) {
            if (c.guid == activeGuid) { activeChar = &c; break; }
        }
        if (!activeChar && !characters.empty()) activeChar = &characters[0];
        if (activeChar) {
            const auto& ch = *activeChar;
            playerName = ch.name;
            // Use live server level if available, otherwise character struct
            playerLevel = gameHandler.getPlayerLevel();
            if (playerLevel == 0) playerLevel = ch.level;
            playerMaxHp = 20 + playerLevel * 10;
            playerHp = playerMaxHp;
        }

        // Derive class color via shared helper
        ImVec4 classColor = activeChar
            ? classColorVec4(static_cast<uint8_t>(activeChar->characterClass))
            : kColorBrightGreen;

        // Name in class color — clickable for self-target, right-click for menu
        ImGui::PushStyleColor(ImGuiCol_Text, classColor);
        if (ImGui::Selectable(playerName.c_str(), false, 0, ImVec2(0, 0))) {
            gameHandler.setTarget(gameHandler.getPlayerGuid());
        }
        if (ImGui::BeginPopupContextItem("PlayerSelfCtx")) {
            ImGui::TextDisabled("%s", playerName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Open Character")) {
                inventoryScreen.setCharacterOpen(true);
            }
            if (ImGui::MenuItem("Toggle PvP")) {
                gameHandler.togglePvp();
            }
            ImGui::Separator();
            bool afk = gameHandler.isAfk();
            bool dnd = gameHandler.isDnd();
            if (ImGui::MenuItem(afk ? "Cancel AFK" : "Set AFK")) {
                gameHandler.toggleAfk();
            }
            if (ImGui::MenuItem(dnd ? "Cancel DND" : "Set DND")) {
                gameHandler.toggleDnd();
            }
            if (gameHandler.isInGroup()) {
                ImGui::Separator();
                if (ImGui::MenuItem("Leave Group")) {
                    gameHandler.leaveGroup();
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("Lv %u", playerLevel);
        if (isDead) {
            ImGui::SameLine();
            ImGui::TextColored(colors::kDarkRed, "DEAD");
        }
        // Group leader crown on self frame when you lead the party/raid
        if (gameHandler.isInGroup() &&
            gameHandler.getPartyData().leaderGuid == gameHandler.getPlayerGuid()) {
            ImGui::SameLine(0, 4);
            ImGui::TextColored(colors::kSymbolGold, "\xe2\x99\x9b");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("You are the group leader");
        }
        if (gameHandler.isAfk()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.3f, 1.0f), "<AFK>");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Away from keyboard — /afk to cancel");
        } else if (gameHandler.isDnd()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.2f, 1.0f), "<DND>");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Do not disturb — /dnd to cancel");
        }
        if (auto* ren = services_.renderer) {
            if (auto* cam = ren->getCameraController()) {
                if (cam->isAutoRunning()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "[Auto-Run]");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto-running — press ` or NumLock to stop");
                }
            }
        }
        if (inCombatConfirmed && !isDead) {
            float combatPulse = 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.2f * combatPulse, 0.2f * combatPulse, 1.0f), "[Combat]");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("You are in combat");
        }

        // Active title — shown in gold below the name/level line
        {
            int32_t titleBit = gameHandler.getChosenTitleBit();
            if (titleBit >= 0) {
                const std::string titleText = gameHandler.getFormattedTitle(
                    static_cast<uint32_t>(titleBit));
                if (!titleText.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 0.9f), "%s", titleText.c_str());
                }
            }
        }

        // Try to get real HP/mana from the player entity
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        if (playerEntity && (playerEntity->getType() == game::ObjectType::PLAYER || playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0) {
                playerHp = unit->getHealth();
                playerMaxHp = unit->getMaxHealth();
            }
        }

        // Health bar — color transitions green→yellow→red as HP drops
        float pct = static_cast<float>(playerHp) / static_cast<float>(playerMaxHp);
        ImVec4 hpColor;
        if (isDead) {
            hpColor = kColorDarkGray;
        } else if (pct > 0.5f) {
            hpColor = colors::kHealthGreen;              // green
        } else if (pct > 0.2f) {
            float t = (pct - 0.2f) / 0.3f;  // 0 at 20%, 1 at 50%
            hpColor = ImVec4(0.9f - 0.7f * t, 0.4f + 0.4f * t, 0.0f, 1.0f); // orange→yellow
        } else {
            // Critical — pulse red when < 20%
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.5f);
            hpColor = ImVec4(0.9f * pulse, 0.05f, 0.05f, 1.0f);    // pulsing red
        }
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hpColor);
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%u / %u", playerHp, playerMaxHp);
        ImGui::ProgressBar(pct, ImVec2(-1, 18), overlay);
        ImGui::PopStyleColor();

        // Mana/Power bar
        if (playerEntity && (playerEntity->getType() == game::ObjectType::PLAYER || playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            uint8_t powerType = unit->getPowerType();
            uint32_t power = unit->getPower();
            uint32_t maxPower = unit->getMaxPower();
            // Rage (1), Focus (2), Energy (3), and Runic Power (6) always cap at 100.
            // Show bar even if server hasn't sent UNIT_FIELD_MAXPOWER1 yet.
            if (maxPower == 0 && (powerType == 1 || powerType == 2 || powerType == 3 || powerType == 6)) maxPower = 100;
            if (maxPower > 0) {
                float mpPct = static_cast<float>(power) / static_cast<float>(maxPower);
                ImVec4 powerColor;
                switch (powerType) {
                    case 0: {
                        // Mana: pulse desaturated blue when critically low (< 20%)
                        if (mpPct < 0.2f) {
                            float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                            powerColor = ImVec4(0.1f, 0.1f, 0.8f * pulse, 1.0f);
                        } else {
                            powerColor = colors::kManaBlue;
                        }
                        break;
                    }
                    case 1: powerColor = colors::kDarkRed; break; // Rage (red)
                    case 2: powerColor = colors::kOrange; break; // Focus (orange)
                    case 3: powerColor = colors::kEnergyYellow; break; // Energy (yellow)
                    case 4: powerColor = colors::kHappinessGreen; break; // Happiness (green)
                    case 6: powerColor = colors::kRunicRed; break; // Runic Power (crimson)
                    case 7: powerColor = colors::kSoulShardPurple; break; // Soul Shards (purple)
                    default: powerColor = colors::kManaBlue; break;
                }
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, powerColor);
                char mpOverlay[64];
                snprintf(mpOverlay, sizeof(mpOverlay), "%u / %u", power, maxPower);
                ImGui::ProgressBar(mpPct, ImVec2(-1, 18), mpOverlay);
                ImGui::PopStyleColor();
            }
        }

        // Death Knight rune bar (class 6) — 6 colored squares with fill fraction
        if (gameHandler.getPlayerClass() == 6) {
            const auto& runes = gameHandler.getPlayerRunes();
            float dt = ImGui::GetIO().DeltaTime;

            ImGui::Spacing();
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float totalW  = ImGui::GetContentRegionAvail().x;
            float spacing = 3.0f;
            float squareW = (totalW - spacing * 5.0f) / 6.0f;
            float squareH = 14.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            for (int i = 0; i < 6; i++) {
                // Client-side prediction: advance fill over ~10s cooldown
                runeClientFill_[i] = runes[i].ready ? 1.0f
                    : std::min(runeClientFill_[i] + dt / 10.0f, runes[i].readyFraction + 0.02f);
                runeClientFill_[i] = std::clamp(runeClientFill_[i], 0.0f, runes[i].ready ? 1.0f : 0.97f);

                float x0 = cursor.x + i * (squareW + spacing);
                float y0 = cursor.y;
                float x1 = x0 + squareW;
                float y1 = y0 + squareH;

                // Background (dark)
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                  IM_COL32(30, 30, 30, 200), 2.0f);

                // Fill color by rune type
                ImVec4 fc;
                switch (runes[i].type) {
                    case game::GameHandler::RuneType::Blood:  fc = ImVec4(0.85f, 0.12f, 0.12f, 1.0f); break;
                    case game::GameHandler::RuneType::Unholy: fc = ImVec4(0.20f, 0.72f, 0.20f, 1.0f); break;
                    case game::GameHandler::RuneType::Frost:  fc = ImVec4(0.30f, 0.55f, 0.90f, 1.0f); break;
                    case game::GameHandler::RuneType::Death:  fc = ImVec4(0.55f, 0.20f, 0.70f, 1.0f); break;
                    default:                                  fc = ImVec4(0.6f,  0.6f,  0.6f,  1.0f); break;
                }
                float fillX = x0 + (x1 - x0) * runeClientFill_[i];
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(fillX, y1),
                                  ImGui::ColorConvertFloat4ToU32(fc), 2.0f);

                // Border
                ImU32 borderCol = runes[i].ready
                    ? IM_COL32(220, 220, 220, 180)
                    : IM_COL32(100, 100, 100, 160);
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), borderCol, 2.0f);
            }
            ImGui::Dummy(ImVec2(totalW, squareH));
        }

        // Combo point display — Rogue (4) and Druid (11) in Cat Form
        {
            uint8_t cls = gameHandler.getPlayerClass();
            const bool isRogue  = (cls == 4);
            const bool isDruid  = (cls == 11);
            if (isRogue || isDruid) {
                uint8_t cp = gameHandler.getComboPoints();
                if (cp > 0 || isRogue) {  // always show for rogue; only when non-zero for druid
                    ImGui::Spacing();
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    float totalW  = ImGui::GetContentRegionAvail().x;
                    constexpr int MAX_CP = 5;
                    constexpr float DOT_R = 7.0f;
                    constexpr float SPACING = 4.0f;
                    float totalDotsW = MAX_CP * (DOT_R * 2.0f) + (MAX_CP - 1) * SPACING;
                    float startX = cursor.x + (totalW - totalDotsW) * 0.5f;
                    float cy = cursor.y + DOT_R;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    for (int i = 0; i < MAX_CP; ++i) {
                        float cx = startX + i * (DOT_R * 2.0f + SPACING) + DOT_R;
                        ImU32 col = (i < static_cast<int>(cp))
                            ? IM_COL32(255, 210, 0, 240)   // bright gold — active
                            : IM_COL32(60,  60, 60, 160);  // dark — empty
                        dl->AddCircleFilled(ImVec2(cx, cy), DOT_R, col);
                        dl->AddCircle(ImVec2(cx, cy), DOT_R, IM_COL32(160, 140, 0, 180), 0, 1.5f);
                    }
                    ImGui::Dummy(ImVec2(totalW, DOT_R * 2.0f));
                }
            }
        }

        // Shaman totem bar (class 7) — 4 slots: Earth, Fire, Water, Air
        if (gameHandler.getPlayerClass() == 7) {
            static constexpr ImVec4 kTotemColors[] = {
                ImVec4(0.80f, 0.55f, 0.25f, 1.0f), // Earth — brown
                ImVec4(1.00f, 0.35f, 0.10f, 1.0f), // Fire  — orange-red
                ImVec4(0.20f, 0.55f, 0.90f, 1.0f), // Water — blue
                ImVec4(0.70f, 0.90f, 1.00f, 1.0f), // Air   — pale sky
            };
            static constexpr const char* kTotemNames[] = { "Earth", "Fire", "Water", "Air" };

            ImGui::Spacing();
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float totalW  = ImGui::GetContentRegionAvail().x;
            float spacing = 3.0f;
            float slotW   = (totalW - spacing * 3.0f) / 4.0f;
            float slotH   = 14.0f;
            ImDrawList* tdl = ImGui::GetWindowDrawList();

            for (int i = 0; i < game::GameHandler::NUM_TOTEM_SLOTS; i++) {
                const auto& ts = gameHandler.getTotemSlot(i);
                float x0 = cursor.x + i * (slotW + spacing);
                float y0 = cursor.y;
                float x1 = x0 + slotW;
                float y1 = y0 + slotH;

                // Background
                tdl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(20, 20, 20, 200), 2.0f);

                if (ts.active()) {
                    float rem = ts.remainingMs();
                    float frac = rem / static_cast<float>(ts.durationMs);
                    float fillX = x0 + (x1 - x0) * frac;
                    tdl->AddRectFilled(ImVec2(x0, y0), ImVec2(fillX, y1),
                                      ImGui::ColorConvertFloat4ToU32(kTotemColors[i]), 2.0f);
                    // Remaining seconds label
                    char secBuf[16];
                    snprintf(secBuf, sizeof(secBuf), "%.0f", rem / 1000.0f);
                    ImVec2 tsz = ImGui::CalcTextSize(secBuf);
                    float lx = x0 + (slotW - tsz.x) * 0.5f;
                    float ly = y0 + (slotH - tsz.y) * 0.5f;
                    tdl->AddText(ImVec2(lx + 1, ly + 1), IM_COL32(0, 0, 0, 180), secBuf);
                    tdl->AddText(ImVec2(lx, ly), IM_COL32(255, 255, 255, 230), secBuf);
                } else {
                    // Inactive — show element letter
                    const char* letter = kTotemNames[i];
                    char single[2] = { letter[0], '\0' };
                    ImVec2 tsz = ImGui::CalcTextSize(single);
                    float lx = x0 + (slotW - tsz.x) * 0.5f;
                    float ly = y0 + (slotH - tsz.y) * 0.5f;
                    tdl->AddText(ImVec2(lx, ly), IM_COL32(80, 80, 80, 200), single);
                }

                // Border
                ImU32 borderCol = ts.active()
                    ? ImGui::ColorConvertFloat4ToU32(kTotemColors[i])
                    : IM_COL32(60, 60, 60, 160);
                tdl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), borderCol, 2.0f);

                // Tooltip on hover
                ImGui::SetCursorScreenPos(ImVec2(x0, y0));
                char totemBtnId[16]; snprintf(totemBtnId, sizeof(totemBtnId), "##totem%d", i);
                ImGui::InvisibleButton(totemBtnId, ImVec2(slotW, slotH));
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (ts.active()) {
                        const std::string& spellNm = gameHandler.getSpellName(ts.spellId);
                        ImGui::TextColored(ImVec4(kTotemColors[i].x, kTotemColors[i].y,
                                                  kTotemColors[i].z, 1.0f),
                                           "%s Totem", kTotemNames[i]);
                        if (!spellNm.empty()) ImGui::Text("%s", spellNm.c_str());
                        ImGui::Text("%.1fs remaining", ts.remainingMs() / 1000.0f);
                    } else {
                        ImGui::TextDisabled("%s Totem (empty)", kTotemNames[i]);
                    }
                    ImGui::EndTooltip();
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + slotH + 2.0f));
        }
    }

    // Melee swing timer — shown when player is auto-attacking
    if (gameHandler.isAutoAttacking()) {
        const uint64_t lastSwingMs = gameHandler.getLastMeleeSwingMs();
        if (lastSwingMs > 0) {
            // Determine weapon speed from the equipped main-hand weapon
            uint32_t weaponDelayMs = 2000;  // Default: 2.0s unarmed
            const auto& mainSlot = gameHandler.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            if (!mainSlot.empty() && mainSlot.item.itemId != 0) {
                const auto* info = gameHandler.getItemInfo(mainSlot.item.itemId);
                if (info && info->delayMs > 0) {
                    weaponDelayMs = info->delayMs;
                }
            }

            // Compute elapsed since last swing
            uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            uint64_t elapsedMs = (nowMs >= lastSwingMs) ? (nowMs - lastSwingMs) : 0;

            // Clamp to weapon delay (cap at 1.0 so the bar fills but doesn't exceed)
            float pct = std::min(static_cast<float>(elapsedMs) / static_cast<float>(weaponDelayMs), 1.0f);

            // Light silver-orange color indicating auto-attack readiness
            ImVec4 swingColor = (pct >= 0.95f)
                ? ImVec4(1.0f, 0.75f, 0.15f, 1.0f)   // gold when ready to swing
                : ImVec4(0.65f, 0.55f, 0.40f, 1.0f);  // muted brown-orange while filling
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, swingColor);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.12f, 0.08f, 0.8f));
            char swingLabel[24];
            float remainSec = std::max(0.0f, (weaponDelayMs - static_cast<float>(elapsedMs)) / 1000.0f);
            if (pct >= 0.98f)
                snprintf(swingLabel, sizeof(swingLabel), "Swing!");
            else
                snprintf(swingLabel, sizeof(swingLabel), "%.1fs", remainSec);
            ImGui::ProgressBar(pct, ImVec2(-1.0f, 8.0f), swingLabel);
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GameScreen::renderPetFrame(game::GameHandler& gameHandler) {
    uint64_t petGuid = gameHandler.getPetGuid();
    if (petGuid == 0) return;

    auto petEntity = gameHandler.getEntityManager().getEntity(petGuid);
    if (!petEntity) return;
    auto* petUnit = petEntity->isUnit() ? static_cast<game::Unit*>(petEntity.get()) : nullptr;
    if (!petUnit) return;

    // Position below player frame. If in a group, push below party frames
    // (party frame at y=120, each member ~50px, up to 4 members → max ~320px + y=120 = ~440).
    // When not grouped, the player frame ends at ~110px so y=125 is fine.
    const int partyMemberCount = gameHandler.isInGroup()
        ? static_cast<int>(gameHandler.getPartyData().members.size()) : 0;
    float petY = (partyMemberCount > 0)
        ? 120.0f + partyMemberCount * 52.0f + 8.0f
        : 125.0f;
    ImGui::SetNextWindowPos(ImVec2(10.0f, petY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.1f, 0.08f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));

    if (ImGui::Begin("##PetFrame", nullptr, flags)) {
        const std::string& petName = petUnit->getName();
        uint32_t petLevel = petUnit->getLevel();

        // Name + level on one row — clicking the pet name targets it
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        char petLabel[96];
        snprintf(petLabel, sizeof(petLabel), "%s",
                 petName.empty() ? "Pet" : petName.c_str());
        if (ImGui::Selectable(petLabel, false, 0, ImVec2(0, 0))) {
            gameHandler.setTarget(petGuid);
        }
        // Right-click context menu on pet name
        if (ImGui::BeginPopupContextItem("PetNameCtx")) {
            ImGui::TextDisabled("%s", petLabel);
            ImGui::Separator();
            if (ImGui::MenuItem("Target Pet")) {
                gameHandler.setTarget(petGuid);
            }
            if (ImGui::MenuItem("Rename Pet")) {
                ImGui::CloseCurrentPopup();
                petRenameOpen_ = true;
                petRenameBuf_[0] = '\0';
            }
            if (ImGui::MenuItem("Dismiss Pet")) {
                gameHandler.dismissPet();
            }
            ImGui::EndPopup();
        }
        // Pet rename modal (opened via context menu)
        if (petRenameOpen_) {
            ImGui::OpenPopup("Rename Pet###PetRename");
            petRenameOpen_ = false;
        }
        if (ImGui::BeginPopupModal("Rename Pet###PetRename", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
            ImGui::Text("Enter new pet name (max 12 characters):");
            ImGui::SetNextItemWidth(180.0f);
            bool submitted = ImGui::InputText("##PetRenameInput", petRenameBuf_, sizeof(petRenameBuf_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("OK") || submitted) {
                std::string newName(petRenameBuf_);
                if (!newName.empty() && newName.size() <= 12) {
                    gameHandler.renamePet(newName);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        if (petLevel > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("Lv %u", petLevel);
        }

        // Health bar
        uint32_t hp    = petUnit->getHealth();
        uint32_t maxHp = petUnit->getMaxHealth();
        if (maxHp > 0) {
            float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
            ImVec4 petHpColor = pct > 0.5f ? colors::kHealthGreen
                              : pct > 0.2f ? ImVec4(0.9f, 0.6f, 0.0f, 1.0f)
                              :              ImVec4(0.9f, 0.15f, 0.15f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, petHpColor);
            char hpText[32];
            snprintf(hpText, sizeof(hpText), "%u/%u", hp, maxHp);
            ImGui::ProgressBar(pct, ImVec2(-1, 14), hpText);
            ImGui::PopStyleColor();
        }

        // Power/mana bar (hunters' pets use focus)
        uint8_t  powerType = petUnit->getPowerType();
        uint32_t power     = petUnit->getPower();
        uint32_t maxPower  = petUnit->getMaxPower();
        if (maxPower == 0 && (powerType == 1 || powerType == 2 || powerType == 3)) maxPower = 100;
        if (maxPower > 0) {
            float mpPct = static_cast<float>(power) / static_cast<float>(maxPower);
            ImVec4 powerColor;
            switch (powerType) {
                case 0: powerColor = colors::kManaBlue; break; // Mana
                case 1: powerColor = colors::kDarkRed; break; // Rage
                case 2: powerColor = colors::kOrange; break; // Focus (hunter pets)
                case 3: powerColor = colors::kEnergyYellow; break; // Energy
                default: powerColor = colors::kManaBlue; break;
            }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, powerColor);
            char mpText[32];
            snprintf(mpText, sizeof(mpText), "%u/%u", power, maxPower);
            ImGui::ProgressBar(mpPct, ImVec2(-1, 14), mpText);
            ImGui::PopStyleColor();
        }

        // Happiness bar — hunter pets store happiness as power type 4
        {
            uint32_t happiness = petUnit->getPowerByType(4);
            uint32_t maxHappiness = petUnit->getMaxPowerByType(4);
            if (maxHappiness > 0 && happiness > 0) {
                float hapPct = static_cast<float>(happiness) / static_cast<float>(maxHappiness);
                // Tier: < 33% = Unhappy (red), < 67% = Content (yellow), >= 67% = Happy (green)
                ImVec4 hapColor = hapPct >= 0.667f ? ImVec4(0.2f, 0.85f, 0.2f, 1.0f)
                                : hapPct >= 0.333f ? ImVec4(0.9f, 0.75f, 0.1f, 1.0f)
                                :                   ImVec4(0.85f, 0.2f, 0.2f, 1.0f);
                const char* hapLabel = hapPct >= 0.667f ? "Happy" : hapPct >= 0.333f ? "Content" : "Unhappy";
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hapColor);
                ImGui::ProgressBar(hapPct, ImVec2(-1, 8), hapLabel);
                ImGui::PopStyleColor();
            }
        }

        // Pet cast bar
        if (auto* pcs = gameHandler.getUnitCastState(petGuid)) {
            float castPct = (pcs->timeTotal > 0.0f)
                ? (pcs->timeTotal - pcs->timeRemaining) / pcs->timeTotal : 0.0f;
            // Orange color to distinguish from health/power bars
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.55f, 0.1f, 1.0f));
            char petCastLabel[48];
            const std::string& spellNm = gameHandler.getSpellName(pcs->spellId);
            if (!spellNm.empty())
                snprintf(petCastLabel, sizeof(petCastLabel), "%s (%.1fs)", spellNm.c_str(), pcs->timeRemaining);
            else
                snprintf(petCastLabel, sizeof(petCastLabel), "Casting... (%.1fs)", pcs->timeRemaining);
            ImGui::ProgressBar(castPct, ImVec2(-1, 10), petCastLabel);
            ImGui::PopStyleColor();
        }

        // Stance row: Passive / Defensive / Aggressive — with Dismiss right-aligned
        {
            static constexpr const char* kReactLabels[]     = { "Psv", "Def", "Agg" };
            static constexpr const char* kReactTooltips[]   = { "Passive", "Defensive", "Aggressive" };
            static constexpr ImVec4 kReactColors[]    = {
                colors::kLightBlue,  // passive  — blue
                ImVec4(0.3f, 0.85f, 0.3f, 1.0f), // defensive — green
                colors::kHostileRed,// aggressive — red
            };
            static constexpr ImVec4 kReactDimColors[] = {
                ImVec4(0.15f, 0.2f, 0.4f, 0.8f),
                ImVec4(0.1f, 0.3f, 0.1f, 0.8f),
                ImVec4(0.4f, 0.1f, 0.1f, 0.8f),
            };
            uint8_t curReact = gameHandler.getPetReact(); // 0=passive,1=defensive,2=aggressive

            // Find each react-type slot in the action bar by known built-in IDs:
            // 1=Passive, 4=Defensive, 6=Aggressive (WoW wire protocol)
            static const uint32_t kReactActionIds[] = { 1u, 4u, 6u };
            uint32_t reactSlotVals[3] = { 0, 0, 0 };
            const int slotTotal = game::GameHandler::PET_ACTION_BAR_SLOTS;
            for (int i = 0; i < slotTotal; ++i) {
                uint32_t sv = gameHandler.getPetActionSlot(i);
                uint32_t aid = sv & 0x00FFFFFFu;
                for (int r = 0; r < 3; ++r) {
                    if (aid == kReactActionIds[r]) { reactSlotVals[r] = sv; break; }
                }
            }

            for (int r = 0; r < 3; ++r) {
                if (r > 0) ImGui::SameLine(0.0f, 3.0f);
                bool active = (curReact == static_cast<uint8_t>(r));
                ImVec4 btnCol = active ? kReactColors[r] : kReactDimColors[r];
                ImGui::PushID(r + 1000);
                ImGui::PushStyleColor(ImGuiCol_Button,        btnCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kReactColors[r]);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kReactColors[r]);
                if (ImGui::Button(kReactLabels[r], ImVec2(34.0f, 16.0f))) {
                    // Use server-provided slot value if available; fall back to raw ID
                    uint32_t action = (reactSlotVals[r] != 0)
                        ? reactSlotVals[r]
                        : kReactActionIds[r];
                    gameHandler.sendPetAction(action, 0);
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", kReactTooltips[r]);
                ImGui::PopID();
            }

            // Dismiss button right-aligned on the same row
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 58.0f);
            if (ImGui::SmallButton("Dismiss")) {
                gameHandler.dismissPet();
            }
        }

        // Pet action bar — show up to 10 action slots from SMSG_PET_SPELLS
        {
            const int slotCount = game::GameHandler::PET_ACTION_BAR_SLOTS;
            // Filter to non-zero slots; lay them out as small icon/text buttons.
            // Raw slot value layout (WotLK 3.3.5): low 24 bits = spell/action ID,
            // high byte = flag (0x80=autocast on, 0x40=can-autocast, 0x0C=type).
            // Built-in commands: id=2 follow, id=3 stay/move, id=5 attack.
            auto* assetMgr = services_.assetManager;
            const float iconSz = 20.0f;
            const float spacing = 2.0f;
            ImGui::Separator();

            int rendered = 0;
            for (int i = 0; i < slotCount; ++i) {
                uint32_t slotVal = gameHandler.getPetActionSlot(i);
                if (slotVal == 0) continue;

                uint32_t actionId = slotVal & 0x00FFFFFFu;
                // Use the authoritative autocast set from SMSG_PET_SPELLS spell list flags.
                bool autocastOn   = gameHandler.isPetSpellAutocast(actionId);

                // Cooldown tracking for pet spells (actionId > 6 are spell IDs)
                float petCd = (actionId > 6) ? gameHandler.getSpellCooldown(actionId) : 0.0f;
                bool petOnCd = (petCd > 0.0f);

                ImGui::PushID(i);
                if (rendered > 0) ImGui::SameLine(0.0f, spacing);

                // Try to show spell icon; fall back to abbreviated text label.
                VkDescriptorSet iconTex = VK_NULL_HANDLE;
                const char* builtinLabel = nullptr;
                if      (actionId == 1) builtinLabel = "Psv";
                else if (actionId == 2) builtinLabel = "Fol";
                else if (actionId == 3) builtinLabel = "Sty";
                else if (actionId == 4) builtinLabel = "Def";
                else if (actionId == 5) builtinLabel = "Atk";
                else if (actionId == 6) builtinLabel = "Agg";
                else if (assetMgr)      iconTex = getSpellIcon(actionId, assetMgr);

                // Dim when on cooldown; tint green when autocast is on
                ImVec4 tint = petOnCd
                    ? ImVec4(0.35f, 0.35f, 0.35f, 0.7f)
                    : (autocastOn ? colors::kLightGreen : ui::colors::kWhite);
                bool clicked = false;
                if (iconTex) {
                    clicked = ImGui::ImageButton("##pa",
                        (ImTextureID)(uintptr_t)iconTex,
                        ImVec2(iconSz, iconSz),
                        ImVec2(0,0), ImVec2(1,1),
                        ImVec4(0.1f,0.1f,0.1f,0.9f), tint);
                } else {
                    char label[8];
                    if (builtinLabel) {
                        snprintf(label, sizeof(label), "%s", builtinLabel);
                    } else {
                        // Show first 3 chars of spell name or spell ID.
                        std::string nm = gameHandler.getSpellName(actionId);
                        if (nm.empty()) snprintf(label, sizeof(label), "?%u", actionId % 100);
                        else            snprintf(label, sizeof(label), "%.3s", nm.c_str());
                    }
                    ImVec4 btnCol = petOnCd ? ImVec4(0.1f,0.1f,0.15f,0.9f)
                                   : (autocastOn ? ImVec4(0.2f,0.5f,0.2f,0.9f)
                                                 : ImVec4(0.2f,0.2f,0.3f,0.9f));
                    ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
                    clicked = ImGui::Button(label, ImVec2(iconSz + 4.0f, iconSz));
                    ImGui::PopStyleColor();
                }

                // Cooldown overlay: dark fill + time text centered on the button
                if (petOnCd && !builtinLabel) {
                    ImVec2 bMin = ImGui::GetItemRectMin();
                    ImVec2 bMax = ImGui::GetItemRectMax();
                    auto* cdDL = ImGui::GetWindowDrawList();
                    cdDL->AddRectFilled(bMin, bMax, IM_COL32(0, 0, 0, 140));
                    char cdTxt[8];
                    if (petCd >= 60.0f)
                        snprintf(cdTxt, sizeof(cdTxt), "%dm", static_cast<int>(petCd / 60.0f));
                    else if (petCd >= 1.0f)
                        snprintf(cdTxt, sizeof(cdTxt), "%d", static_cast<int>(petCd));
                    else
                        snprintf(cdTxt, sizeof(cdTxt), "%.1f", petCd);
                    ImVec2 tsz = ImGui::CalcTextSize(cdTxt);
                    float cx = (bMin.x + bMax.x) * 0.5f;
                    float cy = (bMin.y + bMax.y) * 0.5f;
                    cdDL->AddText(ImVec2(cx - tsz.x * 0.5f, cy - tsz.y * 0.5f),
                                  IM_COL32(255, 255, 255, 230), cdTxt);
                }

                if (clicked && !petOnCd) {
                    // Send pet action; use current target for spells.
                    uint64_t targetGuid = (actionId > 5) ? gameHandler.getTargetGuid() : 0u;
                    gameHandler.sendPetAction(slotVal, targetGuid);
                }
                // Right-click toggles autocast for castable pet spells (actionId > 6)
                if (actionId > 6 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    gameHandler.togglePetSpellAutocast(actionId);
                }

                // Tooltip: rich spell info for pet spells, simple label for built-in commands
                if (ImGui::IsItemHovered()) {
                    if (builtinLabel) {
                        const char* tip = nullptr;
                        if      (actionId == 1) tip = "Passive";
                        else if (actionId == 2) tip = "Follow";
                        else if (actionId == 3) tip = "Stay";
                        else if (actionId == 4) tip = "Defensive";
                        else if (actionId == 5) tip = "Attack";
                        else if (actionId == 6) tip = "Aggressive";
                        if (tip) ImGui::SetTooltip("%s", tip);
                    } else if (actionId > 6) {
                        auto* spellAsset = services_.assetManager;
                        ImGui::BeginTooltip();
                        bool richOk = spellbookScreen.renderSpellInfoTooltip(actionId, gameHandler, spellAsset);
                        if (!richOk) {
                            std::string nm = gameHandler.getSpellName(actionId);
                            if (nm.empty()) nm = "Spell #" + std::to_string(actionId);
                            ImGui::Text("%s", nm.c_str());
                        }
                        ImGui::TextColored(autocastOn
                            ? kColorGreen
                            : kColorGray,
                            "Autocast: %s (right-click to toggle)", autocastOn ? "On" : "Off");
                        if (petOnCd) {
                            if (petCd >= 60.0f)
                                ImGui::TextColored(kColorRed,
                                    "Cooldown: %d min %d sec",
                                    static_cast<int>(petCd) / 60, static_cast<int>(petCd) % 60);
                            else
                                ImGui::TextColored(kColorRed,
                                    "Cooldown: %.1f sec", petCd);
                        }
                        ImGui::EndTooltip();
                    }
                }

                ImGui::PopID();
                ++rendered;
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ============================================================
// Totem Frame (Shaman — below pet frame / player frame)
// ============================================================

void GameScreen::renderTotemFrame(game::GameHandler& gameHandler) {
    // Only show if at least one totem is active
    bool anyActive = false;
    for (int i = 0; i < game::GameHandler::NUM_TOTEM_SLOTS; ++i) {
        if (gameHandler.getTotemSlot(i).active()) { anyActive = true; break; }
    }
    if (!anyActive) return;

    static constexpr struct { const char* name; ImU32 color; } kTotemInfo[4] = {
        { "Earth", IM_COL32(139, 90,  43, 255) },   // brown
        { "Fire",  IM_COL32(220, 80,  30, 255) },   // red-orange
        { "Water", IM_COL32( 30,120, 220, 255) },   // blue
        { "Air",   IM_COL32(180,220, 255, 255) },   // light blue
    };

    // Position: below pet frame / player frame, left side
    // Pet frame is at ~y=200 if active, player frame is at y=20; totem frame near y=300
    // We anchor relative to screen left edge like pet frame
    ImGui::SetNextWindowPos(ImVec2(8.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(130.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoTitleBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.08f, 0.06f, 0.88f));

    if (ImGui::Begin("##TotemFrame", nullptr, flags)) {
        ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.3f, 1.0f), "Totems");
        ImGui::Separator();

        for (int i = 0; i < game::GameHandler::NUM_TOTEM_SLOTS; ++i) {
            const auto& slot = gameHandler.getTotemSlot(i);
            if (!slot.active()) continue;

            ImGui::PushID(i);

            // Colored element dot
            ImVec2 dotPos = ImGui::GetCursorScreenPos();
            dotPos.x += 4.0f; dotPos.y += 6.0f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(dotPos.x + 4.0f, dotPos.y + 4.0f), 4.0f, kTotemInfo[i].color);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);

            // Totem name or spell name
            const std::string& spellName = gameHandler.getSpellName(slot.spellId);
            const char* displayName = spellName.empty() ? kTotemInfo[i].name : spellName.c_str();
            ImGui::Text("%s", displayName);

            // Duration countdown bar
            float remMs  = slot.remainingMs();
            float totMs  = static_cast<float>(slot.durationMs);
            float frac   = (totMs > 0.0f) ? std::min(remMs / totMs, 1.0f) : 0.0f;
            float remSec = remMs / 1000.0f;

            // Color bar with totem element tint
            ImVec4 barCol(
                static_cast<float>((kTotemInfo[i].color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                static_cast<float>((kTotemInfo[i].color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                static_cast<float>((kTotemInfo[i].color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                0.9f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%.0fs", remSec);
            ImGui::ProgressBar(frac, ImVec2(-1, 8), timeBuf);
            ImGui::PopStyleColor();

            ImGui::PopID();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void GameScreen::renderTargetFrame(game::GameHandler& gameHandler) {
    auto target = gameHandler.getTarget();
    if (!target) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    float frameW = 250.0f;
    float frameX = (screenW - frameW) / 2.0f;

    ImGui::SetNextWindowPos(ImVec2(frameX, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(frameW, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    // Determine hostility/level color for border and name (WoW-canonical)
    ImVec4 hostileColor(0.7f, 0.7f, 0.7f, 1.0f);
    if (target->getType() == game::ObjectType::PLAYER) {
        hostileColor = kColorBrightGreen;
    } else if (target->getType() == game::ObjectType::UNIT) {
        auto u = std::static_pointer_cast<game::Unit>(target);
        if (u->getHealth() == 0 && u->getMaxHealth() > 0) {
            hostileColor = kColorDarkGray;
        } else if (u->isHostile()) {
            // Check tapped-by-other: grey name for mobs tagged by someone else
            uint32_t tgtDynFlags = u->getDynamicFlags();
            bool tgtTapped = (tgtDynFlags & 0x0004) != 0 && (tgtDynFlags & 0x0008) == 0;
            if (tgtTapped) {
                hostileColor = kColorGray; // Grey — tapped by other
            } else {
            // WoW level-based color for hostile mobs
            uint32_t playerLv = gameHandler.getPlayerLevel();
            uint32_t mobLv = u->getLevel();
            if (mobLv == 0) {
                // Level 0 = unknown/?? (e.g. high-level raid bosses) — always skull red
                hostileColor = ImVec4(1.0f, 0.1f, 0.1f, 1.0f);
            } else {
                int32_t diff = static_cast<int32_t>(mobLv) - static_cast<int32_t>(playerLv);
                if (game::GameHandler::killXp(playerLv, mobLv) == 0) {
                    hostileColor = kColorGray; // Grey - no XP
                } else if (diff >= 10) {
                    hostileColor = ImVec4(1.0f, 0.1f, 0.1f, 1.0f); // Red - skull/very hard
                } else if (diff >= 5) {
                    hostileColor = ImVec4(1.0f, 0.5f, 0.1f, 1.0f); // Orange - hard
                } else if (diff >= -2) {
                    hostileColor = ImVec4(1.0f, 1.0f, 0.1f, 1.0f); // Yellow - even
                } else {
                    hostileColor = kColorBrightGreen; // Green - easy
                }
            }
            } // end tapped else
        } else {
            hostileColor = kColorBrightGreen; // Friendly
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.85f));
    const uint64_t targetGuid = target->getGuid();
    const bool confirmedCombatWithTarget = gameHandler.isInCombatWith(targetGuid);
    const bool intentTowardTarget =
        gameHandler.hasAutoAttackIntent() &&
        gameHandler.getAutoAttackTargetGuid() == targetGuid &&
        !confirmedCombatWithTarget;
    ImVec4 borderColor = ImVec4(hostileColor.x * 0.8f, hostileColor.y * 0.8f, hostileColor.z * 0.8f, 1.0f);
    if (confirmedCombatWithTarget) {
        float t = ImGui::GetTime();
        float pulse = (std::fmod(t, 0.6f) < 0.3f) ? 1.0f : 0.0f;
        borderColor = ImVec4(1.0f, 0.1f, 0.1f, pulse);
    } else if (intentTowardTarget) {
        borderColor = ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Border, borderColor);

    if (ImGui::Begin("##TargetFrame", nullptr, flags)) {
        // Raid mark icon (Star/Circle/Diamond/Triangle/Moon/Square/Cross/Skull)
        static constexpr struct { const char* sym; ImU32 col; } kRaidMarks[] = {
            { "\xe2\x98\x85", IM_COL32(255, 220,  50, 255) },  // 0 Star     (yellow)
            { "\xe2\x97\x8f", IM_COL32(255, 140,   0, 255) },  // 1 Circle   (orange)
            { "\xe2\x97\x86", IM_COL32(160,  32, 240, 255) },  // 2 Diamond  (purple)
            { "\xe2\x96\xb2", IM_COL32( 50, 200,  50, 255) },  // 3 Triangle (green)
            { "\xe2\x97\x8c", IM_COL32( 80, 160, 255, 255) },  // 4 Moon     (blue)
            { "\xe2\x96\xa0", IM_COL32( 50, 200, 220, 255) },  // 5 Square   (teal)
            { "\xe2\x9c\x9d", IM_COL32(255,  80,  80, 255) },  // 6 Cross    (red)
            { "\xe2\x98\xa0", IM_COL32(255, 255, 255, 255) },  // 7 Skull    (white)
        };
        uint8_t mark = gameHandler.getEntityRaidMark(target->getGuid());
        if (mark < game::GameHandler::kRaidMarkCount) {
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetCursorScreenPos(),
                kRaidMarks[mark].col, kRaidMarks[mark].sym);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18.0f);
        }

        // Entity name and type — Selectable so we can attach a right-click context menu
        std::string name = getEntityName(target);

        // Player targets: use class color instead of the generic green
        ImVec4 nameColor = hostileColor;
        if (target->getType() == game::ObjectType::PLAYER) {
            uint8_t cid = entityClassId(target.get());
            if (cid != 0) nameColor = classColorVec4(cid);
        }

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1,1,1,0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1,1,1,0.12f));
        ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_DontClosePopups,
                          ImVec2(ImGui::CalcTextSize(name.c_str()).x, 0));
        ImGui::PopStyleColor(4);

        // Right-click context menu on target frame
        if (ImGui::BeginPopupContextItem("##TargetFrameCtx")) {
            const bool isPlayer = (target->getType() == game::ObjectType::PLAYER);
            const uint64_t tGuid = target->getGuid();

            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();

            if (ImGui::MenuItem("Set Focus"))
                gameHandler.setFocus(tGuid);
            if (ImGui::MenuItem("Clear Target"))
                gameHandler.clearTarget();
            if (isPlayer) {
                ImGui::Separator();
                if (ImGui::MenuItem("Whisper")) {
                    chatPanel_.setWhisperTarget(name);
                }
                if (ImGui::MenuItem("Follow"))
                    gameHandler.followTarget();
                if (ImGui::MenuItem("Invite to Group"))
                    gameHandler.inviteToGroup(name);
                if (ImGui::MenuItem("Trade"))
                    gameHandler.initiateTrade(tGuid);
                if (ImGui::MenuItem("Duel"))
                    gameHandler.proposeDuel(tGuid);
                if (ImGui::MenuItem("Inspect")) {
                    gameHandler.inspectTarget();
                    socialPanel_.showInspectWindow_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Add Friend"))
                    gameHandler.addFriend(name);
                if (ImGui::MenuItem("Ignore"))
                    gameHandler.addIgnore(name);
                if (ImGui::MenuItem("Report Player"))
                    gameHandler.reportPlayer(tGuid, "Reported via UI");
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Set Raid Mark")) {
                for (int mi = 0; mi < 8; ++mi) {
                    if (ImGui::MenuItem(kRaidMarkNames[mi]))
                        gameHandler.setRaidMark(tGuid, static_cast<uint8_t>(mi));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Mark"))
                    gameHandler.setRaidMark(tGuid, 0xFF);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        // Group leader crown — golden ♛ when the targeted player is the party/raid leader
        if (gameHandler.isInGroup() && target->getType() == game::ObjectType::PLAYER) {
            if (gameHandler.getPartyData().leaderGuid == target->getGuid()) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(colors::kSymbolGold, "\xe2\x99\x9b");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Group Leader");
            }
        }

        // Quest giver indicator — "!" for available quests, "?" for completable quests
        {
            using QGS = game::QuestGiverStatus;
            QGS qgs = gameHandler.getQuestGiverStatus(target->getGuid());
            if (qgs == QGS::AVAILABLE) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(colors::kBrightGold, "!");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Has a quest available");
            } else if (qgs == QGS::AVAILABLE_LOW) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(kColorGray, "!");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Has a low-level quest available");
            } else if (qgs == QGS::REWARD || qgs == QGS::REWARD_REP) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(colors::kBrightGold, "?");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quest ready to turn in");
            } else if (qgs == QGS::INCOMPLETE) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(kColorGray, "?");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quest incomplete");
            }
        }

        // Creature subtitle (e.g. "<Warchief of the Horde>", "Captain of the Guard")
        if (target->getType() == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(target);
            const std::string sub = gameHandler.getCachedCreatureSubName(unit->getEntry());
            if (!sub.empty()) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.9f), "<%s>", sub.c_str());
            }
        }

        // Player guild name (e.g. "<My Guild>") — mirrors NPC subtitle styling
        if (target->getType() == game::ObjectType::PLAYER) {
            uint32_t guildId = gameHandler.getEntityGuildId(target->getGuid());
            if (guildId != 0) {
                const std::string& gn = gameHandler.lookupGuildName(guildId);
                if (!gn.empty()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.9f), "<%s>", gn.c_str());
                }
            }
        }

        // Right-click context menu on the target name
        if (ImGui::BeginPopupContextItem("##TargetNameCtx")) {
            const bool isPlayer = (target->getType() == game::ObjectType::PLAYER);
            const uint64_t tGuid = target->getGuid();

            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();

            if (ImGui::MenuItem("Set Focus")) {
                gameHandler.setFocus(tGuid);
            }
            if (ImGui::MenuItem("Clear Target")) {
                gameHandler.clearTarget();
            }
            if (isPlayer) {
                ImGui::Separator();
                if (ImGui::MenuItem("Whisper")) {
                    chatPanel_.setWhisperTarget(name);
                }
                if (ImGui::MenuItem("Follow")) {
                    gameHandler.followTarget();
                }
                if (ImGui::MenuItem("Invite to Group")) {
                    gameHandler.inviteToGroup(name);
                }
                if (ImGui::MenuItem("Trade")) {
                    gameHandler.initiateTrade(tGuid);
                }
                if (ImGui::MenuItem("Duel")) {
                    gameHandler.proposeDuel(tGuid);
                }
                if (ImGui::MenuItem("Inspect")) {
                    gameHandler.inspectTarget();
                    socialPanel_.showInspectWindow_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Add Friend")) {
                    gameHandler.addFriend(name);
                }
                if (ImGui::MenuItem("Ignore")) {
                    gameHandler.addIgnore(name);
                }
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Set Raid Mark")) {
                for (int mi = 0; mi < 8; ++mi) {
                    if (ImGui::MenuItem(kRaidMarkNames[mi]))
                        gameHandler.setRaidMark(tGuid, static_cast<uint8_t>(mi));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Mark"))
                    gameHandler.setRaidMark(tGuid, 0xFF);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        // Level (for units/players) — colored by difficulty
        if (target->getType() == game::ObjectType::UNIT || target->getType() == game::ObjectType::PLAYER) {
            auto unit = std::static_pointer_cast<game::Unit>(target);
            ImGui::SameLine();
            // Level color matches the hostility/difficulty color
            ImVec4 levelColor = hostileColor;
            if (target->getType() == game::ObjectType::PLAYER) {
                levelColor = ui::colors::kLightGray;
            }
            if (unit->getLevel() == 0)
                ImGui::TextColored(levelColor, "Lv ??");
            else
                ImGui::TextColored(levelColor, "Lv %u", unit->getLevel());
            // Classification badge: Elite / Rare Elite / Boss / Rare
            if (target->getType() == game::ObjectType::UNIT) {
                int rank = gameHandler.getCreatureRank(unit->getEntry());
                if (rank == 1) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[Elite]");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Elite — requires a group");
                } else if (rank == 2) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "[Rare Elite]");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rare Elite — uncommon spawn, group recommended");
                } else if (rank == 3) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(kColorRed, "[Boss]");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Boss — raid / dungeon boss");
                } else if (rank == 4) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "[Rare]");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rare — uncommon spawn with better loot");
                }
            }
            // Creature type label (Beast, Humanoid, Demon, etc.)
            if (target->getType() == game::ObjectType::UNIT) {
                uint32_t ctype = gameHandler.getCreatureType(unit->getEntry());
                const char* ctypeName = nullptr;
                switch (ctype) {
                    case 1:  ctypeName = "Beast"; break;
                    case 2:  ctypeName = "Dragonkin"; break;
                    case 3:  ctypeName = "Demon"; break;
                    case 4:  ctypeName = "Elemental"; break;
                    case 5:  ctypeName = "Giant"; break;
                    case 6:  ctypeName = "Undead"; break;
                    case 7:  ctypeName = "Humanoid"; break;
                    case 8:  ctypeName = "Critter"; break;
                    case 9:  ctypeName = "Mechanical"; break;
                    case 11: ctypeName = "Totem"; break;
                    case 12: ctypeName = "Non-combat Pet"; break;
                    case 13: ctypeName = "Gas Cloud"; break;
                    default: break;
                }
                if (ctypeName) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.9f), "(%s)", ctypeName);
                }
            }
            if (confirmedCombatWithTarget) {
                float cPulse = 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.2f * cPulse, 0.2f * cPulse, 1.0f), "[Attacking]");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Engaged in combat with this target");
            }

            // Health bar
            uint32_t hp = unit->getHealth();
            uint32_t maxHp = unit->getMaxHealth();
            if (maxHp > 0) {
                float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                    pct > 0.5f ? colors::kHealthGreen :
                    pct > 0.2f ? colors::kMidHealthYellow :
                                 colors::kLowHealthRed);

                char overlay[64];
                snprintf(overlay, sizeof(overlay), "%u / %u", hp, maxHp);
                ImGui::ProgressBar(pct, ImVec2(-1, 18), overlay);
                ImGui::PopStyleColor();
                // Target power bar (mana/rage/energy)
                uint8_t targetPowerType = unit->getPowerType();
                uint32_t targetPower = unit->getPower();
                uint32_t targetMaxPower = unit->getMaxPower();
                if (targetMaxPower == 0 && (targetPowerType == 1 || targetPowerType == 3)) targetMaxPower = 100;
                if (targetMaxPower > 0) {
                    float mpPct = static_cast<float>(targetPower) / static_cast<float>(targetMaxPower);
                    ImVec4 targetPowerColor;
                    switch (targetPowerType) {
                        case 0: targetPowerColor = colors::kManaBlue; break; // Mana (blue)
                        case 1: targetPowerColor = colors::kDarkRed; break; // Rage (red)
                        case 2: targetPowerColor = colors::kOrange; break; // Focus (orange)
                        case 3: targetPowerColor = colors::kEnergyYellow; break; // Energy (yellow)
                        case 4: targetPowerColor = colors::kHappinessGreen; break; // Happiness (green)
                        case 6: targetPowerColor = colors::kRunicRed; break; // Runic Power (crimson)
                        case 7: targetPowerColor = colors::kSoulShardPurple; break; // Soul Shards (purple)
                        default: targetPowerColor = colors::kManaBlue; break;
                    }
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, targetPowerColor);
                    char mpOverlay[64];
                    snprintf(mpOverlay, sizeof(mpOverlay), "%u / %u", targetPower, targetMaxPower);
                    ImGui::ProgressBar(mpPct, ImVec2(-1, 18), mpOverlay);
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::TextDisabled("No health data");
            }
        }

        // Combo points — shown when the player has combo points on this target
        {
            uint8_t cp = gameHandler.getComboPoints();
            if (cp > 0 && gameHandler.getComboTarget() == target->getGuid()) {
                const float dotSize = 12.0f;
                const float dotSpacing = 4.0f;
                const int maxCP = 5;
                float totalW = maxCP * dotSize + (maxCP - 1) * dotSpacing;
                float startX = (frameW - totalW) * 0.5f;
                ImGui::SetCursorPosX(startX);
                ImVec2 cursor = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (int ci = 0; ci < maxCP; ++ci) {
                    float cx = cursor.x + ci * (dotSize + dotSpacing) + dotSize * 0.5f;
                    float cy = cursor.y + dotSize * 0.5f;
                    if (ci < static_cast<int>(cp)) {
                        // Lit: yellow for 1-4, red glow for 5
                        ImU32 col = (cp >= 5)
                            ? IM_COL32(255, 50, 30, 255)
                            : IM_COL32(255, 210, 30, 255);
                        dl->AddCircleFilled(ImVec2(cx, cy), dotSize * 0.45f, col);
                        // Subtle glow
                        dl->AddCircle(ImVec2(cx, cy), dotSize * 0.5f, IM_COL32(255, 255, 200, 80), 0, 1.5f);
                    } else {
                        // Unlit: dark outline
                        dl->AddCircle(ImVec2(cx, cy), dotSize * 0.4f, IM_COL32(80, 80, 80, 180), 0, 1.5f);
                    }
                }
                ImGui::Dummy(ImVec2(totalW, dotSize + 2.0f));
            }
        }

        // Target cast bar — shown when the target is casting
        if (gameHandler.isTargetCasting()) {
            float castPct   = gameHandler.getTargetCastProgress();
            float castLeft  = gameHandler.getTargetCastTimeRemaining();
            uint32_t tspell = gameHandler.getTargetCastSpellId();
            bool interruptible = gameHandler.isTargetCastInterruptible();
            const std::string& castName = (tspell != 0) ? gameHandler.getSpellName(tspell) : "";
            // Color: interruptible = green (can Kick/CS), not interruptible = red, both pulse when >80%
            ImVec4 castBarColor;
            if (castPct > 0.8f) {
                float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                if (interruptible)
                    castBarColor = ImVec4(0.2f * pulse, 0.9f * pulse, 0.2f * pulse, 1.0f);  // green pulse
                else
                    castBarColor = ImVec4(1.0f * pulse, 0.1f * pulse, 0.1f * pulse, 1.0f);  // red pulse
            } else {
                castBarColor = interruptible ? colors::kCastGreen   // green = can interrupt
                                             : ImVec4(0.85f, 0.15f, 0.15f, 1.0f); // red = uninterruptible
            }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, castBarColor);
            char castLabel[72];
            if (!castName.empty())
                snprintf(castLabel, sizeof(castLabel), "%s (%.1fs)", castName.c_str(), castLeft);
            else if (tspell != 0)
                snprintf(castLabel, sizeof(castLabel), "Spell #%u (%.1fs)", tspell, castLeft);
            else
                snprintf(castLabel, sizeof(castLabel), "Casting... (%.1fs)", castLeft);
            {
                auto* tcastAsset = services_.assetManager;
                VkDescriptorSet tIcon = (tspell != 0 && tcastAsset)
                    ? getSpellIcon(tspell, tcastAsset) : VK_NULL_HANDLE;
                if (tIcon) {
                    ImGui::Image((ImTextureID)(uintptr_t)tIcon, ImVec2(14, 14));
                    ImGui::SameLine(0, 2);
                    ImGui::ProgressBar(castPct, ImVec2(-1, 14), castLabel);
                } else {
                    ImGui::ProgressBar(castPct, ImVec2(-1, 14), castLabel);
                }
            }
            ImGui::PopStyleColor();
        }

        // Target-of-Target (ToT): show who the current target is targeting
        {
            uint64_t totGuid = 0;
            const auto& tFields = target->getFields();
            auto itLo = tFields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
            if (itLo != tFields.end()) {
                totGuid = itLo->second;
                auto itHi = tFields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                if (itHi != tFields.end())
                    totGuid |= (static_cast<uint64_t>(itHi->second) << 32);
            }
            if (totGuid != 0) {
                auto totEnt = gameHandler.getEntityManager().getEntity(totGuid);
                std::string totName;
                ImVec4 totColor(0.7f, 0.7f, 0.7f, 1.0f);
                if (totGuid == gameHandler.getPlayerGuid()) {
                    auto playerEnt = gameHandler.getEntityManager().getEntity(totGuid);
                    totName = playerEnt ? getEntityName(playerEnt) : "You";
                    totColor = kColorBrightGreen;
                } else if (totEnt) {
                    totName = getEntityName(totEnt);
                    uint8_t cid = entityClassId(totEnt.get());
                    if (cid != 0) totColor = classColorVec4(cid);
                }
                if (!totName.empty()) {
                    ImGui::TextDisabled("▶");
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(totColor, "%s", totName.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Target's target: %s\nClick to target", totName.c_str());
                    }
                    if (ImGui::IsItemClicked()) {
                        gameHandler.setTarget(totGuid);
                    }

                    // Compact health bar for the ToT — essential for healers tracking boss target
                    if (totEnt) {
                        auto totUnit = std::dynamic_pointer_cast<game::Unit>(totEnt);
                        if (totUnit && totUnit->getMaxHealth() > 0) {
                            uint32_t totHp    = totUnit->getHealth();
                            uint32_t totMaxHp = totUnit->getMaxHealth();
                            float totPct = static_cast<float>(totHp) / static_cast<float>(totMaxHp);
                            ImVec4 totBarColor =
                                totPct > 0.5f ? colors::kCastGreen :
                                totPct > 0.2f ? ImVec4(0.75f, 0.75f, 0.2f, 1.0f) :
                                               ImVec4(0.75f, 0.2f, 0.2f, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, totBarColor);
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
                            char totOverlay[32];
                            snprintf(totOverlay, sizeof(totOverlay), "%u%%",
                                     static_cast<unsigned>(totPct * 100.0f + 0.5f));
                            ImGui::ProgressBar(totPct, ImVec2(-1, 10), totOverlay);
                            ImGui::PopStyleColor(2);
                        }
                    }
                }
            }
        }

        // Distance
        const auto& movement = gameHandler.getMovementInfo();
        float dx = target->getX() - movement.x;
        float dy = target->getY() - movement.y;
        float dz = target->getZ() - movement.z;
        float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        ImGui::TextDisabled("%.1f yd", distance);

        // Threat button (shown when in combat and threat data is available)
        if (gameHandler.getTargetThreatList()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.2f, 0.2f, 0.9f));
            if (ImGui::SmallButton("Threat")) combatUI_.showThreatWindow_ = !combatUI_.showThreatWindow_;
            ImGui::PopStyleColor(2);
        }

        // Target auras (buffs/debuffs)
        const auto& targetAuras = gameHandler.getTargetAuras();
        int activeAuras = 0;
        for (const auto& a : targetAuras) {
            if (!a.isEmpty()) activeAuras++;
        }
        if (activeAuras > 0) {
            auto* assetMgr = services_.assetManager;
            constexpr float ICON_SIZE = 24.0f;
            constexpr int ICONS_PER_ROW = 8;

            ImGui::Separator();

            // Build sorted index list: debuffs before buffs, shorter duration first
            uint64_t tNowSort = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            std::vector<size_t> sortedIdx;
            sortedIdx.reserve(targetAuras.size());
            for (size_t i = 0; i < targetAuras.size(); ++i)
                if (!targetAuras[i].isEmpty()) sortedIdx.push_back(i);
            std::sort(sortedIdx.begin(), sortedIdx.end(), [&](size_t a, size_t b) {
                const auto& aa = targetAuras[a]; const auto& ab = targetAuras[b];
                bool aDebuff = (aa.flags & 0x80) != 0;
                bool bDebuff = (ab.flags & 0x80) != 0;
                if (aDebuff != bDebuff) return aDebuff > bDebuff; // debuffs first
                int32_t ra = aa.getRemainingMs(tNowSort);
                int32_t rb = ab.getRemainingMs(tNowSort);
                // Permanent (-1) goes last; shorter remaining goes first
                if (ra < 0 && rb < 0) return false;
                if (ra < 0) return false;
                if (rb < 0) return true;
                return ra < rb;
            });

            int shown = 0;
            for (size_t si = 0; si < sortedIdx.size() && shown < 16; ++si) {
                size_t i = sortedIdx[si];
                const auto& aura = targetAuras[i];
                if (aura.isEmpty()) continue;

                if (shown > 0 && shown % ICONS_PER_ROW != 0) ImGui::SameLine();

                ImGui::PushID(static_cast<int>(10000 + i));

                bool isBuff = (aura.flags & 0x80) == 0;
                ImVec4 auraBorderColor;
                if (isBuff) {
                    auraBorderColor = ImVec4(0.2f, 0.8f, 0.2f, 0.9f);
                } else {
                    // Debuff: color by dispel type, matching player buff bar convention
                    uint8_t dt = gameHandler.getSpellDispelType(aura.spellId);
                    switch (dt) {
                        case 1:  auraBorderColor = ImVec4(0.15f, 0.50f, 1.00f, 0.9f); break; // magic: blue
                        case 2:  auraBorderColor = ImVec4(0.70f, 0.20f, 0.90f, 0.9f); break; // curse: purple
                        case 3:  auraBorderColor = ImVec4(0.55f, 0.30f, 0.10f, 0.9f); break; // disease: brown
                        case 4:  auraBorderColor = ImVec4(0.10f, 0.70f, 0.10f, 0.9f); break; // poison: green
                        default: auraBorderColor = ImVec4(0.80f, 0.20f, 0.20f, 0.9f); break; // other: red
                    }
                }

                VkDescriptorSet iconTex = VK_NULL_HANDLE;
                if (assetMgr) {
                    iconTex = getSpellIcon(aura.spellId, assetMgr);
                }

                if (iconTex) {
                    ImGui::PushStyleColor(ImGuiCol_Button, auraBorderColor);
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
                    ImGui::ImageButton("##taura",
                        (ImTextureID)(uintptr_t)iconTex,
                        ImVec2(ICON_SIZE - 2, ICON_SIZE - 2));
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, auraBorderColor);
                    const std::string& tAuraName = gameHandler.getSpellName(aura.spellId);
                    char label[32];
                    if (!tAuraName.empty())
                        snprintf(label, sizeof(label), "%.6s", tAuraName.c_str());
                    else
                        snprintf(label, sizeof(label), "%u", aura.spellId);
                    ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
                    ImGui::PopStyleColor();
                }

                // Compute remaining once for overlay + tooltip
                uint64_t tNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t tRemainMs = aura.getRemainingMs(tNowMs);

                // Clock-sweep overlay (elapsed = dark area, WoW style)
                if (tRemainMs > 0 && aura.maxDurationMs > 0) {
                    ImVec2 tIconMin = ImGui::GetItemRectMin();
                    ImVec2 tIconMax = ImGui::GetItemRectMax();
                    float tcx = (tIconMin.x + tIconMax.x) * 0.5f;
                    float tcy = (tIconMin.y + tIconMax.y) * 0.5f;
                    float tR  = (tIconMax.x - tIconMin.x) * 0.5f;
                    float tTot = static_cast<float>(aura.maxDurationMs);
                    float tFrac = std::clamp(
                        1.0f - static_cast<float>(tRemainMs) / tTot, 0.0f, 1.0f);
                    if (tFrac > 0.005f) {
                        constexpr int TSEGS = 24;
                        float tSa = -IM_PI * 0.5f;
                        float tEa = tSa + tFrac * 2.0f * IM_PI;
                        ImVec2 tPts[TSEGS + 2];
                        tPts[0] = ImVec2(tcx, tcy);
                        for (int s = 0; s <= TSEGS; ++s) {
                            float a = tSa + (tEa - tSa) * s / static_cast<float>(TSEGS);
                            tPts[s + 1] = ImVec2(tcx + std::cos(a) * tR,
                                                 tcy + std::sin(a) * tR);
                        }
                        ImGui::GetWindowDrawList()->AddConvexPolyFilled(
                            tPts, TSEGS + 2, IM_COL32(0, 0, 0, 145));
                    }
                }

                // Duration countdown overlay
                if (tRemainMs > 0) {
                    ImVec2 iconMin = ImGui::GetItemRectMin();
                    ImVec2 iconMax = ImGui::GetItemRectMax();
                    char timeStr[12];
                    int secs = (tRemainMs + 999) / 1000;
                    if (secs >= 3600)
                        snprintf(timeStr, sizeof(timeStr), "%dh", secs / 3600);
                    else if (secs >= 60)
                        snprintf(timeStr, sizeof(timeStr), "%d:%02d", secs / 60, secs % 60);
                    else
                        snprintf(timeStr, sizeof(timeStr), "%d", secs);
                    ImVec2 textSize = ImGui::CalcTextSize(timeStr);
                    float cx = iconMin.x + (iconMax.x - iconMin.x - textSize.x) * 0.5f;
                    float cy = iconMax.y - textSize.y - 1.0f;
                    // Color by urgency (matches player buff bar)
                    ImU32 tTimerColor;
                    if (tRemainMs < 10000) {
                        float pulse = 0.7f + 0.3f * std::sin(
                            static_cast<float>(ImGui::GetTime()) * 6.0f);
                        tTimerColor = IM_COL32(
                            static_cast<int>(255 * pulse),
                            static_cast<int>(80 * pulse),
                            static_cast<int>(60 * pulse), 255);
                    } else if (tRemainMs < 30000) {
                        tTimerColor = IM_COL32(255, 165, 0, 255);
                    } else {
                        tTimerColor = IM_COL32(255, 255, 255, 255);
                    }
                    ImGui::GetWindowDrawList()->AddText(ImVec2(cx + 1, cy + 1),
                        IM_COL32(0, 0, 0, 200), timeStr);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(cx, cy),
                        tTimerColor, timeStr);
                }

                // Stack / charge count — upper-left corner
                if (aura.charges > 1) {
                    ImVec2 iconMin = ImGui::GetItemRectMin();
                    char chargeStr[8];
                    snprintf(chargeStr, sizeof(chargeStr), "%u", static_cast<unsigned>(aura.charges));
                    ImGui::GetWindowDrawList()->AddText(ImVec2(iconMin.x + 3, iconMin.y + 3),
                        IM_COL32(0, 0, 0, 200), chargeStr);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(iconMin.x + 2, iconMin.y + 2),
                        IM_COL32(255, 220, 50, 255), chargeStr);
                }

                // Tooltip: rich spell info + remaining duration
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    bool richOk = spellbookScreen.renderSpellInfoTooltip(aura.spellId, gameHandler, assetMgr);
                    if (!richOk) {
                        std::string name = spellbookScreen.lookupSpellName(aura.spellId, assetMgr);
                        if (name.empty()) name = "Spell #" + std::to_string(aura.spellId);
                        ImGui::Text("%s", name.c_str());
                    }
                    renderAuraRemaining(tRemainMs);
                    ImGui::EndTooltip();
                }

                ImGui::PopID();
                shown++;
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    // ---- Target-of-Target (ToT) mini frame ----
    // Read target's current target from UNIT_FIELD_TARGET_LO/HI update fields
    if (target) {
        const auto& fields = target->getFields();
        uint64_t totGuid = 0;
        auto loIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
        if (loIt != fields.end()) {
            totGuid = loIt->second;
            auto hiIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
            if (hiIt != fields.end())
                totGuid |= (static_cast<uint64_t>(hiIt->second) << 32);
        }

        if (totGuid != 0) {
            auto totEntity = gameHandler.getEntityManager().getEntity(totGuid);
            if (totEntity) {
                // Position ToT frame just below and right-aligned with the target frame
                float totW = 160.0f;
                float totX = (screenW - totW) / 2.0f + (frameW - totW);
                ImGui::SetNextWindowPos(ImVec2(totX, 30.0f + 130.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(totW, 0.0f), ImGuiCond_Always);

                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.80f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 0.7f));

                if (ImGui::Begin("##ToTFrame", nullptr,
                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar)) {
                    std::string totName = getEntityName(totEntity);
                    // Class color for players; gray for NPCs
                    ImVec4 totNameColor = colors::kSilver;
                    if (totEntity->getType() == game::ObjectType::PLAYER) {
                        uint8_t cid = entityClassId(totEntity.get());
                        if (cid != 0) totNameColor = classColorVec4(cid);
                    }
                    // Selectable so we can attach a right-click context menu
                    ImGui::PushStyleColor(ImGuiCol_Text, totNameColor);
                    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0,0,0,0));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1,1,1,0.08f));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1,1,1,0.12f));
                    if (ImGui::Selectable(totName.c_str(), false,
                            ImGuiSelectableFlags_DontClosePopups,
                            ImVec2(ImGui::CalcTextSize(totName.c_str()).x, 0))) {
                        gameHandler.setTarget(totGuid);
                    }
                    ImGui::PopStyleColor(4);

                    if (ImGui::BeginPopupContextItem("##ToTCtx")) {
                        ImGui::TextDisabled("%s", totName.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Target"))
                            gameHandler.setTarget(totGuid);
                        if (ImGui::MenuItem("Set Focus"))
                            gameHandler.setFocus(totGuid);
                        ImGui::EndPopup();
                    }

                    if (totEntity->getType() == game::ObjectType::UNIT ||
                        totEntity->getType() == game::ObjectType::PLAYER) {
                        auto totUnit = std::static_pointer_cast<game::Unit>(totEntity);
                        if (totUnit->getLevel() > 0) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("Lv%u", totUnit->getLevel());
                        }
                        uint32_t hp = totUnit->getHealth();
                        uint32_t maxHp = totUnit->getMaxHealth();
                        if (maxHp > 0) {
                            float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                pct > 0.5f ? colors::kFriendlyGreen :
                                pct > 0.2f ? ImVec4(0.7f, 0.7f, 0.2f, 1.0f) :
                                             colors::kDangerRed);
                            ImGui::ProgressBar(pct, ImVec2(-1, 10), "");
                            ImGui::PopStyleColor();
                        }

                        // ToT cast bar — green if interruptible, red if not; pulses near completion
                        if (auto* totCs = gameHandler.getUnitCastState(totGuid)) {
                            float totCastPct = (totCs->timeTotal > 0.0f)
                                ? (totCs->timeTotal - totCs->timeRemaining) / totCs->timeTotal : 0.0f;
                            ImVec4 tcColor;
                            if (totCastPct > 0.8f) {
                                float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                                tcColor = totCs->interruptible
                                    ? ImVec4(0.2f * pulse, 0.9f * pulse, 0.2f * pulse, 1.0f)
                                    : ImVec4(1.0f * pulse, 0.1f * pulse, 0.1f * pulse, 1.0f);
                            } else {
                                tcColor = totCs->interruptible
                                    ? colors::kCastGreen
                                    : ImVec4(0.85f, 0.15f, 0.15f, 1.0f);
                            }
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, tcColor);
                            char tcLabel[48];
                            const std::string& tcName = gameHandler.getSpellName(totCs->spellId);
                            if (!tcName.empty())
                                snprintf(tcLabel, sizeof(tcLabel), "%s (%.1fs)", tcName.c_str(), totCs->timeRemaining);
                            else
                                snprintf(tcLabel, sizeof(tcLabel), "Casting... (%.1fs)", totCs->timeRemaining);
                            ImGui::ProgressBar(totCastPct, ImVec2(-1, 8), tcLabel);
                            ImGui::PopStyleColor();
                        }

                        // ToT aura row — compact icons, debuffs first
                        {
                            const std::vector<game::AuraSlot>* totAuras = nullptr;
                            if (totGuid == gameHandler.getPlayerGuid())
                                totAuras = &gameHandler.getPlayerAuras();
                            else if (totGuid == gameHandler.getTargetGuid())
                                totAuras = &gameHandler.getTargetAuras();
                            else
                                totAuras = gameHandler.getUnitAuras(totGuid);

                            if (totAuras) {
                                int totActive = 0;
                                for (const auto& a : *totAuras) if (!a.isEmpty()) totActive++;
                                if (totActive > 0) {
                                    auto* totAsset = services_.assetManager;
                                    constexpr float TA_ICON = 16.0f;
                                    constexpr int   TA_PER_ROW = 8;

                                    ImGui::Separator();

                                    uint64_t taNowMs = static_cast<uint64_t>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch()).count());

                                    std::vector<size_t> taIdx;
                                    taIdx.reserve(totAuras->size());
                                    for (size_t i = 0; i < totAuras->size(); ++i)
                                        if (!(*totAuras)[i].isEmpty()) taIdx.push_back(i);
                                    std::sort(taIdx.begin(), taIdx.end(), [&](size_t a, size_t b) {
                                        bool aD = ((*totAuras)[a].flags & 0x80) != 0;
                                        bool bD = ((*totAuras)[b].flags & 0x80) != 0;
                                        if (aD != bD) return aD > bD;
                                        int32_t ra = (*totAuras)[a].getRemainingMs(taNowMs);
                                        int32_t rb = (*totAuras)[b].getRemainingMs(taNowMs);
                                        if (ra < 0 && rb < 0) return false;
                                        if (ra < 0) return false;
                                        if (rb < 0) return true;
                                        return ra < rb;
                                    });

                                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
                                    int taShown = 0;
                                    for (size_t si = 0; si < taIdx.size() && taShown < 16; ++si) {
                                        const auto& aura = (*totAuras)[taIdx[si]];
                                        bool isBuff = (aura.flags & 0x80) == 0;

                                        if (taShown > 0 && taShown % TA_PER_ROW != 0) ImGui::SameLine();
                                        ImGui::PushID(static_cast<int>(taIdx[si]) + 5000);

                                        ImVec4 borderCol;
                                        if (isBuff) {
                                            borderCol = ImVec4(0.2f, 0.8f, 0.2f, 0.9f);
                                        } else {
                                            uint8_t dt = gameHandler.getSpellDispelType(aura.spellId);
                                            switch (dt) {
                                                case 1: borderCol = ImVec4(0.15f, 0.50f, 1.00f, 0.9f); break;
                                                case 2: borderCol = ImVec4(0.70f, 0.20f, 0.90f, 0.9f); break;
                                                case 3: borderCol = ImVec4(0.55f, 0.30f, 0.10f, 0.9f); break;
                                                case 4: borderCol = ImVec4(0.10f, 0.70f, 0.10f, 0.9f); break;
                                                default: borderCol = ImVec4(0.80f, 0.20f, 0.20f, 0.9f); break;
                                            }
                                        }

                                        VkDescriptorSet taIcon = (totAsset)
                                            ? getSpellIcon(aura.spellId, totAsset) : VK_NULL_HANDLE;
                                        if (taIcon) {
                                            ImGui::PushStyleColor(ImGuiCol_Button, borderCol);
                                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
                                            ImGui::ImageButton("##taura",
                                                (ImTextureID)(uintptr_t)taIcon,
                                                ImVec2(TA_ICON - 2, TA_ICON - 2));
                                            ImGui::PopStyleVar();
                                            ImGui::PopStyleColor();
                                        } else {
                                            ImGui::PushStyleColor(ImGuiCol_Button, borderCol);
                                            char lab[8];
                                            snprintf(lab, sizeof(lab), "%u", aura.spellId % 10000);
                                            ImGui::Button(lab, ImVec2(TA_ICON, TA_ICON));
                                            ImGui::PopStyleColor();
                                        }

                                        // Duration overlay
                                        int32_t taRemain = aura.getRemainingMs(taNowMs);
                                        if (taRemain > 0) {
                                            ImVec2 imin = ImGui::GetItemRectMin();
                                            ImVec2 imax = ImGui::GetItemRectMax();
                                            char ts[12];
                                            fmtDurationCompact(ts, sizeof(ts), (taRemain + 999) / 1000);
                                            ImVec2 tsz = ImGui::CalcTextSize(ts);
                                            float cx = imin.x + (imax.x - imin.x - tsz.x) * 0.5f;
                                            float cy = imax.y - tsz.y;
                                            ImGui::GetWindowDrawList()->AddText(ImVec2(cx + 1, cy + 1), IM_COL32(0, 0, 0, 180), ts);
                                            ImGui::GetWindowDrawList()->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 220), ts);
                                        }

                                        // Tooltip
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::BeginTooltip();
                                            bool richOk = spellbookScreen.renderSpellInfoTooltip(
                                                aura.spellId, gameHandler, totAsset);
                                            if (!richOk) {
                                                std::string nm = spellbookScreen.lookupSpellName(aura.spellId, totAsset);
                                                if (nm.empty()) nm = "Spell #" + std::to_string(aura.spellId);
                                                ImGui::Text("%s", nm.c_str());
                                            }
                                            renderAuraRemaining(taRemain);
                                            ImGui::EndTooltip();
                                        }

                                        ImGui::PopID();
                                        taShown++;
                                    }
                                    ImGui::PopStyleVar();
                                }
                            }
                        }
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
            }
        }
    }
}

void GameScreen::renderFocusFrame(game::GameHandler& gameHandler) {
    auto focus = gameHandler.getFocus();
    if (!focus) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    // Position: right side of screen, mirroring the target frame on the opposite side
    float frameW = 200.0f;
    float frameX = screenW - frameW - 10.0f;

    ImGui::SetNextWindowPos(ImVec2(frameX, 30.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(frameW, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    // Determine color based on relation (same logic as target frame)
    ImVec4 focusColor(0.7f, 0.7f, 0.7f, 1.0f);
    if (focus->getType() == game::ObjectType::PLAYER) {
        // Use class color for player focus targets
        uint8_t cid = entityClassId(focus.get());
        focusColor = (cid != 0) ? classColorVec4(cid) : kColorBrightGreen;
    } else if (focus->getType() == game::ObjectType::UNIT) {
        auto u = std::static_pointer_cast<game::Unit>(focus);
        if (u->getHealth() == 0 && u->getMaxHealth() > 0) {
            focusColor = kColorDarkGray;
        } else if (u->isHostile()) {
            // Tapped-by-other: grey focus frame name
            uint32_t focDynFlags = u->getDynamicFlags();
            bool focTapped = (focDynFlags & 0x0004) != 0 && (focDynFlags & 0x0008) == 0;
            if (focTapped) {
                focusColor = kColorGray;
            } else {
            uint32_t playerLv = gameHandler.getPlayerLevel();
            uint32_t mobLv = u->getLevel();
            if (mobLv == 0) {
                focusColor = ImVec4(1.0f, 0.1f, 0.1f, 1.0f); // ?? level = skull red
            } else {
                int32_t diff = static_cast<int32_t>(mobLv) - static_cast<int32_t>(playerLv);
                if (game::GameHandler::killXp(playerLv, mobLv) == 0)
                    focusColor = kColorGray;
                else if (diff >= 10)
                    focusColor = ImVec4(1.0f, 0.1f, 0.1f, 1.0f);
                else if (diff >= 5)
                    focusColor = ImVec4(1.0f, 0.5f, 0.1f, 1.0f);
                else if (diff >= -2)
                    focusColor = ImVec4(1.0f, 1.0f, 0.1f, 1.0f);
                else
                    focusColor = kColorBrightGreen;
            }
            } // end tapped else
        } else {
            focusColor = kColorBrightGreen;
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.15f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.9f, 0.8f));  // Blue tint = focus

    if (ImGui::Begin("##FocusFrame", nullptr, flags)) {
        // "Focus" label
        ImGui::TextDisabled("[Focus]");
        ImGui::SameLine();

        // Raid mark icon (star, circle, diamond, …) preceding the name
        {
            static constexpr struct { const char* sym; ImU32 col; } kFocusMarks[] = {
                { "\xe2\x98\x85", IM_COL32(255, 204,   0, 255) },  // 0 Star     (yellow)
                { "\xe2\x97\x8f", IM_COL32(255, 103,   0, 255) },  // 1 Circle   (orange)
                { "\xe2\x97\x86", IM_COL32(160,  32, 240, 255) },  // 2 Diamond  (purple)
                { "\xe2\x96\xb2", IM_COL32( 50, 200,  50, 255) },  // 3 Triangle (green)
                { "\xe2\x97\x8c", IM_COL32( 80, 160, 255, 255) },  // 4 Moon     (blue)
                { "\xe2\x96\xa0", IM_COL32( 50, 200, 220, 255) },  // 5 Square   (teal)
                { "\xe2\x9c\x9d", IM_COL32(255,  80,  80, 255) },  // 6 Cross    (red)
                { "\xe2\x98\xa0", IM_COL32(255, 255, 255, 255) },  // 7 Skull    (white)
            };
            uint8_t fmark = gameHandler.getEntityRaidMark(focus->getGuid());
            if (fmark < game::GameHandler::kRaidMarkCount) {
                ImGui::GetWindowDrawList()->AddText(
                    ImGui::GetCursorScreenPos(),
                    kFocusMarks[fmark].col, kFocusMarks[fmark].sym);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18.0f);
            }
        }

        std::string focusName = getEntityName(focus);
        ImGui::PushStyleColor(ImGuiCol_Text, focusColor);
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1,1,1,0.08f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(1,1,1,0.12f));
        ImGui::Selectable(focusName.c_str(), false, ImGuiSelectableFlags_DontClosePopups,
                          ImVec2(ImGui::CalcTextSize(focusName.c_str()).x, 0));
        ImGui::PopStyleColor(4);

        // Right-click context menu on focus frame
        if (ImGui::BeginPopupContextItem("##FocusFrameCtx")) {
            ImGui::TextDisabled("%s", focusName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Target"))
                gameHandler.setTarget(focus->getGuid());
            if (ImGui::MenuItem("Clear Focus"))
                gameHandler.clearFocus();
            if (focus->getType() == game::ObjectType::PLAYER) {
                ImGui::Separator();
                if (ImGui::MenuItem("Whisper")) {
                    chatPanel_.setWhisperTarget(focusName);
                }
                if (ImGui::MenuItem("Invite to Group"))
                    gameHandler.inviteToGroup(focusName);
                if (ImGui::MenuItem("Trade"))
                    gameHandler.initiateTrade(focus->getGuid());
                if (ImGui::MenuItem("Duel"))
                    gameHandler.proposeDuel(focus->getGuid());
                if (ImGui::MenuItem("Inspect")) {
                    gameHandler.setTarget(focus->getGuid());
                    gameHandler.inspectTarget();
                    socialPanel_.showInspectWindow_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Add Friend"))
                    gameHandler.addFriend(focusName);
                if (ImGui::MenuItem("Ignore"))
                    gameHandler.addIgnore(focusName);
            }
            ImGui::EndPopup();
        }

        // Group leader crown — golden ♛ when the focused player is the party/raid leader
        if (gameHandler.isInGroup() && focus->getType() == game::ObjectType::PLAYER) {
            if (gameHandler.getPartyData().leaderGuid == focus->getGuid()) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(colors::kSymbolGold, "\xe2\x99\x9b");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Group Leader");
            }
        }

        // Quest giver indicator and classification badge for NPC focus targets
        if (focus->getType() == game::ObjectType::UNIT) {
            auto focusUnit = std::static_pointer_cast<game::Unit>(focus);

            // Quest indicator: ! / ?
            {
                using QGS = game::QuestGiverStatus;
                QGS qgs = gameHandler.getQuestGiverStatus(focus->getGuid());
                if (qgs == QGS::AVAILABLE) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(colors::kBrightGold, "!");
                } else if (qgs == QGS::AVAILABLE_LOW) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(kColorGray, "!");
                } else if (qgs == QGS::REWARD || qgs == QGS::REWARD_REP) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(colors::kBrightGold, "?");
                } else if (qgs == QGS::INCOMPLETE) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(kColorGray, "?");
                }
            }

            // Classification badge
            int fRank = gameHandler.getCreatureRank(focusUnit->getEntry());
            if (fRank == 1)      { ImGui::SameLine(0,4); ImGui::TextColored(ImVec4(1.0f,0.8f,0.2f,1.0f), "[Elite]"); }
            else if (fRank == 2) { ImGui::SameLine(0,4); ImGui::TextColored(ImVec4(0.8f,0.4f,1.0f,1.0f), "[Rare Elite]"); }
            else if (fRank == 3) { ImGui::SameLine(0,4); ImGui::TextColored(colors::kRed, "[Boss]"); }
            else if (fRank == 4) { ImGui::SameLine(0,4); ImGui::TextColored(ImVec4(0.5f,0.9f,1.0f,1.0f), "[Rare]"); }

            // Creature type
            {
                uint32_t fctype = gameHandler.getCreatureType(focusUnit->getEntry());
                const char* fctName = nullptr;
                switch (fctype) {
                    case 1: fctName="Beast"; break;     case 2: fctName="Dragonkin"; break;
                    case 3: fctName="Demon"; break;     case 4: fctName="Elemental"; break;
                    case 5: fctName="Giant"; break;     case 6: fctName="Undead"; break;
                    case 7: fctName="Humanoid"; break;  case 8: fctName="Critter"; break;
                    case 9: fctName="Mechanical"; break; case 11: fctName="Totem"; break;
                    case 12: fctName="Non-combat Pet"; break; case 13: fctName="Gas Cloud"; break;
                    default: break;
                }
                if (fctName) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 0.9f), "(%s)", fctName);
                }
            }

            // Creature subtitle
            const std::string fSub = gameHandler.getCachedCreatureSubName(focusUnit->getEntry());
            if (!fSub.empty())
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.9f), "<%s>", fSub.c_str());
        }

        // Player guild name on focus frame
        if (focus->getType() == game::ObjectType::PLAYER) {
            uint32_t guildId = gameHandler.getEntityGuildId(focus->getGuid());
            if (guildId != 0) {
                const std::string& gn = gameHandler.lookupGuildName(guildId);
                if (!gn.empty()) {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.9f), "<%s>", gn.c_str());
                }
            }
        }

        if (ImGui::BeginPopupContextItem("##FocusNameCtx")) {
            const bool focusIsPlayer = (focus->getType() == game::ObjectType::PLAYER);
            const uint64_t fGuid = focus->getGuid();
            ImGui::TextDisabled("%s", focusName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Target"))
                gameHandler.setTarget(fGuid);
            if (ImGui::MenuItem("Clear Focus"))
                gameHandler.clearFocus();
            if (focusIsPlayer) {
                ImGui::Separator();
                if (ImGui::MenuItem("Whisper")) {
                    chatPanel_.setWhisperTarget(focusName);
                }
                if (ImGui::MenuItem("Invite to Group"))
                    gameHandler.inviteToGroup(focusName);
                if (ImGui::MenuItem("Trade"))
                    gameHandler.initiateTrade(fGuid);
                if (ImGui::MenuItem("Duel"))
                    gameHandler.proposeDuel(fGuid);
                if (ImGui::MenuItem("Inspect")) {
                    gameHandler.setTarget(fGuid);
                    gameHandler.inspectTarget();
                    socialPanel_.showInspectWindow_ = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Add Friend"))
                    gameHandler.addFriend(focusName);
                if (ImGui::MenuItem("Ignore"))
                    gameHandler.addIgnore(focusName);
            }
            ImGui::EndPopup();
        }

        if (focus->getType() == game::ObjectType::UNIT ||
            focus->getType() == game::ObjectType::PLAYER) {
            auto unit = std::static_pointer_cast<game::Unit>(focus);

            // Level + health on same row
            ImGui::SameLine();
            if (unit->getLevel() == 0)
                ImGui::TextDisabled("Lv ??");
            else
                ImGui::TextDisabled("Lv %u", unit->getLevel());

            uint32_t hp = unit->getHealth();
            uint32_t maxHp = unit->getMaxHealth();
            if (maxHp > 0) {
                float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                    pct > 0.5f ? colors::kFriendlyGreen :
                    pct > 0.2f ? ImVec4(0.7f, 0.7f, 0.2f, 1.0f) :
                                 colors::kDangerRed);
                char overlay[32];
                snprintf(overlay, sizeof(overlay), "%u / %u", hp, maxHp);
                ImGui::ProgressBar(pct, ImVec2(-1, 14), overlay);
                ImGui::PopStyleColor();

                // Power bar
                uint8_t pType = unit->getPowerType();
                uint32_t pwr = unit->getPower();
                uint32_t maxPwr = unit->getMaxPower();
                if (maxPwr == 0 && (pType == 1 || pType == 3)) maxPwr = 100;
                if (maxPwr > 0) {
                    float mpPct = static_cast<float>(pwr) / static_cast<float>(maxPwr);
                    ImVec4 pwrColor;
                    switch (pType) {
                        case 0: pwrColor = colors::kManaBlue; break;
                        case 1: pwrColor = colors::kDarkRed; break;
                        case 3: pwrColor = colors::kEnergyYellow; break;
                        case 6: pwrColor = colors::kRunicRed; break;
                        default: pwrColor = colors::kManaBlue; break;
                    }
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pwrColor);
                    ImGui::ProgressBar(mpPct, ImVec2(-1, 10), "");
                    ImGui::PopStyleColor();
                }
            }

            // Focus cast bar
            const auto* focusCast = gameHandler.getUnitCastState(focus->getGuid());
            if (focusCast) {
                float total = focusCast->timeTotal > 0.f ? focusCast->timeTotal : 1.f;
                float rem   = focusCast->timeRemaining;
                float prog  = std::clamp(1.0f - rem / total, 0.f, 1.f);
                const std::string& spName = gameHandler.getSpellName(focusCast->spellId);
                // Pulse orange when > 80% complete — interrupt window closing
                ImVec4 focusCastColor;
                if (prog > 0.8f) {
                    float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                    focusCastColor = ImVec4(1.0f * pulse, 0.5f * pulse, 0.0f, 1.0f);
                } else {
                    focusCastColor = ImVec4(0.9f, 0.3f, 0.2f, 1.0f);
                }
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, focusCastColor);
                char castBuf[64];
                if (!spName.empty())
                    snprintf(castBuf, sizeof(castBuf), "%s (%.1fs)", spName.c_str(), rem);
                else
                    snprintf(castBuf, sizeof(castBuf), "Casting... (%.1fs)", rem);
                {
                    auto* fcAsset = services_.assetManager;
                    VkDescriptorSet fcIcon = (focusCast->spellId != 0 && fcAsset)
                        ? getSpellIcon(focusCast->spellId, fcAsset) : VK_NULL_HANDLE;
                    if (fcIcon) {
                        ImGui::Image((ImTextureID)(uintptr_t)fcIcon, ImVec2(12, 12));
                        ImGui::SameLine(0, 2);
                        ImGui::ProgressBar(prog, ImVec2(-1, 12), castBuf);
                    } else {
                        ImGui::ProgressBar(prog, ImVec2(-1, 12), castBuf);
                    }
                }
                ImGui::PopStyleColor();
            }
        }

        // Focus auras — buffs first, then debuffs, up to 8 icons wide
        {
            const std::vector<game::AuraSlot>* focusAuras =
                (focus->getGuid() == gameHandler.getTargetGuid())
                    ? &gameHandler.getTargetAuras()
                    : gameHandler.getUnitAuras(focus->getGuid());

            if (focusAuras) {
                int activeCount = 0;
                for (const auto& a : *focusAuras) if (!a.isEmpty()) activeCount++;
                if (activeCount > 0) {
                    auto* focusAsset = services_.assetManager;
                    constexpr float FA_ICON = 20.0f;
                    constexpr int   FA_PER_ROW = 10;

                    ImGui::Separator();

                    uint64_t faNowMs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());

                    // Sort: debuffs first (so hostile-caster info is prominent), then buffs
                    std::vector<size_t> faIdx;
                    faIdx.reserve(focusAuras->size());
                    for (size_t i = 0; i < focusAuras->size(); ++i)
                        if (!(*focusAuras)[i].isEmpty()) faIdx.push_back(i);
                    std::sort(faIdx.begin(), faIdx.end(), [&](size_t a, size_t b) {
                        bool aD = ((*focusAuras)[a].flags & 0x80) != 0;
                        bool bD = ((*focusAuras)[b].flags & 0x80) != 0;
                        if (aD != bD) return aD > bD; // debuffs first
                        int32_t ra = (*focusAuras)[a].getRemainingMs(faNowMs);
                        int32_t rb = (*focusAuras)[b].getRemainingMs(faNowMs);
                        if (ra < 0 && rb < 0) return false;
                        if (ra < 0) return false;
                        if (rb < 0) return true;
                        return ra < rb;
                    });

                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));
                    int faShown = 0;
                    for (size_t si = 0; si < faIdx.size() && faShown < 20; ++si) {
                        const auto& aura = (*focusAuras)[faIdx[si]];
                        bool isBuff = (aura.flags & 0x80) == 0;

                        if (faShown > 0 && faShown % FA_PER_ROW != 0) ImGui::SameLine();
                        ImGui::PushID(static_cast<int>(faIdx[si]) + 3000);

                        ImVec4 borderCol;
                        if (isBuff) {
                            borderCol = ImVec4(0.2f, 0.8f, 0.2f, 0.9f);
                        } else {
                            uint8_t dt = gameHandler.getSpellDispelType(aura.spellId);
                            switch (dt) {
                                case 1: borderCol = ImVec4(0.15f, 0.50f, 1.00f, 0.9f); break;
                                case 2: borderCol = ImVec4(0.70f, 0.20f, 0.90f, 0.9f); break;
                                case 3: borderCol = ImVec4(0.55f, 0.30f, 0.10f, 0.9f); break;
                                case 4: borderCol = ImVec4(0.10f, 0.70f, 0.10f, 0.9f); break;
                                default: borderCol = ImVec4(0.80f, 0.20f, 0.20f, 0.9f); break;
                            }
                        }

                        VkDescriptorSet faIcon = (focusAsset)
                            ? getSpellIcon(aura.spellId, focusAsset) : VK_NULL_HANDLE;
                        if (faIcon) {
                            ImGui::PushStyleColor(ImGuiCol_Button, borderCol);
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
                            ImGui::ImageButton("##faura",
                                (ImTextureID)(uintptr_t)faIcon,
                                ImVec2(FA_ICON - 2, FA_ICON - 2));
                            ImGui::PopStyleVar();
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button, borderCol);
                            char lab[8];
                            snprintf(lab, sizeof(lab), "%u", aura.spellId);
                            ImGui::Button(lab, ImVec2(FA_ICON, FA_ICON));
                            ImGui::PopStyleColor();
                        }

                        // Duration overlay
                        int32_t faRemain = aura.getRemainingMs(faNowMs);
                        if (faRemain > 0) {
                            ImVec2 imin = ImGui::GetItemRectMin();
                            ImVec2 imax = ImGui::GetItemRectMax();
                            char ts[12];
                            fmtDurationCompact(ts, sizeof(ts), (faRemain + 999) / 1000);
                            ImVec2 tsz = ImGui::CalcTextSize(ts);
                            float cx = imin.x + (imax.x - imin.x - tsz.x) * 0.5f;
                            float cy = imax.y - tsz.y - 1.0f;
                            ImGui::GetWindowDrawList()->AddText(ImVec2(cx + 1, cy + 1), IM_COL32(0, 0, 0, 180), ts);
                            ImGui::GetWindowDrawList()->AddText(ImVec2(cx, cy), IM_COL32(255, 255, 255, 220), ts);
                        }

                        // Stack / charge count — upper-left corner (parity with target frame)
                        if (aura.charges > 1) {
                            ImVec2 faMin = ImGui::GetItemRectMin();
                            char chargeStr[8];
                            snprintf(chargeStr, sizeof(chargeStr), "%u", static_cast<unsigned>(aura.charges));
                            ImGui::GetWindowDrawList()->AddText(ImVec2(faMin.x + 3, faMin.y + 3),
                                IM_COL32(0, 0, 0, 200), chargeStr);
                            ImGui::GetWindowDrawList()->AddText(ImVec2(faMin.x + 2, faMin.y + 2),
                                IM_COL32(255, 220, 50, 255), chargeStr);
                        }

                        // Tooltip
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            bool richOk = spellbookScreen.renderSpellInfoTooltip(
                                aura.spellId, gameHandler, focusAsset);
                            if (!richOk) {
                                std::string nm = spellbookScreen.lookupSpellName(aura.spellId, focusAsset);
                                if (nm.empty()) nm = "Spell #" + std::to_string(aura.spellId);
                                ImGui::Text("%s", nm.c_str());
                            }
                            renderAuraRemaining(faRemain);
                            ImGui::EndTooltip();
                        }

                        ImGui::PopID();
                        faShown++;
                    }
                    ImGui::PopStyleVar();
                }
            }
        }

        // Target-of-Focus: who the focus target is currently targeting
        {
            uint64_t fofGuid = 0;
            const auto& fFields = focus->getFields();
            auto fItLo = fFields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
            if (fItLo != fFields.end()) {
                fofGuid = fItLo->second;
                auto fItHi = fFields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                if (fItHi != fFields.end())
                    fofGuid |= (static_cast<uint64_t>(fItHi->second) << 32);
            }
            if (fofGuid != 0) {
                auto fofEnt = gameHandler.getEntityManager().getEntity(fofGuid);
                std::string fofName;
                ImVec4 fofColor(0.7f, 0.7f, 0.7f, 1.0f);
                if (fofGuid == gameHandler.getPlayerGuid()) {
                    fofName = "You";
                    fofColor = kColorBrightGreen;
                } else if (fofEnt) {
                    fofName = getEntityName(fofEnt);
                    uint8_t fcid = entityClassId(fofEnt.get());
                    if (fcid != 0) fofColor = classColorVec4(fcid);
                }
                if (!fofName.empty()) {
                    ImGui::TextDisabled("▶");
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(fofColor, "%s", fofName.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Focus's target: %s\nClick to target", fofName.c_str());
                    if (ImGui::IsItemClicked())
                        gameHandler.setTarget(fofGuid);

                    // Compact health bar for target-of-focus
                    if (fofEnt) {
                        auto fofUnit = std::dynamic_pointer_cast<game::Unit>(fofEnt);
                        if (fofUnit && fofUnit->getMaxHealth() > 0) {
                            float fofPct = static_cast<float>(fofUnit->getHealth()) /
                                           static_cast<float>(fofUnit->getMaxHealth());
                            ImVec4 fofBarColor =
                                fofPct > 0.5f ? colors::kCastGreen :
                                fofPct > 0.2f ? ImVec4(0.75f, 0.75f, 0.2f, 1.0f) :
                                               ImVec4(0.75f, 0.2f, 0.2f, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fofBarColor);
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
                            char fofOverlay[32];
                            snprintf(fofOverlay, sizeof(fofOverlay), "%u%%",
                                     static_cast<unsigned>(fofPct * 100.0f + 0.5f));
                            ImGui::ProgressBar(fofPct, ImVec2(-1, 10), fofOverlay);
                            ImGui::PopStyleColor(2);
                        }
                    }
                }
            }
        }

        // Distance to focus target
        {
            const auto& mv = gameHandler.getMovementInfo();
            float fdx = focus->getX() - mv.x;
            float fdy = focus->getY() - mv.y;
            float fdz = focus->getZ() - mv.z;
            float fdist = std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz);
            ImGui::TextDisabled("%.1f yd", fdist);
        }

        // Clicking the focus frame targets it
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
            gameHandler.setTarget(focus->getGuid());
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GameScreen::updateCharacterGeosets(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    uint32_t instanceId = renderer->getCharacterInstanceId();
    if (instanceId == 0) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();

    // Load ItemDisplayInfo.dbc for geosetGroup lookup
    std::shared_ptr<pipeline::DBCFile> displayInfoDbc;
    if (assetManager) {
        displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    }

    // Helper: get geosetGroup field for an equipped item's displayInfoId
    // DBC binary fields: 7=geosetGroup_1, 8=geosetGroup_2, 9=geosetGroup_3
    auto getGeosetGroup = [&](uint32_t displayInfoId, int groupField) -> uint32_t {
        if (!displayInfoDbc || displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), 7 + groupField);
    };

    // Helper: find first equipped item matching inventoryType, return its displayInfoId
    auto findEquippedDisplayId = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t)
                        return slot.item.displayInfoId;
                }
            }
        }
        return 0;
    };

    // Helper: check if any equipment slot has the given inventoryType
    auto hasEquippedType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty()) {
                for (uint8_t t : types) {
                    if (slot.item.inventoryType == t) return true;
                }
            }
        }
        return false;
    };

    // Base geosets always present (group 0: IDs 0-99, some models use up to 27)
    std::unordered_set<uint16_t> geosets;
    for (uint16_t i = 0; i <= 99; i++) {
        geosets.insert(i);
    }
    // Hair/facial geosets must match the active character's appearance, otherwise
    // we end up forcing a default hair mesh (often perceived as "wrong hair").
    {
        uint8_t hairStyleId = 0;
        uint8_t facialId = 0;
        if (auto* gh = app.getGameHandler()) {
            if (const auto* ch = gh->getActiveCharacter()) {
                hairStyleId = static_cast<uint8_t>((ch->appearanceBytes >> 16) & 0xFF);
                facialId = ch->facialFeatures;
            }
        }
        geosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1)); // Group 1 hair
        geosets.insert(static_cast<uint16_t>(200 + facialId + 1));    // Group 2 facial
    }
    geosets.insert(702);  // Ears: visible (default)
    geosets.insert(2002); // Bare feet mesh (group 20 = CG_FEET, always on)

    // CharGeosets mapping (verified via vertex bounding boxes):
    //   Group 4 (401+) = GLOVES (forearm area, Z~1.1-1.4)
    //   Group 5 (501+) = BOOTS  (shin area, Z~0.1-0.6)
    //   Group 8 (801+) = WRISTBANDS/SLEEVES (controlled by chest armor)
    //   Group 9 (901+) = KNEEPADS
    //   Group 13 (1301+) = TROUSERS/PANTS
    //   Group 15 (1501+) = CAPE/CLOAK
    //   Group 20 (2002) = FEET

    // Gloves: inventoryType 10 → group 4 (forearms)
    // 401=bare forearms, 402+=glove styles covering forearm
    {
        uint32_t did = findEquippedDisplayId({10});
        uint32_t gg = getGeosetGroup(did, 0);
        geosets.insert(static_cast<uint16_t>(gg > 0 ? 401 + gg : 401));
    }

    // Boots: inventoryType 8 → group 5 (shins/lower legs)
    // 501=narrow bare shin, 502=wider (matches thigh width better). Use 502 as bare default.
    // When boots equipped, gg selects boot style: 501+gg (gg=1→502, gg=2→503, etc.)
    {
        uint32_t did = findEquippedDisplayId({8});
        uint32_t gg = getGeosetGroup(did, 0);
        geosets.insert(static_cast<uint16_t>(gg > 0 ? 501 + gg : 502));
    }

    // Chest/Shirt: inventoryType 4 (shirt), 5 (chest), 20 (robe)
    // Controls group 8 (wristbands/sleeve length): 801=bare wrists, 802+=sleeve styles
    // Also controls group 13 (trousers) via GeosetGroup[2] for robes
    {
        uint32_t did = findEquippedDisplayId({4, 5, 20});
        uint32_t gg = getGeosetGroup(did, 0);
        geosets.insert(static_cast<uint16_t>(gg > 0 ? 801 + gg : 801));
        // Robe kilt: GeosetGroup[2] > 0 → show kilt legs (1302+)
        uint32_t gg3 = getGeosetGroup(did, 2);
        if (gg3 > 0) {
            geosets.insert(static_cast<uint16_t>(1301 + gg3));
        }
    }

    // Kneepads: group 9 (always default 902)
    geosets.insert(902);

    // Legs/Pants: inventoryType 7 → group 13 (trousers/thighs)
    // 1301=bare legs, 1302+=pant/kilt styles
    {
        uint32_t did = findEquippedDisplayId({7});
        uint32_t gg = getGeosetGroup(did, 0);
        // Only add if robe hasn't already set a kilt geoset
        if (geosets.count(1302) == 0 && geosets.count(1303) == 0) {
            geosets.insert(static_cast<uint16_t>(gg > 0 ? 1301 + gg : 1301));
        }
    }

    // Back/Cloak: inventoryType 16 → group 15
    geosets.insert(hasEquippedType({16}) ? 1502 : 1501);

    // Tabard: inventoryType 19 → group 12
    if (hasEquippedType({19})) {
        geosets.insert(1201);
    }

    charRenderer->setActiveGeosets(instanceId, geosets);
}

void GameScreen::updateCharacterTextures(game::Inventory& inventory) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    auto* charRenderer = renderer->getCharacterRenderer();
    if (!charRenderer) return;

    auto* assetManager = app.getAssetManager();
    if (!assetManager) return;

    const auto& bodySkinPath = app.getBodySkinPath();
    const auto& underwearPaths = app.getUnderwearPaths();
    uint32_t skinSlot = app.getSkinTextureSlotIndex();

    if (bodySkinPath.empty()) return;

    // Component directory names indexed by region
    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture",   // 0
        "ArmLowerTexture",   // 1
        "HandTexture",       // 2
        "TorsoUpperTexture", // 3
        "TorsoLowerTexture", // 4
        "LegUpperTexture",   // 5
        "LegLowerTexture",   // 6
        "FootTexture",       // 7
    };

    // Load ItemDisplayInfo.dbc
    auto displayInfoDbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    // Collect equipment texture regions from all equipped items
    std::vector<std::pair<int, std::string>> regionLayers;

    for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
        const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
        if (slot.empty() || slot.item.displayInfoId == 0) continue;

        int32_t recIdx = displayInfoDbc->findRecordById(slot.item.displayInfoId);
        if (recIdx < 0) continue;

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            // Actual MPQ files have a gender suffix: _M (male), _F (female), _U (unisex)
            // Try gender-specific first, then unisex fallback
            std::string base = "Item\\TextureComponents\\" +
                std::string(componentDirs[region]) + "\\" + texName;
            // Determine gender suffix from active character
            bool isFemale = false;
            if (auto* gh = app.getGameHandler()) {
                if (auto* ch = gh->getActiveCharacter()) {
                    isFemale = (ch->gender == game::Gender::FEMALE) ||
                               (ch->gender == game::Gender::NONBINARY && ch->useFemaleModel);
                }
            }
            std::string genderPath = base + (isFemale ? "_F.blp" : "_M.blp");
            std::string unisexPath = base + "_U.blp";
            std::string fullPath;
            if (assetManager->fileExists(genderPath)) {
                fullPath = genderPath;
            } else if (assetManager->fileExists(unisexPath)) {
                fullPath = unisexPath;
            } else {
                // Last resort: try without suffix
                fullPath = base + ".blp";
            }
            regionLayers.emplace_back(region, fullPath);
        }
    }

    // Re-composite: base skin + underwear + equipment regions
    // Clear composite cache first to prevent stale textures from being reused
    charRenderer->clearCompositeCache();
    // Use per-instance texture override (not model-level) to avoid deleting cached composites.
    uint32_t instanceId = renderer->getCharacterInstanceId();
    auto* newTex = charRenderer->compositeWithRegions(bodySkinPath, underwearPaths, regionLayers);
    if (newTex != nullptr && instanceId != 0) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(skinSlot), newTex);
    }

    // Cloak cape texture — separate from skin atlas, uses texture slot type-2 (Object Skin)
    uint32_t cloakSlot = app.getCloakTextureSlotIndex();
    if (cloakSlot > 0 && instanceId != 0) {
        // Find equipped cloak (inventoryType 16)
        uint32_t cloakDisplayId = 0;
        for (int s = 0; s < game::Inventory::NUM_EQUIP_SLOTS; s++) {
            const auto& slot = inventory.getEquipSlot(static_cast<game::EquipSlot>(s));
            if (!slot.empty() && slot.item.inventoryType == 16 && slot.item.displayInfoId != 0) {
                cloakDisplayId = slot.item.displayInfoId;
                break;
            }
        }

        if (cloakDisplayId > 0) {
            int32_t recIdx = displayInfoDbc->findRecordById(cloakDisplayId);
            if (recIdx >= 0) {
                // DBC field 3 = modelTexture_1 (cape texture name)
                const auto* dispL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                std::string capeName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), dispL ? (*dispL)["LeftModelTexture"] : 3);
                if (!capeName.empty()) {
                    std::string capePath = "Item\\ObjectComponents\\Cape\\" + capeName + ".blp";
                    auto* capeTex = charRenderer->loadTexture(capePath);
                    if (capeTex != nullptr) {
                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot), capeTex);
                        LOG_INFO("Cloak texture applied: ", capePath);
                    }
                }
            }
        } else {
            // No cloak equipped — clear override so model's default (white) shows
            charRenderer->clearTextureSlotOverride(instanceId, static_cast<uint16_t>(cloakSlot));
        }
    }
}

// ============================================================
// World Map
// ============================================================

void GameScreen::renderWorldMap(game::GameHandler& gameHandler) {
    if (!showWorldMap_) return;

    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return;

    auto* wm = renderer->getWorldMap();
    if (!wm) return;

    // Keep map name in sync with minimap's map name
    auto* minimap = renderer->getMinimap();
    if (minimap) {
        wm->setMapName(minimap->getMapName());
    }
    wm->setServerExplorationMask(
        gameHandler.getPlayerExploredZoneMasks(),
        gameHandler.hasPlayerExploredZoneMasks());

    // Party member dots on world map
    {
        std::vector<rendering::WorldMapPartyDot> dots;
        if (gameHandler.isInGroup()) {
            const auto& partyData = gameHandler.getPartyData();
            for (const auto& member : partyData.members) {
                if (!member.isOnline || !member.hasPartyStats) continue;
                if (member.posX == 0 && member.posY == 0) continue;
                // posY → canonical X (north), posX → canonical Y (west)
                float wowX = static_cast<float>(member.posY);
                float wowY = static_cast<float>(member.posX);
                glm::vec3 rpos = core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f));
                auto ent = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(ent.get());
                ImU32 col = (cid != 0)
                    ? classColorU32(cid, 230)
                    : (member.guid == partyData.leaderGuid
                       ? IM_COL32(255, 210, 0, 230)
                       : IM_COL32(100, 180, 255, 230));
                dots.push_back({ rpos, col, member.name });
            }
        }
        wm->setPartyDots(std::move(dots));
    }

    // Taxi node markers on world map
    {
        std::vector<rendering::WorldMapTaxiNode> taxiNodes;
        const auto& nodes = gameHandler.getTaxiNodes();
        taxiNodes.reserve(nodes.size());
        for (const auto& [id, node] : nodes) {
            rendering::WorldMapTaxiNode wtn;
            wtn.id    = node.id;
            wtn.mapId = node.mapId;
            wtn.wowX  = node.x;
            wtn.wowY  = node.y;
            wtn.wowZ  = node.z;
            wtn.name  = node.name;
            wtn.known = gameHandler.isKnownTaxiNode(id);
            taxiNodes.push_back(std::move(wtn));
        }
        wm->setTaxiNodes(std::move(taxiNodes));
    }

    // Quest POI markers on world map (from SMSG_QUEST_POI_QUERY_RESPONSE / gossip POIs)
    {
        std::vector<rendering::WorldMap::QuestPoi> qpois;
        for (const auto& poi : gameHandler.getGossipPois()) {
            rendering::WorldMap::QuestPoi qp;
            qp.wowX = poi.x;
            qp.wowY = poi.y;
            qp.name = poi.name;
            qpois.push_back(std::move(qp));
        }
        wm->setQuestPois(std::move(qpois));
    }

    // Corpse marker: show skull X on world map when ghost with unclaimed corpse
    {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        bool ghostWithCorpse = gameHandler.isPlayerGhost() &&
                               gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY);
        glm::vec3 corpseRender = ghostWithCorpse
            ? core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f))
            : glm::vec3{};
        wm->setCorpsePos(ghostWithCorpse, corpseRender);
    }

    glm::vec3 playerPos = renderer->getCharacterPosition();
    float playerYaw = renderer->getCharacterYaw();
    auto* window = app.getWindow();
    int screenW = window ? window->getWidth() : 1280;
    int screenH = window ? window->getHeight() : 720;
    wm->render(playerPos, screenW, screenH, playerYaw);

    // Sync showWorldMap_ if the map closed itself (e.g. ESC key inside the overlay).
    if (!wm->isOpen()) showWorldMap_ = false;
}

// ============================================================
// Action Bar
// ============================================================

VkDescriptorSet GameScreen::getSpellIcon(uint32_t spellId, pipeline::AssetManager* am) {
    if (spellId == 0 || !am) return VK_NULL_HANDLE;

    // Check cache first
    auto cit = spellIconCache_.find(spellId);
    if (cit != spellIconCache_.end()) return cit->second;

    // Lazy-load SpellIcon.dbc and Spell.dbc icon IDs
    if (!spellIconDbLoaded_) {
        spellIconDbLoaded_ = true;

        // Load SpellIcon.dbc: field 0 = ID, field 1 = icon path
        auto iconDbc = am->loadDBC("SpellIcon.dbc");
        const auto* iconL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("SpellIcon") : nullptr;
        if (iconDbc && iconDbc->isLoaded()) {
            for (uint32_t i = 0; i < iconDbc->getRecordCount(); i++) {
                uint32_t id = iconDbc->getUInt32(i, iconL ? (*iconL)["ID"] : 0);
                std::string path = iconDbc->getString(i, iconL ? (*iconL)["Path"] : 1);
                if (!path.empty() && id > 0) {
                    spellIconPaths_[id] = path;
                }
            }
        }

        // Load Spell.dbc: SpellIconID field
        auto spellDbc = am->loadDBC("Spell.dbc");
        const auto* spellL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("Spell") : nullptr;
        if (spellDbc && spellDbc->isLoaded()) {
            uint32_t fieldCount = spellDbc->getFieldCount();
            // Helper to load icons for a given field layout
            auto tryLoadIcons = [&](uint32_t idField, uint32_t iconField) {
                spellIconIds_.clear();
                if (iconField >= fieldCount) return;
                for (uint32_t i = 0; i < spellDbc->getRecordCount(); i++) {
                    uint32_t id = spellDbc->getUInt32(i, idField);
                    uint32_t iconId = spellDbc->getUInt32(i, iconField);
                    if (id > 0 && iconId > 0) {
                        spellIconIds_[id] = iconId;
                    }
                }
            };

            // Use expansion-aware layout if available AND the DBC field count
            // matches the expansion's expected format.  Classic=173, TBC=216,
            // WotLK=234 fields.  When Classic is active but the base WotLK DBC
            // is loaded (234 fields), field 117 is NOT IconID — we must use
            // the WotLK field 133 instead.
            uint32_t iconField = 133; // WotLK default
            uint32_t idField = 0;
            if (spellL) {
                uint32_t layoutIcon = (*spellL)["IconID"];
                // Only trust the expansion layout if the DBC has a compatible
                // field count (within ~20 of the layout's icon field).
                if (layoutIcon < fieldCount && fieldCount <= layoutIcon + 20) {
                    iconField = layoutIcon;
                    idField = (*spellL)["ID"];
                }
            }
            tryLoadIcons(idField, iconField);
        }
    }

    // Rate-limit GPU uploads per frame to prevent stalls when many icons are uncached
    // (e.g., first login, after loading screen, or many new auras appearing at once).
    static int gsLoadsThisFrame = 0;
    static int gsLastImGuiFrame = -1;
    int gsCurFrame = ImGui::GetFrameCount();
    if (gsCurFrame != gsLastImGuiFrame) { gsLoadsThisFrame = 0; gsLastImGuiFrame = gsCurFrame; }
    if (gsLoadsThisFrame >= 4) return VK_NULL_HANDLE;  // defer — do NOT cache null here

    // Look up spellId -> SpellIconID -> icon path
    auto iit = spellIconIds_.find(spellId);
    if (iit == spellIconIds_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto pit = spellIconPaths_.find(iit->second);
    if (pit == spellIconPaths_.end()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Path from DBC has no extension — append .blp
    std::string iconPath = pit->second + ".blp";
    auto blpData = am->readFile(iconPath);
    if (blpData.empty()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    auto image = pipeline::BLPLoader::load(blpData);
    if (!image.isValid()) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Upload to Vulkan via VkContext
    auto* window = services_.window;
    auto* vkCtx = window ? window->getVkContext() : nullptr;
    if (!vkCtx) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    ++gsLoadsThisFrame;
    VkDescriptorSet ds = vkCtx->uploadImGuiTexture(image.data.data(), image.width, image.height);
    spellIconCache_[spellId] = ds;
    return ds;
}

// ============================================================
// Mirror Timers (breath / fatigue / feign death)
// ============================================================

void GameScreen::renderMirrorTimers(game::GameHandler& gameHandler) {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;

    static constexpr struct { const char* label; ImVec4 color; } kTimerInfo[3] = {
        { "Fatigue", ImVec4(0.8f, 0.4f, 0.1f, 1.0f) },
        { "Breath",  ImVec4(0.2f, 0.5f, 1.0f, 1.0f) },
        { "Feign",   kColorGray },
    };

    float barW  = 280.0f;
    float barH  = 36.0f;
    float barX  = (screenW - barW) / 2.0f;
    float baseY = screenH - 160.0f;  // Just above the cast bar slot

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoInputs;

    for (int i = 0; i < 3; ++i) {
        const auto& t = gameHandler.getMirrorTimer(i);
        if (!t.active || t.maxValue <= 0) continue;

        float frac = static_cast<float>(t.value) / static_cast<float>(t.maxValue);
        frac = std::max(0.0f, std::min(1.0f, frac));

        char winId[32];
        std::snprintf(winId, sizeof(winId), "##MirrorTimer%d", i);
        ImGui::SetNextWindowPos(ImVec2(barX, baseY - i * (barH + 4.0f)), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(barW, barH), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.88f));
        if (ImGui::Begin(winId, nullptr, flags)) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kTimerInfo[i].color);
            char overlay[48];
            float sec = static_cast<float>(t.value) / 1000.0f;
            std::snprintf(overlay, sizeof(overlay), "%s  %.0fs", kTimerInfo[i].label, sec);
            ImGui::ProgressBar(frac, ImVec2(-1, 20), overlay);
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
}

// ============================================================
// Cooldown Tracker — floating panel showing all active spell CDs
// ============================================================

// ============================================================
// Quest Objective Tracker (right-side HUD)
// ============================================================

void GameScreen::renderQuestObjectiveTracker(game::GameHandler& gameHandler) {
    const auto& questLog = gameHandler.getQuestLog();
    if (questLog.empty()) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    constexpr float TRACKER_W = 220.0f;
    constexpr float RIGHT_MARGIN = 10.0f;
    constexpr int   MAX_QUESTS = 5;

    // Build display list: tracked quests only, or all quests if none tracked
    const auto& trackedIds = gameHandler.getTrackedQuestIds();
    std::vector<const game::GameHandler::QuestLogEntry*> toShow;
    toShow.reserve(MAX_QUESTS);
    if (!trackedIds.empty()) {
        for (const auto& q : questLog) {
            if (q.questId == 0) continue;
            if (trackedIds.count(q.questId)) toShow.push_back(&q);
            if (static_cast<int>(toShow.size()) >= MAX_QUESTS) break;
        }
    }
    // Fallback: show all quests if nothing is tracked
    if (toShow.empty()) {
        for (const auto& q : questLog) {
            if (q.questId == 0) continue;
            toShow.push_back(&q);
            if (static_cast<int>(toShow.size()) >= MAX_QUESTS) break;
        }
    }
    if (toShow.empty()) return;

    float screenH = ImGui::GetIO().DisplaySize.y > 0.0f ? ImGui::GetIO().DisplaySize.y : 720.0f;

    // Default position: top-right, below minimap + buff bar space.
    // questTrackerRightOffset_ stores pixels from the right edge so the tracker
    // stays anchored to the right side when the window is resized.
    if (!questTrackerPosInit_ || questTrackerRightOffset_ < 0.0f) {
        questTrackerRightOffset_ = TRACKER_W + RIGHT_MARGIN; // default: right-aligned
        questTrackerPos_.y = 320.0f;
        questTrackerPosInit_ = true;
    }
    // Recompute X from right offset every frame (handles window resize)
    questTrackerPos_.x = screenW - questTrackerRightOffset_;

    ImGui::SetNextWindowPos(questTrackerPos_, ImGuiCond_Always);
    ImGui::SetNextWindowSize(questTrackerSize_, ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));

    if (ImGui::Begin("##QuestTracker", nullptr, flags)) {
        for (int i = 0; i < static_cast<int>(toShow.size()); ++i) {
            const auto& q = *toShow[i];

            // Clickable quest title — opens quest log
            ImGui::PushID(q.questId);
            ImVec4 titleCol = q.complete ? colors::kWarmGold
                                         : ImVec4(1.0f, 1.0f, 0.85f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, titleCol);
            if (ImGui::Selectable(q.title.c_str(), false,
                                   ImGuiSelectableFlags_DontClosePopups, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                questLogScreen.openAndSelectQuest(q.questId);
            }
            if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##QTCtx")) {
                ImGui::SetTooltip("Click: open Quest Log  |  Right-click: tracking options");
            }
            ImGui::PopStyleColor();

            // Right-click context menu for quest tracker entry
            if (ImGui::BeginPopupContextItem("##QTCtx")) {
                ImGui::TextDisabled("%s", q.title.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Open in Quest Log")) {
                    questLogScreen.openAndSelectQuest(q.questId);
                }
                bool tracked = gameHandler.isQuestTracked(q.questId);
                if (tracked) {
                    if (ImGui::MenuItem("Stop Tracking")) {
                        gameHandler.setQuestTracked(q.questId, false);
                    }
                } else {
                    if (ImGui::MenuItem("Track")) {
                        gameHandler.setQuestTracked(q.questId, true);
                    }
                }
                if (gameHandler.isInGroup() && !q.complete) {
                    if (ImGui::MenuItem("Share Quest")) {
                        gameHandler.shareQuestWithParty(q.questId);
                    }
                }
                if (!q.complete) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Abandon Quest")) {
                        gameHandler.abandonQuest(q.questId);
                        gameHandler.setQuestTracked(q.questId, false);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Objectives line (condensed)
            if (q.complete) {
                ImGui::TextColored(colors::kActiveGreen, "  (Complete)");
            } else {
                // Kill counts — green when complete, gray when in progress
                for (const auto& [entry, progress] : q.killCounts) {
                    bool objDone = (progress.first >= progress.second && progress.second > 0);
                    ImVec4 objColor = objDone ? kColorGreen
                                              : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
                    std::string name = gameHandler.getCachedCreatureName(entry);
                    if (name.empty()) {
                        const auto* goInfo = gameHandler.getCachedGameObjectInfo(entry);
                        if (goInfo && !goInfo->name.empty()) name = goInfo->name;
                    }
                    if (!name.empty()) {
                        ImGui::TextColored(objColor,
                                           "  %s: %u/%u", name.c_str(),
                                           progress.first, progress.second);
                    } else {
                        ImGui::TextColored(objColor,
                                           "  %u/%u", progress.first, progress.second);
                    }
                }
                // Item counts — green when complete, gray when in progress
                for (const auto& [itemId, count] : q.itemCounts) {
                    uint32_t required = 1;
                    auto reqIt = q.requiredItemCounts.find(itemId);
                    if (reqIt != q.requiredItemCounts.end()) required = reqIt->second;
                    bool objDone = (count >= required);
                    ImVec4 objColor = objDone ? kColorGreen
                                              : ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
                    const auto* info = gameHandler.getItemInfo(itemId);
                    const char* itemName = (info && !info->name.empty()) ? info->name.c_str() : nullptr;

                    // Show small icon if available
                    uint32_t dispId = (info && info->displayInfoId) ? info->displayInfoId : 0;
                    VkDescriptorSet iconTex = dispId ? inventoryScreen.getItemIcon(dispId) : VK_NULL_HANDLE;
                    if (iconTex) {
                        ImGui::Image((ImTextureID)(uintptr_t)iconTex, ImVec2(12, 12));
                        if (info && info->valid && ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            inventoryScreen.renderItemTooltip(*info);
                            ImGui::EndTooltip();
                        }
                        ImGui::SameLine(0, 3);
                        ImGui::TextColored(objColor,
                                           "%s: %u/%u", itemName ? itemName : "Item", count, required);
                        if (info && info->valid && ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            inventoryScreen.renderItemTooltip(*info);
                            ImGui::EndTooltip();
                        }
                    } else if (itemName) {
                        ImGui::TextColored(objColor,
                                           "  %s: %u/%u", itemName, count, required);
                        if (info && info->valid && ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            inventoryScreen.renderItemTooltip(*info);
                            ImGui::EndTooltip();
                        }
                    } else {
                        ImGui::TextColored(objColor,
                                           "  Item: %u/%u", count, required);
                    }
                }
                if (q.killCounts.empty() && q.itemCounts.empty() && !q.objectives.empty()) {
                    const std::string& obj = q.objectives;
                    if (obj.size() > 40) {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                           "  %.37s...", obj.c_str());
                    } else {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                           "  %s", obj.c_str());
                    }
                }
            }

            if (i < static_cast<int>(toShow.size()) - 1) {
                ImGui::Spacing();
            }
        }

        // Capture position and size after drag/resize
        ImVec2 newPos  = ImGui::GetWindowPos();
        ImVec2 newSize = ImGui::GetWindowSize();
        bool changed = false;

        // Clamp within screen
        newPos.x = std::clamp(newPos.x, 0.0f, screenW - newSize.x);
        newPos.y = std::clamp(newPos.y, 0.0f, screenH - 40.0f);

        if (std::abs(newPos.x - questTrackerPos_.x) > 0.5f ||
            std::abs(newPos.y - questTrackerPos_.y) > 0.5f) {
            questTrackerPos_ = newPos;
            // Update right offset so resizes keep the new position anchored
            questTrackerRightOffset_ = screenW - newPos.x;
            changed = true;
        }
        if (std::abs(newSize.x - questTrackerSize_.x) > 0.5f ||
            std::abs(newSize.y - questTrackerSize_.y) > 0.5f) {
            questTrackerSize_ = newSize;
            changed = true;
        }
        if (changed) saveSettings();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ============================================================
// Nameplates — world-space health bars projected to screen
// ============================================================

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;

    // Reset mouseover each frame; we'll set it below when the cursor is over a nameplate
    gameHandler.setMouseoverGuid(0);

    auto* appRenderer = services_.renderer;
    if (!appRenderer) return;
    rendering::Camera* camera = appRenderer->getCamera();
    if (!camera) return;

    auto* window = services_.window;
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    const glm::vec3 camPos   = camera->getPosition();
    const uint64_t  playerGuid = gameHandler.getPlayerGuid();
    const uint64_t  targetGuid = gameHandler.getTargetGuid();

    // Build set of creature entries that are kill objectives in active (incomplete) quests.
    std::unordered_set<uint32_t> questKillEntries;
    {
        const auto& questLog = gameHandler.getQuestLog();
        const auto& trackedIds = gameHandler.getTrackedQuestIds();
        for (const auto& q : questLog) {
            if (q.complete || q.questId == 0) continue;
            // Only highlight for tracked quests (or all if nothing tracked).
            if (!trackedIds.empty() && !trackedIds.count(q.questId)) continue;
            for (const auto& obj : q.killObjectives) {
                if (obj.npcOrGoId > 0 && obj.required > 0) {
                    // Check if not already completed.
                    auto it = q.killCounts.find(static_cast<uint32_t>(obj.npcOrGoId));
                    if (it == q.killCounts.end() || it->second.first < it->second.second) {
                        questKillEntries.insert(static_cast<uint32_t>(obj.npcOrGoId));
                    }
                }
            }
        }
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    for (const auto& [guid, entityPtr] : gameHandler.getEntityManager().getEntities()) {
        if (!entityPtr || guid == playerGuid) continue;

        if (!entityPtr->isUnit()) continue;
        auto* unit = static_cast<game::Unit*>(entityPtr.get());
        if (unit->getMaxHealth() == 0) continue;

        bool isPlayer = (entityPtr->getType() == game::ObjectType::PLAYER);
        bool isTarget = (guid == targetGuid);

        // Player nameplates use Shift+V toggle; NPC/enemy nameplates use V toggle
        if (isPlayer && !settingsPanel_.showFriendlyNameplates_) continue;
        if (!isPlayer && !showNameplates_) continue;

        // For corpses (dead units), only show a minimal grey nameplate if selected
        bool isCorpse = (unit->getHealth() == 0);
        if (isCorpse && !isTarget) continue;

        // Prefer the renderer's actual instance position so the nameplate tracks the
        // rendered model exactly (avoids drift from the parallel entity interpolator).
        glm::vec3 renderPos;
        if (!core::Application::getInstance().getRenderPositionForGuid(guid, renderPos)) {
            renderPos = core::coords::canonicalToRender(
                glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        }
        renderPos.z += 2.3f;

        // Cull distance: target or other players up to 40 units; NPC others up to 20 units
        glm::vec3 nameDelta = renderPos - camPos;
        float distSq = glm::dot(nameDelta, nameDelta);
        float cullDist = (isTarget || isPlayer) ? 40.0f : 20.0f;
        if (distSq > cullDist * cullDist) continue;

        // Project to clip space
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.01f) continue;  // Behind camera

        glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        if (ndc.x < -1.2f || ndc.x > 1.2f || ndc.y < -1.2f || ndc.y > 1.2f) continue;

        // NDC → screen pixels.
        // The camera bakes the Vulkan Y-flip into the projection matrix, so
        // NDC y = -1 is the top of the screen and y = 1 is the bottom.
        // Map directly: sy = (ndc.y + 1) / 2 * screenH  (no extra inversion).
        float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        float sy = (ndc.y * 0.5f + 0.5f) * screenH;

        // Fade out in the last 5 units of cull range
        float fadeSq = (cullDist - 5.0f) * (cullDist - 5.0f);
        float dist = std::sqrt(distSq);
        float alpha = distSq < fadeSq ? 1.0f : 1.0f - (dist - (cullDist - 5.0f)) / 5.0f;
        auto A = [&](int v) { return static_cast<int>(v * alpha); };

        // Bar colour by hostility (grey for corpses)
        ImU32 barColor, bgColor;
        if (isCorpse) {
            // Minimal grey bar for selected corpses (loot/skin targets)
            barColor = IM_COL32(140, 140, 140, A(200));
            bgColor  = IM_COL32(70,  70,  70,  A(160));
        } else if (unit->isHostile()) {
            // Check if mob is tapped by another player (grey nameplate)
            uint32_t dynFlags = unit->getDynamicFlags();
            bool tappedByOther = (dynFlags & 0x0004) != 0 && (dynFlags & 0x0008) == 0; // TAPPED but not TAPPED_BY_ALL_THREAT_LIST
            if (tappedByOther) {
                barColor = IM_COL32(160, 160, 160, A(200));
                bgColor  = IM_COL32(80,  80,  80,  A(160));
            } else {
                barColor = IM_COL32(220, 60,  60,  A(200));
                bgColor  = IM_COL32(100, 25,  25,  A(160));
            }
        } else if (isPlayer) {
            // Player nameplates: use class color for easy identification
            uint8_t cid = entityClassId(unit);
            if (cid != 0) {
                ImVec4 cv = classColorVec4(cid);
                barColor = IM_COL32(
                    static_cast<int>(cv.x * 255),
                    static_cast<int>(cv.y * 255),
                    static_cast<int>(cv.z * 255), A(210));
                bgColor  = IM_COL32(
                    static_cast<int>(cv.x * 80),
                    static_cast<int>(cv.y * 80),
                    static_cast<int>(cv.z * 80), A(160));
            } else {
                barColor = IM_COL32(60,  200, 80,  A(200));
                bgColor  = IM_COL32(25,  100, 35,  A(160));
            }
        } else {
            barColor = IM_COL32(60,  200, 80,  A(200));
            bgColor  = IM_COL32(25,  100, 35,  A(160));
        }
        // Check if this unit is targeting the local player (threat indicator)
        bool isTargetingPlayer = false;
        if (unit->isHostile() && !isCorpse) {
            const auto& fields = entityPtr->getFields();
            auto loIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
            if (loIt != fields.end() && loIt->second != 0) {
                uint64_t unitTarget = loIt->second;
                auto hiIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                if (hiIt != fields.end())
                    unitTarget |= (static_cast<uint64_t>(hiIt->second) << 32);
                isTargetingPlayer = (unitTarget == playerGuid);
            }
        }
        // Creature rank for border styling (Elite=gold double border, Boss=red, Rare=silver)
        int creatureRank = -1;
        if (!isPlayer) creatureRank = gameHandler.getCreatureRank(unit->getEntry());

        // Border: gold = currently selected, orange = targeting player, dark = default
        ImU32 borderColor = isTarget
            ? IM_COL32(255, 215, 0,  A(255))
            : isTargetingPlayer
              ? IM_COL32(255, 140, 0,  A(220))   // orange = this mob is targeting you
              : IM_COL32(20,  20,  20, A(180));

        // Bar geometry
        const float barW = 80.0f * settingsPanel_.nameplateScale_;
        const float barH = 8.0f * settingsPanel_.nameplateScale_;
        const float barX = sx - barW * 0.5f;

        // Guard against division by zero when maxHealth hasn't been populated yet
        // (freshly spawned entity with default fields). 0/0 produces NaN which
        // poisons all downstream geometry; +inf is clamped but still wasteful.
        float healthPct = (unit->getMaxHealth() > 0)
            ? std::clamp(static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()), 0.0f, 1.0f)
            : 0.0f;

        drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW,               sy + barH), bgColor,    2.0f);
        // For corpses, don't fill health bar (just show grey background)
        if (!isCorpse) {
            drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW * healthPct,   sy + barH), barColor,   2.0f);
        }
        drawList->AddRect       (ImVec2(barX - 1.0f, sy - 1.0f), ImVec2(barX + barW + 1.0f, sy + barH + 1.0f), borderColor, 2.0f);

        // Elite/Boss/Rare decoration: extra outer border with rank-specific color
        if (creatureRank == 1 || creatureRank == 2) {
            // Elite / Rare Elite: gold double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(255, 200, 50, A(200)), 3.0f);
        } else if (creatureRank == 3) {
            // Boss: red double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(255, 40, 40, A(200)), 3.0f);
        } else if (creatureRank == 4) {
            // Rare: silver double border
            drawList->AddRect(ImVec2(barX - 3.0f, sy - 3.0f),
                              ImVec2(barX + barW + 3.0f, sy + barH + 3.0f),
                              IM_COL32(170, 200, 230, A(200)), 3.0f);
        }

        // HP % text centered on health bar (non-corpse, non-full-health for readability)
        if (!isCorpse && unit->getMaxHealth() > 0) {
            int hpPct = static_cast<int>(healthPct * 100.0f + 0.5f);
            char hpBuf[8];
            snprintf(hpBuf, sizeof(hpBuf), "%d%%", hpPct);
            ImVec2 hpTextSz = ImGui::CalcTextSize(hpBuf);
            float hpTx = sx - hpTextSz.x * 0.5f;
            float hpTy = sy + (barH - hpTextSz.y) * 0.5f;
            drawList->AddText(ImVec2(hpTx + 1.0f, hpTy + 1.0f), IM_COL32(0, 0, 0, A(140)), hpBuf);
            drawList->AddText(ImVec2(hpTx,         hpTy),         IM_COL32(255, 255, 255, A(200)), hpBuf);
        }

        // Cast bar below health bar when unit is casting
        float castBarBaseY = sy + barH + 2.0f;
        float nameplateBottom = castBarBaseY;  // tracks lowest drawn element for debuff dots
        {
            const auto* cs = gameHandler.getUnitCastState(guid);
            if (cs && cs->casting && cs->timeTotal > 0.0f) {
                float castPct = std::clamp((cs->timeTotal - cs->timeRemaining) / cs->timeTotal, 0.0f, 1.0f);
                const float cbH = 6.0f * settingsPanel_.nameplateScale_;

                // Spell icon + name above the cast bar
                const std::string& spellName = gameHandler.getSpellName(cs->spellId);
                {
                    auto* castAm = services_.assetManager;
                    VkDescriptorSet castIcon = (cs->spellId && castAm)
                        ? getSpellIcon(cs->spellId, castAm) : VK_NULL_HANDLE;
                    float iconSz = cbH + 8.0f;
                    if (castIcon) {
                        // Draw icon to the left of the cast bar
                        float iconX = barX - iconSz - 2.0f;
                        float iconY = castBarBaseY;
                        drawList->AddImage((ImTextureID)(uintptr_t)castIcon,
                                           ImVec2(iconX, iconY),
                                           ImVec2(iconX + iconSz, iconY + iconSz));
                        drawList->AddRect(ImVec2(iconX - 1.0f, iconY - 1.0f),
                                          ImVec2(iconX + iconSz + 1.0f, iconY + iconSz + 1.0f),
                                          IM_COL32(0, 0, 0, A(180)), 1.0f);
                    }
                    if (!spellName.empty()) {
                        ImVec2 snSz = ImGui::CalcTextSize(spellName.c_str());
                        float snX = sx - snSz.x * 0.5f;
                        float snY = castBarBaseY;
                        drawList->AddText(ImVec2(snX + 1.0f, snY + 1.0f), IM_COL32(0, 0, 0, A(140)), spellName.c_str());
                        drawList->AddText(ImVec2(snX,         snY),         IM_COL32(255, 210, 100, A(220)), spellName.c_str());
                        castBarBaseY += snSz.y + 2.0f;
                    }
                }

                // Cast bar: green = interruptible, red = uninterruptible; both pulse when >80% complete
                ImU32 cbBg = IM_COL32(30, 25, 40, A(180));
                ImU32 cbFill;
                if (castPct > 0.8f && unit->isHostile()) {
                    float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
                    cbFill = cs->interruptible
                        ? IM_COL32(static_cast<int>(40  * pulse), static_cast<int>(220 * pulse), static_cast<int>(40  * pulse), A(220))  // green pulse
                        : IM_COL32(static_cast<int>(255 * pulse), static_cast<int>(30  * pulse), static_cast<int>(30  * pulse), A(220)); // red pulse
                } else {
                    cbFill = cs->interruptible
                        ? IM_COL32(50,  190, 50,  A(200))   // green = interruptible
                        : IM_COL32(190, 40,  40,  A(200));  // red = uninterruptible
                }
                drawList->AddRectFilled(ImVec2(barX,                   castBarBaseY),
                                        ImVec2(barX + barW,             castBarBaseY + cbH), cbBg,    2.0f);
                drawList->AddRectFilled(ImVec2(barX,                   castBarBaseY),
                                        ImVec2(barX + barW * castPct,   castBarBaseY + cbH), cbFill,  2.0f);
                drawList->AddRect      (ImVec2(barX - 1.0f, castBarBaseY - 1.0f),
                                        ImVec2(barX + barW + 1.0f, castBarBaseY + cbH + 1.0f),
                                        IM_COL32(20, 10, 40, A(200)), 2.0f);

                // Time remaining text
                char timeBuf[12];
                snprintf(timeBuf, sizeof(timeBuf), "%.1fs", cs->timeRemaining);
                ImVec2 timeSz = ImGui::CalcTextSize(timeBuf);
                float timeX = sx - timeSz.x * 0.5f;
                float timeY = castBarBaseY + (cbH - timeSz.y) * 0.5f;
                drawList->AddText(ImVec2(timeX + 1.0f, timeY + 1.0f), IM_COL32(0, 0, 0, A(140)), timeBuf);
                drawList->AddText(ImVec2(timeX,         timeY),         IM_COL32(220, 200, 255, A(220)), timeBuf);
                nameplateBottom = castBarBaseY + cbH + 2.0f;
            }
        }

        // Debuff dot indicators: small colored squares below the nameplate showing
        // player-applied auras on the current hostile target.
        // Colors: Magic=blue, Curse=purple, Disease=yellow, Poison=green, Other=grey
        if (isTarget && unit->isHostile() && !isCorpse) {
            const auto& auras = gameHandler.getTargetAuras();
            const uint64_t pguid = gameHandler.getPlayerGuid();
            const float dotSize = 6.0f * settingsPanel_.nameplateScale_;
            const float dotGap  = 2.0f;
            float dotX = barX;
            for (const auto& aura : auras) {
                if (aura.isEmpty() || aura.casterGuid != pguid) continue;
                uint8_t dispelType = gameHandler.getSpellDispelType(aura.spellId);
                ImU32 dotCol;
                switch (dispelType) {
                    case 1:  dotCol = IM_COL32( 64, 128, 255, A(210)); break; // Magic   - blue
                    case 2:  dotCol = IM_COL32(160,  32, 240, A(210)); break; // Curse   - purple
                    case 3:  dotCol = IM_COL32(180, 140,  40, A(210)); break; // Disease - yellow-brown
                    case 4:  dotCol = IM_COL32( 50, 200,  50, A(210)); break; // Poison  - green
                    default: dotCol = IM_COL32(170, 170, 170, A(170)); break; // Other   - grey
                }
                drawList->AddRectFilled(ImVec2(dotX,          nameplateBottom),
                                        ImVec2(dotX + dotSize, nameplateBottom + dotSize), dotCol, 1.0f);
                drawList->AddRect      (ImVec2(dotX - 1.0f,          nameplateBottom - 1.0f),
                                        ImVec2(dotX + dotSize + 1.0f, nameplateBottom + dotSize + 1.0f),
                                        IM_COL32(0, 0, 0, A(150)), 1.0f);

                // Duration clock-sweep overlay (like target frame auras)
                uint64_t nowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remainMs = aura.getRemainingMs(nowMs);
                if (aura.maxDurationMs > 0 && remainMs > 0) {
                    float pct = 1.0f - static_cast<float>(remainMs) / static_cast<float>(aura.maxDurationMs);
                    pct = std::clamp(pct, 0.0f, 1.0f);
                    float cx = dotX + dotSize * 0.5f;
                    float cy = nameplateBottom + dotSize * 0.5f;
                    float r  = dotSize * 0.5f;
                    float startAngle = -IM_PI * 0.5f;
                    float endAngle   = startAngle + pct * IM_PI * 2.0f;
                    ImVec2 center(cx, cy);
                    const int segments = 12;
                    for (int seg = 0; seg < segments; seg++) {
                        float a0 = startAngle + (endAngle - startAngle) * seg / segments;
                        float a1 = startAngle + (endAngle - startAngle) * (seg + 1) / segments;
                        drawList->AddTriangleFilled(
                            center,
                            ImVec2(cx + r * std::cos(a0), cy + r * std::sin(a0)),
                            ImVec2(cx + r * std::cos(a1), cy + r * std::sin(a1)),
                            IM_COL32(0, 0, 0, A(100)));
                    }
                }

                // Stack count on dot (upper-left corner)
                if (aura.charges > 1) {
                    char stackBuf[8];
                    snprintf(stackBuf, sizeof(stackBuf), "%d", aura.charges);
                    drawList->AddText(ImVec2(dotX + 1.0f, nameplateBottom), IM_COL32(0, 0, 0, A(200)), stackBuf);
                    drawList->AddText(ImVec2(dotX,         nameplateBottom - 1.0f), IM_COL32(255, 255, 255, A(240)), stackBuf);
                }

                // Duration text below dot
                if (remainMs > 0) {
                    char durBuf[8];
                    if (remainMs >= 60000)
                        snprintf(durBuf, sizeof(durBuf), "%dm", remainMs / 60000);
                    else
                        snprintf(durBuf, sizeof(durBuf), "%d", remainMs / 1000);
                    ImVec2 durSz = ImGui::CalcTextSize(durBuf);
                    float durX = dotX + (dotSize - durSz.x) * 0.5f;
                    float durY = nameplateBottom + dotSize + 1.0f;
                    drawList->AddText(ImVec2(durX + 1.0f, durY + 1.0f), IM_COL32(0, 0, 0, A(180)), durBuf);
                    // Color: red if < 5s, yellow if < 15s, white otherwise
                    ImU32 durCol = remainMs < 5000 ? IM_COL32(255, 60, 60, A(240))
                                 : remainMs < 15000 ? IM_COL32(255, 200, 60, A(240))
                                 : IM_COL32(230, 230, 230, A(220));
                    drawList->AddText(ImVec2(durX, durY), durCol, durBuf);
                }

                // Spell name + duration tooltip on hover
                {
                    ImVec2 mouse = ImGui::GetMousePos();
                    if (mouse.x >= dotX && mouse.x < dotX + dotSize &&
                        mouse.y >= nameplateBottom && mouse.y < nameplateBottom + dotSize) {
                        const std::string& dotSpellName = gameHandler.getSpellName(aura.spellId);
                        if (!dotSpellName.empty()) {
                            if (remainMs > 0) {
                                int secs = remainMs / 1000;
                                int mins = secs / 60;
                                secs %= 60;
                                char tipBuf[128];
                                if (mins > 0)
                                    snprintf(tipBuf, sizeof(tipBuf), "%s (%dm %ds)", dotSpellName.c_str(), mins, secs);
                                else
                                    snprintf(tipBuf, sizeof(tipBuf), "%s (%ds)", dotSpellName.c_str(), secs);
                                ImGui::SetTooltip("%s", tipBuf);
                            } else {
                                ImGui::SetTooltip("%s", dotSpellName.c_str());
                            }
                        }
                    }
                }

                dotX += dotSize + dotGap;
                if (dotX + dotSize > barX + barW) break;
            }
        }

        // Name + level label above health bar
        uint32_t level = unit->getLevel();
        const std::string& unitName = unit->getName();
        char labelBuf[96];
        if (isPlayer) {
            // Player nameplates: show name only (no level clutter).
            // Fall back to level as placeholder while the name query is pending.
            if (!unitName.empty())
                snprintf(labelBuf, sizeof(labelBuf), "%s", unitName.c_str());
            else {
                // Name query may be pending; request it now to ensure it gets resolved
                gameHandler.queryPlayerName(unit->getGuid());
                if (level > 0)
                    snprintf(labelBuf, sizeof(labelBuf), "Player (%u)", level);
                else
                    snprintf(labelBuf, sizeof(labelBuf), "Player");
            }
        } else if (level > 0) {
            uint32_t playerLevel = gameHandler.getPlayerLevel();
            // Show skull for units more than 10 levels above the player
            if (playerLevel > 0 && level > playerLevel + 10)
                snprintf(labelBuf, sizeof(labelBuf), "?? %s", unitName.c_str());
            else
                snprintf(labelBuf, sizeof(labelBuf), "%u %s", level, unitName.c_str());
        } else {
            snprintf(labelBuf, sizeof(labelBuf), "%s", unitName.c_str());
        }
        ImVec2 textSize = ImGui::CalcTextSize(labelBuf);
        float nameX = sx - textSize.x * 0.5f;
        float nameY = sy - barH - 12.0f;
        // Name color: players get WoW class colors; NPCs use hostility (red/yellow)
        ImU32 nameColor;
        if (isPlayer) {
            // Class color with cyan fallback for unknown class
            uint8_t cid = entityClassId(unit);
            ImVec4 cc = (cid != 0) ? classColorVec4(cid) : ImVec4(0.31f, 0.78f, 1.0f, 1.0f);
            nameColor = IM_COL32(static_cast<int>(cc.x*255), static_cast<int>(cc.y*255),
                                  static_cast<int>(cc.z*255), A(230));
        } else {
            nameColor = unit->isHostile()
                ? IM_COL32(220,  80,  80, A(230))   // red  — hostile NPC
                : IM_COL32(240, 200, 100, A(230));  // yellow — friendly NPC
        }
        // Sub-label below the name: guild tag for players, subtitle for NPCs
        std::string subLabel;
        if (isPlayer) {
            uint32_t guildId = gameHandler.getEntityGuildId(guid);
            if (guildId != 0) {
                const std::string& gn = gameHandler.lookupGuildName(guildId);
                if (!gn.empty()) subLabel = "<" + gn + ">";
            }
        } else {
            // NPC subtitle (e.g. "<Reagent Vendor>", "<Innkeeper>")
            std::string sub = gameHandler.getCachedCreatureSubName(unit->getEntry());
            if (!sub.empty()) subLabel = "<" + sub + ">";
        }
        if (!subLabel.empty()) nameY -= 10.0f;  // shift name up for sub-label line

        drawList->AddText(ImVec2(nameX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), labelBuf);
        drawList->AddText(ImVec2(nameX,         nameY),         nameColor, labelBuf);

        // Sub-label below the name (WoW-style <Guild Name> or <NPC Title> in lighter color)
        if (!subLabel.empty()) {
            ImVec2 subSz = ImGui::CalcTextSize(subLabel.c_str());
            float subX = sx - subSz.x * 0.5f;
            float subY = nameY + textSize.y + 1.0f;
            drawList->AddText(ImVec2(subX + 1.0f, subY + 1.0f), IM_COL32(0, 0, 0, A(120)), subLabel.c_str());
            drawList->AddText(ImVec2(subX,         subY),         IM_COL32(180, 180, 180, A(200)), subLabel.c_str());
        }

        // Group leader crown to the right of the name on player nameplates
        if (isPlayer && gameHandler.isInGroup() &&
            gameHandler.getPartyData().leaderGuid == guid) {
            float crownX = nameX + textSize.x + 3.0f;
            const char* crownSym = "\xe2\x99\x9b";  // ♛
            drawList->AddText(ImVec2(crownX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), crownSym);
            drawList->AddText(ImVec2(crownX,         nameY),         IM_COL32(255, 215, 0, A(240)), crownSym);
        }

        // Raid mark (if any) to the left of the name
        {
            static constexpr struct { const char* sym; ImU32 col; } kNPMarks[] = {
                { "\xe2\x98\x85", IM_COL32(255,220, 50,230) },  // Star
                { "\xe2\x97\x8f", IM_COL32(255,140,  0,230) },  // Circle
                { "\xe2\x97\x86", IM_COL32(160, 32,240,230) },  // Diamond
                { "\xe2\x96\xb2", IM_COL32( 50,200, 50,230) },  // Triangle
                { "\xe2\x97\x8c", IM_COL32( 80,160,255,230) },  // Moon
                { "\xe2\x96\xa0", IM_COL32( 50,200,220,230) },  // Square
                { "\xe2\x9c\x9d", IM_COL32(255, 80, 80,230) },  // Cross
                { "\xe2\x98\xa0", IM_COL32(255,255,255,230) },  // Skull
            };
            uint8_t raidMark = gameHandler.getEntityRaidMark(guid);
            if (raidMark < game::GameHandler::kRaidMarkCount) {
                float markX = nameX - 14.0f;
                drawList->AddText(ImVec2(markX + 1.0f, nameY + 1.0f), IM_COL32(0,0,0,120), kNPMarks[raidMark].sym);
                drawList->AddText(ImVec2(markX,         nameY),        kNPMarks[raidMark].col, kNPMarks[raidMark].sym);
            }

            // Quest kill objective indicator: small yellow sword icon to the right of the name
            float questIconX = nameX + textSize.x + 4.0f;
            if (!isPlayer && questKillEntries.count(unit->getEntry())) {
                const char* objSym = "\xe2\x9a\x94";  // ⚔ crossed swords (UTF-8)
                drawList->AddText(ImVec2(questIconX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), objSym);
                drawList->AddText(ImVec2(questIconX,         nameY),         IM_COL32(255, 220, 0, A(230)), objSym);
                questIconX += ImGui::CalcTextSize("\xe2\x9a\x94").x + 2.0f;
            }

            // Quest giver indicator: "!" for available quests, "?" for completable/incomplete
            if (!isPlayer) {
                using QGS = game::QuestGiverStatus;
                QGS qgs = gameHandler.getQuestGiverStatus(guid);
                const char* qSym = nullptr;
                ImU32 qCol = IM_COL32(255, 210, 0, A(255));
                if (qgs == QGS::AVAILABLE) {
                    qSym = "!";
                } else if (qgs == QGS::AVAILABLE_LOW) {
                    qSym = "!";
                    qCol = IM_COL32(160, 160, 160, A(220));
                } else if (qgs == QGS::REWARD || qgs == QGS::REWARD_REP) {
                    qSym = "?";
                } else if (qgs == QGS::INCOMPLETE) {
                    qSym = "?";
                    qCol = IM_COL32(160, 160, 160, A(220));
                }
                if (qSym) {
                    drawList->AddText(ImVec2(questIconX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), qSym);
                    drawList->AddText(ImVec2(questIconX,         nameY),         qCol, qSym);
                }
            }
        }

        // Click to target / right-click context: detect clicks inside the nameplate region.
        // Use the wider of name text or health bar for the horizontal hit area so short
        // names like "Wolf" don't produce a tiny clickable strip narrower than the bar.
        if (!ImGui::GetIO().WantCaptureMouse) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float hitLeft  = std::min(nameX, barX) - 2.0f;
            float hitRight = std::max(nameX + textSize.x, barX + barW) + 2.0f;
            float ny0 = nameY - 1.0f;
            float ny1 = sy + barH + 2.0f;
            float nx0 = hitLeft;
            float nx1 = hitRight;
            if (mouse.x >= nx0 && mouse.x <= nx1 && mouse.y >= ny0 && mouse.y <= ny1) {
                // Track mouseover for [target=mouseover] macro conditionals
                gameHandler.setMouseoverGuid(guid);
                // Hover tooltip: name, level/class, guild
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(unitName.c_str());
                if (isPlayer) {
                    uint8_t cid = entityClassId(unit);
                    ImGui::Text("Level %u %s", level, classNameStr(cid));
                } else if (level > 0) {
                    ImGui::Text("Level %u", level);
                }
                if (!subLabel.empty()) ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1.0f), "%s", subLabel.c_str());
                ImGui::EndTooltip();
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    gameHandler.setTarget(guid);
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    nameplateCtxGuid_ = guid;
                    nameplateCtxPos_  = mouse;
                    ImGui::OpenPopup("##NameplateCtx");
                }
            }
        }
    }

    // Render nameplate context popup (uses a tiny overlay window as host)
    if (nameplateCtxGuid_ != 0) {
        ImGui::SetNextWindowPos(nameplateCtxPos_, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
        ImGuiWindowFlags ctxHostFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##NameplateCtxHost", nullptr, ctxHostFlags)) {
            if (ImGui::BeginPopup("##NameplateCtx")) {
                auto entityPtr = gameHandler.getEntityManager().getEntity(nameplateCtxGuid_);
                std::string ctxName = entityPtr ? getEntityName(entityPtr) : "";
                if (!ctxName.empty()) {
                    ImGui::TextDisabled("%s", ctxName.c_str());
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Target"))
                    gameHandler.setTarget(nameplateCtxGuid_);
                if (ImGui::MenuItem("Set Focus"))
                    gameHandler.setFocus(nameplateCtxGuid_);
                bool isPlayer = entityPtr && entityPtr->getType() == game::ObjectType::PLAYER;
                if (isPlayer && !ctxName.empty()) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Whisper")) {
                        chatPanel_.setWhisperTarget(ctxName);
                    }
                    if (ImGui::MenuItem("Invite to Group"))
                        gameHandler.inviteToGroup(ctxName);
                    if (ImGui::MenuItem("Trade"))
                        gameHandler.initiateTrade(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Duel"))
                        gameHandler.proposeDuel(nameplateCtxGuid_);
                    if (ImGui::MenuItem("Inspect")) {
                        gameHandler.setTarget(nameplateCtxGuid_);
                        gameHandler.inspectTarget();
                        socialPanel_.showInspectWindow_ = true;
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add Friend"))
                        gameHandler.addFriend(ctxName);
                    if (ImGui::MenuItem("Ignore"))
                        gameHandler.addIgnore(ctxName);
                }
                ImGui::EndPopup();
            } else {
                nameplateCtxGuid_ = 0;
            }
        }
        ImGui::End();
    }
}

// ============================================================
// Durability Warning (equipment damage indicator)
// ============================================================

void GameScreen::takeScreenshot(game::GameHandler& /*gameHandler*/) {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    // Build path: ~/.wowee/screenshots/WoWee_YYYYMMDD_HHMMSS.png
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.wowee/screenshots";

    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    char filename[128];
    std::snprintf(filename, sizeof(filename),
                  "WoWee_%04d%02d%02d_%02d%02d%02d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string path = dir + "/" + filename;

    if (renderer->captureScreenshot(path)) {
        game::MessageChatData sysMsg;
        sysMsg.type = game::ChatType::SYSTEM;
        sysMsg.language = game::ChatLanguage::UNIVERSAL;
        sysMsg.message = "Screenshot saved: " + path;
        services_.gameHandler->addLocalChatMessage(sysMsg);
    }
}

void GameScreen::renderDurabilityWarning(game::GameHandler& gameHandler) {
    if (gameHandler.getPlayerGuid() == 0) return;

    const auto& inv = gameHandler.getInventory();

    // Scan all equipment slots (skip bag slots which have no durability)
    float minDurPct = 1.0f;
    bool hasBroken = false;

    for (int i = static_cast<int>(game::EquipSlot::HEAD);
             i < static_cast<int>(game::EquipSlot::BAG1); ++i) {
        const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(i));
        if (slot.empty() || slot.item.maxDurability == 0) continue;
        if (slot.item.curDurability == 0) {
            hasBroken = true;
        }
        float pct = static_cast<float>(slot.item.curDurability) /
                    static_cast<float>(slot.item.maxDurability);
        if (pct < minDurPct) minDurPct = pct;
    }

    // Only show warning below 20%
    if (minDurPct >= 0.2f && !hasBroken) return;

    ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;

    // Position: just above the XP bar / action bar area (bottom-center)
    const float warningW = 220.0f;
    const float warningH = 26.0f;
    const float posX = (screenW - warningW) * 0.5f;
    const float posY = screenH - 140.0f;  // above action bar

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(warningW, warningH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##durability_warn", nullptr, flags)) {
        if (hasBroken) {
            ImGui::TextColored(ImVec4(1.0f, 0.15f, 0.15f, 1.0f),
                               "\xef\x94\x9b Gear broken! Visit a repair NPC");
        } else {
            int pctInt = static_cast<int>(minDurPct * 100.0f);
            ImGui::TextColored(colors::kSymbolGold,
                               "\xef\x94\x9b Low durability: %d%%", pctInt);
        }
        if (ImGui::IsWindowHovered())
            ImGui::SetTooltip("Your equipment is damaged. Visit any blacksmith or repair NPC.");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ============================================================
// UI Error Frame (WoW-style center-bottom error overlay)
// ============================================================

void GameScreen::renderUIErrors(game::GameHandler& /*gameHandler*/, float deltaTime) {
    // Age out old entries
    for (auto& e : uiErrors_) e.age += deltaTime;
    uiErrors_.erase(
        std::remove_if(uiErrors_.begin(), uiErrors_.end(),
            [](const UIErrorEntry& e) { return e.age >= kUIErrorLifetime; }),
        uiErrors_.end());

    if (uiErrors_.empty()) return;

    auto* window = services_.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    // Fixed invisible overlay
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("##UIErrors", nullptr, flags)) {
        // Render messages stacked above the action bar (~200px from bottom)
        // The newest message is on top; older ones fade below it.
        const float baseY = screenH - 200.0f;
        const float lineH = 20.0f;
        const int   count = static_cast<int>(uiErrors_.size());

        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = count - 1; i >= 0; --i) {
            const auto& e = uiErrors_[i];
            float alpha = 1.0f - (e.age / kUIErrorLifetime);
            alpha = std::max(0.0f, std::min(1.0f, alpha));

            // Fade fast in the last 0.5 s
            if (e.age > kUIErrorLifetime - 0.5f)
                alpha *= (kUIErrorLifetime - e.age) / 0.5f;

            uint8_t a8 = static_cast<uint8_t>(alpha * 255.0f);
            ImU32 textCol  = IM_COL32(255, 50,  50, a8);
            ImU32 shadowCol= IM_COL32(  0,  0,   0, static_cast<uint8_t>(alpha * 180));

            const char* txt = e.text.c_str();
            ImVec2 sz = ImGui::CalcTextSize(txt);
            float x = std::round((screenW - sz.x) * 0.5f);
            float y = std::round(baseY - (count - 1 - i) * lineH);

            // Drop shadow
            draw->AddText(ImVec2(x + 1, y + 1), shadowCol, txt);
            draw->AddText(ImVec2(x, y), textCol, txt);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void GameScreen::renderQuestMarkers(game::GameHandler& gameHandler) {
    const auto& statuses = gameHandler.getNpcQuestStatuses();
    if (statuses.empty()) return;

    auto* renderer = services_.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* window = services_.window;
    if (!camera || !window) return;

    float screenW = static_cast<float>(window->getWidth());
    float screenH = static_cast<float>(window->getHeight());
    glm::mat4 viewProj = camera->getViewProjectionMatrix();
    auto* drawList = ImGui::GetForegroundDrawList();

    for (const auto& [guid, status] : statuses) {
        // Only show markers for available (!) and reward/completable (?)
        const char* marker = nullptr;
        ImU32 color = IM_COL32(255, 210, 0, 255); // yellow
        if (status == game::QuestGiverStatus::AVAILABLE) {
            marker = "!";
        } else if (status == game::QuestGiverStatus::AVAILABLE_LOW) {
            marker = "!";
            color = IM_COL32(160, 160, 160, 255); // gray
        } else if (status == game::QuestGiverStatus::REWARD ||
                   status == game::QuestGiverStatus::REWARD_REP) {
            marker = "?";
        } else if (status == game::QuestGiverStatus::INCOMPLETE) {
            marker = "?";
            color = IM_COL32(160, 160, 160, 255); // gray
        } else {
            continue;
        }

        // Get entity position (canonical coords)
        auto entity = gameHandler.getEntityManager().getEntity(guid);
        if (!entity) continue;

        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
        glm::vec3 renderPos = core::coords::canonicalToRender(canonical);

        // Get model height for offset
        float heightOffset = 3.0f;
        glm::vec3 boundsCenter;
        float boundsRadius = 0.0f;
        if (core::Application::getInstance().getRenderBoundsForGuid(guid, boundsCenter, boundsRadius)) {
            heightOffset = boundsRadius * 2.0f + 1.0f;
        }
        renderPos.z += heightOffset;

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float sx = (ndc.x + 1.0f) * 0.5f * screenW;
        float sy = (1.0f - ndc.y) * 0.5f * screenH;

        // Skip if off-screen
        if (sx < -50 || sx > screenW + 50 || sy < -50 || sy > screenH + 50) continue;

        // Scale text size based on distance
        float dist = clipPos.w;
        float fontSize = std::clamp(800.0f / dist, 14.0f, 48.0f);

        // Draw outlined text: 4 shadow copies then main text
        ImFont* font = ImGui::GetFont();
        ImU32 outlineColor = IM_COL32(0, 0, 0, 220);
        float off = std::max(1.0f, fontSize * 0.06f);
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, marker);
        float tx = sx - textSize.x * 0.5f;
        float ty = sy - textSize.y * 0.5f;

        drawList->AddText(font, fontSize, ImVec2(tx - off, ty), outlineColor, marker);
        drawList->AddText(font, fontSize, ImVec2(tx + off, ty), outlineColor, marker);
        drawList->AddText(font, fontSize, ImVec2(tx, ty - off), outlineColor, marker);
        drawList->AddText(font, fontSize, ImVec2(tx, ty + off), outlineColor, marker);
        drawList->AddText(font, fontSize, ImVec2(tx, ty), color, marker);
    }
}

void GameScreen::renderMinimapMarkers(game::GameHandler& gameHandler) {
    const auto& statuses = gameHandler.getNpcQuestStatuses();
    auto* renderer = services_.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    auto* window = services_.window;
    if (!camera || !minimap || !window) return;

    float screenW = static_cast<float>(window->getWidth());

    // Minimap parameters (matching minimap.cpp)
    float mapSize = 200.0f;
    float margin = 10.0f;
    float mapRadius = mapSize * 0.5f;
    float centerX = screenW - margin - mapRadius;
    float centerY = margin + mapRadius;
    float viewRadius = minimap->getViewRadius();

    // Use the exact same minimap center as Renderer::renderWorld() to keep markers anchored.
    glm::vec3 playerRender = camera->getPosition();
    if (renderer->getCharacterInstanceId() != 0) {
        playerRender = renderer->getCharacterPosition();
    }

    // Camera bearing for minimap rotation
    float bearing = 0.0f;
    float cosB = 1.0f;
    float sinB = 0.0f;
    if (minimap->isRotateWithCamera()) {
        glm::vec3 fwd = camera->getForward();
        // Render space: +X=West, +Y=North. Camera fwd=(cos(yaw),sin(yaw)).
        // Clockwise bearing from North: atan2(fwd.y, -fwd.x).
        bearing = std::atan2(fwd.y, -fwd.x);
        cosB = std::cos(bearing);
        sinB = std::sin(bearing);
    }

    auto* drawList = ImGui::GetForegroundDrawList();

    auto projectToMinimap = [&](const glm::vec3& worldRenderPos, float& sx, float& sy) -> bool {
        float dx = worldRenderPos.x - playerRender.x;
        float dy = worldRenderPos.y - playerRender.y;

        // Exact inverse of minimap display shader:
        //   shader: mapUV = playerUV + vec2(-rotated.x, rotated.y) * zoom * 2
        //   where rotated = R(bearing) * center, center in [-0.5, 0.5]
        // Inverse: center = R^-1(bearing) * (-deltaUV.x, deltaUV.y) / (zoom*2)
        // With deltaUV.x ∝ +dx (render +X=west=larger U) and deltaUV.y ∝ -dy (V increases south):
        float rx = -(dx * cosB + dy * sinB);
        float ry =   dx * sinB - dy * cosB;

        // Scale to minimap pixels
        float px = rx / viewRadius * mapRadius;
        float py = ry / viewRadius * mapRadius;

        float distFromCenter = std::sqrt(px * px + py * py);
        if (distFromCenter > mapRadius - 3.0f) {
            return false;
        }

        sx = centerX + px;
        sy = centerY + py;
        return true;
    };

    // Build sets of entries that are incomplete objectives for tracked quests.
    // minimapQuestEntries: NPC creature entries (npcOrGoId > 0)
    // minimapQuestGoEntries: game object entries (npcOrGoId < 0, stored as abs value)
    std::unordered_set<uint32_t> minimapQuestEntries;
    std::unordered_set<uint32_t> minimapQuestGoEntries;
    {
        const auto& ql = gameHandler.getQuestLog();
        const auto& tq = gameHandler.getTrackedQuestIds();
        for (const auto& q : ql) {
            if (q.complete || q.questId == 0) continue;
            if (!tq.empty() && !tq.count(q.questId)) continue;
            for (const auto& obj : q.killObjectives) {
                if (obj.required == 0) continue;
                if (obj.npcOrGoId > 0) {
                    auto it = q.killCounts.find(static_cast<uint32_t>(obj.npcOrGoId));
                    if (it == q.killCounts.end() || it->second.first < it->second.second)
                        minimapQuestEntries.insert(static_cast<uint32_t>(obj.npcOrGoId));
                } else if (obj.npcOrGoId < 0) {
                    uint32_t goEntry = static_cast<uint32_t>(-obj.npcOrGoId);
                    auto it = q.killCounts.find(goEntry);
                    if (it == q.killCounts.end() || it->second.first < it->second.second)
                        minimapQuestGoEntries.insert(goEntry);
                }
            }
        }
    }

    // Optional base nearby NPC dots (independent of quest status packets).
    if (settingsPanel_.minimapNpcDots_) {
        ImVec2 mouse = ImGui::GetMousePos();
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit || unit->getHealth() == 0) continue;

            glm::vec3 npcRender = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(npcRender, sx, sy)) continue;

            bool isQuestTarget = minimapQuestEntries.count(unit->getEntry()) != 0;
            if (isQuestTarget) {
                // Quest kill objective: larger gold dot with dark outline
                drawList->AddCircleFilled(ImVec2(sx, sy), 3.5f, IM_COL32(255, 210, 30, 240));
                drawList->AddCircle(ImVec2(sx, sy), 3.5f, IM_COL32(80, 50, 0, 180), 0, 1.0f);
                // Tooltip on hover showing unit name
                float mdx = mouse.x - sx, mdy = mouse.y - sy;
                if (mdx * mdx + mdy * mdy < 64.0f) {
                    const std::string& nm = unit->getName();
                    if (!nm.empty()) ImGui::SetTooltip("%s (quest)", nm.c_str());
                }
            } else {
                ImU32 baseDot = unit->isHostile() ? IM_COL32(220, 70, 70, 220) : IM_COL32(245, 245, 245, 210);
                drawList->AddCircleFilled(ImVec2(sx, sy), 1.0f, baseDot);
            }
        }
    }

    // Nearby other-player dots — shown when NPC dots are enabled.
    // Party members are already drawn as squares above; other players get a small circle.
    if (settingsPanel_.minimapNpcDots_) {
        const uint64_t selfGuid = gameHandler.getPlayerGuid();
        const auto& partyData = gameHandler.getPartyData();
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::PLAYER) continue;
            if (entity->getGuid() == selfGuid) continue;  // skip self (already drawn as arrow)

            // Skip party members (already drawn as squares above)
            bool isPartyMember = false;
            for (const auto& m : partyData.members) {
                if (m.guid == guid) { isPartyMember = true; break; }
            }
            if (isPartyMember) continue;

            glm::vec3 pRender = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(pRender, sx, sy)) continue;

            // Blue dot for other nearby players
            drawList->AddCircleFilled(ImVec2(sx, sy), 2.0f, IM_COL32(80, 160, 255, 220));
        }
    }

    // Lootable corpse dots: small yellow-green diamonds on dead, lootable units.
    // Shown whenever NPC dots are enabled (or always, since they're always useful).
    {
        constexpr uint32_t UNIT_DYNFLAG_LOOTABLE = 0x0001;
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit) continue;
            // Must be dead (health == 0) and marked lootable
            if (unit->getHealth() != 0) continue;
            if (!(unit->getDynamicFlags() & UNIT_DYNFLAG_LOOTABLE)) continue;

            glm::vec3 npcRender = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(npcRender, sx, sy)) continue;

            // Draw a small diamond (rotated square) in light yellow-green
            const float dr = 3.5f;
            ImVec2 top  (sx,      sy - dr);
            ImVec2 right(sx + dr, sy     );
            ImVec2 bot  (sx,      sy + dr);
            ImVec2 left (sx - dr, sy     );
            drawList->AddQuadFilled(top, right, bot, left, IM_COL32(180, 230, 80, 230));
            drawList->AddQuad      (top, right, bot, left, IM_COL32(60,  80,  20, 200), 1.0f);

            // Tooltip on hover
            if (ImGui::IsMouseHoveringRect(ImVec2(sx - dr, sy - dr), ImVec2(sx + dr, sy + dr))) {
                const std::string& nm = unit->getName();
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.3f, 1.0f), "%s",
                                   nm.empty() ? "Lootable corpse" : nm.c_str());
                ImGui::EndTooltip();
            }
        }
    }

    // Interactable game object dots (chests, resource nodes) when NPC dots are enabled.
    // Shown as small orange triangles to distinguish from unit dots and loot corpses.
    if (settingsPanel_.minimapNpcDots_) {
        ImVec2 mouse = ImGui::GetMousePos();
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::GAMEOBJECT) continue;

            // Only show objects that are likely interactive (chests/nodes: type 3;
            // also show type 0=Door when open, but filter by dynamic-flag ACTIVATED).
            // For simplicity, show all game objects that have a non-empty cached name.
            auto go = std::static_pointer_cast<game::GameObject>(entity);
            if (!go) continue;

            // Only show if we have name data (avoids cluttering with unknown objects)
            const auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
            if (!goInfo || !goInfo->isValid()) continue;
            // Skip transport objects (boats/zeppelins): type 15 = MO_TRANSPORT, 11 = TRANSPORT
            if (goInfo->type == 11 || goInfo->type == 15) continue;

            glm::vec3 goRender = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(goRender, sx, sy)) continue;

            // Triangle size and color: bright cyan for quest objectives, amber for others
            bool isQuestGO = minimapQuestGoEntries.count(go->getEntry()) != 0;
            const float ts = isQuestGO ? 4.5f : 3.5f;
            ImVec2 goTip  (sx,        sy - ts);
            ImVec2 goLeft (sx - ts,   sy + ts * 0.6f);
            ImVec2 goRight(sx + ts,   sy + ts * 0.6f);
            if (isQuestGO) {
                drawList->AddTriangleFilled(goTip, goLeft, goRight, IM_COL32(50, 230, 255, 240));
                drawList->AddTriangle(goTip, goLeft, goRight, IM_COL32(0, 60, 80, 200), 1.5f);
            } else {
                drawList->AddTriangleFilled(goTip, goLeft, goRight, IM_COL32(255, 185, 30, 220));
                drawList->AddTriangle(goTip, goLeft, goRight, IM_COL32(100, 60, 0, 180), 1.0f);
            }

            // Tooltip on hover
            float mdx = mouse.x - sx, mdy = mouse.y - sy;
            if (mdx * mdx + mdy * mdy < 64.0f) {
                if (isQuestGO)
                    ImGui::SetTooltip("%s (quest)", goInfo->name.c_str());
                else
                    ImGui::SetTooltip("%s", goInfo->name.c_str());
            }
        }
    }

    // Party member dots on minimap — small colored squares with name tooltip on hover
    if (gameHandler.isInGroup()) {
        const auto& partyData = gameHandler.getPartyData();
        ImVec2 mouse = ImGui::GetMousePos();
        for (const auto& member : partyData.members) {
            if (!member.hasPartyStats) continue;
            bool isOnline = (member.onlineStatus & 0x0001) != 0;
            bool isDead   = (member.onlineStatus & 0x0020) != 0;
            bool isGhost  = (member.onlineStatus & 0x0010) != 0;
            if (!isOnline) continue;
            if (member.posX == 0 && member.posY == 0) continue;

            // Party stat positions: posY = canonical X (north), posX = canonical Y (west)
            glm::vec3 memberRender = core::coords::canonicalToRender(
                glm::vec3(static_cast<float>(member.posY),
                          static_cast<float>(member.posX), 0.0f));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(memberRender, sx, sy)) continue;

            // Determine dot color: class color > leader gold > light blue
            ImU32 dotCol;
            if (isDead || isGhost) {
                dotCol = IM_COL32(140, 140, 140, 200);  // gray for dead
            } else {
                auto mEnt = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(mEnt.get());
                if (cid != 0) {
                    ImVec4 cv = classColorVec4(cid);
                    dotCol = IM_COL32(
                        static_cast<int>(cv.x * 255),
                        static_cast<int>(cv.y * 255),
                        static_cast<int>(cv.z * 255), 230);
                } else if (member.guid == partyData.leaderGuid) {
                    dotCol = IM_COL32(255, 210, 0, 230);  // gold for leader
                } else {
                    dotCol = IM_COL32(100, 180, 255, 230); // blue for others
                }
            }

            // Draw a small square (WoW-style party member dot)
            const float hs = 3.5f;
            drawList->AddRectFilled(ImVec2(sx - hs, sy - hs), ImVec2(sx + hs, sy + hs), dotCol, 1.0f);
            drawList->AddRect(ImVec2(sx - hs, sy - hs), ImVec2(sx + hs, sy + hs),
                              IM_COL32(0, 0, 0, 180), 1.0f, 0, 1.0f);

            // Name tooltip on hover
            float mdx = mouse.x - sx, mdy = mouse.y - sy;
            if (mdx * mdx + mdy * mdy < 64.0f && !member.name.empty()) {
                ImGui::SetTooltip("%s", member.name.c_str());
            }
        }
    }

    for (const auto& [guid, status] : statuses) {
        ImU32 dotColor;
        const char* marker = nullptr;
        if (status == game::QuestGiverStatus::AVAILABLE) {
            dotColor = IM_COL32(255, 210, 0, 255);
            marker = "!";
        } else if (status == game::QuestGiverStatus::AVAILABLE_LOW) {
            dotColor = IM_COL32(160, 160, 160, 255);
            marker = "!";
        } else if (status == game::QuestGiverStatus::REWARD ||
                   status == game::QuestGiverStatus::REWARD_REP) {
            dotColor = IM_COL32(255, 210, 0, 255);
            marker = "?";
        } else if (status == game::QuestGiverStatus::INCOMPLETE) {
            dotColor = IM_COL32(160, 160, 160, 255);
            marker = "?";
        } else {
            continue;
        }

        auto entity = gameHandler.getEntityManager().getEntity(guid);
        if (!entity) continue;

        glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ());
        glm::vec3 npcRender = core::coords::canonicalToRender(canonical);

        float sx = 0.0f, sy = 0.0f;
        if (!projectToMinimap(npcRender, sx, sy)) continue;

        // Draw dot with marker text
        drawList->AddCircleFilled(ImVec2(sx, sy), 5.0f, dotColor);
        ImFont* font = ImGui::GetFont();
        ImVec2 textSize = font->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, marker);
        drawList->AddText(font, 11.0f,
            ImVec2(sx - textSize.x * 0.5f, sy - textSize.y * 0.5f),
            IM_COL32(0, 0, 0, 255), marker);

        // Show NPC name and quest status on hover
        {
            ImVec2 mouse = ImGui::GetMousePos();
            float mdx = mouse.x - sx, mdy = mouse.y - sy;
            if (mdx * mdx + mdy * mdy < 64.0f) {
                std::string npcName;
                if (entity->getType() == game::ObjectType::UNIT) {
                    auto npcUnit = std::static_pointer_cast<game::Unit>(entity);
                    npcName = npcUnit->getName();
                }
                if (!npcName.empty()) {
                    bool hasQuest = (status == game::QuestGiverStatus::AVAILABLE ||
                                     status == game::QuestGiverStatus::AVAILABLE_LOW);
                    ImGui::SetTooltip("%s\n%s", npcName.c_str(),
                                      hasQuest ? "Has a quest for you" : "Quest ready to turn in");
                }
            }
        }
    }

    // Quest kill objective markers — highlight live NPCs matching active quest kill objectives
    {
        // Build map of NPC entry → (quest title, current, required) for tooltips
        struct KillInfo { std::string questTitle; uint32_t current = 0; uint32_t required = 0; };
        std::unordered_map<uint32_t, KillInfo> killInfoMap;
        const auto& trackedIds = gameHandler.getTrackedQuestIds();
        for (const auto& quest : gameHandler.getQuestLog()) {
            if (quest.complete) continue;
            if (!trackedIds.empty() && !trackedIds.count(quest.questId)) continue;
            for (const auto& obj : quest.killObjectives) {
                if (obj.npcOrGoId <= 0 || obj.required == 0) continue;
                uint32_t npcEntry = static_cast<uint32_t>(obj.npcOrGoId);
                auto it = quest.killCounts.find(npcEntry);
                uint32_t current = (it != quest.killCounts.end()) ? it->second.first : 0;
                if (current < obj.required) {
                    killInfoMap[npcEntry] = { quest.title, current, obj.required };
                }
            }
        }

        if (!killInfoMap.empty()) {
            ImVec2 mouse = ImGui::GetMousePos();
            for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                if (!entity || entity->getType() != game::ObjectType::UNIT) continue;
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                if (!unit || unit->getHealth() == 0) continue;
                auto infoIt = killInfoMap.find(unit->getEntry());
                if (infoIt == killInfoMap.end()) continue;

                glm::vec3 unitRender = core::coords::canonicalToRender(
                    glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                float sx = 0.0f, sy = 0.0f;
                if (!projectToMinimap(unitRender, sx, sy)) continue;

                // Gold circle with a dark "x" mark — indicates a quest kill target
                drawList->AddCircleFilled(ImVec2(sx, sy), 5.0f, IM_COL32(255, 185, 0, 240));
                drawList->AddCircle(ImVec2(sx, sy), 5.5f, IM_COL32(0, 0, 0, 180), 12, 1.0f);
                drawList->AddLine(ImVec2(sx - 2.5f, sy - 2.5f), ImVec2(sx + 2.5f, sy + 2.5f),
                                  IM_COL32(20, 20, 20, 230), 1.2f);
                drawList->AddLine(ImVec2(sx + 2.5f, sy - 2.5f), ImVec2(sx - 2.5f, sy + 2.5f),
                                  IM_COL32(20, 20, 20, 230), 1.2f);

                // Tooltip on hover
                float mdx = mouse.x - sx, mdy = mouse.y - sy;
                if (mdx * mdx + mdy * mdy < 64.0f) {
                    const auto& ki = infoIt->second;
                    const std::string& npcName = unit->getName();
                    if (!npcName.empty()) {
                        ImGui::SetTooltip("%s\n%s: %u/%u",
                            npcName.c_str(),
                            ki.questTitle.empty() ? "Quest" : ki.questTitle.c_str(),
                            ki.current, ki.required);
                    } else {
                        ImGui::SetTooltip("%s: %u/%u",
                            ki.questTitle.empty() ? "Quest" : ki.questTitle.c_str(),
                            ki.current, ki.required);
                    }
                }
            }
        }
    }

    // Gossip POI markers (quest / NPC navigation targets)
    for (const auto& poi : gameHandler.getGossipPois()) {
        // Convert WoW canonical coords to render coords for minimap projection
        glm::vec3 poiRender = core::coords::canonicalToRender(glm::vec3(poi.x, poi.y, 0.0f));
        float sx = 0.0f, sy = 0.0f;
        if (!projectToMinimap(poiRender, sx, sy)) continue;

        // Draw as a cyan diamond with tooltip on hover
        const float d = 5.0f;
        ImVec2 pts[4] = {
            { sx,     sy - d },
            { sx + d, sy     },
            { sx,     sy + d },
            { sx - d, sy     },
        };
        drawList->AddConvexPolyFilled(pts, 4, IM_COL32(0, 210, 255, 220));
        drawList->AddPolyline(pts, 4, IM_COL32(255, 255, 255, 160), true, 1.0f);

        // Show name label if cursor is within ~8px
        ImVec2 cursorPos = ImGui::GetMousePos();
        float dx = cursorPos.x - sx, dy = cursorPos.y - sy;
        if (!poi.name.empty() && (dx * dx + dy * dy) < 64.0f) {
            ImGui::SetTooltip("%s", poi.name.c_str());
        }
    }

    // Minimap pings from party members
    for (const auto& ping : gameHandler.getMinimapPings()) {
        glm::vec3 pingRender = core::coords::canonicalToRender(glm::vec3(ping.wowX, ping.wowY, 0.0f));
        float sx = 0.0f, sy = 0.0f;
        if (!projectToMinimap(pingRender, sx, sy)) continue;

        float t = ping.age / game::GameHandler::MinimapPing::LIFETIME;
        float alpha = 1.0f - t;
        float pulse = 1.0f + 1.5f * t;  // expands outward as it fades

        ImU32 col  = IM_COL32(255, 220, 0, static_cast<int>(alpha * 200));
        ImU32 col2 = IM_COL32(255, 150, 0, static_cast<int>(alpha * 100));
        float r1 = 4.0f * pulse;
        float r2 = 8.0f * pulse;
        drawList->AddCircle(ImVec2(sx, sy), r1, col, 16, 2.0f);
        drawList->AddCircle(ImVec2(sx, sy), r2, col2, 16, 1.0f);
        drawList->AddCircleFilled(ImVec2(sx, sy), 2.5f, col);
    }

    // Party member dots on minimap
    {
        const auto& partyData = gameHandler.getPartyData();
        const uint64_t leaderGuid = partyData.leaderGuid;
        for (const auto& member : partyData.members) {
            if (!member.isOnline || !member.hasPartyStats) continue;
            if (member.posX == 0 && member.posY == 0) continue;

            // posX/posY follow same server axis convention as minimap pings:
            // server posX = east/west axis → canonical Y (west)
            // server posY = north/south axis → canonical X (north)
            float wowX = static_cast<float>(member.posY);
            float wowY = static_cast<float>(member.posX);
            glm::vec3 memberRender = core::coords::canonicalToRender(glm::vec3(wowX, wowY, 0.0f));

            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(memberRender, sx, sy)) continue;

            ImU32 dotColor;
            {
                auto mEnt = gameHandler.getEntityManager().getEntity(member.guid);
                uint8_t cid = entityClassId(mEnt.get());
                dotColor = (cid != 0)
                    ? classColorU32(cid, 235)
                    : (member.guid == leaderGuid)
                        ? IM_COL32(255, 210, 0, 235)
                        : IM_COL32(100, 180, 255, 235);
            }
            drawList->AddCircleFilled(ImVec2(sx, sy), 4.0f, dotColor);
            drawList->AddCircle(ImVec2(sx, sy), 4.0f, IM_COL32(255, 255, 255, 160), 12, 1.0f);

            // Raid mark: tiny symbol drawn above the dot
            {
                static constexpr struct { const char* sym; ImU32 col; } kMMMarks[] = {
                    { "\xe2\x98\x85", IM_COL32(255, 220,  50, 255) },
                    { "\xe2\x97\x8f", IM_COL32(255, 140,   0, 255) },
                    { "\xe2\x97\x86", IM_COL32(160,  32, 240, 255) },
                    { "\xe2\x96\xb2", IM_COL32( 50, 200,  50, 255) },
                    { "\xe2\x97\x8c", IM_COL32( 80, 160, 255, 255) },
                    { "\xe2\x96\xa0", IM_COL32( 50, 200, 220, 255) },
                    { "\xe2\x9c\x9d", IM_COL32(255,  80,  80, 255) },
                    { "\xe2\x98\xa0", IM_COL32(255, 255, 255, 255) },
                };
                uint8_t pmk = gameHandler.getEntityRaidMark(member.guid);
                if (pmk < game::GameHandler::kRaidMarkCount) {
                    ImFont* mmFont = ImGui::GetFont();
                    ImVec2 msz = mmFont->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, kMMMarks[pmk].sym);
                    drawList->AddText(mmFont, 9.0f,
                        ImVec2(sx - msz.x * 0.5f, sy - 4.0f - msz.y),
                        kMMMarks[pmk].col, kMMMarks[pmk].sym);
                }
            }

            ImVec2 cursorPos = ImGui::GetMousePos();
            float mdx = cursorPos.x - sx, mdy = cursorPos.y - sy;
            if (!member.name.empty() && (mdx * mdx + mdy * mdy) < 64.0f) {
                uint8_t pmk2 = gameHandler.getEntityRaidMark(member.guid);
                if (pmk2 < game::GameHandler::kRaidMarkCount) {
                    static constexpr const char* kMarkNames[] = {
                        "Star", "Circle", "Diamond", "Triangle",
                        "Moon", "Square", "Cross", "Skull"
                    };
                    ImGui::SetTooltip("%s {%s}", member.name.c_str(), kMarkNames[pmk2]);
                } else {
                    ImGui::SetTooltip("%s", member.name.c_str());
                }
            }
        }
    }

    // BG flag carrier / important player positions (MSG_BATTLEGROUND_PLAYER_POSITIONS)
    {
        const auto& bgPositions = gameHandler.getBgPlayerPositions();
        if (!bgPositions.empty()) {
            ImVec2 mouse = ImGui::GetMousePos();
            // group 0 = typically ally-held flag / first list; group 1 = enemy
            static const ImU32 kBgGroupColors[2] = {
                IM_COL32( 80, 180, 255, 240),  // group 0: blue (alliance)
                IM_COL32(220,  50,  50, 240),  // group 1: red  (horde)
            };
            for (const auto& bp : bgPositions) {
                // Packet coords: wowX=canonical X (north), wowY=canonical Y (west)
                glm::vec3 bpRender = core::coords::canonicalToRender(glm::vec3(bp.wowX, bp.wowY, 0.0f));
                float sx = 0.0f, sy = 0.0f;
                if (!projectToMinimap(bpRender, sx, sy)) continue;

                ImU32 col = kBgGroupColors[bp.group & 1];

                // Draw a flag-like diamond icon
                const float r = 5.0f;
                ImVec2 top  (sx,       sy - r);
                ImVec2 right(sx + r,   sy    );
                ImVec2 bot  (sx,       sy + r);
                ImVec2 left (sx - r,   sy    );
                drawList->AddQuadFilled(top, right, bot, left, col);
                drawList->AddQuad(top, right, bot, left, IM_COL32(255, 255, 255, 180), 1.0f);

                float mdx = mouse.x - sx, mdy = mouse.y - sy;
                if (mdx * mdx + mdy * mdy < 64.0f) {
                    // Show entity name if available, otherwise guid
                    auto ent = gameHandler.getEntityManager().getEntity(bp.guid);
                    if (ent) {
                        std::string nm;
                        if (ent->getType() == game::ObjectType::PLAYER) {
                            auto pl = std::static_pointer_cast<game::Unit>(ent);
                            nm = pl ? pl->getName() : "";
                        }
                        if (!nm.empty())
                            ImGui::SetTooltip("Flag carrier: %s", nm.c_str());
                        else
                            ImGui::SetTooltip("Flag carrier");
                    } else {
                        ImGui::SetTooltip("Flag carrier");
                    }
                }
            }
        }
    }

    // Corpse direction indicator — shown when player is a ghost
    if (gameHandler.isPlayerGhost()) {
        float corpseCanX = 0.0f, corpseCanY = 0.0f;
        if (gameHandler.getCorpseCanonicalPos(corpseCanX, corpseCanY)) {
            glm::vec3 corpseRender = core::coords::canonicalToRender(glm::vec3(corpseCanX, corpseCanY, 0.0f));
            float csx = 0.0f, csy = 0.0f;
            bool onMap = projectToMinimap(corpseRender, csx, csy);

            if (onMap) {
                // Draw a small skull-like X marker at the corpse position
                const float r = 5.0f;
                drawList->AddCircleFilled(ImVec2(csx, csy), r + 1.0f, IM_COL32(0, 0, 0, 140), 12);
                drawList->AddCircle(ImVec2(csx, csy), r + 1.0f, IM_COL32(200, 200, 220, 220), 12, 1.5f);
                // Draw an X in the circle
                drawList->AddLine(ImVec2(csx - 3.0f, csy - 3.0f), ImVec2(csx + 3.0f, csy + 3.0f),
                                  IM_COL32(180, 180, 220, 255), 1.5f);
                drawList->AddLine(ImVec2(csx + 3.0f, csy - 3.0f), ImVec2(csx - 3.0f, csy + 3.0f),
                                  IM_COL32(180, 180, 220, 255), 1.5f);
                // Tooltip on hover
                ImVec2 mouse = ImGui::GetMousePos();
                float mdx = mouse.x - csx, mdy = mouse.y - csy;
                if (mdx * mdx + mdy * mdy < 64.0f) {
                    float dist = gameHandler.getCorpseDistance();
                    if (dist >= 0.0f)
                        ImGui::SetTooltip("Your corpse (%.0f yd)", dist);
                    else
                        ImGui::SetTooltip("Your corpse");
                }
            } else {
                // Corpse is outside minimap — draw an edge arrow pointing toward it
                float dx = corpseRender.x - playerRender.x;
                float dy = corpseRender.y - playerRender.y;
                // Rotate delta into minimap frame (same as projectToMinimap)
                float rx = -(dx * cosB + dy * sinB);
                float ry =   dx * sinB - dy * cosB;
                float len = std::sqrt(rx * rx + ry * ry);
                if (len > 0.001f) {
                    float nx = rx / len;
                    float ny = ry / len;
                    // Place arrow at the minimap edge
                    float edgeR = mapRadius - 7.0f;
                    float ax = centerX + nx * edgeR;
                    float ay = centerY + ny * edgeR;
                    // Arrow pointing outward (toward corpse)
                    float arrowLen = 6.0f;
                    float arrowW = 3.5f;
                    ImVec2 tip(ax + nx * arrowLen, ay + ny * arrowLen);
                    ImVec2 left(ax - ny * arrowW - nx * arrowLen * 0.4f,
                                ay + nx * arrowW - ny * arrowLen * 0.4f);
                    ImVec2 right(ax + ny * arrowW - nx * arrowLen * 0.4f,
                                 ay - nx * arrowW - ny * arrowLen * 0.4f);
                    drawList->AddTriangleFilled(tip, left, right, IM_COL32(180, 180, 240, 230));
                    drawList->AddTriangle(tip, left, right, IM_COL32(0, 0, 0, 180), 1.0f);
                    // Tooltip on hover
                    ImVec2 mouse = ImGui::GetMousePos();
                    float mdx = mouse.x - ax, mdy = mouse.y - ay;
                    if (mdx * mdx + mdy * mdy < 100.0f) {
                        float dist = gameHandler.getCorpseDistance();
                        if (dist >= 0.0f)
                            ImGui::SetTooltip("Your corpse (%.0f yd)", dist);
                        else
                            ImGui::SetTooltip("Your corpse");
                    }
                }
            }
        }
    }

    // Player position arrow at minimap center, pointing in camera facing direction.
    // On a rotating minimap the map already turns so forward = screen-up; on a fixed
    // minimap we rotate the arrow to match the player's compass heading.
    {
        // Compute screen-space facing direction for the arrow.
        // bearing = clockwise angle from screen-north (0 = facing north/up).
        float arrowAngle = 0.0f; // 0 = pointing up (north)
        if (!minimap->isRotateWithCamera()) {
            // Fixed minimap: arrow must show actual facing relative to north.
            glm::vec3 fwd = camera->getForward();
            // +render_y = north = screen-up, +render_x = west = screen-left.
            // bearing from north clockwise: atan2(-fwd.x_west, fwd.y_north)
            //   => sin=east component, cos=north component
            //   In render coords west=+x, east=-x, so sin(bearing)=east=-fwd.x
            arrowAngle = std::atan2(-fwd.x, fwd.y); // clockwise from north in screen space
        }
        // Screen direction the arrow tip points toward
        float nx =  std::sin(arrowAngle); // screen +X = east
        float ny = -std::cos(arrowAngle); // screen -Y = north

        // Draw a chevron-style arrow: tip, two base corners, and a notch at the back
        const float tipLen  = 8.0f;  // tip forward distance
        const float baseW   = 5.0f;  // half-width at base
        const float notchIn = 3.0f;  // how far back the center notch sits
        // Perpendicular direction (rotated 90°)
        float px =  ny; // perpendicular x
        float py = -nx; // perpendicular y

        ImVec2 tip  (centerX + nx * tipLen,  centerY + ny * tipLen);
        ImVec2 baseL(centerX - nx * baseW + px * baseW,  centerY - ny * baseW + py * baseW);
        ImVec2 baseR(centerX - nx * baseW - px * baseW,  centerY - ny * baseW - py * baseW);
        ImVec2 notch(centerX - nx * (baseW - notchIn),   centerY - ny * (baseW - notchIn));

        // Fill: bright white with slight gold tint, dark outline for readability
        drawList->AddTriangleFilled(tip, baseL, notch, IM_COL32(255, 248, 200, 245));
        drawList->AddTriangleFilled(tip, notch, baseR, IM_COL32(255, 248, 200, 245));
        drawList->AddTriangle(tip, baseL, notch, IM_COL32(60, 40, 0, 200), 1.2f);
        drawList->AddTriangle(tip, notch, baseR, IM_COL32(60, 40, 0, 200), 1.2f);
    }

    // Scroll wheel over minimap → zoom in/out
    {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            ImVec2 mouse = ImGui::GetMousePos();
            float mdx = mouse.x - centerX;
            float mdy = mouse.y - centerY;
            if (mdx * mdx + mdy * mdy <= mapRadius * mapRadius) {
                if (wheel > 0.0f)
                    minimap->zoomIn();
                else
                    minimap->zoomOut();
            }
        }
    }

    // Ctrl+click on minimap → send minimap ping to party
    if (ImGui::IsMouseClicked(0) && ImGui::GetIO().KeyCtrl) {
        ImVec2 mouse = ImGui::GetMousePos();
        float mdx = mouse.x - centerX;
        float mdy = mouse.y - centerY;
        float distSq = mdx * mdx + mdy * mdy;
        if (distSq <= mapRadius * mapRadius) {
            // Invert projectToMinimap: px=mdx, py=mdy → rx=px*viewRadius/mapRadius
            float rx = mdx * viewRadius / mapRadius;
            float ry = mdy * viewRadius / mapRadius;
            // rx/ry are in rotated frame; unrotate to get world dx/dy
            // rx = -(dx*cosB + dy*sinB), ry = dx*sinB - dy*cosB
            // Solving: dx = -(rx*cosB - ry*sinB), dy = -(rx*sinB + ry*cosB)
            float wdx = -(rx * cosB - ry * sinB);
            float wdy = -(rx * sinB + ry * cosB);
            // playerRender is in render coords; add delta to get render position then convert to canonical
            glm::vec3 clickRender = playerRender + glm::vec3(wdx, wdy, 0.0f);
            glm::vec3 clickCanon = core::coords::renderToCanonical(clickRender);
            gameHandler.sendMinimapPing(clickCanon.x, clickCanon.y);
        }
    }

    // Persistent coordinate display below the minimap
    {
        glm::vec3 playerCanon = core::coords::renderToCanonical(playerRender);
        char coordBuf[32];
        std::snprintf(coordBuf, sizeof(coordBuf), "%.1f, %.1f", playerCanon.x, playerCanon.y);

        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize();
        ImVec2 textSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, coordBuf);

        float tx = centerX - textSz.x * 0.5f;
        float ty = centerY + mapRadius + 3.0f;

        // Semi-transparent dark background pill
        float pad = 3.0f;
        drawList->AddRectFilled(
            ImVec2(tx - pad, ty - pad),
            ImVec2(tx + textSz.x + pad, ty + textSz.y + pad),
            IM_COL32(0, 0, 0, 140), 4.0f);
        // Coordinate text in warm yellow
        drawList->AddText(font, fontSize, ImVec2(tx, ty), IM_COL32(230, 220, 140, 255), coordBuf);
    }

    // Local time clock — displayed just below the coordinate label
    {
        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm tmLocal{};
#if defined(_WIN32)
        localtime_s(&tmLocal, &tt);
#else
        localtime_r(&tt, &tmLocal);
#endif
        char clockBuf[16];
        std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d",
                      tmLocal.tm_hour, tmLocal.tm_min);

        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize() * 0.9f;  // slightly smaller than coords
        ImVec2 clockSz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, clockBuf);

        float tx = centerX - clockSz.x * 0.5f;
        // Position below the coordinate line (+fontSize of coord + 2px gap)
        float coordLineH = ImGui::GetFontSize();
        float ty = centerY + mapRadius + 3.0f + coordLineH + 2.0f;

        float pad = 2.0f;
        drawList->AddRectFilled(
            ImVec2(tx - pad, ty - pad),
            ImVec2(tx + clockSz.x + pad, ty + clockSz.y + pad),
            IM_COL32(0, 0, 0, 120), 3.0f);
        drawList->AddText(font, fontSize, ImVec2(tx, ty), IM_COL32(200, 200, 220, 220), clockBuf);
    }

    // Zone name display — drawn inside the top edge of the minimap circle
    {
        auto* zmRenderer = renderer ? renderer->getZoneManager() : nullptr;
        uint32_t zoneId = gameHandler.getWorldStateZoneId();
        const game::ZoneInfo* zi = (zmRenderer && zoneId != 0) ? zmRenderer->getZoneInfo(zoneId) : nullptr;
        if (zi && !zi->name.empty()) {
            ImFont* font = ImGui::GetFont();
            float fontSize = ImGui::GetFontSize();
            ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, zi->name.c_str());
            float tx = centerX - ts.x * 0.5f;
            float ty = centerY - mapRadius + 4.0f;  // just inside top edge of the circle
            float pad = 2.0f;
            drawList->AddRectFilled(
                ImVec2(tx - pad, ty - pad),
                ImVec2(tx + ts.x + pad, ty + ts.y + pad),
                IM_COL32(0, 0, 0, 160), 2.0f);
            drawList->AddText(font, fontSize, ImVec2(tx + 1.0f, ty + 1.0f),
                              IM_COL32(0, 0, 0, 180), zi->name.c_str());
            drawList->AddText(font, fontSize, ImVec2(tx, ty),
                              IM_COL32(255, 230, 150, 220), zi->name.c_str());
        }
    }

    // Instance difficulty indicator — just below zone name, inside minimap top edge
    if (gameHandler.isInInstance()) {
        static constexpr const char* kDiffLabels[] = {"Normal", "Heroic", "25 Normal", "25 Heroic"};
        uint32_t diff = gameHandler.getInstanceDifficulty();
        const char* label = (diff < 4) ? kDiffLabels[diff] : "Unknown";

        ImFont* font = ImGui::GetFont();
        float fontSize = ImGui::GetFontSize() * 0.85f;
        ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label);
        float tx = centerX - ts.x * 0.5f;
        // Position below zone name: top edge + zone font size + small gap
        float ty = centerY - mapRadius + 4.0f + ImGui::GetFontSize() + 2.0f;
        float pad = 2.0f;

        // Color-code: heroic=orange, normal=light gray
        ImU32 bgCol = gameHandler.isInstanceHeroic() ? IM_COL32(120, 60, 0, 180) : IM_COL32(0, 0, 0, 160);
        ImU32 textCol = gameHandler.isInstanceHeroic() ? IM_COL32(255, 180, 50, 255) : IM_COL32(200, 200, 200, 220);

        drawList->AddRectFilled(
            ImVec2(tx - pad, ty - pad),
            ImVec2(tx + ts.x + pad, ty + ts.y + pad),
            bgCol, 2.0f);
        drawList->AddText(font, fontSize, ImVec2(tx, ty), textCol, label);
    }

    // Hover tooltip and right-click context menu
    {
        ImVec2 mouse = ImGui::GetMousePos();
        float mdx = mouse.x - centerX;
        float mdy = mouse.y - centerY;
        bool overMinimap = (mdx * mdx + mdy * mdy <= mapRadius * mapRadius);

        if (overMinimap) {
            ImGui::BeginTooltip();
            // Compute the world coordinate under the mouse cursor
            // Inverse of projectToMinimap: pixel offset → world offset in render space → canonical
            float rxW = mdx / mapRadius * viewRadius;
            float ryW = mdy / mapRadius * viewRadius;
            // Un-rotate: [dx, dy] = R^-1 * [rxW, ryW]
            //  where R applied: rx = -(dx*cosB + dy*sinB), ry = dx*sinB - dy*cosB
            float hoverDx = -cosB * rxW + sinB * ryW;
            float hoverDy = -sinB * rxW - cosB * ryW;
            glm::vec3 hoverRender(playerRender.x + hoverDx, playerRender.y + hoverDy, playerRender.z);
            glm::vec3 hoverCanon = core::coords::renderToCanonical(hoverRender);
            ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.5f, 1.0f), "%.1f, %.1f", hoverCanon.x, hoverCanon.y);
            ImGui::TextColored(colors::kMediumGray, "Ctrl+click to ping");
            ImGui::EndTooltip();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##minimapContextMenu");
            }
        }

        if (ImGui::BeginPopup("##minimapContextMenu")) {
            ImGui::TextColored(ui::colors::kTooltipGold, "Minimap");
            ImGui::Separator();

            // Zoom controls
            if (ImGui::MenuItem("Zoom In")) {
                minimap->zoomIn();
            }
            if (ImGui::MenuItem("Zoom Out")) {
                minimap->zoomOut();
            }

            ImGui::Separator();

            // Toggle options with checkmarks
            bool rotWithCam = minimap->isRotateWithCamera();
            if (ImGui::MenuItem("Rotate with Camera", nullptr, rotWithCam)) {
                minimap->setRotateWithCamera(!rotWithCam);
            }

            bool squareShape = minimap->isSquareShape();
            if (ImGui::MenuItem("Square Shape", nullptr, squareShape)) {
                minimap->setSquareShape(!squareShape);
            }

            bool npcDots = settingsPanel_.minimapNpcDots_;
            if (ImGui::MenuItem("Show NPC Dots", nullptr, npcDots)) {
                settingsPanel_.minimapNpcDots_ = !settingsPanel_.minimapNpcDots_;
            }

            ImGui::EndPopup();
        }
    }

    auto applyMuteState = [&]() {
        auto* ac = services_.audioCoordinator;
        float masterScale = settingsPanel_.soundMuted_ ? 0.0f : static_cast<float>(settingsPanel_.pendingMasterVolume) / 100.0f;
        audio::AudioEngine::instance().setMasterVolume(masterScale);
        if (!ac) return;
        if (auto* music = ac->getMusicManager()) {
            music->setVolume(settingsPanel_.pendingMusicVolume);
        }
        if (auto* ambient = ac->getAmbientSoundManager()) {
            ambient->setVolumeScale(settingsPanel_.pendingAmbientVolume / 100.0f);
        }
        if (auto* ui = ac->getUiSoundManager()) {
            ui->setVolumeScale(settingsPanel_.pendingUiVolume / 100.0f);
        }
        if (auto* combat = ac->getCombatSoundManager()) {
            combat->setVolumeScale(settingsPanel_.pendingCombatVolume / 100.0f);
        }
        if (auto* spell = ac->getSpellSoundManager()) {
            spell->setVolumeScale(settingsPanel_.pendingSpellVolume / 100.0f);
        }
        if (auto* movement = ac->getMovementSoundManager()) {
            movement->setVolumeScale(settingsPanel_.pendingMovementVolume / 100.0f);
        }
        if (auto* footstep = ac->getFootstepManager()) {
            footstep->setVolumeScale(settingsPanel_.pendingFootstepVolume / 100.0f);
        }
        if (auto* npcVoice = ac->getNpcVoiceManager()) {
            npcVoice->setVolumeScale(settingsPanel_.pendingNpcVoiceVolume / 100.0f);
        }
        if (auto* mount = ac->getMountSoundManager()) {
            mount->setVolumeScale(settingsPanel_.pendingMountVolume / 100.0f);
        }
        if (auto* activity = ac->getActivitySoundManager()) {
            activity->setVolumeScale(settingsPanel_.pendingActivityVolume / 100.0f);
        }
    };

    // Zone name label above the minimap (centered, WoW-style)
    // Prefer the server-reported zone/area name (from SMSG_INIT_WORLD_STATES) so sub-zones
    // like Ironforge or Wailing Caverns display correctly; fall back to renderer zone name.
    {
        std::string wsZoneName;
        uint32_t wsZoneId = gameHandler.getWorldStateZoneId();
        if (wsZoneId != 0)
            wsZoneName = gameHandler.getWhoAreaName(wsZoneId);
        const std::string& rendererZoneName = renderer ? renderer->getCurrentZoneName() : std::string{};
        const std::string& zoneName = !wsZoneName.empty() ? wsZoneName : rendererZoneName;
        if (!zoneName.empty()) {
            auto* fgDl = ImGui::GetForegroundDrawList();
            float zoneTextY = centerY - mapRadius - 16.0f;
            ImFont* font = ImGui::GetFont();

            // Weather icon appended to zone name when active
            uint32_t wType = gameHandler.getWeatherType();
            float wIntensity = gameHandler.getWeatherIntensity();
            const char* weatherIcon = nullptr;
            ImU32 weatherColor = IM_COL32(255, 255, 255, 200);
            if (wType == 1 && wIntensity > 0.05f) {           // Rain
                weatherIcon = " \xe2\x9b\x86";               // U+26C6 ⛆
                weatherColor = IM_COL32(140, 180, 240, 220);
            } else if (wType == 2 && wIntensity > 0.05f) {    // Snow
                weatherIcon = " \xe2\x9d\x84";               // U+2744 ❄
                weatherColor = IM_COL32(210, 230, 255, 220);
            } else if (wType == 3 && wIntensity > 0.05f) {    // Storm/Fog
                weatherIcon = " \xe2\x98\x81";               // U+2601 ☁
                weatherColor = IM_COL32(160, 160, 190, 220);
            }

            std::string displayName = zoneName;
            // Build combined string if weather active
            std::string fullLabel = weatherIcon ? (zoneName + weatherIcon) : zoneName;
            ImVec2 tsz = font->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, fullLabel.c_str());
            float tzx = centerX - tsz.x * 0.5f;

            // Shadow pass
            fgDl->AddText(font, 12.0f, ImVec2(tzx + 1.0f, zoneTextY + 1.0f),
                IM_COL32(0, 0, 0, 180), zoneName.c_str());
            // Zone name in gold
            fgDl->AddText(font, 12.0f, ImVec2(tzx, zoneTextY),
                IM_COL32(255, 220, 120, 230), zoneName.c_str());
            // Weather symbol in its own color appended after
            if (weatherIcon) {
                ImVec2 nameSz = font->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, zoneName.c_str());
                fgDl->AddText(font, 12.0f, ImVec2(tzx + nameSz.x, zoneTextY), weatherColor, weatherIcon);
            }
        }
    }

    // Speaker mute button at the minimap top-right corner
    ImGui::SetNextWindowPos(ImVec2(centerX + mapRadius - 26.0f, centerY - mapRadius + 4.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(22.0f, 22.0f), ImGuiCond_Always);
    ImGuiWindowFlags muteFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##MinimapMute", nullptr, muteFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 size(20.0f, 20.0f);
        if (ImGui::InvisibleButton("##MinimapMuteButton", size)) {
            settingsPanel_.soundMuted_ = !settingsPanel_.soundMuted_;
            if (settingsPanel_.soundMuted_) {
                settingsPanel_.preMuteVolume_ = audio::AudioEngine::instance().getMasterVolume();
            }
            applyMuteState();
            saveSettings();
        }
        bool hovered = ImGui::IsItemHovered();
        ImU32 bg = settingsPanel_.soundMuted_ ? IM_COL32(135, 42, 42, 230) : IM_COL32(38, 38, 38, 210);
        if (hovered) bg = settingsPanel_.soundMuted_ ? IM_COL32(160, 58, 58, 230) : IM_COL32(65, 65, 65, 220);
        ImU32 fg = IM_COL32(255, 255, 255, 245);
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 4.0f);
        draw->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(p.x + size.x - 0.5f, p.y + size.y - 0.5f),
                      IM_COL32(255, 255, 255, 42), 4.0f);
        draw->AddRectFilled(ImVec2(p.x + 4.0f, p.y + 8.0f), ImVec2(p.x + 7.0f, p.y + 12.0f), fg, 1.0f);
        draw->AddTriangleFilled(ImVec2(p.x + 7.0f, p.y + 7.0f),
                                ImVec2(p.x + 7.0f, p.y + 13.0f),
                                ImVec2(p.x + 11.8f, p.y + 10.0f), fg);
        if (settingsPanel_.soundMuted_) {
            draw->AddLine(ImVec2(p.x + 13.5f, p.y + 6.2f), ImVec2(p.x + 17.2f, p.y + 13.8f), fg, 1.8f);
            draw->AddLine(ImVec2(p.x + 17.2f, p.y + 6.2f), ImVec2(p.x + 13.5f, p.y + 13.8f), fg, 1.8f);
        } else {
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 3.6f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.4f);
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 5.5f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.2f);
        }
        if (hovered) ImGui::SetTooltip(settingsPanel_.soundMuted_ ? "Unmute" : "Mute");
    }
    ImGui::End();

    // Friends button at top-left of minimap
    {
        const auto& contacts = gameHandler.getContacts();
        int onlineCount = 0;
        for (const auto& c : contacts)
            if (c.isFriend() && c.isOnline()) ++onlineCount;

        ImGui::SetNextWindowPos(ImVec2(centerX - mapRadius + 4.0f, centerY - mapRadius + 4.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(22.0f, 22.0f), ImGuiCond_Always);
        ImGuiWindowFlags friendsBtnFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##MinimapFriendsBtn", nullptr, friendsBtnFlags)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 sz(20.0f, 20.0f);
            if (ImGui::InvisibleButton("##FriendsBtnInv", sz)) {
                socialPanel_.showSocialFrame_ = !socialPanel_.showSocialFrame_;
            }
            bool hovered = ImGui::IsItemHovered();
            ImU32 bg = socialPanel_.showSocialFrame_
                ? IM_COL32(42, 100, 42, 230)
                : IM_COL32(38, 38, 38, 210);
            if (hovered) bg = socialPanel_.showSocialFrame_ ? IM_COL32(58, 130, 58, 230) : IM_COL32(65, 65, 65, 220);
            draw->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 4.0f);
            draw->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f),
                          ImVec2(p.x + sz.x - 0.5f, p.y + sz.y - 0.5f),
                          IM_COL32(255, 255, 255, 42), 4.0f);
            // Simple smiley-face dots as "social" icon
            ImU32 fg = IM_COL32(255, 255, 255, 245);
            draw->AddCircle(ImVec2(p.x + 10.0f, p.y + 10.0f), 6.5f, fg, 16, 1.2f);
            draw->AddCircleFilled(ImVec2(p.x + 7.5f, p.y + 8.0f), 1.2f, fg);
            draw->AddCircleFilled(ImVec2(p.x + 12.5f, p.y + 8.0f), 1.2f, fg);
            draw->PathArcTo(ImVec2(p.x + 10.0f, p.y + 11.5f), 3.0f, 0.2f, 2.9f, 8);
            draw->PathStroke(fg, 0, 1.2f);
            // Small green dot if friends online
            if (onlineCount > 0) {
                draw->AddCircleFilled(ImVec2(p.x + sz.x - 3.5f, p.y + 3.5f),
                                      3.5f, IM_COL32(50, 220, 50, 255));
            }
            if (hovered) {
                if (onlineCount > 0)
                    ImGui::SetTooltip("Friends (%d online)", onlineCount);
                else
                    ImGui::SetTooltip("Friends");
            }
        }
        ImGui::End();
    }

    // Zoom buttons at the bottom edge of the minimap
    ImGui::SetNextWindowPos(ImVec2(centerX - 22, centerY + mapRadius - 30), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(44, 24), ImGuiCond_Always);
    ImGuiWindowFlags zoomFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##MinimapZoom", nullptr, zoomFlags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
        if (ImGui::SmallButton("-")) {
            if (minimap) minimap->zoomOut();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) {
            if (minimap) minimap->zoomIn();
        }
        ImGui::PopStyleVar(2);
    }
    ImGui::End();

    // Clock display at bottom-right of minimap (local time)
    {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &tt);
#else
        localtime_r(&tt, &tmBuf);
#endif
        char clockText[16];
        std::snprintf(clockText, sizeof(clockText), "%d:%02d %s",
                      (tmBuf.tm_hour % 12 == 0) ? 12 : tmBuf.tm_hour % 12,
                      tmBuf.tm_min,
                      tmBuf.tm_hour >= 12 ? "PM" : "AM");
        ImVec2 clockSz = ImGui::CalcTextSize(clockText);
        float clockW = clockSz.x + 10.0f;
        float clockH = clockSz.y + 6.0f;
        ImGui::SetNextWindowPos(ImVec2(centerX + mapRadius - clockW - 2.0f,
                                       centerY + mapRadius - clockH - 2.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(clockW, clockH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGuiWindowFlags clockFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##MinimapClock", nullptr, clockFlags)) {
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.8f, 0.85f), "%s", clockText);
        }
        ImGui::End();
    }

    // Indicators below the minimap (stacked: new mail, then BG queue, then latency)
    float indicatorX = centerX - mapRadius;
    float nextIndicatorY = centerY + mapRadius + 4.0f;
    const float indicatorW = mapRadius * 2.0f;
    constexpr float kIndicatorH = 22.0f;
    ImGuiWindowFlags indicatorFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;

    // "New Mail" indicator
    if (gameHandler.hasNewMail()) {
        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##NewMailIndicator", nullptr, indicatorFlags)) {
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, pulse), "New Mail!");
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
    }

    // Unspent talent points indicator
    {
        uint8_t unspent = gameHandler.getUnspentTalentPoints();
        if (unspent > 0) {
            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##TalentIndicator", nullptr, indicatorFlags)) {
                float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.5f);
                char talentBuf[40];
                snprintf(talentBuf, sizeof(talentBuf), "! %u Talent Point%s Available",
                         static_cast<unsigned>(unspent), unspent == 1 ? "" : "s");
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f * pulse, pulse), "%s", talentBuf);
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

    // BG queue status indicator (when in queue but not yet invited)
    for (const auto& slot : gameHandler.getBgQueues()) {
        if (slot.statusId != 1) continue;  // STATUS_WAIT_QUEUE only

        std::string bgName;
        if (slot.arenaType > 0) {
            bgName = std::to_string(slot.arenaType) + "v" + std::to_string(slot.arenaType) + " Arena";
        } else {
            switch (slot.bgTypeId) {
                case 1: bgName = "AV"; break;
                case 2: bgName = "WSG"; break;
                case 3: bgName = "AB"; break;
                case 7: bgName = "EotS"; break;
                case 9: bgName = "SotA"; break;
                case 11: bgName = "IoC"; break;
                default: bgName = "BG"; break;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##BgQueueIndicator", nullptr, indicatorFlags)) {
            float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.5f);
            if (slot.avgWaitTimeSec > 0) {
                int avgMin = static_cast<int>(slot.avgWaitTimeSec) / 60;
                int avgSec = static_cast<int>(slot.avgWaitTimeSec) % 60;
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, pulse),
                    "Queue: %s (~%d:%02d)", bgName.c_str(), avgMin, avgSec);
            } else {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, pulse),
                    "In Queue: %s", bgName.c_str());
            }
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
        break;  // Show at most one queue slot indicator
    }

    // LFG queue indicator — shown when Dungeon Finder queue is active (Queued or RoleCheck)
    {
        using LfgState = game::GameHandler::LfgState;
        LfgState lfgSt = gameHandler.getLfgState();
        if (lfgSt == LfgState::Queued || lfgSt == LfgState::RoleCheck) {
            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##LfgQueueIndicator", nullptr, indicatorFlags)) {
                if (lfgSt == LfgState::RoleCheck) {
                    float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, pulse), "LFG: Role Check...");
                } else {
                    uint32_t qMs  = gameHandler.getLfgTimeInQueueMs();
                    int      qMin = static_cast<int>(qMs / 60000);
                    int      qSec = static_cast<int>((qMs % 60000) / 1000);
                    float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.2f);
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, pulse),
                        "LFG: %d:%02d", qMin, qSec);
                }
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

    // Calendar pending invites indicator (WotLK only)
    {
        auto* expReg = services_.expansionRegistry;
        bool isWotLK = expReg && expReg->getActive() && expReg->getActive()->id == "wotlk";
        if (isWotLK) {
            uint32_t calPending = gameHandler.getCalendarPendingInvites();
            if (calPending > 0) {
                ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
                if (ImGui::Begin("##CalendarIndicator", nullptr, indicatorFlags)) {
                    float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f);
                    char calBuf[48];
                    snprintf(calBuf, sizeof(calBuf), "Calendar: %u Invite%s",
                             calPending, calPending == 1 ? "" : "s");
                    ImGui::TextColored(ImVec4(0.6f, 0.5f, 1.0f, pulse), "%s", calBuf);
                }
                ImGui::End();
                nextIndicatorY += kIndicatorH;
            }
        }
    }

    // Taxi flight indicator — shown while on a flight path
    if (gameHandler.isOnTaxiFlight()) {
        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        if (ImGui::Begin("##TaxiIndicator", nullptr, indicatorFlags)) {
            const std::string& dest = gameHandler.getTaxiDestName();
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 1.0f);
            if (dest.empty()) {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, pulse), "\xe2\x9c\x88 In Flight");
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "\xe2\x9c\x88 \xe2\x86\x92 %s", dest.c_str());
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, pulse), "%s", buf);
            }
        }
        ImGui::End();
        nextIndicatorY += kIndicatorH;
    }

    // Latency + FPS indicator — centered at top of screen
    uint32_t latMs = gameHandler.getLatencyMs();
    if (settingsPanel_.showLatencyMeter_ && gameHandler.getState() == game::WorldState::IN_WORLD) {
        float currentFps = ImGui::GetIO().Framerate;
        ImVec4 latColor;
        if      (latMs < 100) latColor = ImVec4(0.3f, 1.0f, 0.3f, 0.9f);
        else if (latMs < 250) latColor = ImVec4(1.0f, 1.0f, 0.3f, 0.9f);
        else if (latMs < 500) latColor = ImVec4(1.0f, 0.6f, 0.1f, 0.9f);
        else                  latColor = ImVec4(1.0f, 0.2f, 0.2f, 0.9f);

        ImVec4 fpsColor;
        if      (currentFps >= 60.0f) fpsColor = ImVec4(0.3f, 1.0f, 0.3f, 0.9f);
        else if (currentFps >= 30.0f) fpsColor = ImVec4(1.0f, 1.0f, 0.3f, 0.9f);
        else                          fpsColor = ImVec4(1.0f, 0.3f, 0.3f, 0.9f);

        char infoText[64];
        if (latMs > 0)
            snprintf(infoText, sizeof(infoText), "%.0f fps  |  %u ms", currentFps, latMs);
        else
            snprintf(infoText, sizeof(infoText), "%.0f fps", currentFps);

        ImVec2 textSize = ImGui::CalcTextSize(infoText);
        float latW = textSize.x + 16.0f;
        float latH = textSize.y + 8.0f;
        ImGuiIO& lio = ImGui::GetIO();
        float latX = (lio.DisplaySize.x - latW) * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(latX, 4.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(latW, latH), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##LatencyIndicator", nullptr, indicatorFlags)) {
            // Color the FPS and latency portions differently
            ImGui::TextColored(fpsColor, "%.0f fps", currentFps);
            if (latMs > 0) {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 0.7f), "|");
                ImGui::SameLine(0, 4);
                ImGui::TextColored(latColor, "%u ms", latMs);
            }
        }
        ImGui::End();
    }

    // Low durability warning — shown when any equipped item has < 20% durability
    if (gameHandler.getState() == game::WorldState::IN_WORLD) {
        const auto& inv = gameHandler.getInventory();
        float lowestDurPct = 1.0f;
        for (int i = 0; i < game::Inventory::NUM_EQUIP_SLOTS; ++i) {
            const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(i));
            if (slot.empty()) continue;
            const auto& it = slot.item;
            if (it.maxDurability > 0) {
                float pct = static_cast<float>(it.curDurability) / static_cast<float>(it.maxDurability);
                if (pct < lowestDurPct) lowestDurPct = pct;
            }
        }
        if (lowestDurPct < 0.20f) {
            bool critical = (lowestDurPct < 0.05f);
            float pulse = critical
                ? (0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f))
                : 1.0f;
            ImVec4 durWarnColor = critical
                ? ImVec4(1.0f, 0.2f, 0.2f, pulse)
                : ImVec4(1.0f, 0.65f, 0.1f, 0.9f);
            const char* durWarnText = critical ? "Item breaking!" : "Low durability";

            ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
            if (ImGui::Begin("##DurabilityIndicator", nullptr, indicatorFlags)) {
                ImGui::TextColored(durWarnColor, "%s", durWarnText);
            }
            ImGui::End();
            nextIndicatorY += kIndicatorH;
        }
    }

    // Local time clock — always visible below minimap indicators
    {
        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        struct tm tmBuf;
#ifdef _WIN32
        localtime_s(&tmBuf, &tt);
#else
        localtime_r(&tt, &tmBuf);
#endif
        char clockStr[16];
        snprintf(clockStr, sizeof(clockStr), "%02d:%02d", tmBuf.tm_hour, tmBuf.tm_min);

        ImGui::SetNextWindowPos(ImVec2(indicatorX, nextIndicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(indicatorW, kIndicatorH), ImGuiCond_Always);
        ImGuiWindowFlags clockFlags = indicatorFlags & ~ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##ClockIndicator", nullptr, clockFlags)) {
            ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 0.75f), "%s", clockStr);
            if (ImGui::IsItemHovered()) {
                char fullTime[32];
                snprintf(fullTime, sizeof(fullTime), "%02d:%02d:%02d (local)",
                         tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
                ImGui::SetTooltip("%s", fullTime);
            }
        }
        ImGui::End();
    }
}

void GameScreen::saveSettings() {
    std::string path = SettingsPanel::getSettingsPath();
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save settings to ", path);
        return;
    }

    // Interface
    out << "ui_opacity=" << settingsPanel_.pendingUiOpacity << "\n";
    out << "minimap_rotate=" << (settingsPanel_.pendingMinimapRotate ? 1 : 0) << "\n";
    out << "minimap_square=" << (settingsPanel_.pendingMinimapSquare ? 1 : 0) << "\n";
    out << "minimap_npc_dots=" << (settingsPanel_.pendingMinimapNpcDots ? 1 : 0) << "\n";
    out << "show_latency_meter=" << (settingsPanel_.pendingShowLatencyMeter ? 1 : 0) << "\n";
    out << "show_dps_meter=" << (settingsPanel_.showDPSMeter_ ? 1 : 0) << "\n";
    out << "show_cooldown_tracker=" << (settingsPanel_.showCooldownTracker_ ? 1 : 0) << "\n";
    out << "separate_bags=" << (settingsPanel_.pendingSeparateBags ? 1 : 0) << "\n";
    out << "show_keyring=" << (settingsPanel_.pendingShowKeyring ? 1 : 0) << "\n";
    out << "action_bar_scale=" << settingsPanel_.pendingActionBarScale << "\n";
    out << "nameplate_scale=" << settingsPanel_.nameplateScale_ << "\n";
    out << "show_friendly_nameplates=" << (settingsPanel_.showFriendlyNameplates_ ? 1 : 0) << "\n";
    out << "show_action_bar2=" << (settingsPanel_.pendingShowActionBar2 ? 1 : 0) << "\n";
    out << "action_bar2_offset_x=" << settingsPanel_.pendingActionBar2OffsetX << "\n";
    out << "action_bar2_offset_y=" << settingsPanel_.pendingActionBar2OffsetY << "\n";
    out << "show_right_bar=" << (settingsPanel_.pendingShowRightBar ? 1 : 0) << "\n";
    out << "show_left_bar=" << (settingsPanel_.pendingShowLeftBar ? 1 : 0) << "\n";
    out << "right_bar_offset_y=" << settingsPanel_.pendingRightBarOffsetY << "\n";
    out << "left_bar_offset_y=" << settingsPanel_.pendingLeftBarOffsetY << "\n";
    out << "damage_flash=" << (settingsPanel_.damageFlashEnabled_ ? 1 : 0) << "\n";
    out << "low_health_vignette=" << (settingsPanel_.lowHealthVignetteEnabled_ ? 1 : 0) << "\n";

    // Audio
    out << "sound_muted=" << (settingsPanel_.soundMuted_ ? 1 : 0) << "\n";
    out << "use_original_soundtrack=" << (settingsPanel_.pendingUseOriginalSoundtrack ? 1 : 0) << "\n";
    out << "master_volume=" << settingsPanel_.pendingMasterVolume << "\n";
    out << "music_volume=" << settingsPanel_.pendingMusicVolume << "\n";
    out << "ambient_volume=" << settingsPanel_.pendingAmbientVolume << "\n";
    out << "ui_volume=" << settingsPanel_.pendingUiVolume << "\n";
    out << "combat_volume=" << settingsPanel_.pendingCombatVolume << "\n";
    out << "spell_volume=" << settingsPanel_.pendingSpellVolume << "\n";
    out << "movement_volume=" << settingsPanel_.pendingMovementVolume << "\n";
    out << "footstep_volume=" << settingsPanel_.pendingFootstepVolume << "\n";
    out << "npc_voice_volume=" << settingsPanel_.pendingNpcVoiceVolume << "\n";
    out << "mount_volume=" << settingsPanel_.pendingMountVolume << "\n";
    out << "activity_volume=" << settingsPanel_.pendingActivityVolume << "\n";

    // Gameplay
    out << "auto_loot=" << (settingsPanel_.pendingAutoLoot ? 1 : 0) << "\n";
    out << "auto_sell_grey=" << (settingsPanel_.pendingAutoSellGrey ? 1 : 0) << "\n";
    out << "auto_repair=" << (settingsPanel_.pendingAutoRepair ? 1 : 0) << "\n";
    out << "graphics_preset=" << static_cast<int>(settingsPanel_.currentGraphicsPreset) << "\n";
    out << "ground_clutter_density=" << settingsPanel_.pendingGroundClutterDensity << "\n";
    out << "shadows=" << (settingsPanel_.pendingShadows ? 1 : 0) << "\n";
    out << "shadow_distance=" << settingsPanel_.pendingShadowDistance << "\n";
    out << "brightness=" << settingsPanel_.pendingBrightness << "\n";
    out << "water_refraction=" << (settingsPanel_.pendingWaterRefraction ? 1 : 0) << "\n";
    out << "antialiasing=" << settingsPanel_.pendingAntiAliasing << "\n";
    out << "fxaa=" << (settingsPanel_.pendingFXAA ? 1 : 0) << "\n";
    out << "normal_mapping=" << (settingsPanel_.pendingNormalMapping ? 1 : 0) << "\n";
    out << "normal_map_strength=" << settingsPanel_.pendingNormalMapStrength << "\n";
    out << "pom=" << (settingsPanel_.pendingPOM ? 1 : 0) << "\n";
    out << "pom_quality=" << settingsPanel_.pendingPOMQuality << "\n";
    out << "upscaling_mode=" << settingsPanel_.pendingUpscalingMode << "\n";
    out << "fsr=" << (settingsPanel_.pendingFSR ? 1 : 0) << "\n";
    out << "fsr_quality=" << settingsPanel_.pendingFSRQuality << "\n";
    out << "fsr_sharpness=" << settingsPanel_.pendingFSRSharpness << "\n";
    out << "fsr2_jitter_sign=" << settingsPanel_.pendingFSR2JitterSign << "\n";
    out << "fsr2_mv_scale_x=" << settingsPanel_.pendingFSR2MotionVecScaleX << "\n";
    out << "fsr2_mv_scale_y=" << settingsPanel_.pendingFSR2MotionVecScaleY << "\n";
    out << "amd_fsr3_framegen=" << (settingsPanel_.pendingAMDFramegen ? 1 : 0) << "\n";

    // Controls
    out << "mouse_sensitivity=" << settingsPanel_.pendingMouseSensitivity << "\n";
    out << "invert_mouse=" << (settingsPanel_.pendingInvertMouse ? 1 : 0) << "\n";
    out << "extended_zoom=" << (settingsPanel_.pendingExtendedZoom ? 1 : 0) << "\n";
    out << "camera_stiffness=" << settingsPanel_.pendingCameraStiffness << "\n";
    out << "camera_pivot_height=" << settingsPanel_.pendingPivotHeight << "\n";
    out << "fov=" << settingsPanel_.pendingFov << "\n";

    // Quest tracker position/size
    out << "quest_tracker_right_offset=" << questTrackerRightOffset_ << "\n";
    out << "quest_tracker_y=" << questTrackerPos_.y << "\n";
    out << "quest_tracker_w=" << questTrackerSize_.x << "\n";
    out << "quest_tracker_h=" << questTrackerSize_.y << "\n";

    // Chat
    out << "chat_active_tab=" << chatPanel_.activeChatTab << "\n";
    out << "chat_timestamps=" << (chatPanel_.chatShowTimestamps ? 1 : 0) << "\n";
    out << "chat_font_size=" << chatPanel_.chatFontSize << "\n";
    out << "chat_autojoin_general=" << (chatPanel_.chatAutoJoinGeneral ? 1 : 0) << "\n";
    out << "chat_autojoin_trade=" << (chatPanel_.chatAutoJoinTrade ? 1 : 0) << "\n";
    out << "chat_autojoin_localdefense=" << (chatPanel_.chatAutoJoinLocalDefense ? 1 : 0) << "\n";
    out << "chat_autojoin_lfg=" << (chatPanel_.chatAutoJoinLFG ? 1 : 0) << "\n";
    out << "chat_autojoin_local=" << (chatPanel_.chatAutoJoinLocal ? 1 : 0) << "\n";

    out.close();

    // Save keybindings to the same config file (appends [Keybindings] section)
    KeybindingManager::getInstance().saveToConfigFile(path);

    LOG_INFO("Settings saved to ", path);
}

void GameScreen::loadSettings() {
    std::string path = SettingsPanel::getSettingsPath();
    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        try {
            // Interface
            if (key == "ui_opacity") {
                int v = std::stoi(val);
                if (v >= 20 && v <= 100) {
                    settingsPanel_.pendingUiOpacity = v;
                    settingsPanel_.uiOpacity_ = static_cast<float>(v) / 100.0f;
                }
            } else if (key == "minimap_rotate") {
                // Ignore persisted rotate state; keep north-up.
                settingsPanel_.minimapRotate_ = false;
                settingsPanel_.pendingMinimapRotate = false;
            } else if (key == "minimap_square") {
                int v = std::stoi(val);
                settingsPanel_.minimapSquare_ = (v != 0);
                settingsPanel_.pendingMinimapSquare = settingsPanel_.minimapSquare_;
            } else if (key == "minimap_npc_dots") {
                int v = std::stoi(val);
                settingsPanel_.minimapNpcDots_ = (v != 0);
                settingsPanel_.pendingMinimapNpcDots = settingsPanel_.minimapNpcDots_;
            } else if (key == "show_latency_meter") {
                settingsPanel_.showLatencyMeter_ = (std::stoi(val) != 0);
                settingsPanel_.pendingShowLatencyMeter = settingsPanel_.showLatencyMeter_;
            } else if (key == "show_dps_meter") {
                settingsPanel_.showDPSMeter_ = (std::stoi(val) != 0);
            } else if (key == "show_cooldown_tracker") {
                settingsPanel_.showCooldownTracker_ = (std::stoi(val) != 0);
            } else if (key == "separate_bags") {
                settingsPanel_.pendingSeparateBags = (std::stoi(val) != 0);
                inventoryScreen.setSeparateBags(settingsPanel_.pendingSeparateBags);
            } else if (key == "show_keyring") {
                settingsPanel_.pendingShowKeyring = (std::stoi(val) != 0);
                inventoryScreen.setShowKeyring(settingsPanel_.pendingShowKeyring);
            } else if (key == "action_bar_scale") {
                settingsPanel_.pendingActionBarScale = std::clamp(std::stof(val), 0.5f, 1.5f);
            } else if (key == "nameplate_scale") {
                settingsPanel_.nameplateScale_ = std::clamp(std::stof(val), 0.5f, 2.0f);
            } else if (key == "show_friendly_nameplates") {
                settingsPanel_.showFriendlyNameplates_ = (std::stoi(val) != 0);
            } else if (key == "show_action_bar2") {
                settingsPanel_.pendingShowActionBar2 = (std::stoi(val) != 0);
            } else if (key == "action_bar2_offset_x") {
                settingsPanel_.pendingActionBar2OffsetX = std::clamp(std::stof(val), -600.0f, 600.0f);
            } else if (key == "action_bar2_offset_y") {
                settingsPanel_.pendingActionBar2OffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "show_right_bar") {
                settingsPanel_.pendingShowRightBar = (std::stoi(val) != 0);
            } else if (key == "show_left_bar") {
                settingsPanel_.pendingShowLeftBar = (std::stoi(val) != 0);
            } else if (key == "right_bar_offset_y") {
                settingsPanel_.pendingRightBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "left_bar_offset_y") {
                settingsPanel_.pendingLeftBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "damage_flash") {
                settingsPanel_.damageFlashEnabled_ = (std::stoi(val) != 0);
            } else if (key == "low_health_vignette") {
                settingsPanel_.lowHealthVignetteEnabled_ = (std::stoi(val) != 0);
            }
            // Audio
            else if (key == "sound_muted") {
                settingsPanel_.soundMuted_ = (std::stoi(val) != 0);
                if (settingsPanel_.soundMuted_) {
                    // Apply mute on load; settingsPanel_.preMuteVolume_ will be set when AudioEngine is available
                    audio::AudioEngine::instance().setMasterVolume(0.0f);
                }
            }
            else if (key == "use_original_soundtrack") settingsPanel_.pendingUseOriginalSoundtrack = (std::stoi(val) != 0);
            else if (key == "master_volume") settingsPanel_.pendingMasterVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "music_volume") settingsPanel_.pendingMusicVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ambient_volume") settingsPanel_.pendingAmbientVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ui_volume") settingsPanel_.pendingUiVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "combat_volume") settingsPanel_.pendingCombatVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "spell_volume") settingsPanel_.pendingSpellVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "movement_volume") settingsPanel_.pendingMovementVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "footstep_volume") settingsPanel_.pendingFootstepVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "npc_voice_volume") settingsPanel_.pendingNpcVoiceVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "mount_volume") settingsPanel_.pendingMountVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "activity_volume") settingsPanel_.pendingActivityVolume = std::clamp(std::stoi(val), 0, 100);
            // Gameplay
            else if (key == "auto_loot") settingsPanel_.pendingAutoLoot = (std::stoi(val) != 0);
            else if (key == "auto_sell_grey") settingsPanel_.pendingAutoSellGrey = (std::stoi(val) != 0);
            else if (key == "auto_repair") settingsPanel_.pendingAutoRepair = (std::stoi(val) != 0);
            else if (key == "graphics_preset") {
                int presetVal = std::clamp(std::stoi(val), 0, 4);
                settingsPanel_.currentGraphicsPreset = static_cast<SettingsPanel::GraphicsPreset>(presetVal);
                settingsPanel_.pendingGraphicsPreset = settingsPanel_.currentGraphicsPreset;
            }
            else if (key == "ground_clutter_density") settingsPanel_.pendingGroundClutterDensity = std::clamp(std::stoi(val), 0, 150);
            else if (key == "shadows") settingsPanel_.pendingShadows = (std::stoi(val) != 0);
            else if (key == "shadow_distance") settingsPanel_.pendingShadowDistance = std::clamp(std::stof(val), 40.0f, 500.0f);
            else if (key == "brightness") {
                settingsPanel_.pendingBrightness = std::clamp(std::stoi(val), 0, 100);
                if (auto* r = services_.renderer)
                    r->setBrightness(static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f);
            }
            else if (key == "water_refraction") settingsPanel_.pendingWaterRefraction = (std::stoi(val) != 0);
            else if (key == "antialiasing") settingsPanel_.pendingAntiAliasing = std::clamp(std::stoi(val), 0, 3);
            else if (key == "fxaa") settingsPanel_.pendingFXAA = (std::stoi(val) != 0);
            else if (key == "normal_mapping") settingsPanel_.pendingNormalMapping = (std::stoi(val) != 0);
            else if (key == "normal_map_strength") settingsPanel_.pendingNormalMapStrength = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "pom") settingsPanel_.pendingPOM = (std::stoi(val) != 0);
            else if (key == "pom_quality") settingsPanel_.pendingPOMQuality = std::clamp(std::stoi(val), 0, 2);
            else if (key == "upscaling_mode") {
                settingsPanel_.pendingUpscalingMode = std::clamp(std::stoi(val), 0, 2);
                settingsPanel_.pendingFSR = (settingsPanel_.pendingUpscalingMode == 1);
            } else if (key == "fsr") {
                settingsPanel_.pendingFSR = (std::stoi(val) != 0);
                // Backward compatibility: old configs only had fsr=0/1.
                if (settingsPanel_.pendingUpscalingMode == 0 && settingsPanel_.pendingFSR) settingsPanel_.pendingUpscalingMode = 1;
            }
            else if (key == "fsr_quality") settingsPanel_.pendingFSRQuality = std::clamp(std::stoi(val), 0, 3);
            else if (key == "fsr_sharpness") settingsPanel_.pendingFSRSharpness = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "fsr2_jitter_sign") settingsPanel_.pendingFSR2JitterSign = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_x") settingsPanel_.pendingFSR2MotionVecScaleX = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_y") settingsPanel_.pendingFSR2MotionVecScaleY = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "amd_fsr3_framegen") settingsPanel_.pendingAMDFramegen = (std::stoi(val) != 0);
            // Controls
            else if (key == "mouse_sensitivity") settingsPanel_.pendingMouseSensitivity = std::clamp(std::stof(val), 0.05f, 1.0f);
            else if (key == "invert_mouse") settingsPanel_.pendingInvertMouse = (std::stoi(val) != 0);
            else if (key == "extended_zoom") settingsPanel_.pendingExtendedZoom = (std::stoi(val) != 0);
            else if (key == "camera_stiffness") settingsPanel_.pendingCameraStiffness = std::clamp(std::stof(val), 5.0f, 100.0f);
            else if (key == "camera_pivot_height") settingsPanel_.pendingPivotHeight = std::clamp(std::stof(val), 0.0f, 3.0f);
            else if (key == "fov") {
                settingsPanel_.pendingFov = std::clamp(std::stof(val), 45.0f, 110.0f);
                if (auto* renderer = services_.renderer) {
                    if (auto* camera = renderer->getCamera()) camera->setFov(settingsPanel_.pendingFov);
                }
            }
            // Quest tracker position/size
            else if (key == "quest_tracker_x") {
                // Legacy: ignore absolute X (right_offset supersedes it)
                (void)val;
            }
            else if (key == "quest_tracker_right_offset") {
                questTrackerRightOffset_ = std::stof(val);
                questTrackerPosInit_ = true;
            }
            else if (key == "quest_tracker_y") {
                questTrackerPos_.y = std::stof(val);
                questTrackerPosInit_ = true;
            }
            else if (key == "quest_tracker_w") {
                questTrackerSize_.x = std::max(100.0f, std::stof(val));
            }
            else if (key == "quest_tracker_h") {
                questTrackerSize_.y = std::max(60.0f, std::stof(val));
            }
            // Chat
            else if (key == "chat_active_tab") chatPanel_.activeChatTab = std::clamp(std::stoi(val), 0, 3);
            else if (key == "chat_timestamps") chatPanel_.chatShowTimestamps = (std::stoi(val) != 0);
            else if (key == "chat_font_size") chatPanel_.chatFontSize = std::clamp(std::stoi(val), 0, 2);
            else if (key == "chat_autojoin_general") chatPanel_.chatAutoJoinGeneral = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_trade") chatPanel_.chatAutoJoinTrade = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_localdefense") chatPanel_.chatAutoJoinLocalDefense = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_lfg") chatPanel_.chatAutoJoinLFG = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_local") chatPanel_.chatAutoJoinLocal = (std::stoi(val) != 0);
        } catch (...) {}
    }

    // Load keybindings from the same config file
    KeybindingManager::getInstance().loadFromConfigFile(path);

    LOG_INFO("Settings loaded from ", path);
}

// ============================================================
// Mail Window
// ============================================================



// ============================================================
// Bank Window
// ============================================================


// ============================================================
// Guild Bank Window
// ============================================================


// ============================================================
// Auction House Window
// ============================================================



// ---------------------------------------------------------------------------
// Screen-space weather overlay (rain / snow / storm)
// ---------------------------------------------------------------------------
void GameScreen::renderWeatherOverlay(game::GameHandler& gameHandler) {
    uint32_t wType     = gameHandler.getWeatherType();
    float    intensity = gameHandler.getWeatherIntensity();
    if (wType == 0 || intensity < 0.05f) return;

    const ImGuiIO& io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;
    if (sw <= 0.0f || sh <= 0.0f) return;

    // Seeded RNG for weather particle positions — replaces std::rand() which
    // shares global state and has modulo bias.
    static std::mt19937 wxRng(std::random_device{}());
    auto wxRandInt = [](int maxExcl) {
        return std::uniform_int_distribution<int>(0, std::max(0, maxExcl - 1))(wxRng);
    };

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float dt = std::min(io.DeltaTime, 0.05f);   // cap delta at 50ms to avoid teleporting particles

    if (wType == 1 || wType == 3) {
        // ── Rain / Storm ─────────────────────────────────────────────────────
        constexpr int MAX_DROPS = 300;
        struct RainState {
            float x[MAX_DROPS], y[MAX_DROPS];
            bool  initialized = false;
            uint32_t lastType = 0;
            float lastW = 0.0f, lastH = 0.0f;
        };
        static RainState rs;

        // Re-seed if weather type or screen size changed
        if (!rs.initialized || rs.lastType != wType ||
            rs.lastW != sw   || rs.lastH != sh) {
            for (int i = 0; i < MAX_DROPS; ++i) {
                rs.x[i] = static_cast<float>(wxRandInt(static_cast<int>(sw) + 200)) - 100.0f;
                rs.y[i] = static_cast<float>(wxRandInt(static_cast<int>(sh)));
            }
            rs.initialized = true;
            rs.lastType = wType;
            rs.lastW = sw;
            rs.lastH = sh;
        }

        const float fallSpeed = (wType == 3) ? 680.0f : 440.0f;
        const float windSpeed = (wType == 3) ? 110.0f :  65.0f;
        const int   numDrops  = static_cast<int>(MAX_DROPS * std::min(1.0f, intensity));
        const float alpha     = std::min(1.0f, 0.28f + intensity * 0.38f);
        const uint8_t alphaU8 = static_cast<uint8_t>(alpha * 255.0f);
        const ImU32  dropCol  = IM_COL32(175, 195, 225, alphaU8);
        const float  dropLen  = 7.0f + intensity * 7.0f;
        // Normalised wind direction for the trail endpoint
        const float invSpeed  = 1.0f / std::sqrt(fallSpeed * fallSpeed + windSpeed * windSpeed);
        const float trailDx   = -windSpeed * invSpeed * dropLen;
        const float trailDy   = -fallSpeed * invSpeed * dropLen;

        for (int i = 0; i < numDrops; ++i) {
            rs.x[i] += windSpeed * dt;
            rs.y[i] += fallSpeed * dt;
            if (rs.y[i] > sh + 10.0f) {
                rs.y[i] = -10.0f;
                rs.x[i] = static_cast<float>(wxRandInt(static_cast<int>(sw) + 200)) - 100.0f;
            }
            if (rs.x[i] > sw + 100.0f) rs.x[i] -= sw + 200.0f;
            dl->AddLine(ImVec2(rs.x[i], rs.y[i]),
                        ImVec2(rs.x[i] + trailDx, rs.y[i] + trailDy),
                        dropCol, 1.0f);
        }

        // Storm: dark fog-vignette at screen edges
        if (wType == 3) {
            const float vigAlpha = std::min(1.0f, 0.12f + intensity * 0.18f);
            const ImU32 vigCol   = IM_COL32(60, 65, 80, static_cast<uint8_t>(vigAlpha * 255.0f));
            const float vigW = sw * 0.22f;
            const float vigH = sh * 0.22f;
            dl->AddRectFilledMultiColor(ImVec2(0,       0),      ImVec2(vigW, sh),     vigCol, IM_COL32_BLACK_TRANS, IM_COL32_BLACK_TRANS, vigCol);
            dl->AddRectFilledMultiColor(ImVec2(sw-vigW, 0),      ImVec2(sw,   sh),     IM_COL32_BLACK_TRANS, vigCol, vigCol, IM_COL32_BLACK_TRANS);
            dl->AddRectFilledMultiColor(ImVec2(0,       0),      ImVec2(sw,   vigH),   vigCol, vigCol, IM_COL32_BLACK_TRANS, IM_COL32_BLACK_TRANS);
            dl->AddRectFilledMultiColor(ImVec2(0,       sh-vigH),ImVec2(sw,   sh),     IM_COL32_BLACK_TRANS, IM_COL32_BLACK_TRANS, vigCol, vigCol);
        }

    } else if (wType == 2) {
        // ── Snow ─────────────────────────────────────────────────────────────
        constexpr int MAX_FLAKES = 120;
        struct SnowState {
            float x[MAX_FLAKES], y[MAX_FLAKES], phase[MAX_FLAKES];
            bool  initialized = false;
            float lastW = 0.0f, lastH = 0.0f;
        };
        static SnowState ss;

        if (!ss.initialized || ss.lastW != sw || ss.lastH != sh) {
            for (int i = 0; i < MAX_FLAKES; ++i) {
                ss.x[i]     = static_cast<float>(wxRandInt(static_cast<int>(sw)));
                ss.y[i]     = static_cast<float>(wxRandInt(static_cast<int>(sh)));
                ss.phase[i] = static_cast<float>(wxRandInt(628)) * 0.01f;
            }
            ss.initialized = true;
            ss.lastW = sw;
            ss.lastH = sh;
        }

        const float fallSpeed = 45.0f + intensity * 45.0f;
        const int   numFlakes = static_cast<int>(MAX_FLAKES * std::min(1.0f, intensity));
        const float alpha     = std::min(1.0f, 0.5f + intensity * 0.3f);
        const uint8_t alphaU8 = static_cast<uint8_t>(alpha * 255.0f);
        const float   radius  = 1.5f + intensity * 1.5f;
        const float   time    = static_cast<float>(ImGui::GetTime());

        for (int i = 0; i < numFlakes; ++i) {
            float sway = std::sin(time * 0.7f + ss.phase[i]) * 18.0f;
            ss.x[i] += sway * dt;
            ss.y[i] += fallSpeed * dt;
            ss.phase[i] += dt * 0.25f;
            if (ss.y[i] > sh + 5.0f) {
                ss.y[i] = -5.0f;
                ss.x[i] = static_cast<float>(wxRandInt(static_cast<int>(sw)));
            }
            if (ss.x[i] < -5.0f) ss.x[i] += sw + 10.0f;
            if (ss.x[i] > sw + 5.0f) ss.x[i] -= sw + 10.0f;
            // Two-tone: bright centre dot + transparent outer ring for depth
            dl->AddCircleFilled(ImVec2(ss.x[i], ss.y[i]), radius, IM_COL32(220, 235, 255, alphaU8));
            dl->AddCircleFilled(ImVec2(ss.x[i], ss.y[i]), radius * 0.45f, IM_COL32(245, 250, 255, std::min(255, alphaU8 + 30)));
        }
    }
}

// ---------------------------------------------------------------------------
// Dungeon Finder window (toggle with hotkey or bag-bar button)
// ---------------------------------------------------------------------------
// ============================================================
// Instance Lockouts
// ============================================================




// ─── Threat Window ────────────────────────────────────────────────────────────
// ─── BG Scoreboard ────────────────────────────────────────────────────────────






}} // namespace wowee::ui
