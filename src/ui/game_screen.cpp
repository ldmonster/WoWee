#include "ui/game_screen.hpp"
#include "rendering/character_preview.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "core/spawn_presets.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
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
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_layout.hpp"

#include "game/expansion_profile.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>
#include <unordered_set>

namespace {
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
    initChatTabs();
}

void GameScreen::initChatTabs() {
    chatTabs_.clear();
    // General tab: shows everything
    chatTabs_.push_back({"General", 0xFFFFFFFF});
    // Combat tab: system + loot messages
    chatTabs_.push_back({"Combat", (1u << static_cast<uint8_t>(game::ChatType::SYSTEM)) |
                                    (1u << static_cast<uint8_t>(game::ChatType::LOOT))});
    // Whispers tab
    chatTabs_.push_back({"Whispers", (1u << static_cast<uint8_t>(game::ChatType::WHISPER)) |
                                      (1u << static_cast<uint8_t>(game::ChatType::WHISPER_INFORM))});
    // Trade/LFG tab: channel messages
    chatTabs_.push_back({"Trade/LFG", (1u << static_cast<uint8_t>(game::ChatType::CHANNEL))});
}

bool GameScreen::shouldShowMessage(const game::MessageChatData& msg, int tabIndex) const {
    if (tabIndex < 0 || tabIndex >= static_cast<int>(chatTabs_.size())) return true;
    const auto& tab = chatTabs_[tabIndex];
    if (tab.typeMask == 0xFFFFFFFF) return true;  // General tab shows all

    uint32_t typeBit = 1u << static_cast<uint8_t>(msg.type);

    // For Trade/LFG tab, also filter by channel name
    if (tabIndex == 3 && msg.type == game::ChatType::CHANNEL) {
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

void GameScreen::render(game::GameHandler& gameHandler) {
    // Set up chat bubble callback (once)
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

    // Apply UI transparency setting
    float prevAlpha = ImGui::GetStyle().Alpha;
    ImGui::GetStyle().Alpha = uiOpacity_;

    // Sync minimap opacity with UI opacity
    {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setOpacity(uiOpacity_);
            }
        }
    }

    // Apply initial settings when renderer becomes available
    if (!minimapSettingsApplied_) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimapRotate_ = false;
                pendingMinimapRotate = false;
                minimap->setRotateWithCamera(false);
                minimap->setSquareShape(minimapSquare_);
                minimapSettingsApplied_ = true;
            }
            if (auto* zm = renderer->getZoneManager()) {
                zm->setUseOriginalSoundtrack(pendingUseOriginalSoundtrack);
            }
            if (auto* tm = renderer->getTerrainManager()) {
                tm->setGroundClutterDensityScale(static_cast<float>(pendingGroundClutterDensity) / 100.0f);
            }
            // Restore mute state: save actual master volume first, then apply mute
            if (soundMuted_) {
                float actual = audio::AudioEngine::instance().getMasterVolume();
                preMuteVolume_ = (actual > 0.0f) ? actual
                    : static_cast<float>(pendingMasterVolume) / 100.0f;
                audio::AudioEngine::instance().setMasterVolume(0.0f);
            }
        }
    }

    // Apply saved volume settings once when audio managers first become available
    if (!volumeSettingsApplied_) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer && renderer->getUiSoundManager()) {
            float masterScale = soundMuted_ ? 0.0f : static_cast<float>(pendingMasterVolume) / 100.0f;
            audio::AudioEngine::instance().setMasterVolume(masterScale);
            if (auto* music = renderer->getMusicManager()) {
                music->setVolume(pendingMusicVolume);
            }
            if (auto* ambient = renderer->getAmbientSoundManager()) {
                ambient->setVolumeScale(pendingAmbientVolume / 100.0f);
            }
            if (auto* ui = renderer->getUiSoundManager()) {
                ui->setVolumeScale(pendingUiVolume / 100.0f);
            }
            if (auto* combat = renderer->getCombatSoundManager()) {
                combat->setVolumeScale(pendingCombatVolume / 100.0f);
            }
            if (auto* spell = renderer->getSpellSoundManager()) {
                spell->setVolumeScale(pendingSpellVolume / 100.0f);
            }
            if (auto* movement = renderer->getMovementSoundManager()) {
                movement->setVolumeScale(pendingMovementVolume / 100.0f);
            }
            if (auto* footstep = renderer->getFootstepManager()) {
                footstep->setVolumeScale(pendingFootstepVolume / 100.0f);
            }
            if (auto* npcVoice = renderer->getNpcVoiceManager()) {
                npcVoice->setVolumeScale(pendingNpcVoiceVolume / 100.0f);
            }
            if (auto* mount = renderer->getMountSoundManager()) {
                mount->setVolumeScale(pendingMountVolume / 100.0f);
            }
            if (auto* activity = renderer->getActivitySoundManager()) {
                activity->setVolumeScale(pendingActivityVolume / 100.0f);
            }
            volumeSettingsApplied_ = true;
        }
    }

    // Apply saved MSAA setting once when renderer is available
    if (!msaaSettingsApplied_ && pendingAntiAliasing > 0) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            static const VkSampleCountFlagBits aaSamples[] = {
                VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
            };
            renderer->setMsaaSamples(aaSamples[pendingAntiAliasing]);
            msaaSettingsApplied_ = true;
        }
    } else {
        msaaSettingsApplied_ = true;
    }

    // Apply saved water refraction setting once when renderer is available
    if (!waterRefractionApplied_) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            renderer->setWaterRefractionEnabled(pendingWaterRefraction);
            waterRefractionApplied_ = true;
        }
    }

    // Apply saved normal mapping / POM settings once when WMO renderer is available
    if (!normalMapSettingsApplied_) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            if (auto* wr = renderer->getWMORenderer()) {
                wr->setNormalMappingEnabled(pendingNormalMapping);
                wr->setNormalMapStrength(pendingNormalMapStrength);
                wr->setPOMEnabled(pendingPOM);
                wr->setPOMQuality(pendingPOMQuality);
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(pendingNormalMapping);
                    cr->setNormalMapStrength(pendingNormalMapStrength);
                    cr->setPOMEnabled(pendingPOM);
                    cr->setPOMQuality(pendingPOMQuality);
                }
                normalMapSettingsApplied_ = true;
            }
        }
    }

    // Apply saved upscaling setting once when renderer is available
    if (!fsrSettingsApplied_) {
        auto* renderer = core::Application::getInstance().getRenderer();
        if (renderer) {
            static const float fsrScales[] = { 0.77f, 0.67f, 0.59f, 1.00f };
            pendingFSRQuality = std::clamp(pendingFSRQuality, 0, 3);
            renderer->setFSRQuality(fsrScales[pendingFSRQuality]);
            renderer->setFSRSharpness(pendingFSRSharpness);
            renderer->setFSR2DebugTuning(pendingFSR2JitterSign, pendingFSR2MotionVecScaleX, pendingFSR2MotionVecScaleY);
            renderer->setAmdFsr3FramegenEnabled(pendingAMDFramegen);
            // Safety fallback: persisted FSR2 can still hang on some systems during startup.
            // Require explicit opt-in for startup FSR2; otherwise fall back to FSR1.
            const bool allowStartupFsr2 = (std::getenv("WOWEE_ALLOW_STARTUP_FSR2") != nullptr);
            int effectiveMode = pendingUpscalingMode;
            if (effectiveMode == 2 && !allowStartupFsr2) {
                static bool warnedStartupFsr2Fallback = false;
                if (!warnedStartupFsr2Fallback) {
                    LOG_WARNING("Startup FSR2 is disabled by default for stability; falling back to FSR1. Set WOWEE_ALLOW_STARTUP_FSR2=1 to override.");
                    warnedStartupFsr2Fallback = true;
                }
                effectiveMode = 1;
                pendingUpscalingMode = 1;
                pendingFSR = true;
            }

            // If explicitly enabled, still defer FSR2 until fully in-world.
            if (effectiveMode == 2 && gameHandler.getState() != game::WorldState::IN_WORLD) {
                renderer->setFSREnabled(false);
                renderer->setFSR2Enabled(false);
            } else {
                renderer->setFSREnabled(effectiveMode == 1);
                renderer->setFSR2Enabled(effectiveMode == 2);
                fsrSettingsApplied_ = true;
            }
        }
    }

    // Apply auto-loot setting to GameHandler every frame (cheap bool sync)
    gameHandler.setAutoLoot(pendingAutoLoot);

    // Sync chat auto-join settings to GameHandler
    gameHandler.chatAutoJoin.general = chatAutoJoinGeneral_;
    gameHandler.chatAutoJoin.trade = chatAutoJoinTrade_;
    gameHandler.chatAutoJoin.localDefense = chatAutoJoinLocalDefense_;
    gameHandler.chatAutoJoin.lfg = chatAutoJoinLFG_;
    gameHandler.chatAutoJoin.local = chatAutoJoinLocal_;

    // Process targeting input before UI windows
    processTargetInput(gameHandler);

    // Player unit frame (top-left)
    renderPlayerFrame(gameHandler);

    // Pet frame (below player frame, only when player has an active pet)
    if (gameHandler.hasPet()) {
        renderPetFrame(gameHandler);
    }

    // Target frame (only when we have a target)
    if (gameHandler.hasTarget()) {
        renderTargetFrame(gameHandler);
    }

    // Render windows
    if (showPlayerInfo) {
        renderPlayerInfo(gameHandler);
    }

    if (showEntityWindow) {
        renderEntityList(gameHandler);
    }

    if (showChatWindow) {
        renderChatWindow(gameHandler);
    }

    // ---- New UI elements ----
    renderActionBar(gameHandler);
    renderBagBar(gameHandler);
    renderXpBar(gameHandler);
    renderCastBar(gameHandler);
    renderMirrorTimers(gameHandler);
    renderQuestObjectiveTracker(gameHandler);
    renderNameplates(gameHandler);  // player names always shown; NPC plates gated by showNameplates_
    renderBattlegroundScore(gameHandler);
    renderCombatText(gameHandler);
    renderPartyFrames(gameHandler);
    renderBossFrames(gameHandler);
    renderGroupInvitePopup(gameHandler);
    renderDuelRequestPopup(gameHandler);
    renderLootRollPopup(gameHandler);
    renderTradeRequestPopup(gameHandler);
    renderSummonRequestPopup(gameHandler);
    renderSharedQuestPopup(gameHandler);
    renderItemTextWindow(gameHandler);
    renderGuildInvitePopup(gameHandler);
    renderReadyCheckPopup(gameHandler);
    renderGuildRoster(gameHandler);
    renderBuffBar(gameHandler);
    renderLootWindow(gameHandler);
    renderGossipWindow(gameHandler);
    renderQuestDetailsWindow(gameHandler);
    renderQuestRequestItemsWindow(gameHandler);
    renderQuestOfferRewardWindow(gameHandler);
    renderVendorWindow(gameHandler);
    renderTrainerWindow(gameHandler);
    renderTaxiWindow(gameHandler);
    renderMailWindow(gameHandler);
    renderMailComposeWindow(gameHandler);
    renderBankWindow(gameHandler);
    renderGuildBankWindow(gameHandler);
    renderAuctionHouseWindow(gameHandler);
    renderDungeonFinderWindow(gameHandler);
    renderInstanceLockouts(gameHandler);
    // renderQuestMarkers(gameHandler);  // Disabled - using 3D billboard markers now
    renderMinimapMarkers(gameHandler);
    renderDeathScreen(gameHandler);
    renderReclaimCorpseButton(gameHandler);
    renderResurrectDialog(gameHandler);
    renderTalentWipeConfirmDialog(gameHandler);
    renderChatBubbles(gameHandler);
    renderEscapeMenu();
    renderSettingsWindow();
    renderDingEffect();
    renderAchievementToast();
    renderZoneText();

    // World map (M key toggle handled inside)
    renderWorldMap(gameHandler);

    // Quest Log (L key toggle handled inside)
    questLogScreen.render(gameHandler);

    // Spellbook (P key toggle handled inside)
    spellbookScreen.render(gameHandler, core::Application::getInstance().getAssetManager());

    // Talents (N key toggle handled inside)
    talentScreen.render(gameHandler);

    // Set up inventory screen asset manager + player appearance (re-init on character switch)
    {
        uint64_t activeGuid = gameHandler.getActiveCharacterGuid();
        if (activeGuid != 0 && activeGuid != inventoryScreenCharGuid_) {
            auto* am = core::Application::getInstance().getAssetManager();
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
        if (!vendorBagsOpened_) {
            vendorBagsOpened_ = true;
            if (inventoryScreen.isSeparateBags()) {
                inventoryScreen.openAllBags();
            } else if (!inventoryScreen.isOpen()) {
                inventoryScreen.setOpen(true);
            }
        }
    } else {
        vendorBagsOpened_ = false;
    }

    // Bags (B key toggle handled inside)
    inventoryScreen.setGameHandler(&gameHandler);
    inventoryScreen.render(gameHandler.getInventory(), gameHandler.getMoneyCopper());

    // Character screen (C key toggle handled inside render())
    inventoryScreen.renderCharacterScreen(gameHandler);

    if (inventoryScreen.consumeEquipmentDirty() || gameHandler.consumeOnlineEquipmentDirty()) {
        updateCharacterGeosets(gameHandler.getInventory());
        updateCharacterTextures(gameHandler.getInventory());
        core::Application::getInstance().loadEquippedWeapons();
        inventoryScreen.markPreviewDirty();
        // Update renderer weapon type for animation selection
        auto* r = core::Application::getInstance().getRenderer();
        if (r) {
            const auto& mh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            r->setEquippedWeaponType(mh.empty() ? 0 : mh.item.inventoryType);
        }
    }

    // Update renderer face-target position and selection circle
    auto* renderer = core::Application::getInstance().getRenderer();
    if (renderer) {
        renderer->setInCombat(gameHandler.isInCombat());
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
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "In World");
            break;
        case game::WorldState::AUTHENTICATED:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Authenticated");
            break;
        case game::WorldState::ENTERING_WORLD:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Entering World...");
            break;
        default:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "State: %d", static_cast<int>(state));
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
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Player");
                        break;
                    case game::ObjectType::UNIT:
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Unit");
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

void GameScreen::renderChatWindow(game::GameHandler& gameHandler) {
    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
    float chatW = std::min(500.0f, screenW * 0.4f);
    float chatH = 220.0f;
    float chatX = 8.0f;
    float chatY = screenH - chatH - 80.0f;  // Above action bar
    if (chatWindowLocked) {
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
    if (chatWindowLocked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar;
    }
    ImGui::Begin("Chat", nullptr, flags);

    if (!chatWindowLocked) {
        chatWindowPos_ = ImGui::GetWindowPos();
    }

    // Chat tabs
    if (ImGui::BeginTabBar("ChatTabs")) {
        for (int i = 0; i < static_cast<int>(chatTabs_.size()); ++i) {
            if (ImGui::BeginTabItem(chatTabs_[i].name.c_str())) {
                activeChatTab_ = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    // Chat history
    const auto& chatHistory = gameHandler.getChatHistory();

    // Apply chat font size scaling
    float chatScale = chatFontSize_ == 0 ? 0.85f : (chatFontSize_ == 2 ? 1.2f : 1.0f);
    ImGui::SetWindowFontScale(chatScale);

    ImGui::BeginChild("ChatHistory", ImVec2(0, -70), true, ImGuiWindowFlags_HorizontalScrollbar);
    bool chatHistoryHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Helper: parse WoW color code |cAARRGGBB → ImVec4
    auto parseWowColor = [](const std::string& text, size_t pos) -> ImVec4 {
        // |cAARRGGBB (10 chars total: |c + 8 hex)
        if (pos + 10 > text.size()) return ImVec4(1, 1, 1, 1);
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
        ImVec4 qColor(1, 1, 1, 1);
        switch (info->quality) {
            case 0: qColor = ImVec4(0.62f, 0.62f, 0.62f, 1.0f); break; // Poor
            case 1: qColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break;    // Common
            case 2: qColor = ImVec4(0.12f, 1.0f, 0.0f, 1.0f); break;   // Uncommon
            case 3: qColor = ImVec4(0.0f, 0.44f, 0.87f, 1.0f); break;  // Rare
            case 4: qColor = ImVec4(0.64f, 0.21f, 0.93f, 1.0f); break; // Epic
            case 5: qColor = ImVec4(1.0f, 0.50f, 0.0f, 1.0f); break;   // Legendary
        }
        ImGui::TextColored(qColor, "%s", info->name.c_str());

        // Slot type
        if (info->inventoryType > 0) {
            const char* slotName = "";
            switch (info->inventoryType) {
                case 1:  slotName = "Head"; break;
                case 2:  slotName = "Neck"; break;
                case 3:  slotName = "Shoulder"; break;
                case 4:  slotName = "Shirt"; break;
                case 5:  slotName = "Chest"; break;
                case 6:  slotName = "Waist"; break;
                case 7:  slotName = "Legs"; break;
                case 8:  slotName = "Feet"; break;
                case 9:  slotName = "Wrist"; break;
                case 10: slotName = "Hands"; break;
                case 11: slotName = "Finger"; break;
                case 12: slotName = "Trinket"; break;
                case 13: slotName = "One-Hand"; break;
                case 14: slotName = "Shield"; break;
                case 15: slotName = "Ranged"; break;
                case 16: slotName = "Back"; break;
                case 17: slotName = "Two-Hand"; break;
                case 18: slotName = "Bag"; break;
                case 19: slotName = "Tabard"; break;
                case 20: slotName = "Robe"; break;
                case 21: slotName = "Main Hand"; break;
                case 22: slotName = "Off Hand"; break;
                case 23: slotName = "Held In Off-hand"; break;
                case 25: slotName = "Thrown"; break;
                case 26: slotName = "Ranged"; break;
            }
            if (slotName[0]) {
                if (!info->subclassName.empty())
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s  %s", slotName, info->subclassName.c_str());
                else
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", slotName);
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

        if (isWeapon && info->damageMax > 0.0f && info->delayMs > 0) {
            float speed = static_cast<float>(info->delayMs) / 1000.0f;
            float dps = ((info->damageMin + info->damageMax) * 0.5f) / speed;
            ImGui::Text("%.1f DPS", dps);
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
        if (info->sellPrice > 0) {
            uint32_t g = info->sellPrice / 10000;
            uint32_t s = (info->sellPrice / 100) % 100;
            uint32_t c = info->sellPrice % 100;
            ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "Sell: %ug %us %uc", g, s, c);
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
                    ImGui::Text("%.1f DPS", dps);
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

            // Find next WoW item link: |cXXXXXXXX|Hitem:ENTRY:...|h[Name]|h|r
            size_t linkStart = text.find("|c", pos);
            // Also handle bare |Hitem: without color prefix
            size_t bareLinkStart = text.find("|Hitem:", pos);

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
                    hStart = text.find("|Hitem:", linkStart + 10);
                } else if (nextSpecial == bareLinkStart) {
                    hStart = bareLinkStart;
                }

                if (hStart != std::string::npos) {
                    // Parse item entry: |Hitem:ENTRY:...
                    size_t entryStart = hStart + 7; // skip "|Hitem:"
                    size_t entryEnd = text.find(':', entryStart);
                    uint32_t itemEntry = 0;
                    if (entryEnd != std::string::npos) {
                        itemEntry = static_cast<uint32_t>(strtoul(
                            text.substr(entryStart, entryEnd - entryStart).c_str(), nullptr, 10));
                    }

                    // Find display name: |h[Name]|h
                    size_t nameTagStart = text.find("|h[", hStart);
                    size_t nameTagEnd = (nameTagStart != std::string::npos)
                        ? text.find("]|h", nameTagStart + 3) : std::string::npos;

                    std::string itemName = "Unknown Item";
                    if (nameTagStart != std::string::npos && nameTagEnd != std::string::npos) {
                        itemName = text.substr(nameTagStart + 3, nameTagEnd - nameTagStart - 3);
                    }

                    // Find end of entire link sequence (|r or after ]|h)
                    size_t linkEnd = (nameTagEnd != std::string::npos) ? nameTagEnd + 3 : hStart + 7;
                    size_t resetPos = text.find("|r", linkEnd);
                    if (resetPos != std::string::npos && resetPos <= linkEnd + 2) {
                        linkEnd = resetPos + 2;
                    }

                    // Ensure item info is cached (trigger query if needed)
                    if (itemEntry > 0) {
                        gameHandler.ensureItemInfo(itemEntry);
                    }

                    // Render bracketed item name in quality color
                    std::string display = "[" + itemName + "]";
                    ImGui::PushStyleColor(ImGuiCol_Text, linkColor);
                    ImGui::TextWrapped("%s", display.c_str());
                    ImGui::PopStyleColor();

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if (itemEntry > 0) {
                            renderItemLinkTooltip(itemEntry);
                        }
                    }

                    // Shift-click: insert item link into chat input
                    if (ImGui::IsItemClicked() && ImGui::GetIO().KeyShift) {
                        std::string linkText = text.substr(nextSpecial, linkEnd - nextSpecial);
                        size_t curLen = strlen(chatInputBuffer);
                        if (curLen + linkText.size() + 1 < sizeof(chatInputBuffer)) {
                            strncat(chatInputBuffer, linkText.c_str(), sizeof(chatInputBuffer) - curLen - 1);
                            chatInputMoveCursorToEnd = true;
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

    for (const auto& msg : chatHistory) {
        if (!shouldShowMessage(msg, activeChatTab_)) continue;
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
        if (chatShowTimestamps_) {
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

        if (msg.type == game::ChatType::SYSTEM) {
            renderTextWithLinks(tsPrefix + processedMessage, color);
        } else if (msg.type == game::ChatType::TEXT_EMOTE) {
            renderTextWithLinks(tsPrefix + processedMessage, color);
        } else if (!resolvedSenderName.empty()) {
            if (msg.type == game::ChatType::SAY ||
                msg.type == game::ChatType::MONSTER_SAY || msg.type == game::ChatType::MONSTER_PARTY) {
                std::string fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " says: " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else if (msg.type == game::ChatType::YELL || msg.type == game::ChatType::MONSTER_YELL) {
                std::string fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " yells: " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else if (msg.type == game::ChatType::WHISPER ||
                       msg.type == game::ChatType::MONSTER_WHISPER || msg.type == game::ChatType::RAID_BOSS_WHISPER) {
                std::string fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " whispers: " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else if (msg.type == game::ChatType::WHISPER_INFORM) {
                // Outgoing whisper — show "To Name: message" (WoW-style)
                const std::string& target = !msg.receiverName.empty() ? msg.receiverName : resolvedSenderName;
                std::string fullMsg = tsPrefix + "To " + target + ": " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else if (msg.type == game::ChatType::EMOTE ||
                       msg.type == game::ChatType::MONSTER_EMOTE || msg.type == game::ChatType::RAID_BOSS_EMOTE) {
                std::string fullMsg = tsPrefix + tagPrefix + resolvedSenderName + " " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else if (msg.type == game::ChatType::CHANNEL && !msg.channelName.empty()) {
                int chIdx = gameHandler.getChannelIndex(msg.channelName);
                std::string chDisplay = chIdx > 0
                    ? "[" + std::to_string(chIdx) + ". " + msg.channelName + "]"
                    : "[" + msg.channelName + "]";
                std::string fullMsg = tsPrefix + chDisplay + " [" + tagPrefix + resolvedSenderName + "]: " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else {
                std::string fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + tagPrefix + resolvedSenderName + ": " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            }
        } else {
            // No sender name. For group/channel types show a bracket prefix;
            // for sender-specific types (SAY, YELL, WHISPER, etc.) just show the
            // raw message — these are server-side announcements without a speaker.
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
                std::string fullMsg = tsPrefix + "[" + std::string(getChatTypeName(msg.type)) + "] " + processedMessage;
                renderTextWithLinks(fullMsg, color);
            } else {
                // SAY, YELL, WHISPER, unknown BG_SYSTEM_* types, etc. — no prefix
                renderTextWithLinks(tsPrefix + processedMessage, color);
            }
        }
    }

    // Auto-scroll to bottom
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    // Reset font scale after chat history
    ImGui::SetWindowFontScale(1.0f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // Lock toggle
    ImGui::Checkbox("Lock", &chatWindowLocked);
    ImGui::SameLine();
    ImGui::TextDisabled(chatWindowLocked ? "(locked)" : "(movable)");

    // Chat input
    ImGui::Text("Type:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* chatTypes[] = { "SAY", "YELL", "PARTY", "GUILD", "WHISPER", "RAID", "OFFICER", "BATTLEGROUND", "RAID WARNING", "INSTANCE" };
    ImGui::Combo("##ChatType", &selectedChatType, chatTypes, 10);

    // Auto-fill whisper target when switching to WHISPER mode
    if (selectedChatType == 4 && lastChatType != 4) {
        // Just switched to WHISPER mode
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::PLAYER) {
                auto player = std::static_pointer_cast<game::Player>(target);
                if (!player->getName().empty()) {
                    strncpy(whisperTargetBuffer, player->getName().c_str(), sizeof(whisperTargetBuffer) - 1);
                    whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                }
            }
        }
    }
    lastChatType = selectedChatType;

    // Show whisper target field if WHISPER is selected
    if (selectedChatType == 4) {
        ImGui::SameLine();
        ImGui::Text("To:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##WhisperTarget", whisperTargetBuffer, sizeof(whisperTargetBuffer));
    }

    ImGui::SameLine();
    ImGui::Text("Message:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(-1);
    if (refocusChatInput) {
        ImGui::SetKeyboardFocusHere();
        refocusChatInput = false;
    }

    // Detect chat channel prefix as user types and switch the dropdown
    {
        std::string buf(chatInputBuffer);
        if (buf.size() >= 2 && buf[0] == '/') {
            // Find the command and check if there's a space after it
            size_t sp = buf.find(' ', 1);
            if (sp != std::string::npos) {
                std::string cmd = buf.substr(1, sp - 1);
                for (char& c : cmd) c = std::tolower(c);
                int detected = -1;
                if (cmd == "s" || cmd == "say") detected = 0;
                else if (cmd == "y" || cmd == "yell" || cmd == "shout") detected = 1;
                else if (cmd == "p" || cmd == "party") detected = 2;
                else if (cmd == "g" || cmd == "guild") detected = 3;
                else if (cmd == "w" || cmd == "whisper" || cmd == "tell" || cmd == "t") detected = 4;
                else if (cmd == "raid" || cmd == "rsay" || cmd == "ra") detected = 5;
                else if (cmd == "o" || cmd == "officer" || cmd == "osay") detected = 6;
                else if (cmd == "bg" || cmd == "battleground") detected = 7;
                else if (cmd == "rw" || cmd == "raidwarning") detected = 8;
                else if (cmd == "i" || cmd == "instance") detected = 9;
                if (detected >= 0 && selectedChatType != detected) {
                    selectedChatType = detected;
                    // Strip the prefix, keep only the message part
                    std::string remaining = buf.substr(sp + 1);
                    // For whisper, first word after /w is the target
                    if (detected == 4) {
                        size_t msgStart = remaining.find(' ');
                        if (msgStart != std::string::npos) {
                            std::string wTarget = remaining.substr(0, msgStart);
                            strncpy(whisperTargetBuffer, wTarget.c_str(), sizeof(whisperTargetBuffer) - 1);
                            whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                            remaining = remaining.substr(msgStart + 1);
                        } else {
                            // Just the target name so far, no message yet
                            strncpy(whisperTargetBuffer, remaining.c_str(), sizeof(whisperTargetBuffer) - 1);
                            whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                            remaining = "";
                        }
                    }
                    strncpy(chatInputBuffer, remaining.c_str(), sizeof(chatInputBuffer) - 1);
                    chatInputBuffer[sizeof(chatInputBuffer) - 1] = '\0';
                    chatInputMoveCursorToEnd = true;
                }
            }
        }
    }

    // Color the input text based on current chat type
    ImVec4 inputColor;
    switch (selectedChatType) {
        case 1: inputColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;  // YELL - red
        case 2: inputColor = ImVec4(0.4f, 0.6f, 1.0f, 1.0f); break;  // PARTY - blue
        case 3: inputColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); break;  // GUILD - green
        case 4: inputColor = ImVec4(1.0f, 0.5f, 1.0f, 1.0f); break;  // WHISPER - pink
        case 5: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // RAID - orange
        case 6: inputColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); break;  // OFFICER - green
        case 7: inputColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break;  // BG - orange
        case 8: inputColor = ImVec4(1.0f, 0.3f, 0.0f, 1.0f); break;  // RAID WARNING - red-orange
        case 9: inputColor = ImVec4(0.4f, 0.6f, 1.0f, 1.0f); break;  // INSTANCE - blue
        default: inputColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break; // SAY - white
    }
    ImGui::PushStyleColor(ImGuiCol_Text, inputColor);

    auto inputCallback = [](ImGuiInputTextCallbackData* data) -> int {
        auto* self = static_cast<GameScreen*>(data->UserData);
        if (self && self->chatInputMoveCursorToEnd) {
            int len = static_cast<int>(std::strlen(data->Buf));
            data->CursorPos = len;
            data->SelectionStart = len;
            data->SelectionEnd = len;
            self->chatInputMoveCursorToEnd = false;
        }
        return 0;
    };

    ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways;
    if (ImGui::InputText("##ChatInput", chatInputBuffer, sizeof(chatInputBuffer), inputFlags, inputCallback, this)) {
        sendChatMessage(gameHandler);
        // Close chat input on send so movement keys work immediately.
        refocusChatInput = false;
        ImGui::ClearActiveID();
    }
    ImGui::PopStyleColor();

    if (ImGui::IsItemActive()) {
        chatInputActive = true;
    } else {
        chatInputActive = false;
    }

    // Click in chat history area (received messages) → focus input.
    {
        if (chatHistoryHovered && ImGui::IsMouseClicked(0)) {
            refocusChatInput = true;
        }
    }

    ImGui::End();
}

void GameScreen::processTargetInput(game::GameHandler& gameHandler) {
    auto& io = ImGui::GetIO();
    auto& input = core::Input::getInstance();

    // Tab targeting (when keyboard not captured by UI)
    if (!io.WantCaptureKeyboard) {
        if (input.isKeyJustPressed(SDL_SCANCODE_TAB)) {
            const auto& movement = gameHandler.getMovementInfo();
            gameHandler.tabTarget(movement.x, movement.y, movement.z);
        }

        if (input.isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
            if (showSettingsWindow) {
                // Close settings window if open
                showSettingsWindow = false;
            } else if (showEscapeMenu) {
                showEscapeMenu = false;
                showEscapeSettingsNotice = false;
            } else if (gameHandler.isCasting()) {
                gameHandler.cancelCast();
            } else if (gameHandler.isLootWindowOpen()) {
                gameHandler.closeLoot();
            } else if (gameHandler.isGossipWindowOpen()) {
                gameHandler.closeGossip();
            } else {
                showEscapeMenu = true;
            }
        }

        // V — toggle nameplates (WoW default keybinding)
        if (input.isKeyJustPressed(SDL_SCANCODE_V)) {
            showNameplates_ = !showNameplates_;
        }

        // Action bar keys (1-9, 0, -, =)
        static const SDL_Scancode actionBarKeys[] = {
            SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
            SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
            SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS
        };
        const bool shiftDown = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
        const auto& bar = gameHandler.getActionBar();
        for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
            if (input.isKeyJustPressed(actionBarKeys[i])) {
                int slotIdx = shiftDown ? (game::GameHandler::SLOTS_PER_BAR + i) : i;
                if (bar[slotIdx].type == game::ActionBarSlot::SPELL && bar[slotIdx].isReady()) {
                    uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                    gameHandler.castSpell(bar[slotIdx].id, target);
                } else if (bar[slotIdx].type == game::ActionBarSlot::ITEM && bar[slotIdx].id != 0) {
                    gameHandler.useItemById(bar[slotIdx].id);
                }
            }
        }

    }

    // Slash key: focus chat input — always works unless already typing in chat
    if (!chatInputActive && input.isKeyJustPressed(SDL_SCANCODE_SLASH)) {
        refocusChatInput = true;
        chatInputBuffer[0] = '/';
        chatInputBuffer[1] = '\0';
        chatInputMoveCursorToEnd = true;
    }

    // Enter key: focus chat input (empty) — always works unless already typing
    if (!chatInputActive && input.isKeyJustPressed(SDL_SCANCODE_RETURN)) {
        refocusChatInput = true;
    }

    // Cursor affordance: show hand cursor over interactable game objects.
    if (!io.WantCaptureMouse) {
        auto* renderer = core::Application::getInstance().getRenderer();
        auto* camera = renderer ? renderer->getCamera() : nullptr;
        auto* window = core::Application::getInstance().getWindow();
        if (camera && window) {
            glm::vec2 mousePos = input.getMousePosition();
            float screenW = static_cast<float>(window->getWidth());
            float screenH = static_cast<float>(window->getHeight());
            rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
            float closestT = 1e30f;
            bool hoverInteractableGo = false;
            for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                if (entity->getType() != game::ObjectType::GAMEOBJECT) continue;

                glm::vec3 hitCenter;
                float hitRadius = 0.0f;
                bool hasBounds = core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
                if (!hasBounds) {
                    hitRadius = 2.5f;
                    hitCenter = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
                    hitCenter.z += 1.2f;
                } else {
                    hitRadius = std::max(hitRadius * 1.1f, 0.8f);
                }

                float hitT;
                if (raySphereIntersect(ray, hitCenter, hitRadius, hitT) && hitT < closestT) {
                    closestT = hitT;
                    hoverInteractableGo = true;
                }
            }
            if (hoverInteractableGo) {
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
        float dragDist = glm::length(releasePos - leftClickPressPos_);
        constexpr float CLICK_THRESHOLD = 5.0f;  // pixels

        if (dragDist < CLICK_THRESHOLD) {
            auto* renderer = core::Application::getInstance().getRenderer();
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = core::Application::getInstance().getWindow();

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
                gameHandler.setTarget(target->getGuid());
                gameHandler.interactWithGameObject(target->getGuid());
                return;
            }
        }

        // If no target or right-clicking in world, try to pick one under cursor
        {
            auto* renderer = core::Application::getInstance().getRenderer();
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = core::Application::getInstance().getWindow();
            if (camera && window) {
                glm::vec2 mousePos = input.getMousePosition();
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());
                rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
                float closestT = 1e30f;
                uint64_t closestGuid = 0;
                game::ObjectType closestType = game::ObjectType::OBJECT;
                float closestHostileUnitT = 1e30f;
                uint64_t closestHostileUnitGuid = 0;
                const uint64_t myGuid = gameHandler.getPlayerGuid();
                for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
                    auto t = entity->getType();
                    if (t != game::ObjectType::UNIT &&
                        t != game::ObjectType::PLAYER &&
                        t != game::ObjectType::GAMEOBJECT) continue;
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
                            // Do not hard-filter by GO type here. Some realms/content
                            // classify usable objects (including some chests) with types
                            // that look decorative in cache data.
                            hitRadius = 2.5f;
                            heightOffset = 1.2f;
                        }
                        hitCenter = core::coords::canonicalToRender(
                            glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
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
                            closestType = t;
                        }
                    }
                }
                // Prefer hostile monsters over nearby gameobjects/others when right-click picking.
                if (closestHostileUnitGuid != 0) {
                    closestGuid = closestHostileUnitGuid;
                    closestType = game::ObjectType::UNIT;
                }
                if (closestGuid != 0) {
                    if (closestType == game::ObjectType::GAMEOBJECT) {
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
        ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
        : (inCombatConfirmed
            ? ImVec4(1.0f, 0.2f, 0.2f, 1.0f)
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

        // Name in green (friendly player color) — clickable for self-target
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
        if (ImGui::Selectable(playerName.c_str(), false, 0, ImVec2(0, 0))) {
            gameHandler.setTarget(gameHandler.getPlayerGuid());
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("Lv %u", playerLevel);
        if (isDead) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "DEAD");
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

        // Health bar
        float pct = static_cast<float>(playerHp) / static_cast<float>(playerMaxHp);
        ImVec4 hpColor = isDead ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
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
                    case 0: powerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break; // Mana (blue)
                    case 1: powerColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); break; // Rage (red)
                    case 2: powerColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f); break; // Focus (orange)
                    case 3: powerColor = ImVec4(0.9f, 0.9f, 0.2f, 1.0f); break; // Energy (yellow)
                    case 4: powerColor = ImVec4(0.5f, 0.9f, 0.3f, 1.0f); break; // Happiness (green)
                    case 6: powerColor = ImVec4(0.8f, 0.1f, 0.2f, 1.0f); break; // Runic Power (crimson)
                    case 7: powerColor = ImVec4(0.4f, 0.1f, 0.6f, 1.0f); break; // Soul Shards (purple)
                    default: powerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break;
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
    auto* petUnit = dynamic_cast<game::Unit*>(petEntity.get());
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
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
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
                case 0: powerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break; // Mana
                case 1: powerColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); break; // Rage
                case 2: powerColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f); break; // Focus (hunter pets)
                case 3: powerColor = ImVec4(0.9f, 0.9f, 0.2f, 1.0f); break; // Energy
                default: powerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break;
            }
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, powerColor);
            char mpText[32];
            snprintf(mpText, sizeof(mpText), "%u/%u", power, maxPower);
            ImGui::ProgressBar(mpPct, ImVec2(-1, 14), mpText);
            ImGui::PopStyleColor();
        }

        // Dismiss button (compact, right-aligned)
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
        if (ImGui::SmallButton("Dismiss")) {
            gameHandler.dismissPet();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GameScreen::renderTargetFrame(game::GameHandler& gameHandler) {
    auto target = gameHandler.getTarget();
    if (!target) return;

    auto* window = core::Application::getInstance().getWindow();
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
        hostileColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
    } else if (target->getType() == game::ObjectType::UNIT) {
        auto u = std::static_pointer_cast<game::Unit>(target);
        if (u->getHealth() == 0 && u->getMaxHealth() > 0) {
            hostileColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        } else if (u->isHostile()) {
            // WoW level-based color for hostile mobs
            uint32_t playerLv = gameHandler.getPlayerLevel();
            uint32_t mobLv = u->getLevel();
            int32_t diff = static_cast<int32_t>(mobLv) - static_cast<int32_t>(playerLv);
            if (game::GameHandler::killXp(playerLv, mobLv) == 0) {
                hostileColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // Grey - no XP
            } else if (diff >= 10) {
                hostileColor = ImVec4(1.0f, 0.1f, 0.1f, 1.0f); // Red - skull/very hard
            } else if (diff >= 5) {
                hostileColor = ImVec4(1.0f, 0.5f, 0.1f, 1.0f); // Orange - hard
            } else if (diff >= -2) {
                hostileColor = ImVec4(1.0f, 1.0f, 0.1f, 1.0f); // Yellow - even
            } else {
                hostileColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Green - easy
            }
        } else {
            hostileColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f); // Friendly
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
        static const struct { const char* sym; ImU32 col; } kRaidMarks[] = {
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

        // Entity name and type
        std::string name = getEntityName(target);

        ImVec4 nameColor = hostileColor;

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(nameColor, "%s", name.c_str());

        // Level (for units/players) — colored by difficulty
        if (target->getType() == game::ObjectType::UNIT || target->getType() == game::ObjectType::PLAYER) {
            auto unit = std::static_pointer_cast<game::Unit>(target);
            ImGui::SameLine();
            // Level color matches the hostility/difficulty color
            ImVec4 levelColor = hostileColor;
            if (target->getType() == game::ObjectType::PLAYER) {
                levelColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            }
            ImGui::TextColored(levelColor, "Lv %u", unit->getLevel());

            // Health bar
            uint32_t hp = unit->getHealth();
            uint32_t maxHp = unit->getMaxHealth();
            if (maxHp > 0) {
                float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                    pct > 0.5f ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) :
                    pct > 0.2f ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f) :
                                 ImVec4(0.8f, 0.2f, 0.2f, 1.0f));

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
                        case 0: targetPowerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break; // Mana (blue)
                        case 1: targetPowerColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); break; // Rage (red)
                        case 2: targetPowerColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f); break; // Focus (orange)
                        case 3: targetPowerColor = ImVec4(0.9f, 0.9f, 0.2f, 1.0f); break; // Energy (yellow)
                        case 4: targetPowerColor = ImVec4(0.5f, 0.9f, 0.3f, 1.0f); break; // Happiness (green)
                        case 6: targetPowerColor = ImVec4(0.8f, 0.1f, 0.2f, 1.0f); break; // Runic Power (crimson)
                        case 7: targetPowerColor = ImVec4(0.4f, 0.1f, 0.6f, 1.0f); break; // Soul Shards (purple)
                        default: targetPowerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break;
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

        // Target cast bar — shown when the target is casting
        if (gameHandler.isTargetCasting()) {
            float castPct   = gameHandler.getTargetCastProgress();
            float castLeft  = gameHandler.getTargetCastTimeRemaining();
            uint32_t tspell = gameHandler.getTargetCastSpellId();
            const std::string& castName = (tspell != 0) ? gameHandler.getSpellName(tspell) : "";
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.3f, 0.2f, 1.0f));
            char castLabel[72];
            if (!castName.empty())
                snprintf(castLabel, sizeof(castLabel), "%s (%.1fs)", castName.c_str(), castLeft);
            else
                snprintf(castLabel, sizeof(castLabel), "Casting... (%.1fs)", castLeft);
            ImGui::ProgressBar(castPct, ImVec2(-1, 14), castLabel);
            ImGui::PopStyleColor();
        }

        // Distance
        const auto& movement = gameHandler.getMovementInfo();
        float dx = target->getX() - movement.x;
        float dy = target->getY() - movement.y;
        float dz = target->getZ() - movement.z;
        float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        ImGui::TextDisabled("%.1f yd", distance);

        // Target auras (buffs/debuffs)
        const auto& targetAuras = gameHandler.getTargetAuras();
        int activeAuras = 0;
        for (const auto& a : targetAuras) {
            if (!a.isEmpty()) activeAuras++;
        }
        if (activeAuras > 0) {
            auto* assetMgr = core::Application::getInstance().getAssetManager();
            constexpr float ICON_SIZE = 24.0f;
            constexpr int ICONS_PER_ROW = 8;

            ImGui::Separator();

            int shown = 0;
            for (size_t i = 0; i < targetAuras.size() && shown < 16; ++i) {
                const auto& aura = targetAuras[i];
                if (aura.isEmpty()) continue;

                if (shown > 0 && shown % ICONS_PER_ROW != 0) ImGui::SameLine();

                ImGui::PushID(static_cast<int>(10000 + i));

                bool isBuff = (aura.flags & 0x80) == 0;
                ImVec4 auraBorderColor = isBuff ? ImVec4(0.2f, 0.8f, 0.2f, 0.9f) : ImVec4(0.8f, 0.2f, 0.2f, 0.9f);

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
                    char label[8];
                    snprintf(label, sizeof(label), "%u", aura.spellId);
                    ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
                    ImGui::PopStyleColor();
                }

                // Compute remaining once for overlay + tooltip
                uint64_t tNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t tRemainMs = aura.getRemainingMs(tNowMs);

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
                    ImGui::GetWindowDrawList()->AddText(ImVec2(cx + 1, cy + 1),
                        IM_COL32(0, 0, 0, 200), timeStr);
                    ImGui::GetWindowDrawList()->AddText(ImVec2(cx, cy),
                        IM_COL32(255, 255, 255, 255), timeStr);
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

                // Tooltip
                if (ImGui::IsItemHovered()) {
                    std::string name = spellbookScreen.lookupSpellName(aura.spellId, assetMgr);
                    if (name.empty()) name = "Spell #" + std::to_string(aura.spellId);
                    if (tRemainMs > 0) {
                        int seconds = tRemainMs / 1000;
                        if (seconds < 60) {
                            ImGui::SetTooltip("%s (%ds)", name.c_str(), seconds);
                        } else {
                            ImGui::SetTooltip("%s (%dm %ds)", name.c_str(), seconds / 60, seconds % 60);
                        }
                    } else {
                        ImGui::SetTooltip("%s", name.c_str());
                    }
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
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", totName.c_str());

                    if (totEntity->getType() == game::ObjectType::UNIT ||
                        totEntity->getType() == game::ObjectType::PLAYER) {
                        auto totUnit = std::static_pointer_cast<game::Unit>(totEntity);
                        uint32_t hp = totUnit->getHealth();
                        uint32_t maxHp = totUnit->getMaxHealth();
                        if (maxHp > 0) {
                            float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                                pct > 0.5f ? ImVec4(0.2f, 0.7f, 0.2f, 1.0f) :
                                pct > 0.2f ? ImVec4(0.7f, 0.7f, 0.2f, 1.0f) :
                                             ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
                            ImGui::ProgressBar(pct, ImVec2(-1, 10), "");
                            ImGui::PopStyleColor();
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

void GameScreen::sendChatMessage(game::GameHandler& gameHandler) {
    if (strlen(chatInputBuffer) > 0) {
        std::string input(chatInputBuffer);
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
            for (char& c : cmdLower) c = std::tolower(c);

            // Special commands
            if (cmdLower == "logout") {
                core::Application::getInstance().logoutToLogin();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /invite command
            if (cmdLower == "invite" && spacePos != std::string::npos) {
                std::string targetName = command.substr(spacePos + 1);
                gameHandler.inviteToGroup(targetName);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /inspect command
            if (cmdLower == "inspect") {
                gameHandler.inspectTarget();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /time command
            if (cmdLower == "time") {
                gameHandler.queryServerTime();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /played command
            if (cmdLower == "played") {
                gameHandler.requestPlayedTime();
                chatInputBuffer[0] = '\0';
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
                    chatInputBuffer[0] = '\0';
                    return;
                }

                if (cmdLower == "who" && (query == "help" || query == "?")) {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "Who commands: /who [name/filter], /whois <name>, /online";
                    gameHandler.addLocalChatMessage(msg);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                gameHandler.queryWho(query);
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
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
                            chatInputBuffer[0] = '\0';
                            return;
                        } else if (subCmd == "remove" || subCmd == "delete" || subCmd == "rem") {
                            std::string playerName = args.substr(subCmdSpace + 1);
                            gameHandler.removeFriend(playerName);
                            chatInputBuffer[0] = '\0';
                            return;
                        }
                    } else {
                        // /addfriend name or /friend name (assume add)
                        gameHandler.addFriend(args);
                        chatInputBuffer[0] = '\0';
                        return;
                    }
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /friend add <name> or /friend remove <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /removefriend or /delfriend command
            if (cmdLower == "removefriend" || cmdLower == "delfriend" || cmdLower == "remfriend") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeFriend(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /removefriend <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /ignore command
            if (cmdLower == "ignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.addIgnore(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /unignore command
            if (cmdLower == "unignore") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.removeIgnore(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /unignore <name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /dismount command
            if (cmdLower == "dismount") {
                gameHandler.dismount();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /sit command
            if (cmdLower == "sit") {
                gameHandler.setStandState(1);  // 1 = sit
                chatInputBuffer[0] = '\0';
                return;
            }

            // /stand command
            if (cmdLower == "stand") {
                gameHandler.setStandState(0);  // 0 = stand
                chatInputBuffer[0] = '\0';
                return;
            }

            // /kneel command
            if (cmdLower == "kneel") {
                gameHandler.setStandState(8);  // 8 = kneel
                chatInputBuffer[0] = '\0';
                return;
            }

            // /logout command (already exists but using /logout instead of going to login)
            if (cmdLower == "logout" || cmdLower == "camp") {
                gameHandler.requestLogout();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /cancellogout command
            if (cmdLower == "cancellogout") {
                gameHandler.cancelLogout();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /helm command
            if (cmdLower == "helm" || cmdLower == "helmet" || cmdLower == "showhelm") {
                gameHandler.toggleHelm();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /cloak command
            if (cmdLower == "cloak" || cmdLower == "showcloak") {
                gameHandler.toggleCloak();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /follow command
            if (cmdLower == "follow" || cmdLower == "f") {
                gameHandler.followTarget();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /assist command
            if (cmdLower == "assist") {
                gameHandler.assistTarget();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /pvp command
            if (cmdLower == "pvp") {
                gameHandler.togglePvp();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /ginfo command
            if (cmdLower == "ginfo" || cmdLower == "guildinfo") {
                gameHandler.requestGuildInfo();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /groster command
            if (cmdLower == "groster" || cmdLower == "guildroster") {
                gameHandler.requestGuildRoster();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gmotd command
            if (cmdLower == "gmotd" || cmdLower == "guildmotd") {
                if (spacePos != std::string::npos) {
                    std::string motd = command.substr(spacePos + 1);
                    gameHandler.setGuildMotd(motd);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gmotd <message>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gpromote command
            if (cmdLower == "gpromote" || cmdLower == "guildpromote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.promoteGuildMember(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gpromote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gdemote command
            if (cmdLower == "gdemote" || cmdLower == "guilddemote") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.demoteGuildMember(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gdemote <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gquit command
            if (cmdLower == "gquit" || cmdLower == "guildquit" || cmdLower == "leaveguild") {
                gameHandler.leaveGuild();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /ginvite command
            if (cmdLower == "ginvite" || cmdLower == "guildinvite") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.inviteToGuild(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /ginvite <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gkick command
            if (cmdLower == "gkick" || cmdLower == "guildkick") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.kickGuildMember(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gkick <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gcreate command
            if (cmdLower == "gcreate" || cmdLower == "guildcreate") {
                if (spacePos != std::string::npos) {
                    std::string guildName = command.substr(spacePos + 1);
                    gameHandler.createGuild(guildName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gcreate <guild name>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gdisband command
            if (cmdLower == "gdisband" || cmdLower == "guilddisband") {
                gameHandler.disbandGuild();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /gleader command
            if (cmdLower == "gleader" || cmdLower == "guildleader") {
                if (spacePos != std::string::npos) {
                    std::string playerName = command.substr(spacePos + 1);
                    gameHandler.setGuildLeader(playerName);
                    chatInputBuffer[0] = '\0';
                    return;
                }

                game::MessageChatData msg;
                msg.type = game::ChatType::SYSTEM;
                msg.language = game::ChatLanguage::UNIVERSAL;
                msg.message = "Usage: /gleader <player>";
                gameHandler.addLocalChatMessage(msg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /readycheck command
            if (cmdLower == "readycheck" || cmdLower == "rc") {
                gameHandler.initiateReadyCheck();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /ready command (respond yes to ready check)
            if (cmdLower == "ready") {
                gameHandler.respondToReadyCheck(true);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /notready command (respond no to ready check)
            if (cmdLower == "notready" || cmdLower == "nr") {
                gameHandler.respondToReadyCheck(false);
                chatInputBuffer[0] = '\0';
                return;
            }

            // /yield or /forfeit command
            if (cmdLower == "yield" || cmdLower == "forfeit" || cmdLower == "surrender") {
                gameHandler.forfeitDuel();
                chatInputBuffer[0] = '\0';
                return;
            }

            // AFK command
            if (cmdLower == "afk" || cmdLower == "away") {
                std::string afkMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleAfk(afkMsg);
                chatInputBuffer[0] = '\0';
                return;
            }

            // DND command
            if (cmdLower == "dnd" || cmdLower == "busy") {
                std::string dndMsg = (spacePos != std::string::npos) ? command.substr(spacePos + 1) : "";
                gameHandler.toggleDnd(dndMsg);
                chatInputBuffer[0] = '\0';
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
                    chatInputBuffer[0] = '\0';
                    return;
                }
                // Set whisper target to last whisper sender
                strncpy(whisperTargetBuffer, lastSender.c_str(), sizeof(whisperTargetBuffer) - 1);
                whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                if (spacePos != std::string::npos) {
                    // /r message — send reply immediately
                    std::string replyMsg = command.substr(spacePos + 1);
                    gameHandler.sendChatMessage(game::ChatType::WHISPER, replyMsg, lastSender);
                }
                // Switch to whisper tab
                selectedChatType = 4;
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "leave" || cmdLower == "leaveparty") {
                gameHandler.leaveParty();
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "clearmaintank") {
                gameHandler.clearMainTank();
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "clearmainassist") {
                gameHandler.clearMainAssist();
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "raidinfo") {
                gameHandler.requestRaidInfo();
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "startattack") {
                if (gameHandler.hasTarget()) {
                    gameHandler.startAutoAttack(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You have no target.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "stopattack") {
                gameHandler.stopAutoAttack();
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "stopcasting") {
                gameHandler.stopCasting();
                chatInputBuffer[0] = '\0';
                return;
            }

            // Targeting commands
            if (cmdLower == "cleartarget") {
                gameHandler.clearTarget();
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "targetenemy") {
                gameHandler.targetEnemy(false);
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "targetfriend") {
                gameHandler.targetFriend(false);
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "targetlasttarget" || cmdLower == "targetlast") {
                gameHandler.targetLastTarget();
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastenemy") {
                gameHandler.targetEnemy(true);  // Reverse direction
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "targetlastfriend") {
                gameHandler.targetFriend(true);  // Reverse direction
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "focus") {
                if (gameHandler.hasTarget()) {
                    gameHandler.setFocus(gameHandler.getTargetGuid());
                } else {
                    game::MessageChatData msg;
                    msg.type = game::ChatType::SYSTEM;
                    msg.language = game::ChatLanguage::UNIVERSAL;
                    msg.message = "You must target a unit to set as focus.";
                    gameHandler.addLocalChatMessage(msg);
                }
                chatInputBuffer[0] = '\0';
                return;
            }

            if (cmdLower == "clearfocus") {
                gameHandler.clearFocus();
                chatInputBuffer[0] = '\0';
                return;
            }

            // /unstuck command — resets player position to floor height
            if (cmdLower == "unstuck") {
                gameHandler.unstuck();
                chatInputBuffer[0] = '\0';
                return;
            }
            // /unstuckgy command — move to nearest graveyard
            if (cmdLower == "unstuckgy") {
                gameHandler.unstuckGy();
                chatInputBuffer[0] = '\0';
                return;
            }
            // /unstuckhearth command — teleport to hearthstone bind point
            if (cmdLower == "unstuckhearth") {
                gameHandler.unstuckHearth();
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
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
                    chatInputBuffer[0] = '\0';
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
                chatInputBuffer[0] = '\0';
                return;
            } else if (cmdLower == "leave") {
                // /leave ChannelName
                if (spacePos != std::string::npos) {
                    std::string channelName = command.substr(spacePos + 1);
                    gameHandler.leaveChannel(channelName);
                }
                chatInputBuffer[0] = '\0';
                return;
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
                    chatInputBuffer[0] = '\0';
                    return;
                } else {
                    chatInputBuffer[0] = '\0';
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
                        strncpy(whisperTargetBuffer, target.c_str(), sizeof(whisperTargetBuffer) - 1);
                        whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                    } else {
                        // /w PlayerName — switch to whisper mode with target set
                        strncpy(whisperTargetBuffer, rest.c_str(), sizeof(whisperTargetBuffer) - 1);
                        whisperTargetBuffer[sizeof(whisperTargetBuffer) - 1] = '\0';
                        message = "";
                        isChannelCommand = true;
                    }
                } else {
                    // Just "/w" — switch to whisper mode
                    message = "";
                    isChannelCommand = true;
                }
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
                    auto* renderer = core::Application::getInstance().getRenderer();
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

                    chatInputBuffer[0] = '\0';
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
                switch (selectedChatType) {
                    case 0: type = game::ChatType::SAY; break;
                    case 1: type = game::ChatType::YELL; break;
                    case 2: type = game::ChatType::PARTY; break;
                    case 3: type = game::ChatType::GUILD; break;
                    case 4: type = game::ChatType::WHISPER; target = whisperTargetBuffer; break;
                    case 5: type = game::ChatType::RAID; break;
                    case 6: type = game::ChatType::OFFICER; break;
                    case 7: type = game::ChatType::BATTLEGROUND; break;
                    case 8: type = game::ChatType::RAID_WARNING; break;
                    case 9: type = game::ChatType::PARTY; break; // INSTANCE uses PARTY
                    default: type = game::ChatType::SAY; break;
                }
            }
        } else {
            // No slash command, use the selected chat type from dropdown
            switch (selectedChatType) {
                case 0: type = game::ChatType::SAY; break;
                case 1: type = game::ChatType::YELL; break;
                case 2: type = game::ChatType::PARTY; break;
                case 3: type = game::ChatType::GUILD; break;
                case 4: type = game::ChatType::WHISPER; target = whisperTargetBuffer; break;
                case 5: type = game::ChatType::RAID; break;
                case 6: type = game::ChatType::OFFICER; break;
                case 7: type = game::ChatType::BATTLEGROUND; break;
                case 8: type = game::ChatType::RAID_WARNING; break;
                case 9: type = game::ChatType::PARTY; break; // INSTANCE uses PARTY
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
                chatInputBuffer[0] = '\0';
                return;
            }

            gameHandler.sendChatMessage(game::ChatType::SAY, cmd, "");
            msg.message = "PortBot executed: " + cmd;
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer[0] = '\0';
            return;
        }

        // Validate whisper has a target
        if (type == game::ChatType::WHISPER && target.empty()) {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "You must specify a player name for whisper.";
            gameHandler.addLocalChatMessage(msg);
            chatInputBuffer[0] = '\0';
            return;
        }

        // Don't send empty messages — but switch chat type if a channel shortcut was used
        if (!message.empty()) {
            gameHandler.sendChatMessage(type, message, target);
        }

        // Switch chat type dropdown when channel shortcut used (with or without message)
        if (switchChatType >= 0) {
            selectedChatType = switchChatType;
        }

        // Clear input
        chatInputBuffer[0] = '\0';
    }
}

const char* GameScreen::getChatTypeName(game::ChatType type) const {
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

ImVec4 GameScreen::getChatTypeColor(game::ChatType type) const {
    switch (type) {
        case game::ChatType::SAY:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White
        case game::ChatType::YELL:
            return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // Red
        case game::ChatType::EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::TEXT_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange
        case game::ChatType::PARTY:
            return ImVec4(0.5f, 0.5f, 1.0f, 1.0f);  // Light blue
        case game::ChatType::GUILD:
            return ImVec4(0.3f, 1.0f, 0.3f, 1.0f);  // Green
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
            return ImVec4(1.0f, 1.0f, 0.3f, 1.0f);  // Yellow
        case game::ChatType::MONSTER_SAY:
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // White (same as SAY)
        case game::ChatType::MONSTER_YELL:
            return ImVec4(1.0f, 0.3f, 0.3f, 1.0f);  // Red (same as YELL)
        case game::ChatType::MONSTER_EMOTE:
            return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);  // Orange (same as EMOTE)
        case game::ChatType::CHANNEL:
            return ImVec4(1.0f, 0.7f, 0.7f, 1.0f);  // Light pink
        case game::ChatType::ACHIEVEMENT:
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Bright yellow
        default:
            return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray
    }
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
    static const char* componentDirs[] = {
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
    // Texture component region fields (8 regions: ArmUpper..Foot)
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

    glm::vec3 playerPos = renderer->getCharacterPosition();
    auto* window = app.getWindow();
    int screenW = window ? window->getWidth() : 1280;
    int screenH = window ? window->getHeight() : 720;
    wm->render(playerPos, screenW, screenH);
}

// ============================================================
// Action Bar (Phase 3)
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
            // Try expansion layout first
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
            // If the DBC has WotLK-range field count (≥200 fields), it's the binary
            // WotLK Spell.dbc (CSV fallback). Use WotLK layout regardless of expansion,
            // since Turtle/Classic CSV files are garbled and fall back to WotLK binary.
            if (fieldCount >= 200) {
                tryLoadIcons(0, 133); // WotLK IconID field
            } else if (spellL) {
                tryLoadIcons((*spellL)["ID"], (*spellL)["IconID"]);
            }
            // Fallback to WotLK field 133 if expansion layout yielded nothing
            if (spellIconIds_.empty() && fieldCount > 133) {
                tryLoadIcons(0, 133);
            }
        }
    }

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
    auto* window = core::Application::getInstance().getWindow();
    auto* vkCtx = window ? window->getVkContext() : nullptr;
    if (!vkCtx) {
        spellIconCache_[spellId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    VkDescriptorSet ds = vkCtx->uploadImGuiTexture(image.data.data(), image.width, image.height);
    spellIconCache_[spellId] = ds;
    return ds;
}

void GameScreen::renderActionBar(game::GameHandler& gameHandler) {
    // Use ImGui's display size — always in sync with the current swap-chain/frame,
    // whereas window->getWidth/Height() can lag by one frame on resize events.
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;
    auto* assetMgr = core::Application::getInstance().getAssetManager();

    float slotSize = 48.0f;
    float spacing = 4.0f;
    float padding = 8.0f;
    float barW = 12 * slotSize + 11 * spacing + padding * 2;
    float barH = slotSize + 24.0f;
    float barX = (screenW - barW) / 2.0f;
    float barY = screenH - barH;

    ImGui::SetNextWindowPos(ImVec2(barX, barY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(barW, barH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.9f));

    // Per-slot rendering lambda — shared by both action bars
    const auto& bar = gameHandler.getActionBar();
    static const char* keyLabels1[] = {"1","2","3","4","5","6","7","8","9","0","-","="};
    // "⇧N" labels for bar 2 (UTF-8: E2 87 A7 = U+21E7 UPWARDS WHITE ARROW)
    static const char* keyLabels2[] = {
        "\xe2\x87\xa7" "1", "\xe2\x87\xa7" "2", "\xe2\x87\xa7" "3",
        "\xe2\x87\xa7" "4", "\xe2\x87\xa7" "5", "\xe2\x87\xa7" "6",
        "\xe2\x87\xa7" "7", "\xe2\x87\xa7" "8", "\xe2\x87\xa7" "9",
        "\xe2\x87\xa7" "0", "\xe2\x87\xa7" "-", "\xe2\x87\xa7" "="
    };

    auto renderBarSlot = [&](int absSlot, const char* keyLabel) {
        ImGui::BeginGroup();
        ImGui::PushID(absSlot);

        const auto& slot = bar[absSlot];
        bool onCooldown = !slot.isReady();

        auto getSpellName = [&](uint32_t spellId) -> std::string {
            std::string name = spellbookScreen.lookupSpellName(spellId, assetMgr);
            if (!name.empty()) return name;
            return "Spell #" + std::to_string(spellId);
        };

        // Try to get icon texture for this slot
        VkDescriptorSet iconTex = VK_NULL_HANDLE;
        const game::ItemDef* barItemDef = nullptr;
        uint32_t itemDisplayInfoId = 0;
        std::string itemNameFromQuery;
        if (slot.type == game::ActionBarSlot::SPELL && slot.id != 0) {
            iconTex = getSpellIcon(slot.id, assetMgr);
        } else if (slot.type == game::ActionBarSlot::ITEM && slot.id != 0) {
            auto& inv = gameHandler.getInventory();
            for (int bi = 0; bi < inv.getBackpackSize(); bi++) {
                const auto& bs = inv.getBackpackSlot(bi);
                if (!bs.empty() && bs.item.itemId == slot.id) { barItemDef = &bs.item; break; }
            }
            if (!barItemDef) {
                for (int ei = 0; ei < game::Inventory::NUM_EQUIP_SLOTS; ei++) {
                    const auto& es = inv.getEquipSlot(static_cast<game::EquipSlot>(ei));
                    if (!es.empty() && es.item.itemId == slot.id) { barItemDef = &es.item; break; }
                }
            }
            if (!barItemDef) {
                for (int bag = 0; bag < game::Inventory::NUM_BAG_SLOTS && !barItemDef; bag++) {
                    for (int si = 0; si < inv.getBagSize(bag); si++) {
                        const auto& bs = inv.getBagSlot(bag, si);
                        if (!bs.empty() && bs.item.itemId == slot.id) { barItemDef = &bs.item; break; }
                    }
                }
            }
            if (barItemDef && barItemDef->displayInfoId != 0)
                itemDisplayInfoId = barItemDef->displayInfoId;
            if (itemDisplayInfoId == 0) {
                if (auto* info = gameHandler.getItemInfo(slot.id)) {
                    itemDisplayInfoId = info->displayInfoId;
                    if (itemNameFromQuery.empty() && !info->name.empty())
                        itemNameFromQuery = info->name;
                }
            }
            if (itemDisplayInfoId != 0)
                iconTex = inventoryScreen.getItemIcon(itemDisplayInfoId);
        }

        bool clicked = false;
        if (iconTex) {
            ImVec4 tintColor(1, 1, 1, 1);
            ImVec4 bgColor(0.1f, 0.1f, 0.1f, 0.9f);
            if (onCooldown) { tintColor = ImVec4(0.4f, 0.4f, 0.4f, 0.8f); }
            clicked = ImGui::ImageButton("##icon",
                (ImTextureID)(uintptr_t)iconTex,
                ImVec2(slotSize, slotSize),
                ImVec2(0, 0), ImVec2(1, 1),
                bgColor, tintColor);
        } else {
            if (onCooldown)         ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
            else if (slot.isEmpty())ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            else                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.5f, 0.9f));

            char label[32];
            if (slot.type == game::ActionBarSlot::SPELL) {
                std::string spellName = getSpellName(slot.id);
                if (spellName.size() > 6) spellName = spellName.substr(0, 6);
                snprintf(label, sizeof(label), "%s", spellName.c_str());
            } else if (slot.type == game::ActionBarSlot::ITEM && barItemDef) {
                std::string itemName = barItemDef->name;
                if (itemName.size() > 6) itemName = itemName.substr(0, 6);
                snprintf(label, sizeof(label), "%s", itemName.c_str());
            } else if (slot.type == game::ActionBarSlot::ITEM) {
                snprintf(label, sizeof(label), "Item");
            } else if (slot.type == game::ActionBarSlot::MACRO) {
                snprintf(label, sizeof(label), "Macro");
            } else {
                snprintf(label, sizeof(label), "--");
            }
            clicked = ImGui::Button(label, ImVec2(slotSize, slotSize));
            ImGui::PopStyleColor();
        }

        bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        bool hoveredOnRelease = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                ImGui::IsMouseReleased(ImGuiMouseButton_Left);

        if (hoveredOnRelease && spellbookScreen.isDraggingSpell()) {
            gameHandler.setActionBarSlot(absSlot, game::ActionBarSlot::SPELL,
                spellbookScreen.getDragSpellId());
            spellbookScreen.consumeDragSpell();
        } else if (hoveredOnRelease && inventoryScreen.isHoldingItem()) {
            const auto& held = inventoryScreen.getHeldItem();
            gameHandler.setActionBarSlot(absSlot, game::ActionBarSlot::ITEM, held.itemId);
            inventoryScreen.returnHeldItem(gameHandler.getInventory());
        } else if (clicked && actionBarDragSlot_ >= 0) {
            if (absSlot != actionBarDragSlot_) {
                const auto& dragSrc = bar[actionBarDragSlot_];
                gameHandler.setActionBarSlot(actionBarDragSlot_, slot.type, slot.id);
                gameHandler.setActionBarSlot(absSlot, dragSrc.type, dragSrc.id);
            }
            actionBarDragSlot_ = -1;
            actionBarDragIcon_ = 0;
        } else if (clicked && !slot.isEmpty()) {
            if (slot.type == game::ActionBarSlot::SPELL && slot.isReady()) {
                uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                gameHandler.castSpell(slot.id, target);
            } else if (slot.type == game::ActionBarSlot::ITEM && slot.id != 0) {
                gameHandler.useItemById(slot.id);
            }
        } else if (rightClicked && !slot.isEmpty()) {
            actionBarDragSlot_ = absSlot;
            actionBarDragIcon_ = iconTex;
        }

        // Tooltip
        if (ImGui::IsItemHovered() && !slot.isEmpty() && slot.id != 0) {
            ImGui::BeginTooltip();
            if (slot.type == game::ActionBarSlot::SPELL) {
                ImGui::Text("%s", getSpellName(slot.id).c_str());
                if (slot.id == 8690) {
                    uint32_t mapId = 0; glm::vec3 pos;
                    if (gameHandler.getHomeBind(mapId, pos)) {
                        const char* mapName = "Unknown";
                        switch (mapId) {
                            case 0: mapName = "Eastern Kingdoms"; break;
                            case 1: mapName = "Kalimdor"; break;
                            case 530: mapName = "Outland"; break;
                            case 571: mapName = "Northrend"; break;
                        }
                        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "Home: %s", mapName);
                    }
                    ImGui::TextDisabled("Use: Teleport home");
                }
            } else if (slot.type == game::ActionBarSlot::ITEM) {
                if (barItemDef && !barItemDef->name.empty())
                    ImGui::Text("%s", barItemDef->name.c_str());
                else if (!itemNameFromQuery.empty())
                    ImGui::Text("%s", itemNameFromQuery.c_str());
                else
                    ImGui::Text("Item #%u", slot.id);
            }
            if (onCooldown) {
                float cd = slot.cooldownRemaining;
                if (cd >= 60.0f)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                        "Cooldown: %d min %d sec", (int)cd/60, (int)cd%60);
                else
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Cooldown: %.1f sec", cd);
            }
            ImGui::EndTooltip();
        }

        // Cooldown overlay: WoW-style clock-sweep + time text
        if (onCooldown) {
            ImVec2 btnMin = ImGui::GetItemRectMin();
            ImVec2 btnMax = ImGui::GetItemRectMax();
            float cx = (btnMin.x + btnMax.x) * 0.5f;
            float cy = (btnMin.y + btnMax.y) * 0.5f;
            float r  = (btnMax.x - btnMin.x) * 0.5f;
            auto* dl = ImGui::GetWindowDrawList();

            float total       = (slot.cooldownTotal > 0.0f) ? slot.cooldownTotal : 1.0f;
            float elapsed     = total - slot.cooldownRemaining;
            float elapsedFrac = std::min(1.0f, std::max(0.0f, elapsed / total));
            if (elapsedFrac > 0.005f) {
                constexpr int N_SEGS = 32;
                float startAngle = -IM_PI * 0.5f;
                float endAngle   = startAngle + elapsedFrac * 2.0f * IM_PI;
                float fanR       = r * 1.5f;
                ImVec2 pts[N_SEGS + 2];
                pts[0] = ImVec2(cx, cy);
                for (int s = 0; s <= N_SEGS; ++s) {
                    float a = startAngle + (endAngle - startAngle) * s / static_cast<float>(N_SEGS);
                    pts[s + 1] = ImVec2(cx + std::cos(a) * fanR, cy + std::sin(a) * fanR);
                }
                dl->AddConvexPolyFilled(pts, N_SEGS + 2, IM_COL32(0, 0, 0, 170));
            }

            char cdText[16];
            float cd = slot.cooldownRemaining;
            if (cd >= 60.0f) snprintf(cdText, sizeof(cdText), "%dm", (int)cd / 60);
            else             snprintf(cdText, sizeof(cdText), "%.0f", cd);
            ImVec2 textSize = ImGui::CalcTextSize(cdText);
            float tx = cx - textSize.x * 0.5f;
            float ty = cy - textSize.y * 0.5f;
            dl->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 220), cdText);
            dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), cdText);
        }

        // Key label below
        ImGui::TextDisabled("%s", keyLabel);

        ImGui::PopID();
        ImGui::EndGroup();
    };

    // Bar 2 (slots 12-23) — only show if at least one slot is populated
    if (pendingShowActionBar2) {
        bool bar2HasContent = false;
        for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i)
            if (!bar[game::GameHandler::SLOTS_PER_BAR + i].isEmpty()) { bar2HasContent = true; break; }

        float bar2X = barX + pendingActionBar2OffsetX;
        float bar2Y = barY - barH - 2.0f + pendingActionBar2OffsetY;
        ImGui::SetNextWindowPos(ImVec2(bar2X, bar2Y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(barW, barH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            bar2HasContent ? ImVec4(0.05f, 0.05f, 0.05f, 0.85f) : ImVec4(0.05f, 0.05f, 0.05f, 0.4f));
        if (ImGui::Begin("##ActionBar2", nullptr, flags)) {
            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                if (i > 0) ImGui::SameLine(0, spacing);
                renderBarSlot(game::GameHandler::SLOTS_PER_BAR + i, keyLabels2[i]);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }

    // Bar 1 (slots 0-11)
    if (ImGui::Begin("##ActionBar", nullptr, flags)) {
        for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
            if (i > 0) ImGui::SameLine(0, spacing);
            renderBarSlot(i, keyLabels1[i]);
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);

    // Right side vertical bar (bar 3, slots 24-35)
    if (pendingShowRightBar) {
        bool bar3HasContent = false;
        for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i)
            if (!bar[game::GameHandler::SLOTS_PER_BAR * 2 + i].isEmpty()) { bar3HasContent = true; break; }

        float sideBarW = slotSize + padding * 2;
        float sideBarH = game::GameHandler::SLOTS_PER_BAR * slotSize + (game::GameHandler::SLOTS_PER_BAR - 1) * spacing + padding * 2;
        float sideBarX = screenW - sideBarW - 4.0f;
        float sideBarY = (screenH - sideBarH) / 2.0f + pendingRightBarOffsetY;

        ImGui::SetNextWindowPos(ImVec2(sideBarX, sideBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(sideBarW, sideBarH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            bar3HasContent ? ImVec4(0.05f, 0.05f, 0.05f, 0.85f) : ImVec4(0.05f, 0.05f, 0.05f, 0.4f));
        if (ImGui::Begin("##ActionBarRight", nullptr, flags)) {
            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                renderBarSlot(game::GameHandler::SLOTS_PER_BAR * 2 + i, "");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }

    // Left side vertical bar (bar 4, slots 36-47)
    if (pendingShowLeftBar) {
        bool bar4HasContent = false;
        for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i)
            if (!bar[game::GameHandler::SLOTS_PER_BAR * 3 + i].isEmpty()) { bar4HasContent = true; break; }

        float sideBarW = slotSize + padding * 2;
        float sideBarH = game::GameHandler::SLOTS_PER_BAR * slotSize + (game::GameHandler::SLOTS_PER_BAR - 1) * spacing + padding * 2;
        float sideBarX = 4.0f;
        float sideBarY = (screenH - sideBarH) / 2.0f + pendingLeftBarOffsetY;

        ImGui::SetNextWindowPos(ImVec2(sideBarX, sideBarY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(sideBarW, sideBarH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,
            bar4HasContent ? ImVec4(0.05f, 0.05f, 0.05f, 0.85f) : ImVec4(0.05f, 0.05f, 0.05f, 0.4f));
        if (ImGui::Begin("##ActionBarLeft", nullptr, flags)) {
            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                renderBarSlot(game::GameHandler::SLOTS_PER_BAR * 3 + i, "");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }

    // Handle action bar drag: render icon at cursor and detect drop outside
    if (actionBarDragSlot_ >= 0) {
        ImVec2 mousePos = ImGui::GetMousePos();

        // Draw dragged icon at cursor
        if (actionBarDragIcon_) {
            ImGui::GetForegroundDrawList()->AddImage(
                (ImTextureID)(uintptr_t)actionBarDragIcon_,
                ImVec2(mousePos.x - 20, mousePos.y - 20),
                ImVec2(mousePos.x + 20, mousePos.y + 20));
        } else {
            ImGui::GetForegroundDrawList()->AddRectFilled(
                ImVec2(mousePos.x - 20, mousePos.y - 20),
                ImVec2(mousePos.x + 20, mousePos.y + 20),
                IM_COL32(80, 80, 120, 180));
        }

        // On right mouse release, check if outside the action bar area
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            bool insideBar = (mousePos.x >= barX && mousePos.x <= barX + barW &&
                              mousePos.y >= barY && mousePos.y <= barY + barH);
            if (!insideBar) {
                // Dropped outside - clear the slot
                gameHandler.setActionBarSlot(actionBarDragSlot_, game::ActionBarSlot::EMPTY, 0);
            }
            actionBarDragSlot_ = -1;
            actionBarDragIcon_ = 0;
        }
    }
}

// ============================================================
// Bag Bar
// ============================================================

void GameScreen::renderBagBar(game::GameHandler& gameHandler) {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;
    auto* assetMgr = core::Application::getInstance().getAssetManager();

    float slotSize = 42.0f;
    float spacing = 4.0f;
    float padding = 6.0f;

    // 5 slots: backpack + 4 bags
    float barW = 5 * slotSize + 4 * spacing + padding * 2;
    float barH = slotSize + padding * 2;

    // Position in bottom right corner
    float barX = screenW - barW - 10.0f;
    float barY = screenH - barH - 10.0f;

    ImGui::SetNextWindowPos(ImVec2(barX, barY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(barW, barH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.9f));

    if (ImGui::Begin("##BagBar", nullptr, flags)) {
        auto& inv = gameHandler.getInventory();

        // Load backpack icon if needed
        if (!backpackIconTexture_ && assetMgr && assetMgr->isInitialized()) {
            auto blpData = assetMgr->readFile("Interface\\Buttons\\Button-Backpack-Up.blp");
            if (!blpData.empty()) {
                auto image = pipeline::BLPLoader::load(blpData);
                if (image.isValid()) {
                    auto* w = core::Application::getInstance().getWindow();
                    auto* vkCtx = w ? w->getVkContext() : nullptr;
                    if (vkCtx)
                        backpackIconTexture_ = vkCtx->uploadImGuiTexture(image.data.data(), image.width, image.height);
                }
            }
        }

        // Track bag slot screen rects for drop detection
        ImVec2 bagSlotMins[4], bagSlotMaxs[4];

        // Slots 1-4: Bag slots (leftmost)
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ImGui::SameLine(0, spacing);
            ImGui::PushID(i + 1);

            game::EquipSlot bagSlot = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + i);
            const auto& bagItem = inv.getEquipSlot(bagSlot);

            VkDescriptorSet bagIcon = VK_NULL_HANDLE;
            if (!bagItem.empty() && bagItem.item.displayInfoId != 0) {
                bagIcon = inventoryScreen.getItemIcon(bagItem.item.displayInfoId);
            }
            // Render the slot as an invisible button so we control all interaction
            ImVec2 cpos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##bagSlot", ImVec2(slotSize, slotSize));
            bagSlotMins[i] = cpos;
            bagSlotMaxs[i] = ImVec2(cpos.x + slotSize, cpos.y + slotSize);

            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Draw background + icon
            if (bagIcon) {
                dl->AddRectFilled(bagSlotMins[i], bagSlotMaxs[i], IM_COL32(25, 25, 25, 230));
                dl->AddImage((ImTextureID)(uintptr_t)bagIcon, bagSlotMins[i], bagSlotMaxs[i]);
            } else {
                dl->AddRectFilled(bagSlotMins[i], bagSlotMaxs[i], IM_COL32(38, 38, 38, 204));
            }

            // Hover highlight
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (hovered && bagBarPickedSlot_ < 0) {
                dl->AddRect(bagSlotMins[i], bagSlotMaxs[i], IM_COL32(255, 255, 255, 100));
            }

            // Track which slot was pressed for drag detection
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && bagBarPickedSlot_ < 0 && bagIcon) {
                bagBarDragSource_ = i;
            }

            // Click toggles bag open/close (handled in mouse release section below)

            // Dim the slot being dragged
            if (bagBarPickedSlot_ == i) {
                dl->AddRectFilled(bagSlotMins[i], bagSlotMaxs[i], IM_COL32(0, 0, 0, 150));
            }

            // Tooltip
            if (hovered && bagBarPickedSlot_ < 0) {
                if (bagIcon)
                    ImGui::SetTooltip("%s", bagItem.item.name.c_str());
                else
                    ImGui::SetTooltip("Empty Bag Slot");
            }

            // Open bag indicator
            if (inventoryScreen.isSeparateBags() && inventoryScreen.isBagOpen(i)) {
                dl->AddRect(bagSlotMins[i], bagSlotMaxs[i], IM_COL32(255, 255, 255, 255), 3.0f, 0, 2.0f);
            }

            // Accept dragged item from inventory
            if (hovered && inventoryScreen.isHoldingItem()) {
                const auto& heldItem = inventoryScreen.getHeldItem();
                if ((heldItem.inventoryType == 18 || heldItem.bagSlots > 0) &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    auto& inventory = gameHandler.getInventory();
                    inventoryScreen.dropHeldItemToEquipSlot(inventory, bagSlot);
                }
            }

            ImGui::PopID();
        }

        // Drag lifecycle: press on a slot sets bagBarDragSource_,
        // dragging 3+ pixels promotes to bagBarPickedSlot_ (visual drag),
        // releasing completes swap or click
        if (bagBarDragSource_ >= 0) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) && bagBarPickedSlot_ < 0) {
                // If an inventory window is open, hand off drag to inventory held-item
                // so the bag can be dropped into backpack/bag slots.
                if (inventoryScreen.isOpen() || inventoryScreen.isCharacterOpen()) {
                    auto equip = static_cast<game::EquipSlot>(
                        static_cast<int>(game::EquipSlot::BAG1) + bagBarDragSource_);
                    if (inventoryScreen.beginPickupFromEquipSlot(inv, equip)) {
                        bagBarDragSource_ = -1;
                    } else {
                        bagBarPickedSlot_ = bagBarDragSource_;
                    }
                } else {
                    // Mouse moved enough — start visual drag
                    bagBarPickedSlot_ = bagBarDragSource_;
                }
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (bagBarPickedSlot_ >= 0) {
                    // Was dragging — check for drop target
                    ImVec2 mousePos = ImGui::GetIO().MousePos;
                    int dropTarget = -1;
                    for (int j = 0; j < 4; ++j) {
                        if (j == bagBarPickedSlot_) continue;
                        if (mousePos.x >= bagSlotMins[j].x && mousePos.x <= bagSlotMaxs[j].x &&
                            mousePos.y >= bagSlotMins[j].y && mousePos.y <= bagSlotMaxs[j].y) {
                            dropTarget = j;
                            break;
                        }
                    }
                    if (dropTarget >= 0) {
                        gameHandler.swapBagSlots(bagBarPickedSlot_, dropTarget);
                    }
                    bagBarPickedSlot_ = -1;
                } else {
                    // Was just a click (no drag) — toggle bag
                    int slot = bagBarDragSource_;
                    auto equip = static_cast<game::EquipSlot>(static_cast<int>(game::EquipSlot::BAG1) + slot);
                    if (!inv.getEquipSlot(equip).empty()) {
                        if (inventoryScreen.isSeparateBags())
                            inventoryScreen.toggleBag(slot);
                        else
                            inventoryScreen.toggle();
                    }
                }
                bagBarDragSource_ = -1;
            }
        }

        // Backpack (rightmost slot)
        ImGui::SameLine(0, spacing);
        ImGui::PushID(0);
        if (backpackIconTexture_) {
            if (ImGui::ImageButton("##backpack", (ImTextureID)(uintptr_t)backpackIconTexture_,
                                   ImVec2(slotSize, slotSize),
                                   ImVec2(0, 0), ImVec2(1, 1),
                                   ImVec4(0.1f, 0.1f, 0.1f, 0.9f),
                                   ImVec4(1, 1, 1, 1))) {
                if (inventoryScreen.isSeparateBags())
                    inventoryScreen.toggleBackpack();
                else
                    inventoryScreen.toggle();
            }
        } else {
            if (ImGui::Button("B", ImVec2(slotSize, slotSize))) {
                if (inventoryScreen.isSeparateBags())
                    inventoryScreen.toggleBackpack();
                else
                    inventoryScreen.toggle();
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Backpack");
        }
        if (inventoryScreen.isSeparateBags() &&
            inventoryScreen.isBackpackOpen()) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 r0 = ImGui::GetItemRectMin();
            ImVec2 r1 = ImGui::GetItemRectMax();
            dl->AddRect(r0, r1, IM_COL32(255, 255, 255, 255), 3.0f, 0, 2.0f);
        }
        ImGui::PopID();

    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);

    // Draw dragged bag icon following cursor
    if (bagBarPickedSlot_ >= 0) {
        auto& inv2 = gameHandler.getInventory();
        auto pickedEquip = static_cast<game::EquipSlot>(
            static_cast<int>(game::EquipSlot::BAG1) + bagBarPickedSlot_);
        const auto& pickedItem = inv2.getEquipSlot(pickedEquip);
        VkDescriptorSet pickedIcon = VK_NULL_HANDLE;
        if (!pickedItem.empty() && pickedItem.item.displayInfoId != 0) {
            pickedIcon = inventoryScreen.getItemIcon(pickedItem.item.displayInfoId);
        }
        if (pickedIcon) {
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            float sz = 40.0f;
            ImVec2 p0(mousePos.x - sz * 0.5f, mousePos.y - sz * 0.5f);
            ImVec2 p1(mousePos.x + sz * 0.5f, mousePos.y + sz * 0.5f);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddImage((ImTextureID)(uintptr_t)pickedIcon, p0, p1);
            fg->AddRect(p0, p1, IM_COL32(200, 200, 200, 255), 0.0f, 0, 2.0f);
        }
    }
}

// ============================================================
// XP Bar
// ============================================================

void GameScreen::renderXpBar(game::GameHandler& gameHandler) {
    uint32_t nextLevelXp = gameHandler.getPlayerNextLevelXp();
    if (nextLevelXp == 0) return; // No XP data yet (level 80 or not initialized)

    uint32_t currentXp  = gameHandler.getPlayerXp();
    uint32_t restedXp   = gameHandler.getPlayerRestedXp();
    bool     isResting  = gameHandler.isPlayerResting();
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;
    auto* window = core::Application::getInstance().getWindow();
    (void)window;  // Not used for positioning; kept for AssetManager if needed

    // Position just above both action bars (bar1 at screenH-barH, bar2 above that)
    float slotSize = 48.0f;
    float spacing = 4.0f;
    float padding = 8.0f;
    float barW = 12 * slotSize + 11 * spacing + padding * 2;
    float barH = slotSize + 24.0f;

    float xpBarH = 20.0f;
    float xpBarW = barW;
    float xpBarX = (screenW - xpBarW) / 2.0f;
    // XP bar sits just above whichever bar is topmost.
    // bar1 top edge: screenH - barH
    // bar2 top edge (when visible): bar1 top - barH - 2 + bar2 vertical offset
    float bar1TopY = screenH - barH;
    float xpBarY;
    if (pendingShowActionBar2) {
        float bar2TopY = bar1TopY - barH - 2.0f + pendingActionBar2OffsetY;
        xpBarY = bar2TopY - xpBarH - 2.0f;
    } else {
        xpBarY = bar1TopY - xpBarH - 2.0f;
    }

    ImGui::SetNextWindowPos(ImVec2(xpBarX, xpBarY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(xpBarW, xpBarH + 4.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.05f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));

    if (ImGui::Begin("##XpBar", nullptr, flags)) {
        float pct = static_cast<float>(currentXp) / static_cast<float>(nextLevelXp);
        if (pct > 1.0f) pct = 1.0f;

        // Custom segmented XP bar (20 bubbles)
        ImVec2 barMin = ImGui::GetCursorScreenPos();
        ImVec2 barSize = ImVec2(ImGui::GetContentRegionAvail().x, xpBarH - 4.0f);
        ImVec2 barMax = ImVec2(barMin.x + barSize.x, barMin.y + barSize.y);
        auto* drawList = ImGui::GetWindowDrawList();

        ImU32 bg      = IM_COL32(15, 15, 20, 220);
        ImU32 fg      = IM_COL32(148, 51, 238, 255);
        ImU32 fgRest  = IM_COL32(200, 170, 255, 220); // lighter purple for rested portion
        ImU32 seg     = IM_COL32(35, 35, 45, 255);
        drawList->AddRectFilled(barMin, barMax, bg, 2.0f);
        drawList->AddRect(barMin, barMax, IM_COL32(80, 80, 90, 220), 2.0f);

        float fillW = barSize.x * pct;
        if (fillW > 0.0f) {
            drawList->AddRectFilled(barMin, ImVec2(barMin.x + fillW, barMax.y), fg, 2.0f);
        }

        // Rested XP overlay: draw from current XP fill to (currentXp + restedXp) fill
        if (restedXp > 0) {
            float restedEndPct = std::min(1.0f, static_cast<float>(currentXp + restedXp)
                                                / static_cast<float>(nextLevelXp));
            float restedStartX = barMin.x + fillW;
            float restedEndX   = barMin.x + barSize.x * restedEndPct;
            if (restedEndX > restedStartX) {
                drawList->AddRectFilled(ImVec2(restedStartX, barMin.y),
                                        ImVec2(restedEndX,   barMax.y),
                                        fgRest, 2.0f);
            }
        }

        const int segments = 20;
        float segW = barSize.x / static_cast<float>(segments);
        for (int i = 1; i < segments; ++i) {
            float x = barMin.x + segW * i;
            drawList->AddLine(ImVec2(x, barMin.y + 1.0f), ImVec2(x, barMax.y - 1.0f), seg, 1.0f);
        }

        // Rest indicator "zzz" to the right of the bar when resting
        if (isResting) {
            const char* zzz = "zzz";
            ImVec2 zSize = ImGui::CalcTextSize(zzz);
            float zx = barMax.x - zSize.x - 4.0f;
            float zy = barMin.y + (barSize.y - zSize.y) * 0.5f;
            drawList->AddText(ImVec2(zx, zy), IM_COL32(180, 150, 255, 220), zzz);
        }

        char overlay[96];
        if (restedXp > 0) {
            snprintf(overlay, sizeof(overlay), "%u / %u XP  (+%u rested)", currentXp, nextLevelXp, restedXp);
        } else {
            snprintf(overlay, sizeof(overlay), "%u / %u XP", currentXp, nextLevelXp);
        }
        ImVec2 textSize = ImGui::CalcTextSize(overlay);
        float tx = barMin.x + (barSize.x - textSize.x) * 0.5f;
        float ty = barMin.y + (barSize.y - textSize.y) * 0.5f;
        drawList->AddText(ImVec2(tx, ty), IM_COL32(230, 230, 230, 255), overlay);

        ImGui::Dummy(barSize);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ============================================================
// Cast Bar (Phase 3)
// ============================================================

void GameScreen::renderCastBar(game::GameHandler& gameHandler) {
    if (!gameHandler.isCasting()) return;

    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;

    float barW = 300.0f;
    float barX = (screenW - barW) / 2.0f;
    float barY = screenH - 120.0f;

    ImGui::SetNextWindowPos(ImVec2(barX, barY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(barW, 40), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.9f));

    if (ImGui::Begin("##CastBar", nullptr, flags)) {
        float progress = gameHandler.getCastProgress();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.6f, 0.2f, 1.0f));

        char overlay[64];
        uint32_t currentSpellId = gameHandler.getCurrentCastSpellId();
        if (gameHandler.getCurrentCastSpellId() == 0) {
            snprintf(overlay, sizeof(overlay), "Opening... (%.1fs)", gameHandler.getCastTimeRemaining());
        } else {
            const std::string& spellName = gameHandler.getSpellName(currentSpellId);
            if (!spellName.empty())
                snprintf(overlay, sizeof(overlay), "%s (%.1fs)", spellName.c_str(), gameHandler.getCastTimeRemaining());
            else
                snprintf(overlay, sizeof(overlay), "Casting... (%.1fs)", gameHandler.getCastTimeRemaining());
        }
        ImGui::ProgressBar(progress, ImVec2(-1, 20), overlay);
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ============================================================
// Mirror Timers (breath / fatigue / feign death)
// ============================================================

void GameScreen::renderMirrorTimers(game::GameHandler& gameHandler) {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float screenW = displaySize.x > 0.0f ? displaySize.x : 1280.0f;
    float screenH = displaySize.y > 0.0f ? displaySize.y : 720.0f;

    static const struct { const char* label; ImVec4 color; } kTimerInfo[3] = {
        { "Fatigue", ImVec4(0.8f, 0.4f, 0.1f, 1.0f) },
        { "Breath",  ImVec4(0.2f, 0.5f, 1.0f, 1.0f) },
        { "Feign",   ImVec4(0.6f, 0.6f, 0.6f, 1.0f) },
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
// Quest Objective Tracker (right-side HUD)
// ============================================================

void GameScreen::renderQuestObjectiveTracker(game::GameHandler& gameHandler) {
    const auto& questLog = gameHandler.getQuestLog();
    if (questLog.empty()) return;

    auto* window = core::Application::getInstance().getWindow();
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

    float x = screenW - TRACKER_W - RIGHT_MARGIN;
    float y = 200.0f;  // below minimap area

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(TRACKER_W, 0), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 2.0f));

    if (ImGui::Begin("##QuestTracker", nullptr, flags)) {
        for (int i = 0; i < static_cast<int>(toShow.size()); ++i) {
            const auto& q = *toShow[i];

            // Clickable quest title — opens quest log
            ImGui::PushID(q.questId);
            ImVec4 titleCol = q.complete ? ImVec4(1.0f, 0.84f, 0.0f, 1.0f)
                                         : ImVec4(1.0f, 1.0f, 0.85f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, titleCol);
            if (ImGui::Selectable(q.title.c_str(), false,
                                   ImGuiSelectableFlags_None, ImVec2(TRACKER_W - 12.0f, 0))) {
                questLogScreen.openAndSelectQuest(q.questId);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to open Quest Log");
            }
            ImGui::PopStyleColor();
            ImGui::PopID();

            // Objectives line (condensed)
            if (q.complete) {
                ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "  (Complete)");
            } else {
                // Kill counts
                for (const auto& [entry, progress] : q.killCounts) {
                    std::string creatureName = gameHandler.getCachedCreatureName(entry);
                    if (!creatureName.empty()) {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                           "  %s: %u/%u", creatureName.c_str(),
                                           progress.first, progress.second);
                    } else {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                           "  %u/%u", progress.first, progress.second);
                    }
                }
                // Item counts
                for (const auto& [itemId, count] : q.itemCounts) {
                    uint32_t required = 1;
                    auto reqIt = q.requiredItemCounts.find(itemId);
                    if (reqIt != q.requiredItemCounts.end()) required = reqIt->second;
                    const auto* info = gameHandler.getItemInfo(itemId);
                    const char* itemName = (info && !info->name.empty()) ? info->name.c_str() : nullptr;
                    if (itemName) {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                                           "  %s: %u/%u", itemName, count, required);
                    } else {
                        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
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
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ============================================================
// Floating Combat Text (Phase 2)
// ============================================================

void GameScreen::renderCombatText(game::GameHandler& gameHandler) {
    const auto& entries = gameHandler.getCombatText();
    if (entries.empty()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    // Render combat text entries overlaid on screen
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, 400));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##CombatText", nullptr, flags)) {
        // Incoming events (enemy attacks player) float near screen center (over the player).
        // Outgoing events (player attacks enemy) float on the right side (near the target).
        const float incomingX = screenW * 0.40f;
        const float outgoingX = screenW * 0.68f;

        int inIdx = 0, outIdx = 0;
        for (const auto& entry : entries) {
            float alpha = 1.0f - (entry.age / game::CombatTextEntry::LIFETIME);
            float yOffset = 200.0f - entry.age * 60.0f;
            const bool outgoing = entry.isPlayerSource;

            ImVec4 color;
            char text[64];
            switch (entry.type) {
                case game::CombatTextEntry::MELEE_DAMAGE:
                case game::CombatTextEntry::SPELL_DAMAGE:
                    snprintf(text, sizeof(text), "-%d", entry.amount);
                    color = outgoing ?
                        ImVec4(1.0f, 1.0f, 0.3f, alpha) :   // Outgoing = yellow
                        ImVec4(1.0f, 0.3f, 0.3f, alpha);     // Incoming = red
                    break;
                case game::CombatTextEntry::CRIT_DAMAGE:
                    snprintf(text, sizeof(text), "-%d!", entry.amount);
                    color = outgoing ?
                        ImVec4(1.0f, 0.8f, 0.0f, alpha) :   // Outgoing crit = bright yellow
                        ImVec4(1.0f, 0.5f, 0.0f, alpha);     // Incoming crit = orange
                    break;
                case game::CombatTextEntry::HEAL:
                    snprintf(text, sizeof(text), "+%d", entry.amount);
                    color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                    break;
                case game::CombatTextEntry::CRIT_HEAL:
                    snprintf(text, sizeof(text), "+%d!", entry.amount);
                    color = ImVec4(0.3f, 1.0f, 0.3f, alpha);
                    break;
                case game::CombatTextEntry::MISS:
                    snprintf(text, sizeof(text), "Miss");
                    color = ImVec4(0.7f, 0.7f, 0.7f, alpha);
                    break;
                case game::CombatTextEntry::DODGE:
                    // outgoing=true: enemy dodged player's attack
                    // outgoing=false: player dodged incoming attack
                    snprintf(text, sizeof(text), outgoing ? "Dodge" : "You Dodge");
                    color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                     : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                    break;
                case game::CombatTextEntry::PARRY:
                    snprintf(text, sizeof(text), outgoing ? "Parry" : "You Parry");
                    color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                     : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                    break;
                case game::CombatTextEntry::BLOCK:
                    snprintf(text, sizeof(text), outgoing ? "Block" : "You Block");
                    color = outgoing ? ImVec4(0.6f, 0.6f, 0.6f, alpha)
                                     : ImVec4(0.4f, 0.9f, 1.0f, alpha);
                    break;
                case game::CombatTextEntry::PERIODIC_DAMAGE:
                    snprintf(text, sizeof(text), "-%d", entry.amount);
                    color = outgoing ?
                        ImVec4(1.0f, 0.9f, 0.3f, alpha) :   // Outgoing DoT = pale yellow
                        ImVec4(1.0f, 0.4f, 0.4f, alpha);     // Incoming DoT = pale red
                    break;
                case game::CombatTextEntry::PERIODIC_HEAL:
                    snprintf(text, sizeof(text), "+%d", entry.amount);
                    color = ImVec4(0.4f, 1.0f, 0.5f, alpha);
                    break;
                case game::CombatTextEntry::ENVIRONMENTAL:
                    snprintf(text, sizeof(text), "-%d", entry.amount);
                    color = ImVec4(0.9f, 0.5f, 0.2f, alpha);  // Orange for environmental
                    break;
                case game::CombatTextEntry::ENERGIZE:
                    snprintf(text, sizeof(text), "+%d", entry.amount);
                    color = ImVec4(0.3f, 0.6f, 1.0f, alpha);  // Blue for mana/energy
                    break;
                case game::CombatTextEntry::XP_GAIN:
                    snprintf(text, sizeof(text), "+%d XP", entry.amount);
                    color = ImVec4(0.7f, 0.3f, 1.0f, alpha);  // Purple for XP
                    break;
                case game::CombatTextEntry::IMMUNE:
                    snprintf(text, sizeof(text), "Immune!");
                    color = ImVec4(0.9f, 0.9f, 0.9f, alpha);  // White for immune
                    break;
                default:
                    snprintf(text, sizeof(text), "%d", entry.amount);
                    color = ImVec4(1.0f, 1.0f, 1.0f, alpha);
                    break;
            }

            // Outgoing → right side (near target), incoming → center-left (near player)
            int& idx = outgoing ? outIdx : inIdx;
            float baseX = outgoing ? outgoingX : incomingX;
            float xOffset = baseX + (idx % 3 - 1) * 60.0f;
            ++idx;
            ImGui::SetCursorPos(ImVec2(xOffset, yOffset));
            ImGui::TextColored(color, "%s", text);
        }
    }
    ImGui::End();
}

// ============================================================
// Nameplates — world-space health bars projected to screen
// ============================================================

void GameScreen::renderNameplates(game::GameHandler& gameHandler) {
    if (gameHandler.getState() != game::WorldState::IN_WORLD) return;

    auto* appRenderer = core::Application::getInstance().getRenderer();
    if (!appRenderer) return;
    rendering::Camera* camera = appRenderer->getCamera();
    if (!camera) return;

    auto* window = core::Application::getInstance().getWindow();
    if (!window) return;
    const float screenW = static_cast<float>(window->getWidth());
    const float screenH = static_cast<float>(window->getHeight());

    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    const glm::vec3 camPos   = camera->getPosition();
    const uint64_t  playerGuid = gameHandler.getPlayerGuid();
    const uint64_t  targetGuid = gameHandler.getTargetGuid();

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    for (const auto& [guid, entityPtr] : gameHandler.getEntityManager().getEntities()) {
        if (!entityPtr || guid == playerGuid) continue;

        auto* unit = dynamic_cast<game::Unit*>(entityPtr.get());
        if (!unit || unit->getMaxHealth() == 0) continue;

        bool isPlayer = (entityPtr->getType() == game::ObjectType::PLAYER);
        bool isTarget = (guid == targetGuid);

        // Player nameplates are always shown; NPC nameplates respect the V-key toggle
        if (!isPlayer && !showNameplates_) continue;

        // Convert canonical WoW position → render space, raise to head height
        glm::vec3 renderPos = core::coords::canonicalToRender(
            glm::vec3(unit->getX(), unit->getY(), unit->getZ()));
        renderPos.z += 2.3f;

        // Cull distance: target or other players up to 40 units; NPC others up to 20 units
        float dist = glm::length(renderPos - camPos);
        float cullDist = (isTarget || isPlayer) ? 40.0f : 20.0f;
        if (dist > cullDist) continue;

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
        float alpha = dist < (cullDist - 5.0f) ? 1.0f : 1.0f - (dist - (cullDist - 5.0f)) / 5.0f;
        auto A = [&](int v) { return static_cast<int>(v * alpha); };

        // Bar colour by hostility
        ImU32 barColor, bgColor;
        if (unit->isHostile()) {
            barColor = IM_COL32(220, 60,  60,  A(200));
            bgColor  = IM_COL32(100, 25,  25,  A(160));
        } else {
            barColor = IM_COL32(60,  200, 80,  A(200));
            bgColor  = IM_COL32(25,  100, 35,  A(160));
        }
        ImU32 borderColor = isTarget
            ? IM_COL32(255, 215, 0,  A(255))
            : IM_COL32(20,  20,  20, A(180));

        // Bar geometry
        constexpr float barW = 80.0f;
        constexpr float barH = 8.0f;
        const float barX = sx - barW * 0.5f;

        float healthPct = std::clamp(
            static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth()),
            0.0f, 1.0f);

        drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW,               sy + barH), bgColor,    2.0f);
        drawList->AddRectFilled(ImVec2(barX,                 sy), ImVec2(barX + barW * healthPct,   sy + barH), barColor,   2.0f);
        drawList->AddRect       (ImVec2(barX - 1.0f, sy - 1.0f), ImVec2(barX + barW + 1.0f, sy + barH + 1.0f), borderColor, 2.0f);

        // Name + level label above health bar
        uint32_t level = unit->getLevel();
        const std::string& unitName = unit->getName();
        char labelBuf[96];
        if (isPlayer) {
            // Player nameplates: show name only (no level clutter).
            // Fall back to level as placeholder while the name query is pending.
            if (!unitName.empty())
                snprintf(labelBuf, sizeof(labelBuf), "%s", unitName.c_str());
            else if (level > 0)
                snprintf(labelBuf, sizeof(labelBuf), "Player (%u)", level);
            else
                snprintf(labelBuf, sizeof(labelBuf), "Player");
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
        // Name color: other player=cyan, hostile=red, non-hostile=yellow (WoW convention)
        ImU32 nameColor = isPlayer
            ? IM_COL32( 80, 200, 255, A(230))   // cyan — other players
            : unit->isHostile()
                ? IM_COL32(220,  80,  80, A(230))   // red  — hostile NPC
                : IM_COL32(240, 200, 100, A(230));  // yellow — friendly NPC
        drawList->AddText(ImVec2(nameX + 1.0f, nameY + 1.0f), IM_COL32(0, 0, 0, A(160)), labelBuf);
        drawList->AddText(ImVec2(nameX,         nameY),         nameColor, labelBuf);

        // Raid mark (if any) to the left of the name
        {
            static const struct { const char* sym; ImU32 col; } kNPMarks[] = {
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
        }

        // Click to target: detect left-click inside the combined nameplate region
        if (!ImGui::GetIO().WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float nx0 = nameX - 2.0f;
            float ny0 = nameY - 1.0f;
            float nx1 = nameX + textSize.x + 2.0f;
            float ny1 = sy + barH + 2.0f;
            if (mouse.x >= nx0 && mouse.x <= nx1 && mouse.y >= ny0 && mouse.y <= ny1) {
                gameHandler.setTarget(guid);
            }
        }
    }
}

// ============================================================
// Party Frames (Phase 4)
// ============================================================

void GameScreen::renderPartyFrames(game::GameHandler& gameHandler) {
    if (!gameHandler.isInGroup()) return;

    const auto& partyData = gameHandler.getPartyData();
    const bool isRaid = (partyData.groupType == 1);
    float frameY = 120.0f;

    // ---- Raid frame layout ----
    if (isRaid) {
        // Organize members by subgroup (0-7, up to 5 members each)
        constexpr int MAX_SUBGROUPS = 8;
        constexpr int MAX_PER_GROUP = 5;
        std::vector<const game::GroupMember*> subgroups[MAX_SUBGROUPS];
        for (const auto& m : partyData.members) {
            int sg = m.subGroup < MAX_SUBGROUPS ? m.subGroup : 0;
            if (static_cast<int>(subgroups[sg].size()) < MAX_PER_GROUP)
                subgroups[sg].push_back(&m);
        }

        // Count non-empty subgroups to determine layout
        int activeSgs = 0;
        for (int sg = 0; sg < MAX_SUBGROUPS; sg++)
            if (!subgroups[sg].empty()) activeSgs++;

        // Compact raid cell: name + 2 narrow bars
        constexpr float CELL_W = 90.0f;
        constexpr float CELL_H = 42.0f;
        constexpr float BAR_H  = 7.0f;
        constexpr float CELL_PAD = 3.0f;

        float winW = activeSgs * (CELL_W + CELL_PAD) + CELL_PAD + 8.0f;
        float winH = MAX_PER_GROUP * (CELL_H + CELL_PAD) + CELL_PAD + 20.0f;

        auto* window = core::Application::getInstance().getWindow();
        float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
        float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
        float raidX = (screenW - winW) / 2.0f;
        float raidY = screenH - winH - 120.0f;  // above action bar area

        ImGui::SetNextWindowPos(ImVec2(raidX, raidY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);

        ImGuiWindowFlags raidFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoScrollbar;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(CELL_PAD, CELL_PAD));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.1f, 0.85f));

        if (ImGui::Begin("##RaidFrames", nullptr, raidFlags)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 winPos = ImGui::GetWindowPos();

            int colIdx = 0;
            for (int sg = 0; sg < MAX_SUBGROUPS; sg++) {
                if (subgroups[sg].empty()) continue;

                float colX = winPos.x + CELL_PAD + colIdx * (CELL_W + CELL_PAD);

                for (int row = 0; row < static_cast<int>(subgroups[sg].size()); row++) {
                    const auto& m = *subgroups[sg][row];
                    float cellY = winPos.y + CELL_PAD + 14.0f + row * (CELL_H + CELL_PAD);

                    ImVec2 cellMin(colX, cellY);
                    ImVec2 cellMax(colX + CELL_W, cellY + CELL_H);

                    // Cell background
                    bool isTarget = (gameHandler.getTargetGuid() == m.guid);
                    ImU32 bg = isTarget ? IM_COL32(60, 80, 120, 200) : IM_COL32(30, 30, 40, 180);
                    draw->AddRectFilled(cellMin, cellMax, bg, 3.0f);
                    if (isTarget)
                        draw->AddRect(cellMin, cellMax, IM_COL32(100, 150, 255, 200), 3.0f);

                    // Dead/ghost overlay
                    bool isOnline = (m.onlineStatus & 0x0001) != 0;
                    bool isDead   = (m.onlineStatus & 0x0020) != 0;
                    bool isGhost  = (m.onlineStatus & 0x0010) != 0;

                    // Name text (truncated)
                    char truncName[16];
                    snprintf(truncName, sizeof(truncName), "%.12s", m.name.c_str());
                    ImU32 nameCol = (!isOnline || isDead || isGhost)
                        ? IM_COL32(140, 140, 140, 200) : IM_COL32(220, 220, 220, 255);
                    draw->AddText(ImVec2(cellMin.x + 4.0f, cellMin.y + 3.0f), nameCol, truncName);

                    // Health bar
                    uint32_t hp = m.hasPartyStats ? m.curHealth : 0;
                    uint32_t maxHp = m.hasPartyStats ? m.maxHealth : 0;
                    if (maxHp > 0) {
                        float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                        float barY = cellMin.y + 16.0f;
                        ImVec2 barBg(cellMin.x + 3.0f, barY);
                        ImVec2 barBgEnd(cellMax.x - 3.0f, barY + BAR_H);
                        draw->AddRectFilled(barBg, barBgEnd, IM_COL32(40, 40, 40, 200), 2.0f);
                        ImVec2 barFill(barBg.x, barBg.y);
                        ImVec2 barFillEnd(barBg.x + (barBgEnd.x - barBg.x) * pct, barBgEnd.y);
                        ImU32 hpCol = pct > 0.5f ? IM_COL32(60, 180, 60, 255) :
                                      pct > 0.2f ? IM_COL32(200, 180, 50, 255) :
                                                   IM_COL32(200, 60, 60, 255);
                        draw->AddRectFilled(barFill, barFillEnd, hpCol, 2.0f);
                    }

                    // Power bar
                    if (m.hasPartyStats && m.maxPower > 0) {
                        float pct = static_cast<float>(m.curPower) / static_cast<float>(m.maxPower);
                        float barY = cellMin.y + 16.0f + BAR_H + 2.0f;
                        ImVec2 barBg(cellMin.x + 3.0f, barY);
                        ImVec2 barBgEnd(cellMax.x - 3.0f, barY + BAR_H - 2.0f);
                        draw->AddRectFilled(barBg, barBgEnd, IM_COL32(30, 30, 40, 200), 2.0f);
                        ImVec2 barFill(barBg.x, barBg.y);
                        ImVec2 barFillEnd(barBg.x + (barBgEnd.x - barBg.x) * pct, barBgEnd.y);
                        ImU32 pwrCol;
                        switch (m.powerType) {
                            case 0: pwrCol = IM_COL32(50, 80, 220, 255); break; // Mana
                            case 1: pwrCol = IM_COL32(200, 50, 50, 255); break; // Rage
                            case 3: pwrCol = IM_COL32(220, 210, 50, 255); break; // Energy
                            case 6: pwrCol = IM_COL32(180, 30, 50, 255); break; // Runic Power
                            default: pwrCol = IM_COL32(80, 120, 80, 255); break;
                        }
                        draw->AddRectFilled(barFill, barFillEnd, pwrCol, 2.0f);
                    }

                    // Clickable invisible region over the whole cell
                    ImGui::SetCursorScreenPos(cellMin);
                    ImGui::PushID(static_cast<int>(m.guid));
                    if (ImGui::InvisibleButton("raidCell", ImVec2(CELL_W, CELL_H))) {
                        gameHandler.setTarget(m.guid);
                    }
                    ImGui::PopID();
                }
                colIdx++;
            }

            // Subgroup header row
            colIdx = 0;
            for (int sg = 0; sg < MAX_SUBGROUPS; sg++) {
                if (subgroups[sg].empty()) continue;
                float colX = winPos.x + CELL_PAD + colIdx * (CELL_W + CELL_PAD);
                char sgLabel[8];
                snprintf(sgLabel, sizeof(sgLabel), "G%d", sg + 1);
                draw->AddText(ImVec2(colX + CELL_W / 2 - 8.0f, winPos.y + CELL_PAD), IM_COL32(160, 160, 180, 200), sgLabel);
                colIdx++;
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }

    // ---- Party frame layout (5-man) ----
    ImGui::SetNextWindowPos(ImVec2(10.0f, frameY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200.0f, 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.1f, 0.8f));

    if (ImGui::Begin("##PartyFrames", nullptr, flags)) {
        for (const auto& member : partyData.members) {
            ImGui::PushID(static_cast<int>(member.guid));

            // Name with level and status info
            std::string label = member.name;
            if (member.hasPartyStats && member.level > 0) {
                label += " [" + std::to_string(member.level) + "]";
            }
            if (member.hasPartyStats) {
                bool isOnline = (member.onlineStatus & 0x0001) != 0;
                bool isDead = (member.onlineStatus & 0x0020) != 0;
                bool isGhost = (member.onlineStatus & 0x0010) != 0;
                if (!isOnline) label += " (offline)";
                else if (isDead || isGhost) label += " (dead)";
            }

            // Clickable name to target
            if (ImGui::Selectable(label.c_str(), gameHandler.getTargetGuid() == member.guid)) {
                gameHandler.setTarget(member.guid);
            }

            // Health bar: prefer party stats, fall back to entity
            uint32_t hp = 0, maxHp = 0;
            if (member.hasPartyStats && member.maxHealth > 0) {
                hp = member.curHealth;
                maxHp = member.maxHealth;
            } else {
                auto entity = gameHandler.getEntityManager().getEntity(member.guid);
                if (entity && (entity->getType() == game::ObjectType::PLAYER || entity->getType() == game::ObjectType::UNIT)) {
                    auto unit = std::static_pointer_cast<game::Unit>(entity);
                    hp = unit->getHealth();
                    maxHp = unit->getMaxHealth();
                }
            }
            if (maxHp > 0) {
                float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                    pct > 0.5f ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) :
                    pct > 0.2f ? ImVec4(0.8f, 0.8f, 0.2f, 1.0f) :
                                 ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                ImGui::ProgressBar(pct, ImVec2(-1, 12), "");
                ImGui::PopStyleColor();
            }

            // Power bar (mana/rage/energy) from party stats
            if (member.hasPartyStats && member.maxPower > 0) {
                float powerPct = static_cast<float>(member.curPower) / static_cast<float>(member.maxPower);
                ImVec4 powerColor;
                switch (member.powerType) {
                    case 0: powerColor = ImVec4(0.2f, 0.2f, 0.9f, 1.0f); break; // Mana (blue)
                    case 1: powerColor = ImVec4(0.9f, 0.2f, 0.2f, 1.0f); break; // Rage (red)
                    case 2: powerColor = ImVec4(0.9f, 0.6f, 0.1f, 1.0f); break; // Focus (orange)
                    case 3: powerColor = ImVec4(0.9f, 0.9f, 0.2f, 1.0f); break; // Energy (yellow)
                    case 4: powerColor = ImVec4(0.5f, 0.9f, 0.3f, 1.0f); break; // Happiness (green)
                    case 6: powerColor = ImVec4(0.8f, 0.1f, 0.2f, 1.0f); break; // Runic Power (crimson)
                    case 7: powerColor = ImVec4(0.4f, 0.1f, 0.6f, 1.0f); break; // Soul Shards (purple)
                    default: powerColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); break;
                }
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, powerColor);
                ImGui::ProgressBar(powerPct, ImVec2(-1, 8), "");
                ImGui::PopStyleColor();
            }

            // Party member cast bar — shows when the party member is casting
            if (auto* cs = gameHandler.getUnitCastState(member.guid)) {
                float castPct = (cs->timeTotal > 0.0f)
                    ? (cs->timeTotal - cs->timeRemaining) / cs->timeTotal : 0.0f;
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.8f, 0.2f, 1.0f));
                char pcastLabel[48];
                const std::string& spellNm = gameHandler.getSpellName(cs->spellId);
                if (!spellNm.empty())
                    snprintf(pcastLabel, sizeof(pcastLabel), "%s (%.1fs)", spellNm.c_str(), cs->timeRemaining);
                else
                    snprintf(pcastLabel, sizeof(pcastLabel), "Casting... (%.1fs)", cs->timeRemaining);
                ImGui::ProgressBar(castPct, ImVec2(-1, 10), pcastLabel);
                ImGui::PopStyleColor();
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ============================================================
// Boss Encounter Frames
// ============================================================

void GameScreen::renderBossFrames(game::GameHandler& gameHandler) {
    // Collect active boss unit slots
    struct BossSlot { uint32_t slot; uint64_t guid; };
    std::vector<BossSlot> active;
    for (uint32_t s = 0; s < game::GameHandler::kMaxEncounterSlots; ++s) {
        uint64_t g = gameHandler.getEncounterUnitGuid(s);
        if (g != 0) active.push_back({s, g});
    }
    if (active.empty()) return;

    const float frameW = 200.0f;
    const float startX = ImGui::GetIO().DisplaySize.x - frameW - 10.0f;
    float frameY = 120.0f;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.05f, 0.05f, 0.85f));

    ImGui::SetNextWindowPos(ImVec2(startX, frameY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(frameW, 0.0f), ImGuiCond_Always);

    if (ImGui::Begin("##BossFrames", nullptr, flags)) {
        for (const auto& bs : active) {
            ImGui::PushID(static_cast<int>(bs.guid));

            // Try to resolve name and health from entity manager
            std::string name = "Boss";
            uint32_t hp = 0, maxHp = 0;
            auto entity = gameHandler.getEntityManager().getEntity(bs.guid);
            if (entity && (entity->getType() == game::ObjectType::UNIT ||
                           entity->getType() == game::ObjectType::PLAYER)) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                const auto& n = unit->getName();
                if (!n.empty()) name = n;
                hp    = unit->getHealth();
                maxHp = unit->getMaxHealth();
            }

            // Clickable name to target
            if (ImGui::Selectable(name.c_str(), gameHandler.getTargetGuid() == bs.guid)) {
                gameHandler.setTarget(bs.guid);
            }

            if (maxHp > 0) {
                float pct = static_cast<float>(hp) / static_cast<float>(maxHp);
                // Boss health bar in red shades
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                    pct > 0.5f ? ImVec4(0.8f, 0.2f, 0.2f, 1.0f) :
                    pct > 0.2f ? ImVec4(0.9f, 0.5f, 0.1f, 1.0f) :
                                 ImVec4(1.0f, 0.8f, 0.1f, 1.0f));
                char label[32];
                std::snprintf(label, sizeof(label), "%u / %u", hp, maxHp);
                ImGui::ProgressBar(pct, ImVec2(-1, 14), label);
                ImGui::PopStyleColor();
            }

            // Boss cast bar — shown when the boss is casting (critical for interrupt)
            if (auto* cs = gameHandler.getUnitCastState(bs.guid)) {
                float castPct  = (cs->timeTotal > 0.0f)
                    ? (cs->timeTotal - cs->timeRemaining) / cs->timeTotal : 0.0f;
                uint32_t bspell = cs->spellId;
                const std::string& bcastName = (bspell != 0)
                    ? gameHandler.getSpellName(bspell) : "";
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.9f, 0.3f, 0.2f, 1.0f));
                char bcastLabel[72];
                if (!bcastName.empty())
                    snprintf(bcastLabel, sizeof(bcastLabel), "%s (%.1fs)",
                             bcastName.c_str(), cs->timeRemaining);
                else
                    snprintf(bcastLabel, sizeof(bcastLabel), "Casting... (%.1fs)", cs->timeRemaining);
                ImGui::ProgressBar(castPct, ImVec2(-1, 12), bcastLabel);
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
            ImGui::Spacing();
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ============================================================
// Group Invite Popup (Phase 4)
// ============================================================

void GameScreen::renderGroupInvitePopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingGroupInvite()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 150, 200), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);

    if (ImGui::Begin("Group Invite", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s has invited you to a group.", gameHandler.getPendingInviterName().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(130, 30))) {
            gameHandler.acceptGroupInvite();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(130, 30))) {
            gameHandler.declineGroupInvite();
        }
    }
    ImGui::End();
}

void GameScreen::renderDuelRequestPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingDuelRequest()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 150, 250), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);

    if (ImGui::Begin("Duel Request", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s challenges you to a duel!", gameHandler.getDuelChallengerName().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(130, 30))) {
            gameHandler.acceptDuel();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(130, 30))) {
            gameHandler.forfeitDuel();
        }
    }
    ImGui::End();
}

void GameScreen::renderItemTextWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isItemTextOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW * 0.5f - 200, screenH * 0.15f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);

    bool open = true;
    if (!ImGui::Begin("Book", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) gameHandler.closeItemText();
        return;
    }
    if (!open) {
        ImGui::End();
        gameHandler.closeItemText();
        return;
    }

    // Parchment-toned background text
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.1f, 0.0f, 1.0f));
    ImGui::TextWrapped("%s", gameHandler.getItemText().c_str());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(80, 0))) {
        gameHandler.closeItemText();
    }

    ImGui::End();
}

void GameScreen::renderSharedQuestPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingSharedQuest()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 175, 490), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    if (ImGui::Begin("Shared Quest", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s has shared a quest with you:", gameHandler.getSharedQuestSharerName().c_str());
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "\"%s\"", gameHandler.getSharedQuestTitle().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(130, 30))) {
            gameHandler.acceptSharedQuest();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(130, 30))) {
            gameHandler.declineSharedQuest();
        }
    }
    ImGui::End();
}

void GameScreen::renderSummonRequestPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingSummonRequest()) return;

    // Tick the timeout down
    float dt = ImGui::GetIO().DeltaTime;
    gameHandler.tickSummonTimeout(dt);
    if (!gameHandler.hasPendingSummonRequest()) return;  // expired

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 175, 430), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    if (ImGui::Begin("Summon Request", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s is summoning you.", gameHandler.getSummonerName().c_str());
        float t = gameHandler.getSummonTimeoutSec();
        if (t > 0.0f) {
            ImGui::Text("Time remaining: %.0fs", t);
        }
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(130, 30))) {
            gameHandler.acceptSummon();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(130, 30))) {
            gameHandler.declineSummon();
        }
    }
    ImGui::End();
}

void GameScreen::renderTradeRequestPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingTradeRequest()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 150, 370), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);

    if (ImGui::Begin("Trade Request", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::Text("%s wants to trade with you.", gameHandler.getTradePeerName().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(130, 30))) {
            gameHandler.acceptTradeRequest();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(130, 30))) {
            gameHandler.declineTradeRequest();
        }
    }
    ImGui::End();
}

void GameScreen::renderLootRollPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingLootRoll()) return;

    const auto& roll = gameHandler.getPendingLootRoll();

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 175, 310), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    if (ImGui::Begin("Loot Roll", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        // Quality color for item name
        static const ImVec4 kQualityColors[] = {
            ImVec4(0.6f, 0.6f, 0.6f, 1.0f),  // 0=poor (grey)
            ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // 1=common (white)
            ImVec4(0.1f, 1.0f, 0.1f, 1.0f),  // 2=uncommon (green)
            ImVec4(0.0f, 0.44f, 0.87f, 1.0f),// 3=rare (blue)
            ImVec4(0.64f, 0.21f, 0.93f, 1.0f),// 4=epic (purple)
            ImVec4(1.0f, 0.5f, 0.0f, 1.0f),  // 5=legendary (orange)
        };
        uint8_t q = roll.itemQuality;
        ImVec4 col = (q < 6) ? kQualityColors[q] : kQualityColors[1];

        ImGui::Text("An item is up for rolls:");
        ImGui::TextColored(col, "[%s]", roll.itemName.c_str());
        ImGui::Spacing();

        if (ImGui::Button("Need", ImVec2(80, 30))) {
            gameHandler.sendLootRoll(roll.objectGuid, roll.slot, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Greed", ImVec2(80, 30))) {
            gameHandler.sendLootRoll(roll.objectGuid, roll.slot, 1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Disenchant", ImVec2(95, 30))) {
            gameHandler.sendLootRoll(roll.objectGuid, roll.slot, 2);
        }
        ImGui::SameLine();
        if (ImGui::Button("Pass", ImVec2(70, 30))) {
            gameHandler.sendLootRoll(roll.objectGuid, roll.slot, 96);
        }
    }
    ImGui::End();
}

void GameScreen::renderGuildInvitePopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingGuildInvite()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 175, 250), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    if (ImGui::Begin("Guild Invite", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("%s has invited you to join %s.",
                           gameHandler.getPendingGuildInviterName().c_str(),
                           gameHandler.getPendingGuildInviteGuildName().c_str());
        ImGui::Spacing();

        if (ImGui::Button("Accept", ImVec2(155, 30))) {
            gameHandler.acceptGuildInvite();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(155, 30))) {
            gameHandler.declineGuildInvite();
        }
    }
    ImGui::End();
}

void GameScreen::renderReadyCheckPopup(game::GameHandler& gameHandler) {
    if (!gameHandler.hasPendingReadyCheck()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 175, screenH / 2 - 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    if (ImGui::Begin("Ready Check", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
        const std::string& initiator = gameHandler.getReadyCheckInitiator();
        if (initiator.empty()) {
            ImGui::Text("A ready check has been initiated!");
        } else {
            ImGui::TextWrapped("%s has initiated a ready check!", initiator.c_str());
        }
        ImGui::Spacing();

        if (ImGui::Button("Ready", ImVec2(155, 30))) {
            gameHandler.respondToReadyCheck(true);
            gameHandler.dismissReadyCheck();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not Ready", ImVec2(155, 30))) {
            gameHandler.respondToReadyCheck(false);
            gameHandler.dismissReadyCheck();
        }
    }
    ImGui::End();
}

void GameScreen::renderGuildRoster(game::GameHandler& gameHandler) {
    // O key toggle (WoW default Social/Guild keybind)
    if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_O)) {
        showGuildRoster_ = !showGuildRoster_;
        if (showGuildRoster_) {
            // Open friends tab directly if not in guild
            if (!gameHandler.isInGuild()) {
                guildRosterTab_ = 2;  // Friends tab
            } else {
                // Re-query guild name if we have guildId but no name yet
                if (gameHandler.getGuildName().empty()) {
                    const auto* ch = gameHandler.getActiveCharacter();
                    if (ch && ch->hasGuild()) {
                        gameHandler.queryGuildInfo(ch->guildId);
                    }
                }
                gameHandler.requestGuildRoster();
                gameHandler.requestGuildInfo();
            }
        }
    }

    // Petition creation dialog (shown when NPC sends SMSG_PETITION_SHOWLIST)
    if (gameHandler.hasPetitionShowlist()) {
        ImGui::OpenPopup("CreateGuildPetition");
        gameHandler.clearPetitionDialog();
    }
    if (ImGui::BeginPopupModal("CreateGuildPetition", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Create Guild Charter");
        ImGui::Separator();
        uint32_t cost = gameHandler.getPetitionCost();
        uint32_t gold = cost / 10000;
        uint32_t silver = (cost % 10000) / 100;
        uint32_t copper = cost % 100;
        ImGui::Text("Cost: %ug %us %uc", gold, silver, copper);
        ImGui::Spacing();
        ImGui::Text("Guild Name:");
        ImGui::InputText("##petitionname", petitionNameBuffer_, sizeof(petitionNameBuffer_));
        ImGui::Spacing();
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            if (petitionNameBuffer_[0] != '\0') {
                gameHandler.buyPetition(gameHandler.getPetitionNpcGuid(), petitionNameBuffer_);
                petitionNameBuffer_[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            petitionNameBuffer_[0] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!showGuildRoster_) return;

    // Get zone manager for name lookup
    game::ZoneManager* zoneManager = nullptr;
    if (auto* renderer = core::Application::getInstance().getRenderer()) {
        zoneManager = renderer->getZoneManager();
    }

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 375, screenH / 2 - 250), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(750, 500), ImGuiCond_Once);

    std::string title = gameHandler.isInGuild() ? (gameHandler.getGuildName() + " - Social") : "Social";
    bool open = showGuildRoster_;
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        // Tab bar: Roster | Guild Info
        if (ImGui::BeginTabBar("GuildTabs")) {
            if (ImGui::BeginTabItem("Roster")) {
                guildRosterTab_ = 0;
                if (!gameHandler.hasGuildRoster()) {
                    ImGui::Text("Loading roster...");
                } else {
                    const auto& roster = gameHandler.getGuildRoster();

                    // MOTD
                    if (!roster.motd.empty()) {
                        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "MOTD: %s", roster.motd.c_str());
                        ImGui::Separator();
                    }

                    // Count online
                    int onlineCount = 0;
                    for (const auto& m : roster.members) {
                        if (m.online) ++onlineCount;
                    }
                    ImGui::Text("%d members (%d online)", (int)roster.members.size(), onlineCount);
                    ImGui::Separator();

                    const auto& rankNames = gameHandler.getGuildRankNames();

                    // Table
                    if (ImGui::BeginTable("GuildRoster", 7,
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_Sortable)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
                        ImGui::TableSetupColumn("Rank");
                        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                        ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn("Note");
                        ImGui::TableSetupColumn("Officer Note");
                        ImGui::TableHeadersRow();

                        // Online members first, then offline
                        auto sortedMembers = roster.members;
                        std::sort(sortedMembers.begin(), sortedMembers.end(), [](const auto& a, const auto& b) {
                            if (a.online != b.online) return a.online > b.online;
                            return a.name < b.name;
                        });

                        static const char* classNames[] = {
                            "Unknown", "Warrior", "Paladin", "Hunter", "Rogue",
                            "Priest", "Death Knight", "Shaman", "Mage", "Warlock",
                            "", "Druid"
                        };

                        for (const auto& m : sortedMembers) {
                            ImGui::TableNextRow();
                            ImVec4 textColor = m.online ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                        : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

                            ImGui::TableNextColumn();
                            ImGui::TextColored(textColor, "%s", m.name.c_str());

                            // Right-click context menu
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                                selectedGuildMember_ = m.name;
                                ImGui::OpenPopup("GuildMemberContext");
                            }

                            ImGui::TableNextColumn();
                            // Show rank name instead of index
                            if (m.rankIndex < rankNames.size()) {
                                ImGui::TextColored(textColor, "%s", rankNames[m.rankIndex].c_str());
                            } else {
                                ImGui::TextColored(textColor, "Rank %u", m.rankIndex);
                            }

                            ImGui::TableNextColumn();
                            ImGui::TextColored(textColor, "%u", m.level);

                            ImGui::TableNextColumn();
                            const char* className = (m.classId < 12) ? classNames[m.classId] : "Unknown";
                            ImGui::TextColored(textColor, "%s", className);

                            ImGui::TableNextColumn();
                            // Zone name lookup
                            if (zoneManager) {
                                const auto* zoneInfo = zoneManager->getZoneInfo(m.zoneId);
                                if (zoneInfo && !zoneInfo->name.empty()) {
                                    ImGui::TextColored(textColor, "%s", zoneInfo->name.c_str());
                                } else {
                                    ImGui::TextColored(textColor, "%u", m.zoneId);
                                }
                            } else {
                                ImGui::TextColored(textColor, "%u", m.zoneId);
                            }

                            ImGui::TableNextColumn();
                            ImGui::TextColored(textColor, "%s", m.publicNote.c_str());

                            ImGui::TableNextColumn();
                            ImGui::TextColored(textColor, "%s", m.officerNote.c_str());
                        }
                        ImGui::EndTable();
                    }

                    // Context menu popup
                    if (ImGui::BeginPopup("GuildMemberContext")) {
                        ImGui::Text("%s", selectedGuildMember_.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Promote")) {
                            gameHandler.promoteGuildMember(selectedGuildMember_);
                        }
                        if (ImGui::MenuItem("Demote")) {
                            gameHandler.demoteGuildMember(selectedGuildMember_);
                        }
                        if (ImGui::MenuItem("Kick")) {
                            gameHandler.kickGuildMember(selectedGuildMember_);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set Public Note...")) {
                            showGuildNoteEdit_ = true;
                            editingOfficerNote_ = false;
                            guildNoteEditBuffer_[0] = '\0';
                            // Pre-fill with existing note
                            for (const auto& mem : roster.members) {
                                if (mem.name == selectedGuildMember_) {
                                    snprintf(guildNoteEditBuffer_, sizeof(guildNoteEditBuffer_), "%s", mem.publicNote.c_str());
                                    break;
                                }
                            }
                        }
                        if (ImGui::MenuItem("Set Officer Note...")) {
                            showGuildNoteEdit_ = true;
                            editingOfficerNote_ = true;
                            guildNoteEditBuffer_[0] = '\0';
                            for (const auto& mem : roster.members) {
                                if (mem.name == selectedGuildMember_) {
                                    snprintf(guildNoteEditBuffer_, sizeof(guildNoteEditBuffer_), "%s", mem.officerNote.c_str());
                                    break;
                                }
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set as Leader")) {
                            gameHandler.setGuildLeader(selectedGuildMember_);
                        }
                        ImGui::EndPopup();
                    }

                    // Note edit modal
                    if (showGuildNoteEdit_) {
                        ImGui::OpenPopup("EditGuildNote");
                        showGuildNoteEdit_ = false;
                    }
                    if (ImGui::BeginPopupModal("EditGuildNote", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::Text("%s %s for %s:",
                            editingOfficerNote_ ? "Officer" : "Public", "Note", selectedGuildMember_.c_str());
                        ImGui::InputText("##guildnote", guildNoteEditBuffer_, sizeof(guildNoteEditBuffer_));
                        if (ImGui::Button("Save")) {
                            if (editingOfficerNote_) {
                                gameHandler.setGuildOfficerNote(selectedGuildMember_, guildNoteEditBuffer_);
                            } else {
                                gameHandler.setGuildPublicNote(selectedGuildMember_, guildNoteEditBuffer_);
                            }
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) {
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Guild Info")) {
                guildRosterTab_ = 1;
                const auto& infoData = gameHandler.getGuildInfoData();
                const auto& queryData = gameHandler.getGuildQueryData();
                const auto& roster = gameHandler.getGuildRoster();
                const auto& rankNames = gameHandler.getGuildRankNames();

                // Guild name (large, gold)
                ImGui::PushFont(nullptr);  // default font
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "<%s>", gameHandler.getGuildName().c_str());
                ImGui::PopFont();
                ImGui::Separator();

                // Creation date
                if (infoData.isValid()) {
                    ImGui::Text("Created: %u/%u/%u", infoData.creationDay, infoData.creationMonth, infoData.creationYear);
                    ImGui::Text("Members: %u  |  Accounts: %u", infoData.numMembers, infoData.numAccounts);
                }
                ImGui::Spacing();

                // Guild description / info text
                if (!roster.guildInfo.empty()) {
                    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Description:");
                    ImGui::TextWrapped("%s", roster.guildInfo.c_str());
                }
                ImGui::Spacing();

                // MOTD with edit button
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "MOTD:");
                ImGui::SameLine();
                if (!roster.motd.empty()) {
                    ImGui::TextWrapped("%s", roster.motd.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(not set)");
                }
                if (ImGui::Button("Set MOTD")) {
                    showMotdEdit_ = true;
                    snprintf(guildMotdEditBuffer_, sizeof(guildMotdEditBuffer_), "%s", roster.motd.c_str());
                }
                ImGui::Spacing();

                // MOTD edit modal
                if (showMotdEdit_) {
                    ImGui::OpenPopup("EditMotd");
                    showMotdEdit_ = false;
                }
                if (ImGui::BeginPopupModal("EditMotd", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("Set Message of the Day:");
                    ImGui::InputText("##motdinput", guildMotdEditBuffer_, sizeof(guildMotdEditBuffer_));
                    if (ImGui::Button("Save", ImVec2(120, 0))) {
                        gameHandler.setGuildMotd(guildMotdEditBuffer_);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                // Emblem info
                if (queryData.isValid()) {
                    ImGui::Separator();
                    ImGui::Text("Emblem: Style %u, Color %u  |  Border: Style %u, Color %u  |  BG: %u",
                        queryData.emblemStyle, queryData.emblemColor,
                        queryData.borderStyle, queryData.borderColor, queryData.backgroundColor);
                }

                // Rank list
                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Ranks:");
                for (size_t i = 0; i < rankNames.size(); ++i) {
                    if (rankNames[i].empty()) continue;
                    // Show rank permission summary from roster data
                    if (i < roster.ranks.size()) {
                        uint32_t rights = roster.ranks[i].rights;
                        std::string perms;
                        if (rights & 0x01) perms += "Invite ";
                        if (rights & 0x02) perms += "Remove ";
                        if (rights & 0x40) perms += "Promote ";
                        if (rights & 0x80) perms += "Demote ";
                        if (rights & 0x04) perms += "OChat ";
                        if (rights & 0x10) perms += "MOTD ";
                        ImGui::Text("  %zu. %s", i + 1, rankNames[i].c_str());
                        if (!perms.empty()) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[%s]", perms.c_str());
                        }
                    } else {
                        ImGui::Text("  %zu. %s", i + 1, rankNames[i].c_str());
                    }
                }

                // Rank management buttons
                ImGui::Spacing();
                if (ImGui::Button("Add Rank")) {
                    showAddRankModal_ = true;
                    addRankNameBuffer_[0] = '\0';
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete Last Rank")) {
                    gameHandler.deleteGuildRank();
                }

                // Add rank modal
                if (showAddRankModal_) {
                    ImGui::OpenPopup("AddGuildRank");
                    showAddRankModal_ = false;
                }
                if (ImGui::BeginPopupModal("AddGuildRank", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::Text("New Rank Name:");
                    ImGui::InputText("##rankname", addRankNameBuffer_, sizeof(addRankNameBuffer_));
                    if (ImGui::Button("Add", ImVec2(120, 0))) {
                        if (addRankNameBuffer_[0] != '\0') {
                            gameHandler.addGuildRank(addRankNameBuffer_);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::EndTabItem();
            }

            // ---- Friends tab ----
            if (ImGui::BeginTabItem("Friends")) {
                guildRosterTab_ = 2;
                const auto& contacts = gameHandler.getContacts();

                // Filter to friends only
                int friendCount = 0;
                for (const auto& c : contacts) {
                    if (!c.isFriend()) continue;
                    ++friendCount;

                    // Status dot
                    ImU32 dotColor = c.isOnline()
                        ? IM_COL32(80, 200, 80, 255)
                        : IM_COL32(120, 120, 120, 255);
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddCircleFilled(
                        ImVec2(cursor.x + 6.0f, cursor.y + 8.0f), 5.0f, dotColor);
                    ImGui::Dummy(ImVec2(14.0f, 0.0f));
                    ImGui::SameLine();

                    // Name
                    const char* displayName = c.name.empty() ? "(unknown)" : c.name.c_str();
                    ImVec4 nameCol = c.isOnline()
                        ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                        : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
                    ImGui::TextColored(nameCol, "%s", displayName);

                    // Level and status on same line (right-aligned)
                    if (c.isOnline()) {
                        ImGui::SameLine();
                        const char* statusLabel =
                            (c.status == 2) ? "(AFK)" :
                            (c.status == 3) ? "(DND)" : "";
                        if (c.level > 0) {
                            ImGui::TextDisabled("Lv %u %s", c.level, statusLabel);
                        } else if (*statusLabel) {
                            ImGui::TextDisabled("%s", statusLabel);
                        }
                    }
                }

                if (friendCount == 0) {
                    ImGui::TextDisabled("No friends online.");
                }

                ImGui::Separator();
                ImGui::TextDisabled("Right-click a player's name in chat to add friends.");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    showGuildRoster_ = open;
}

// ============================================================
// Buff/Debuff Bar (Phase 3)
// ============================================================

void GameScreen::renderBuffBar(game::GameHandler& gameHandler) {
    const auto& auras = gameHandler.getPlayerAuras();

    // Count non-empty auras
    int activeCount = 0;
    for (const auto& a : auras) {
        if (!a.isEmpty()) activeCount++;
    }
    if (activeCount == 0 && !gameHandler.hasPet()) return;

    auto* assetMgr = core::Application::getInstance().getAssetManager();

    // Position below the player frame in top-left
    constexpr float ICON_SIZE = 32.0f;
    constexpr int ICONS_PER_ROW = 8;
    float barW = ICONS_PER_ROW * (ICON_SIZE + 4.0f) + 8.0f;
    // Dock under player frame in top-left (player frame is at 10, 30 with ~110px height)
    ImGui::SetNextWindowPos(ImVec2(10.0f, 145.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(barW, 0), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 2.0f));

    if (ImGui::Begin("##BuffBar", nullptr, flags)) {
        int shown = 0;
        for (size_t i = 0; i < auras.size() && shown < 16; ++i) {
            const auto& aura = auras[i];
            if (aura.isEmpty()) continue;

            if (shown > 0 && shown % ICONS_PER_ROW != 0) ImGui::SameLine();

            ImGui::PushID(static_cast<int>(i));

            bool isBuff = (aura.flags & 0x80) == 0;  // 0x80 = negative/debuff flag
            ImVec4 borderColor = isBuff ? ImVec4(0.2f, 0.8f, 0.2f, 0.9f) : ImVec4(0.8f, 0.2f, 0.2f, 0.9f);

            // Try to get spell icon
            VkDescriptorSet iconTex = VK_NULL_HANDLE;
            if (assetMgr) {
                iconTex = getSpellIcon(aura.spellId, assetMgr);
            }

            if (iconTex) {
                ImGui::PushStyleColor(ImGuiCol_Button, borderColor);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                ImGui::ImageButton("##aura",
                    (ImTextureID)(uintptr_t)iconTex,
                    ImVec2(ICON_SIZE - 4, ICON_SIZE - 4));
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, borderColor);
                char label[8];
                snprintf(label, sizeof(label), "%u", aura.spellId);
                ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
                ImGui::PopStyleColor();
            }

            // Compute remaining duration once (shared by overlay and tooltip)
            uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            int32_t remainMs = aura.getRemainingMs(nowMs);

            // Duration countdown overlay — always visible on the icon bottom
            if (remainMs > 0) {
                ImVec2 iconMin = ImGui::GetItemRectMin();
                ImVec2 iconMax = ImGui::GetItemRectMax();
                char timeStr[12];
                int secs = (remainMs + 999) / 1000;  // ceiling seconds
                if (secs >= 3600)
                    snprintf(timeStr, sizeof(timeStr), "%dh", secs / 3600);
                else if (secs >= 60)
                    snprintf(timeStr, sizeof(timeStr), "%d:%02d", secs / 60, secs % 60);
                else
                    snprintf(timeStr, sizeof(timeStr), "%d", secs);
                ImVec2 textSize = ImGui::CalcTextSize(timeStr);
                float cx = iconMin.x + (iconMax.x - iconMin.x - textSize.x) * 0.5f;
                float cy = iconMax.y - textSize.y - 2.0f;
                // Drop shadow for readability over any icon colour
                ImGui::GetWindowDrawList()->AddText(ImVec2(cx + 1, cy + 1),
                    IM_COL32(0, 0, 0, 200), timeStr);
                ImGui::GetWindowDrawList()->AddText(ImVec2(cx, cy),
                    IM_COL32(255, 255, 255, 255), timeStr);
            }

            // Stack / charge count overlay — upper-left corner of the icon
            if (aura.charges > 1) {
                ImVec2 iconMin = ImGui::GetItemRectMin();
                char chargeStr[8];
                snprintf(chargeStr, sizeof(chargeStr), "%u", static_cast<unsigned>(aura.charges));
                // Drop shadow then bright yellow text
                ImGui::GetWindowDrawList()->AddText(ImVec2(iconMin.x + 3, iconMin.y + 3),
                    IM_COL32(0, 0, 0, 200), chargeStr);
                ImGui::GetWindowDrawList()->AddText(ImVec2(iconMin.x + 2, iconMin.y + 2),
                    IM_COL32(255, 220, 50, 255), chargeStr);
            }

            // Right-click to cancel buffs / dismount
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                if (gameHandler.isMounted()) {
                    gameHandler.dismount();
                } else if (isBuff) {
                    gameHandler.cancelAura(aura.spellId);
                }
            }

            // Tooltip with spell name and countdown
            if (ImGui::IsItemHovered()) {
                std::string name = spellbookScreen.lookupSpellName(aura.spellId, assetMgr);
                if (name.empty()) name = "Spell #" + std::to_string(aura.spellId);
                if (remainMs > 0) {
                    int seconds = remainMs / 1000;
                    if (seconds < 60) {
                        ImGui::SetTooltip("%s (%ds)", name.c_str(), seconds);
                    } else {
                        ImGui::SetTooltip("%s (%dm %ds)", name.c_str(), seconds / 60, seconds % 60);
                    }
                } else {
                    ImGui::SetTooltip("%s", name.c_str());
                }
            }

            ImGui::PopID();
            shown++;
        }
        // Dismiss Pet button
        if (gameHandler.hasPet()) {
            if (shown > 0) ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.2f, 0.2f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.3f, 0.3f, 1.0f));
            if (ImGui::Button("Dismiss Pet", ImVec2(-1, 0))) {
                gameHandler.dismissPet();
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ============================================================
// Loot Window (Phase 5)
// ============================================================

void GameScreen::renderLootWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isLootWindowOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 150, 200), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);

    bool open = true;
    if (ImGui::Begin("Loot", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& loot = gameHandler.getCurrentLoot();

        // Gold
        if (loot.gold > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%ug %us %uc",
                               loot.getGold(), loot.getSilver(), loot.getCopper());
            ImGui::Separator();
        }

        // Items with icons and labels
        constexpr float iconSize = 32.0f;
        int lootSlotClicked = -1;  // defer loot pickup to avoid iterator invalidation
        for (const auto& item : loot.items) {
            ImGui::PushID(item.slotIndex);

            // Get item info for name and quality
            const auto* info = gameHandler.getItemInfo(item.itemId);
            std::string itemName;
            game::ItemQuality quality = game::ItemQuality::COMMON;
            if (info && !info->name.empty()) {
                itemName = info->name;
                quality = static_cast<game::ItemQuality>(info->quality);
            } else {
                itemName = "Item #" + std::to_string(item.itemId);
            }
            ImVec4 qColor = InventoryScreen::getQualityColor(quality);

            // Get item icon
            uint32_t displayId = item.displayInfoId;
            if (displayId == 0 && info) displayId = info->displayInfoId;
            VkDescriptorSet iconTex = inventoryScreen.getItemIcon(displayId);

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float rowH = std::max(iconSize, ImGui::GetTextLineHeight() * 2.0f);

            // Invisible selectable for click handling
            if (ImGui::Selectable("##loot", false, 0, ImVec2(0, rowH))) {
                lootSlotClicked = item.slotIndex;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                lootSlotClicked = item.slotIndex;
            }
            bool hovered = ImGui::IsItemHovered();

            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Draw hover highlight
            if (hovered) {
                drawList->AddRectFilled(cursor,
                    ImVec2(cursor.x + ImGui::GetContentRegionAvail().x + iconSize + 8.0f,
                           cursor.y + rowH),
                    IM_COL32(255, 255, 255, 30));
            }

            // Draw icon
            if (iconTex) {
                drawList->AddImage((ImTextureID)(uintptr_t)iconTex,
                    cursor, ImVec2(cursor.x + iconSize, cursor.y + iconSize));
                drawList->AddRect(cursor, ImVec2(cursor.x + iconSize, cursor.y + iconSize),
                    ImGui::ColorConvertFloat4ToU32(qColor));
            } else {
                drawList->AddRectFilled(cursor,
                    ImVec2(cursor.x + iconSize, cursor.y + iconSize),
                    IM_COL32(40, 40, 50, 200));
                drawList->AddRect(cursor, ImVec2(cursor.x + iconSize, cursor.y + iconSize),
                    IM_COL32(80, 80, 80, 200));
            }

            // Draw item name
            float textX = cursor.x + iconSize + 6.0f;
            float textY = cursor.y + 2.0f;
            drawList->AddText(ImVec2(textX, textY),
                ImGui::ColorConvertFloat4ToU32(qColor), itemName.c_str());

            // Draw count if > 1
            if (item.count > 1) {
                char countStr[32];
                snprintf(countStr, sizeof(countStr), "x%u", item.count);
                float countY = textY + ImGui::GetTextLineHeight();
                drawList->AddText(ImVec2(textX, countY), IM_COL32(200, 200, 200, 220), countStr);
            }

            ImGui::PopID();
        }

        // Process deferred loot pickup (after loop to avoid iterator invalidation)
        if (lootSlotClicked >= 0) {
            gameHandler.lootItem(static_cast<uint8_t>(lootSlotClicked));
        }

        if (loot.items.empty() && loot.gold == 0) {
            gameHandler.closeLoot();
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            gameHandler.closeLoot();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeLoot();
    }
}

// ============================================================
// Gossip Window (Phase 5)
// ============================================================

void GameScreen::renderGossipWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isGossipWindowOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 200, 150), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);

    bool open = true;
    if (ImGui::Begin("NPC Dialog", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& gossip = gameHandler.getCurrentGossip();

        // NPC name (from creature cache)
        auto npcEntity = gameHandler.getEntityManager().getEntity(gossip.npcGuid);
        if (npcEntity && npcEntity->getType() == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(npcEntity);
            if (!unit->getName().empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", unit->getName().c_str());
                ImGui::Separator();
            }
        }

        ImGui::Spacing();

        // Gossip option icons - matches WoW GossipOptionIcon enum
        static const char* gossipIcons[] = {
            "[Chat]",          // 0 = GOSSIP_ICON_CHAT
            "[Vendor]",        // 1 = GOSSIP_ICON_VENDOR
            "[Taxi]",          // 2 = GOSSIP_ICON_TAXI
            "[Trainer]",       // 3 = GOSSIP_ICON_TRAINER
            "[Interact]",      // 4 = GOSSIP_ICON_INTERACT_1
            "[Interact]",      // 5 = GOSSIP_ICON_INTERACT_2
            "[Banker]",        // 6 = GOSSIP_ICON_MONEY_BAG (banker)
            "[Chat]",          // 7 = GOSSIP_ICON_TALK
            "[Tabard]",        // 8 = GOSSIP_ICON_TABARD
            "[Battlemaster]",  // 9 = GOSSIP_ICON_BATTLE
            "[Option]",        // 10 = GOSSIP_ICON_DOT
        };

        // Default text for server-sent gossip option placeholders
        static const std::unordered_map<std::string, std::string> gossipPlaceholders = {
            {"GOSSIP_OPTION_BANKER", "I would like to check my deposit box."},
            {"GOSSIP_OPTION_AUCTIONEER", "I'd like to browse your auctions."},
            {"GOSSIP_OPTION_VENDOR", "I want to browse your goods."},
            {"GOSSIP_OPTION_TAXIVENDOR", "I'd like to fly."},
            {"GOSSIP_OPTION_TRAINER", "I seek training."},
            {"GOSSIP_OPTION_INNKEEPER", "Make this inn your home."},
            {"GOSSIP_OPTION_SPIRITGUIDE", "Return me to life."},
            {"GOSSIP_OPTION_SPIRITHEALER", "Bring me back to life."},
            {"GOSSIP_OPTION_STABLEPET", "I'd like to stable my pet."},
            {"GOSSIP_OPTION_ARMORER", "I need to repair my equipment."},
            {"GOSSIP_OPTION_GOSSIP", "What can you tell me?"},
            {"GOSSIP_OPTION_BATTLEFIELD", "I'd like to go to the battleground."},
            {"GOSSIP_OPTION_TABARDDESIGNER", "I want to create a guild tabard."},
            {"GOSSIP_OPTION_PETITIONER", "I want to create a guild."},
        };

        for (const auto& opt : gossip.options) {
            ImGui::PushID(static_cast<int>(opt.id));

            // Determine icon label - use text-based detection for shared icons
            const char* icon = (opt.icon < 11) ? gossipIcons[opt.icon] : "[Option]";
            if (opt.text == "GOSSIP_OPTION_AUCTIONEER") icon = "[Auctioneer]";
            else if (opt.text == "GOSSIP_OPTION_BANKER") icon = "[Banker]";
            else if (opt.text == "GOSSIP_OPTION_VENDOR") icon = "[Vendor]";
            else if (opt.text == "GOSSIP_OPTION_TRAINER") icon = "[Trainer]";
            else if (opt.text == "GOSSIP_OPTION_INNKEEPER") icon = "[Innkeeper]";
            else if (opt.text == "GOSSIP_OPTION_STABLEPET") icon = "[Stable Master]";
            else if (opt.text == "GOSSIP_OPTION_ARMORER") icon = "[Repair]";

            // Resolve placeholder text from server
            std::string displayText = opt.text;
            auto placeholderIt = gossipPlaceholders.find(displayText);
            if (placeholderIt != gossipPlaceholders.end()) {
                displayText = placeholderIt->second;
            }

            std::string processedText = replaceGenderPlaceholders(displayText, gameHandler);
            std::string label = std::string(icon) + " " + processedText;
            if (ImGui::Selectable(label.c_str())) {
                gameHandler.selectGossipOption(opt.id);
            }
            ImGui::PopID();
        }

        // Fallback: some spirit healers don't send gossip options.
        if (gossip.options.empty() && gameHandler.isPlayerGhost()) {
            bool isSpirit = false;
            if (npcEntity && npcEntity->getType() == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(npcEntity);
                std::string name = unit->getName();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                if (name.find("spirit healer") != std::string::npos ||
                    name.find("spirit guide") != std::string::npos) {
                    isSpirit = true;
                }
            }
            if (isSpirit) {
                if (ImGui::Selectable("[Spiritguide] Return to Graveyard")) {
                    gameHandler.activateSpiritHealer(gossip.npcGuid);
                    gameHandler.closeGossip();
                }
            }
        }

        // Quest items
        if (!gossip.quests.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "Quests:");
            for (size_t qi = 0; qi < gossip.quests.size(); qi++) {
                const auto& quest = gossip.quests[qi];
                ImGui::PushID(static_cast<int>(qi));
                char qlabel[256];
                snprintf(qlabel, sizeof(qlabel), "[%d] %s", quest.questLevel, quest.title.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
                if (ImGui::Selectable(qlabel)) {
                    gameHandler.selectGossipQuest(quest.questId);
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            gameHandler.closeGossip();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeGossip();
    }
}

// ============================================================
// Quest Details Window
// ============================================================

void GameScreen::renderQuestDetailsWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isQuestDetailsOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 225, screenH / 2 - 200), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Appearing);

    bool open = true;
    const auto& quest = gameHandler.getQuestDetails();
    std::string processedTitle = replaceGenderPlaceholders(quest.title, gameHandler);
    if (ImGui::Begin(processedTitle.c_str(), &open)) {
        // Quest description
        if (!quest.details.empty()) {
            std::string processedDetails = replaceGenderPlaceholders(quest.details, gameHandler);
            ImGui::TextWrapped("%s", processedDetails.c_str());
        }

        // Objectives
        if (!quest.objectives.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Objectives:");
            std::string processedObjectives = replaceGenderPlaceholders(quest.objectives, gameHandler);
            ImGui::TextWrapped("%s", processedObjectives.c_str());
        }

        // Rewards
        if (quest.rewardXp > 0 || quest.rewardMoney > 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Rewards:");
            if (quest.rewardXp > 0) {
                ImGui::Text("  %u experience", quest.rewardXp);
            }
            if (quest.rewardMoney > 0) {
                uint32_t gold = quest.rewardMoney / 10000;
                uint32_t silver = (quest.rewardMoney % 10000) / 100;
                uint32_t copper = quest.rewardMoney % 100;
                if (gold > 0) ImGui::Text("  %ug %us %uc", gold, silver, copper);
                else if (silver > 0) ImGui::Text("  %us %uc", silver, copper);
                else ImGui::Text("  %uc", copper);
            }
        }

        if (quest.suggestedPlayers > 1) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Suggested players: %u", quest.suggestedPlayers);
        }

        // Accept / Decline buttons
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float buttonW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Accept", ImVec2(buttonW, 0))) {
            gameHandler.acceptQuest();
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(buttonW, 0))) {
            gameHandler.declineQuest();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.declineQuest();
    }
}

// ============================================================
// Quest Request Items Window (turn-in progress check)
// ============================================================

void GameScreen::renderQuestRequestItemsWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isQuestRequestItemsOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 225, screenH / 2 - 200), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(450, 350), ImGuiCond_Appearing);

    bool open = true;
    const auto& quest = gameHandler.getQuestRequestItems();
    auto countItemInInventory = [&](uint32_t itemId) -> uint32_t {
        const auto& inv = gameHandler.getInventory();
        uint32_t total = 0;
        for (int i = 0; i < inv.getBackpackSize(); ++i) {
            const auto& slot = inv.getBackpackSlot(i);
            if (!slot.empty() && slot.item.itemId == itemId) total += slot.item.stackCount;
        }
        for (int bag = 0; bag < game::Inventory::NUM_BAG_SLOTS; ++bag) {
            int bagSize = inv.getBagSize(bag);
            for (int s = 0; s < bagSize; ++s) {
                const auto& slot = inv.getBagSlot(bag, s);
                if (!slot.empty() && slot.item.itemId == itemId) total += slot.item.stackCount;
            }
        }
        return total;
    };

    std::string processedTitle = replaceGenderPlaceholders(quest.title, gameHandler);
    if (ImGui::Begin(processedTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        if (!quest.completionText.empty()) {
            std::string processedCompletionText = replaceGenderPlaceholders(quest.completionText, gameHandler);
            ImGui::TextWrapped("%s", processedCompletionText.c_str());
        }

        // Required items
        if (!quest.requiredItems.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Required Items:");
            for (const auto& item : quest.requiredItems) {
                uint32_t have = countItemInInventory(item.itemId);
                bool enough = have >= item.count;
                auto* info = gameHandler.getItemInfo(item.itemId);
                const char* name = (info && info->valid) ? info->name.c_str() : nullptr;
                if (name && *name) {
                    ImGui::TextColored(enough ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                                       "  %s  %u/%u", name, have, item.count);
                } else {
                    ImGui::TextColored(enough ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f) : ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
                                       "  Item %u  %u/%u", item.itemId, have, item.count);
                }
            }
        }

        if (quest.requiredMoney > 0) {
            ImGui::Spacing();
            uint32_t g = quest.requiredMoney / 10000;
            uint32_t s = (quest.requiredMoney % 10000) / 100;
            uint32_t c = quest.requiredMoney % 100;
            ImGui::Text("Required money: %ug %us %uc", g, s, c);
        }

        // Complete / Cancel buttons
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float buttonW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Complete Quest", ImVec2(buttonW, 0))) {
            gameHandler.completeQuest();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonW, 0))) {
            gameHandler.closeQuestRequestItems();
        }

        if (!quest.isCompletable()) {
            ImGui::TextDisabled("Server flagged this quest as incomplete; completion will be server-validated.");
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeQuestRequestItems();
    }
}

// ============================================================
// Quest Offer Reward Window (choose reward)
// ============================================================

void GameScreen::renderQuestOfferRewardWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isQuestOfferRewardOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 225, screenH / 2 - 200), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Appearing);

    bool open = true;
    const auto& quest = gameHandler.getQuestOfferReward();
    static int selectedChoice = -1;

    // Auto-select if only one choice reward
    if (quest.choiceRewards.size() == 1 && selectedChoice == -1) {
        selectedChoice = 0;
    }

    std::string processedTitle = replaceGenderPlaceholders(quest.title, gameHandler);
    if (ImGui::Begin(processedTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        if (!quest.rewardText.empty()) {
            std::string processedRewardText = replaceGenderPlaceholders(quest.rewardText, gameHandler);
            ImGui::TextWrapped("%s", processedRewardText.c_str());
        }

        // Choice rewards (pick one)
        if (!quest.choiceRewards.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Choose a reward:");

            for (size_t i = 0; i < quest.choiceRewards.size(); ++i) {
                const auto& item = quest.choiceRewards[i];
                auto* info = gameHandler.getItemInfo(item.itemId);

                bool selected = (selectedChoice == static_cast<int>(i));

                // Get item icon if we have displayInfoId
                VkDescriptorSet iconTex = VK_NULL_HANDLE;
                if (info && info->valid && info->displayInfoId != 0) {
                    iconTex = inventoryScreen.getItemIcon(info->displayInfoId);
                }

                // Quality color
                ImVec4 qualityColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White (poor)
                if (info && info->valid) {
                    switch (info->quality) {
                        case 1: qualityColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break; // Common (white)
                        case 2: qualityColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); break; // Uncommon (green)
                        case 3: qualityColor = ImVec4(0.0f, 0.5f, 1.0f, 1.0f); break; // Rare (blue)
                        case 4: qualityColor = ImVec4(0.64f, 0.21f, 0.93f, 1.0f); break; // Epic (purple)
                        case 5: qualityColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); break; // Legendary (orange)
                    }
                }

                // Render item with icon + visible selectable label
                ImGui::PushID(static_cast<int>(i));
                std::string label;
                if (info && info->valid && !info->name.empty()) {
                    label = info->name;
                } else {
                    label = "Item " + std::to_string(item.itemId);
                }
                if (item.count > 1) {
                    label += " x" + std::to_string(item.count);
                }
                if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(0, 24))) {
                    selectedChoice = static_cast<int>(i);
                }
                if (ImGui::IsItemHovered() && iconTex) {
                    ImGui::SetTooltip("Reward option");
                }
                if (iconTex) {
                    ImGui::SameLine();
                    ImGui::Image((void*)(intptr_t)iconTex, ImVec2(18, 18));
                }
                ImGui::PopID();
            }
        }

        // Fixed rewards (always given)
        if (!quest.fixedRewards.empty()) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "You will also receive:");
            for (const auto& item : quest.fixedRewards) {
                auto* info = gameHandler.getItemInfo(item.itemId);
                if (info && info->valid)
                    ImGui::Text("  %s x%u", info->name.c_str(), item.count);
                else
                    ImGui::Text("  Item %u x%u", item.itemId, item.count);
            }
        }

        // Money / XP rewards
        if (quest.rewardXp > 0 || quest.rewardMoney > 0) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Rewards:");
            if (quest.rewardXp > 0)
                ImGui::Text("  %u experience", quest.rewardXp);
            if (quest.rewardMoney > 0) {
                uint32_t g = quest.rewardMoney / 10000;
                uint32_t s = (quest.rewardMoney % 10000) / 100;
                uint32_t c = quest.rewardMoney % 100;
                if (g > 0) ImGui::Text("  %ug %us %uc", g, s, c);
                else if (s > 0) ImGui::Text("  %us %uc", s, c);
                else ImGui::Text("  %uc", c);
            }
        }

        // Complete button
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        float buttonW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        bool canComplete = quest.choiceRewards.empty() || selectedChoice >= 0;
        if (!canComplete) ImGui::BeginDisabled();
        if (ImGui::Button("Complete Quest", ImVec2(buttonW, 0))) {
            uint32_t rewardIdx = 0;
            if (!quest.choiceRewards.empty() && selectedChoice >= 0 &&
                selectedChoice < static_cast<int>(quest.choiceRewards.size())) {
                // Server expects the original slot index from its fixed-size reward array.
                rewardIdx = quest.choiceRewards[static_cast<size_t>(selectedChoice)].choiceSlot;
            }
            gameHandler.chooseQuestReward(rewardIdx);
            selectedChoice = -1;
        }
        if (!canComplete) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonW, 0))) {
            gameHandler.closeQuestOfferReward();
            selectedChoice = -1;
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeQuestOfferReward();
        selectedChoice = -1;
    }
}

// ============================================================
// Vendor Window (Phase 5)
// ============================================================

void GameScreen::renderVendorWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isVendorWindowOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 200, 100), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::Begin("Vendor", &open)) {
        const auto& vendor = gameHandler.getVendorItems();

        // Show player money
        uint64_t money = gameHandler.getMoneyCopper();
        uint32_t mg = static_cast<uint32_t>(money / 10000);
        uint32_t ms = static_cast<uint32_t>((money / 100) % 100);
        uint32_t mc = static_cast<uint32_t>(money % 100);
        ImGui::Text("Your money: %ug %us %uc", mg, ms, mc);
        ImGui::Separator();

        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Right-click bag items to sell");
        ImGui::Separator();

        const auto& buyback = gameHandler.getBuybackItems();
        if (!buyback.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.0f, 1.0f), "Buy Back");
            if (ImGui::BeginTable("BuybackTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Buy", ImGuiTableColumnFlags_WidthFixed, 62.0f);
                ImGui::TableHeadersRow();
                // Show only the most recently sold item (LIFO).
                const int i = 0;
                const auto& entry = buyback[0];
                uint32_t sellPrice = entry.item.sellPrice;
                if (sellPrice == 0) {
                    if (auto* info = gameHandler.getItemInfo(entry.item.itemId); info && info->valid) {
                        sellPrice = info->sellPrice;
                    }
                }
                uint64_t price = static_cast<uint64_t>(sellPrice) *
                                 static_cast<uint64_t>(entry.count > 0 ? entry.count : 1);
                uint32_t g = static_cast<uint32_t>(price / 10000);
                uint32_t s = static_cast<uint32_t>((price / 100) % 100);
                uint32_t c = static_cast<uint32_t>(price % 100);
                bool canAfford = money >= price;

                ImGui::TableNextRow();
                ImGui::PushID(8000 + i);
                ImGui::TableSetColumnIndex(0);
                const char* name = entry.item.name.empty() ? "Unknown Item" : entry.item.name.c_str();
                if (entry.count > 1) {
                    ImGui::Text("%s x%u", name, entry.count);
                } else {
                    ImGui::Text("%s", name);
                }
                ImGui::TableSetColumnIndex(1);
                if (!canAfford) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::Text("%ug %us %uc", g, s, c);
                if (!canAfford) ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                if (!canAfford) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Buy Back##buyback_0")) {
                    gameHandler.buyBackItem(0);
                }
                if (!canAfford) ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::EndTable();
            }
            ImGui::Separator();
        }

        if (vendor.items.empty()) {
            ImGui::TextDisabled("This vendor has nothing for sale.");
        } else {
            if (ImGui::BeginTable("VendorTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Stock", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Buy", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableHeadersRow();

                // Quality colors (matching WoW)
                static const ImVec4 qualityColors[] = {
                    ImVec4(0.6f, 0.6f, 0.6f, 1.0f),  // 0 Poor (gray)
                    ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // 1 Common (white)
                    ImVec4(0.12f, 1.0f, 0.0f, 1.0f),  // 2 Uncommon (green)
                    ImVec4(0.0f, 0.44f, 0.87f, 1.0f), // 3 Rare (blue)
                    ImVec4(0.64f, 0.21f, 0.93f, 1.0f),// 4 Epic (purple)
                    ImVec4(1.0f, 0.5f, 0.0f, 1.0f),   // 5 Legendary (orange)
                };

                for (int vi = 0; vi < static_cast<int>(vendor.items.size()); ++vi) {
                    const auto& item = vendor.items[vi];
                    ImGui::TableNextRow();
                    ImGui::PushID(vi);

                    ImGui::TableSetColumnIndex(0);
                    auto* info = gameHandler.getItemInfo(item.itemId);
                    if (info && info->valid) {
                        uint32_t q = info->quality < 6 ? info->quality : 1;
                        ImGui::TextColored(qualityColors[q], "%s", info->name.c_str());
                        // Tooltip with stats on hover
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(qualityColors[q], "%s", info->name.c_str());
                            if (info->damageMax > 0.0f) {
                                ImGui::Text("%.0f - %.0f Damage", info->damageMin, info->damageMax);
                                if (info->delayMs > 0) {
                                    float speed = static_cast<float>(info->delayMs) / 1000.0f;
                                    float dps = ((info->damageMin + info->damageMax) * 0.5f) / speed;
                                    ImGui::Text("Speed %.2f", speed);
                                    ImGui::Text("%.1f damage per second", dps);
                                }
                            }
                            if (info->armor > 0) ImGui::Text("Armor: %d", info->armor);
                            if (info->stamina > 0) ImGui::Text("+%d Stamina", info->stamina);
                            if (info->strength > 0) ImGui::Text("+%d Strength", info->strength);
                            if (info->agility > 0) ImGui::Text("+%d Agility", info->agility);
                            if (info->intellect > 0) ImGui::Text("+%d Intellect", info->intellect);
                            if (info->spirit > 0) ImGui::Text("+%d Spirit", info->spirit);
                            ImGui::EndTooltip();
                        }
                    } else {
                        ImGui::Text("Item %u", item.itemId);
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (item.buyPrice == 0 && item.extendedCost != 0) {
                        // Token-only item (no gold cost)
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[Tokens]");
                    } else {
                        uint32_t g = item.buyPrice / 10000;
                        uint32_t s = (item.buyPrice / 100) % 100;
                        uint32_t c = item.buyPrice % 100;
                        bool canAfford = money >= item.buyPrice;
                        if (!canAfford) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        ImGui::Text("%ug %us %uc", g, s, c);
                        if (!canAfford) ImGui::PopStyleColor();
                    }

                    ImGui::TableSetColumnIndex(2);
                    if (item.maxCount < 0) {
                        ImGui::Text("Inf");
                    } else {
                        ImGui::Text("%d", item.maxCount);
                    }

                    ImGui::TableSetColumnIndex(3);
                    std::string buyBtnId = "Buy##vendor_" + std::to_string(vi);
                    if (ImGui::SmallButton(buyBtnId.c_str())) {
                        gameHandler.buyItem(vendor.vendorGuid, item.itemId, item.slot, 1);
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeVendor();
    }
}

// ============================================================
// Trainer
// ============================================================

void GameScreen::renderTrainerWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isTrainerWindowOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 225, 100), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::Begin("Trainer", &open)) {
        const auto& trainer = gameHandler.getTrainerSpells();

        // NPC name
        auto npcEntity = gameHandler.getEntityManager().getEntity(trainer.trainerGuid);
        if (npcEntity && npcEntity->getType() == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(npcEntity);
            if (!unit->getName().empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", unit->getName().c_str());
            }
        }

        // Greeting
        if (!trainer.greeting.empty()) {
            ImGui::TextWrapped("%s", trainer.greeting.c_str());
        }
        ImGui::Separator();

        // Player money
        uint64_t money = gameHandler.getMoneyCopper();
        uint32_t mg = static_cast<uint32_t>(money / 10000);
        uint32_t ms = static_cast<uint32_t>((money / 100) % 100);
        uint32_t mc = static_cast<uint32_t>(money % 100);
        ImGui::Text("Your money: %ug %us %uc", mg, ms, mc);

        // Filter checkbox
        static bool showUnavailable = false;
        ImGui::Checkbox("Show unavailable spells", &showUnavailable);
        ImGui::Separator();

        if (trainer.spells.empty()) {
            ImGui::TextDisabled("This trainer has nothing to teach you.");
        } else {
            // Known spells for checking
            const auto& knownSpells = gameHandler.getKnownSpells();
            auto isKnown = [&](uint32_t id) {
                if (id == 0) return true;
                // Check if spell is in knownSpells list
                bool found = knownSpells.count(id);
                if (found) return true;

                // Also check if spell is in trainer list with state=2 (explicitly known)
                // state=0 means unavailable (could be no prereqs, wrong level, etc.) - don't count as known
                for (const auto& ts : trainer.spells) {
                    if (ts.spellId == id && ts.state == 2) {
                        return true;
                    }
                }
                return false;
            };
            uint32_t playerLevel = gameHandler.getPlayerLevel();

            // Renders spell rows into the current table
            auto renderSpellRows = [&](const std::vector<const game::TrainerSpell*>& spells) {
                for (const auto* spell : spells) {
                    // Check prerequisites client-side first
                    bool prereq1Met = isKnown(spell->chainNode1);
                    bool prereq2Met = isKnown(spell->chainNode2);
                    bool prereq3Met = isKnown(spell->chainNode3);
                    bool prereqsMet = prereq1Met && prereq2Met && prereq3Met;
                    bool levelMet = (spell->reqLevel == 0 || playerLevel >= spell->reqLevel);
                    bool alreadyKnown = isKnown(spell->spellId);

                    // Dynamically determine effective state based on current prerequisites
                    // Server sends state, but we override if prerequisites are now met
                    uint8_t effectiveState = spell->state;
                    if (spell->state == 1 && prereqsMet && levelMet) {
                        // Server said unavailable, but we now meet all requirements
                        effectiveState = 0;  // Treat as available
                    }

                    // Filter: skip unavailable spells if checkbox is unchecked
                    // Use effectiveState so spells with newly met prereqs aren't filtered
                    if (!showUnavailable && effectiveState == 1) {
                        continue;
                    }

                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(spell->spellId));

                    ImVec4 color;
                    const char* statusLabel;
                    // WotLK trainer states: 0=available, 1=unavailable, 2=known
                    if (effectiveState == 2 || alreadyKnown) {
                        color = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
                        statusLabel = "Known";
                    } else if (effectiveState == 0) {
                        color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                        statusLabel = "Available";
                    } else {
                        color = ImVec4(0.6f, 0.3f, 0.3f, 1.0f);
                        statusLabel = "Unavailable";
                    }

                    // Spell name
                    ImGui::TableSetColumnIndex(0);
                    const std::string& name = gameHandler.getSpellName(spell->spellId);
                    const std::string& rank = gameHandler.getSpellRank(spell->spellId);
                    if (!name.empty()) {
                        if (!rank.empty())
                            ImGui::TextColored(color, "%s (%s)", name.c_str(), rank.c_str());
                        else
                            ImGui::TextColored(color, "%s", name.c_str());
                    } else {
                        ImGui::TextColored(color, "Spell #%u", spell->spellId);
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        if (!name.empty()) {
                            ImGui::Text("%s", name.c_str());
                            if (!rank.empty()) ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", rank.c_str());
                        }
                        ImGui::Text("Status: %s", statusLabel);
                        if (spell->reqLevel > 0) {
                            ImVec4 lvlColor = levelMet ? ImVec4(0.7f, 0.7f, 0.7f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                            ImGui::TextColored(lvlColor, "Required Level: %u", spell->reqLevel);
                        }
                        if (spell->reqSkill > 0) ImGui::Text("Required Skill: %u (value %u)", spell->reqSkill, spell->reqSkillValue);
                        auto showPrereq = [&](uint32_t node) {
                            if (node == 0) return;
                            bool met = isKnown(node);
                            const std::string& pname = gameHandler.getSpellName(node);
                            ImVec4 pcolor = met ? ImVec4(0.3f, 0.9f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                            if (!pname.empty())
                                ImGui::TextColored(pcolor, "Requires: %s%s", pname.c_str(), met ? " (known)" : "");
                            else
                                ImGui::TextColored(pcolor, "Requires: Spell #%u%s", node, met ? " (known)" : "");
                        };
                        showPrereq(spell->chainNode1);
                        showPrereq(spell->chainNode2);
                        showPrereq(spell->chainNode3);
                        ImGui::EndTooltip();
                    }

                    // Level
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(color, "%u", spell->reqLevel);

                    // Cost
                    ImGui::TableSetColumnIndex(2);
                    if (spell->spellCost > 0) {
                        uint32_t g = spell->spellCost / 10000;
                        uint32_t s = (spell->spellCost / 100) % 100;
                        uint32_t c = spell->spellCost % 100;
                        bool canAfford = money >= spell->spellCost;
                        ImVec4 costColor = canAfford ? color : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        ImGui::TextColored(costColor, "%ug %us %uc", g, s, c);
                    } else {
                        ImGui::TextColored(color, "Free");
                    }

                    // Train button - only enabled if available, affordable, prereqs met
                    ImGui::TableSetColumnIndex(3);
                    // Use effectiveState so newly available spells (after learning prereqs) can be trained
                    bool canTrain = !alreadyKnown && effectiveState == 0
                                  && prereqsMet && levelMet
                                  && (money >= spell->spellCost);

                    // Debug logging for first 3 spells to see why buttons are disabled
                    static int logCount = 0;
                    static uint64_t lastTrainerGuid = 0;
                    if (trainer.trainerGuid != lastTrainerGuid) {
                        logCount = 0;
                        lastTrainerGuid = trainer.trainerGuid;
                    }
                    if (logCount < 3) {
                        LOG_INFO("Trainer button debug: spellId=", spell->spellId,
                                " alreadyKnown=", alreadyKnown, " state=", (int)spell->state,
                                " prereqsMet=", prereqsMet, " (", prereq1Met, ",", prereq2Met, ",", prereq3Met, ")",
                                " levelMet=", levelMet,
                                " reqLevel=", spell->reqLevel, " playerLevel=", playerLevel,
                                " chain1=", spell->chainNode1, " chain2=", spell->chainNode2, " chain3=", spell->chainNode3,
                                " canAfford=", (money >= spell->spellCost),
                                " canTrain=", canTrain);
                        logCount++;
                    }

                    if (!canTrain) ImGui::BeginDisabled();
                    if (ImGui::SmallButton("Train")) {
                        gameHandler.trainSpell(spell->spellId);
                    }
                    if (!canTrain) ImGui::EndDisabled();

                    ImGui::PopID();
                }
            };

            auto renderSpellTable = [&](const char* tableId, const std::vector<const game::TrainerSpell*>& spells) {
                if (ImGui::BeginTable(tableId, 4,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Spell", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                    ImGui::TableHeadersRow();
                    renderSpellRows(spells);
                    ImGui::EndTable();
                }
            };

            const auto& tabs = gameHandler.getTrainerTabs();
            if (tabs.size() > 1) {
                // Multiple tabs - show tab bar
                if (ImGui::BeginTabBar("TrainerTabs")) {
                    for (size_t i = 0; i < tabs.size(); i++) {
                        char tabLabel[64];
                        snprintf(tabLabel, sizeof(tabLabel), "%s (%zu)",
                            tabs[i].name.c_str(), tabs[i].spells.size());

                        if (ImGui::BeginTabItem(tabLabel)) {
                            char tableId[32];
                            snprintf(tableId, sizeof(tableId), "TT%zu", i);
                            renderSpellTable(tableId, tabs[i].spells);
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            } else {
                // Single tab or no categorization - flat list
                std::vector<const game::TrainerSpell*> allSpells;
                allSpells.reserve(trainer.spells.size());
                for (const auto& spell : trainer.spells) {
                    allSpells.push_back(&spell);
                }
                renderSpellTable("TrainerTable", allSpells);
            }
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeTrainer();
    }
}

// ============================================================
// Teleporter Panel
// ============================================================

// ============================================================
// Escape Menu
// ============================================================

void GameScreen::renderEscapeMenu() {
    if (!showEscapeMenu) return;

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;
    ImVec2 size(260.0f, 220.0f);
    ImVec2 pos((screenW - size.x) * 0.5f, (screenH - size.y) * 0.5f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##EscapeMenu", nullptr, flags)) {
        ImGui::Text("Game Menu");
        ImGui::Separator();

        if (ImGui::Button("Logout", ImVec2(-1, 0))) {
            core::Application::getInstance().logoutToLogin();
            showEscapeMenu = false;
            showEscapeSettingsNotice = false;
        }
        if (ImGui::Button("Quit", ImVec2(-1, 0))) {
            auto* renderer = core::Application::getInstance().getRenderer();
            if (renderer) {
                if (auto* music = renderer->getMusicManager()) {
                    music->stopMusic(0.0f);
                }
            }
            core::Application::getInstance().shutdown();
        }
        if (ImGui::Button("Settings", ImVec2(-1, 0))) {
            showEscapeSettingsNotice = false;
            showSettingsWindow = true;
            settingsInit = false;
            showEscapeMenu = false;
        }
        if (ImGui::Button("Instance Lockouts", ImVec2(-1, 0))) {
            showInstanceLockouts_ = true;
            showEscapeMenu = false;
        }

        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 10.0f));
        if (ImGui::Button("Back to Game", ImVec2(-1, 0))) {
            showEscapeMenu = false;
            showEscapeSettingsNotice = false;
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

// ============================================================
// Taxi Window
// ============================================================

void GameScreen::renderTaxiWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isTaxiWindowOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 200, 150), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Always);

    bool open = true;
    if (ImGui::Begin("Flight Master", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& taxiData = gameHandler.getTaxiData();
        const auto& nodes = gameHandler.getTaxiNodes();
        uint32_t currentNode = gameHandler.getTaxiCurrentNode();

        // Get current node's map to filter destinations
        uint32_t currentMapId = 0;
        auto curIt = nodes.find(currentNode);
        if (curIt != nodes.end()) {
            currentMapId = curIt->second.mapId;
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Current: %s", curIt->second.name.c_str());
            ImGui::Separator();
        }

        ImGui::Text("Select a destination:");
        ImGui::Spacing();

        static uint32_t selectedNodeId = 0;
        int destCount = 0;
        if (ImGui::BeginTable("TaxiNodes", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Destination", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Cost", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (const auto& [nodeId, node] : nodes) {
                if (nodeId == currentNode) continue;
                if (node.mapId != currentMapId) continue;
                if (!taxiData.isNodeKnown(nodeId)) continue;

                uint32_t costCopper = gameHandler.getTaxiCostTo(nodeId);
                uint32_t gold = costCopper / 10000;
                uint32_t silver = (costCopper / 100) % 100;
                uint32_t copper = costCopper % 100;

                ImGui::PushID(static_cast<int>(nodeId));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                bool isSelected = (selectedNodeId == nodeId);
                if (ImGui::Selectable(node.name.c_str(), isSelected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedNodeId = nodeId;
                    LOG_INFO("Taxi UI: Selected dest=", nodeId);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        LOG_INFO("Taxi UI: Double-click activate dest=", nodeId);
                        gameHandler.activateTaxi(nodeId);
                    }
                }

                ImGui::TableSetColumnIndex(1);
                if (gold > 0) {
                    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "%ug %us %uc", gold, silver, copper);
                } else if (silver > 0) {
                    ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%us %uc", silver, copper);
                } else {
                    ImGui::TextColored(ImVec4(0.72f, 0.45f, 0.2f, 1.0f), "%uc", copper);
                }

                ImGui::TableSetColumnIndex(2);
                if (ImGui::SmallButton("Fly")) {
                    selectedNodeId = nodeId;
                    LOG_INFO("Taxi UI: Fly clicked dest=", nodeId);
                    gameHandler.activateTaxi(nodeId);
                }

                ImGui::PopID();
                destCount++;
            }
            ImGui::EndTable();
        }

        if (destCount == 0) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No destinations available.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (selectedNodeId != 0 && ImGui::Button("Fly Selected", ImVec2(-1, 0))) {
            LOG_INFO("Taxi UI: Fly Selected dest=", selectedNodeId);
            gameHandler.activateTaxi(selectedNodeId);
        }
        if (ImGui::Button("Close", ImVec2(-1, 0))) {
            gameHandler.closeTaxi();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeTaxi();
    }
}

// ============================================================
// Death Screen
// ============================================================

void GameScreen::renderDeathScreen(game::GameHandler& gameHandler) {
    if (!gameHandler.showDeathDialog()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Dark red overlay covering the whole screen
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.0f, 0.0f, 0.45f));
    ImGui::Begin("##DeathOverlay", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::End();
    ImGui::PopStyleColor();

    // "Release Spirit" dialog centered on screen
    float dlgW = 280.0f;
    float dlgH = 100.0f;
    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - dlgW / 2, screenH * 0.35f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.0f, 0.0f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));

    if (ImGui::Begin("##DeathDialog", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {

        ImGui::Spacing();
        // Center "You are dead." text
        const char* deathText = "You are dead.";
        float textW = ImGui::CalcTextSize(deathText).x;
        ImGui::SetCursorPosX((dlgW - textW) / 2);
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", deathText);

        ImGui::Spacing();
        ImGui::Spacing();

        // Center the Release Spirit button
        float btnW = 180.0f;
        ImGui::SetCursorPosX((dlgW - btnW) / 2);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Release Spirit", ImVec2(btnW, 30))) {
            gameHandler.releaseSpirit();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GameScreen::renderReclaimCorpseButton(game::GameHandler& gameHandler) {
    if (!gameHandler.isPlayerGhost() || !gameHandler.canReclaimCorpse()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    float btnW = 220.0f, btnH = 36.0f;
    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - btnW / 2, screenH * 0.72f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(btnW + 16.0f, btnH + 16.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.7f));
    if (ImGui::Begin("##ReclaimCorpse", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.15f, 0.35f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.25f, 1.0f));
        if (ImGui::Button("Resurrect from Corpse", ImVec2(btnW, btnH))) {
            gameHandler.reclaimCorpse();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void GameScreen::renderResurrectDialog(game::GameHandler& gameHandler) {
    if (!gameHandler.showResurrectDialog()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    float dlgW = 300.0f;
    float dlgH = 110.0f;
    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - dlgW / 2, screenH * 0.3f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.8f, 1.0f));

    if (ImGui::Begin("##ResurrectDialog", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {

        ImGui::Spacing();
        const std::string& casterName = gameHandler.getResurrectCasterName();
        std::string text = casterName.empty()
            ? "Return to life?"
            : casterName + " wishes to resurrect you.";
        float textW = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::SetCursorPosX(std::max(4.0f, (dlgW - textW) / 2));
        ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s", text.c_str());

        ImGui::Spacing();
        ImGui::Spacing();

        float btnW = 100.0f;
        float spacing = 20.0f;
        ImGui::SetCursorPosX((dlgW - btnW * 2 - spacing) / 2);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Accept", ImVec2(btnW, 30))) {
            gameHandler.acceptResurrect();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, spacing);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Decline", ImVec2(btnW, 30))) {
            gameHandler.declineResurrect();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ============================================================
// Talent Wipe Confirm Dialog
// ============================================================

void GameScreen::renderTalentWipeConfirmDialog(game::GameHandler& gameHandler) {
    if (!gameHandler.showTalentWipeConfirmDialog()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    float dlgW = 340.0f;
    float dlgH = 130.0f;
    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - dlgW / 2, screenH * 0.3f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(dlgW, dlgH), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.1f, 0.1f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.8f, 0.7f, 0.2f, 1.0f));

    if (ImGui::Begin("##TalentWipeDialog", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {

        ImGui::Spacing();
        uint32_t cost = gameHandler.getTalentWipeCost();
        uint32_t gold = cost / 10000;
        uint32_t silver = (cost % 10000) / 100;
        uint32_t copper = cost % 100;
        char costStr[64];
        if (gold > 0)
            std::snprintf(costStr, sizeof(costStr), "%ug %us %uc", gold, silver, copper);
        else if (silver > 0)
            std::snprintf(costStr, sizeof(costStr), "%us %uc", silver, copper);
        else
            std::snprintf(costStr, sizeof(costStr), "%uc", copper);

        std::string text = "Reset your talents for ";
        text += costStr;
        text += "?";
        float textW = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::SetCursorPosX(std::max(4.0f, (dlgW - textW) / 2));
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s", text.c_str());

        ImGui::Spacing();
        ImGui::SetCursorPosX(8.0f);
        ImGui::TextDisabled("All talent points will be refunded.");
        ImGui::Spacing();

        float btnW = 110.0f;
        float spacing = 20.0f;
        ImGui::SetCursorPosX((dlgW - btnW * 2 - spacing) / 2);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Confirm", ImVec2(btnW, 30))) {
            gameHandler.confirmTalentWipe();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, spacing);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.3f, 0.3f, 1.0f));
        if (ImGui::Button("Cancel", ImVec2(btnW, 30))) {
            gameHandler.cancelTalentWipe();
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

// ============================================================
// Settings Window
// ============================================================

void GameScreen::renderSettingsWindow() {
    if (!showSettingsWindow) return;

    auto* window = core::Application::getInstance().getWindow();
    auto* renderer = core::Application::getInstance().getRenderer();
    if (!window) return;

    static const int kResolutions[][2] = {
        {1280, 720},
        {1600, 900},
        {1920, 1080},
        {2560, 1440},
        {3840, 2160},
    };
    static const int kResCount = sizeof(kResolutions) / sizeof(kResolutions[0]);
    constexpr int kDefaultResW = 1920;
    constexpr int kDefaultResH = 1080;
    constexpr bool kDefaultFullscreen = false;
    constexpr bool kDefaultVsync = true;
    constexpr bool kDefaultShadows = true;
    constexpr int kDefaultMusicVolume = 30;
    constexpr float kDefaultMouseSensitivity = 0.2f;
    constexpr bool kDefaultInvertMouse = false;
    constexpr int kDefaultGroundClutterDensity = 100;

    int defaultResIndex = 0;
    for (int i = 0; i < kResCount; i++) {
        if (kResolutions[i][0] == kDefaultResW && kResolutions[i][1] == kDefaultResH) {
            defaultResIndex = i;
            break;
        }
    }

    if (!settingsInit) {
        pendingFullscreen = window->isFullscreen();
        pendingVsync = window->isVsyncEnabled();
        if (renderer) {
            renderer->setShadowsEnabled(pendingShadows);
            renderer->setShadowDistance(pendingShadowDistance);
            // Read non-volume settings from actual state (volumes come from saved settings)
            if (auto* cameraController = renderer->getCameraController()) {
                pendingMouseSensitivity = cameraController->getMouseSensitivity();
                pendingInvertMouse = cameraController->isInvertMouse();
                cameraController->setExtendedZoom(pendingExtendedZoom);
            }
        }
        pendingResIndex = 0;
        int curW = window->getWidth();
        int curH = window->getHeight();
        for (int i = 0; i < kResCount; i++) {
            if (kResolutions[i][0] == curW && kResolutions[i][1] == curH) {
                pendingResIndex = i;
                break;
            }
        }
        pendingUiOpacity = static_cast<int>(std::lround(uiOpacity_ * 100.0f));
        pendingMinimapRotate = minimapRotate_;
        pendingMinimapSquare = minimapSquare_;
        pendingMinimapNpcDots = minimapNpcDots_;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setRotateWithCamera(minimapRotate_);
                minimap->setSquareShape(minimapSquare_);
            }
            if (auto* zm = renderer->getZoneManager()) {
                pendingUseOriginalSoundtrack = zm->getUseOriginalSoundtrack();
            }
        }
        settingsInit = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;
    ImVec2 size(520.0f, std::min(screenH * 0.9f, 720.0f));
    ImVec2 pos((screenW - size.x) * 0.5f, (screenH - size.y) * 0.5f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##SettingsWindow", nullptr, flags)) {
        ImGui::Text("Settings");
        ImGui::Separator();

        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            // ============================================================
            // VIDEO TAB
            // ============================================================
            if (ImGui::BeginTabItem("Video")) {
                ImGui::Spacing();

                if (ImGui::Checkbox("Fullscreen", &pendingFullscreen)) {
                    window->setFullscreen(pendingFullscreen);
                    saveSettings();
                }
                if (ImGui::Checkbox("VSync", &pendingVsync)) {
                    window->setVsync(pendingVsync);
                    saveSettings();
                }
                if (ImGui::Checkbox("Shadows", &pendingShadows)) {
                    if (renderer) renderer->setShadowsEnabled(pendingShadows);
                    saveSettings();
                }
                if (pendingShadows) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::SliderFloat("Distance##shadow", &pendingShadowDistance, 40.0f, 500.0f, "%.0f")) {
                        if (renderer) renderer->setShadowDistance(pendingShadowDistance);
                        saveSettings();
                    }
                }
                {
                    bool fsrActive = renderer && (renderer->isFSREnabled() || renderer->isFSR2Enabled());
                    if (!fsrActive && pendingWaterRefraction) {
                        // FSR was disabled while refraction was on — auto-disable
                        pendingWaterRefraction = false;
                        if (renderer) renderer->setWaterRefractionEnabled(false);
                    }
                    if (!fsrActive) ImGui::BeginDisabled();
                    if (ImGui::Checkbox("Water Refraction (requires FSR)", &pendingWaterRefraction)) {
                        if (renderer) renderer->setWaterRefractionEnabled(pendingWaterRefraction);
                        saveSettings();
                    }
                    if (!fsrActive) ImGui::EndDisabled();
                }
                {
                    const char* aaLabels[] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
                    bool fsr2Active = renderer && renderer->isFSR2Enabled();
                    if (fsr2Active) {
                        ImGui::BeginDisabled();
                        int disabled = 0;
                        ImGui::Combo("Anti-Aliasing (FSR3)", &disabled, "Off (FSR3 active)\0", 1);
                        ImGui::EndDisabled();
                    } else if (ImGui::Combo("Anti-Aliasing", &pendingAntiAliasing, aaLabels, 4)) {
                        static const VkSampleCountFlagBits aaSamples[] = {
                            VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                            VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
                        };
                        if (renderer) renderer->setMsaaSamples(aaSamples[pendingAntiAliasing]);
                        saveSettings();
                    }
                }
                // FSR Upscaling
                {
                    // FSR mode selection: Off, FSR 1.0 (Spatial), FSR 3.x (Temporal)
                    const char* fsrModeLabels[] = { "Off", "FSR 1.0 (Spatial)", "FSR 3.x (Temporal)" };
                    int fsrMode = pendingUpscalingMode;
                    if (ImGui::Combo("Upscaling", &fsrMode, fsrModeLabels, 3)) {
                        pendingUpscalingMode = fsrMode;
                        pendingFSR = (fsrMode == 1);
                        if (renderer) {
                            renderer->setFSREnabled(fsrMode == 1);
                            renderer->setFSR2Enabled(fsrMode == 2);
                        }
                        saveSettings();
                    }
                    if (fsrMode > 0) {
                        if (fsrMode == 2 && renderer) {
                            ImGui::TextDisabled("FSR3 backend: %s",
                                renderer->isAmdFsr2SdkAvailable() ? "AMD FidelityFX SDK" : "Internal fallback");
                            if (renderer->isAmdFsr3FramegenSdkAvailable()) {
                                if (ImGui::Checkbox("AMD FSR3 Frame Generation (Experimental)", &pendingAMDFramegen)) {
                                    renderer->setAmdFsr3FramegenEnabled(pendingAMDFramegen);
                                    saveSettings();
                                }
                                const char* runtimeStatus = "Unavailable";
                                if (renderer->isAmdFsr3FramegenRuntimeActive()) {
                                    runtimeStatus = "Active";
                                } else if (renderer->isAmdFsr3FramegenRuntimeReady()) {
                                    runtimeStatus = "Ready";
                                } else {
                                    runtimeStatus = "Unavailable";
                                }
                                ImGui::TextDisabled("Runtime: %s (%s)",
                                    runtimeStatus, renderer->getAmdFsr3FramegenRuntimePath());
                                if (!renderer->isAmdFsr3FramegenRuntimeReady()) {
                                    const std::string& runtimeErr = renderer->getAmdFsr3FramegenRuntimeError();
                                    if (!runtimeErr.empty()) {
                                        ImGui::TextDisabled("Reason: %s", runtimeErr.c_str());
                                    }
                                }
                            } else {
                                ImGui::BeginDisabled();
                                bool disabledFg = false;
                                ImGui::Checkbox("AMD FSR3 Frame Generation (Experimental)", &disabledFg);
                                ImGui::EndDisabled();
                                ImGui::TextDisabled("Requires FidelityFX-SDK framegen headers.");
                            }
                        }
                        const char* fsrQualityLabels[] = { "Native (100%)", "Ultra Quality (77%)", "Quality (67%)", "Balanced (59%)" };
                        static const float fsrScaleFactors[] = { 0.77f, 0.67f, 0.59f, 1.00f };
                        static const int displayToInternal[] = { 3, 0, 1, 2 };
                        pendingFSRQuality = std::clamp(pendingFSRQuality, 0, 3);
                        int fsrQualityDisplay = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (displayToInternal[i] == pendingFSRQuality) {
                                fsrQualityDisplay = i;
                                break;
                            }
                        }
                        if (ImGui::Combo("FSR Quality", &fsrQualityDisplay, fsrQualityLabels, 4)) {
                            pendingFSRQuality = displayToInternal[fsrQualityDisplay];
                            if (renderer) renderer->setFSRQuality(fsrScaleFactors[pendingFSRQuality]);
                            saveSettings();
                        }
                        if (ImGui::SliderFloat("FSR Sharpness", &pendingFSRSharpness, 0.0f, 2.0f, "%.1f")) {
                            if (renderer) renderer->setFSRSharpness(pendingFSRSharpness);
                            saveSettings();
                        }
                        if (fsrMode == 2) {
                            ImGui::SeparatorText("FSR3 Tuning");
                            if (ImGui::SliderFloat("Jitter Sign", &pendingFSR2JitterSign, -2.0f, 2.0f, "%.2f")) {
                                if (renderer) {
                                    renderer->setFSR2DebugTuning(
                                        pendingFSR2JitterSign,
                                        pendingFSR2MotionVecScaleX,
                                        pendingFSR2MotionVecScaleY);
                                }
                                saveSettings();
                            }
                            ImGui::TextDisabled("Tip: 0.38 is the current recommended default.");
                        }
                    }
                }
                if (ImGui::SliderInt("Ground Clutter Density", &pendingGroundClutterDensity, 0, 150, "%d%%")) {
                    if (renderer) {
                        if (auto* tm = renderer->getTerrainManager()) {
                            tm->setGroundClutterDensityScale(static_cast<float>(pendingGroundClutterDensity) / 100.0f);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("Normal Mapping", &pendingNormalMapping)) {
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setNormalMappingEnabled(pendingNormalMapping);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setNormalMappingEnabled(pendingNormalMapping);
                        }
                    }
                    saveSettings();
                }
                if (pendingNormalMapping) {
                    if (ImGui::SliderFloat("Normal Map Strength", &pendingNormalMapStrength, 0.0f, 2.0f, "%.1f")) {
                        if (renderer) {
                            if (auto* wr = renderer->getWMORenderer()) {
                                wr->setNormalMapStrength(pendingNormalMapStrength);
                            }
                            if (auto* cr = renderer->getCharacterRenderer()) {
                                cr->setNormalMapStrength(pendingNormalMapStrength);
                            }
                        }
                        saveSettings();
                    }
                }
                if (ImGui::Checkbox("Parallax Mapping", &pendingPOM)) {
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setPOMEnabled(pendingPOM);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setPOMEnabled(pendingPOM);
                        }
                    }
                    saveSettings();
                }
                if (pendingPOM) {
                    const char* pomLabels[] = { "Low", "Medium", "High" };
                    if (ImGui::Combo("Parallax Quality", &pendingPOMQuality, pomLabels, 3)) {
                        if (renderer) {
                            if (auto* wr = renderer->getWMORenderer()) {
                                wr->setPOMQuality(pendingPOMQuality);
                            }
                            if (auto* cr = renderer->getCharacterRenderer()) {
                                cr->setPOMQuality(pendingPOMQuality);
                            }
                        }
                        saveSettings();
                    }
                }

                const char* resLabel = "Resolution";
                const char* resItems[kResCount];
                char resBuf[kResCount][16];
                for (int i = 0; i < kResCount; i++) {
                    snprintf(resBuf[i], sizeof(resBuf[i]), "%dx%d", kResolutions[i][0], kResolutions[i][1]);
                    resItems[i] = resBuf[i];
                }
                if (ImGui::Combo(resLabel, &pendingResIndex, resItems, kResCount)) {
                    window->applyResolution(kResolutions[pendingResIndex][0], kResolutions[pendingResIndex][1]);
                    saveSettings();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restore Video Defaults", ImVec2(-1, 0))) {
                    pendingFullscreen = kDefaultFullscreen;
                    pendingVsync = kDefaultVsync;
                    pendingShadows = kDefaultShadows;
                    pendingShadowDistance = 300.0f;
                    pendingGroundClutterDensity = kDefaultGroundClutterDensity;
                    pendingAntiAliasing = 0;
                    pendingNormalMapping = true;
                    pendingNormalMapStrength = 0.8f;
                    pendingPOM = true;
                    pendingPOMQuality = 1;
                    pendingResIndex = defaultResIndex;
                    window->setFullscreen(pendingFullscreen);
                    window->setVsync(pendingVsync);
                    window->applyResolution(kResolutions[pendingResIndex][0], kResolutions[pendingResIndex][1]);
                    pendingWaterRefraction = false;
                    if (renderer) {
                        renderer->setShadowsEnabled(pendingShadows);
                        renderer->setShadowDistance(pendingShadowDistance);
                    }
                    if (renderer) renderer->setWaterRefractionEnabled(pendingWaterRefraction);
                    if (renderer) renderer->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
                    if (renderer) {
                        if (auto* tm = renderer->getTerrainManager()) {
                            tm->setGroundClutterDensityScale(static_cast<float>(pendingGroundClutterDensity) / 100.0f);
                        }
                    }
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setNormalMappingEnabled(pendingNormalMapping);
                            wr->setNormalMapStrength(pendingNormalMapStrength);
                            wr->setPOMEnabled(pendingPOM);
                            wr->setPOMQuality(pendingPOMQuality);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setNormalMappingEnabled(pendingNormalMapping);
                            cr->setNormalMapStrength(pendingNormalMapStrength);
                            cr->setPOMEnabled(pendingPOM);
                            cr->setPOMQuality(pendingPOMQuality);
                        }
                    }
                    saveSettings();
                }

                ImGui::EndTabItem();
            }

            // ============================================================
            // INTERFACE TAB
            // ============================================================
            if (ImGui::BeginTabItem("Interface")) {
                ImGui::Spacing();
                ImGui::BeginChild("InterfaceSettings", ImVec2(0, 360), true);

                ImGui::SeparatorText("Action Bars");
                ImGui::Spacing();

                if (ImGui::Checkbox("Show Second Action Bar", &pendingShowActionBar2)) {
                    saveSettings();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(Shift+1 through Shift+=)");

                if (pendingShowActionBar2) {
                    ImGui::Spacing();
                    ImGui::TextUnformatted("Second Bar Position Offset");
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Horizontal##bar2x", &pendingActionBar2OffsetX, -600.0f, 600.0f, "%.0f px")) {
                        saveSettings();
                    }
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Vertical##bar2y", &pendingActionBar2OffsetY, -400.0f, 400.0f, "%.0f px")) {
                        saveSettings();
                    }
                    if (ImGui::Button("Reset Position##bar2")) {
                        pendingActionBar2OffsetX = 0.0f;
                        pendingActionBar2OffsetY = 0.0f;
                        saveSettings();
                    }
                }

                ImGui::Spacing();
                if (ImGui::Checkbox("Show Right Side Bar", &pendingShowRightBar)) {
                    saveSettings();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(Slots 25-36)");
                if (pendingShowRightBar) {
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Vertical Offset##rbar", &pendingRightBarOffsetY, -400.0f, 400.0f, "%.0f px")) {
                        saveSettings();
                    }
                }

                ImGui::Spacing();
                if (ImGui::Checkbox("Show Left Side Bar", &pendingShowLeftBar)) {
                    saveSettings();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(Slots 37-48)");
                if (pendingShowLeftBar) {
                    ImGui::SetNextItemWidth(160.0f);
                    if (ImGui::SliderFloat("Vertical Offset##lbar", &pendingLeftBarOffsetY, -400.0f, 400.0f, "%.0f px")) {
                        saveSettings();
                    }
                }

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // ============================================================
            // AUDIO TAB
            // ============================================================
            if (ImGui::BeginTabItem("Audio")) {
                ImGui::Spacing();
                ImGui::BeginChild("AudioSettings", ImVec2(0, 360), true);

                // Helper lambda to apply audio settings
                auto applyAudioSettings = [&]() {
                    if (!renderer) return;
                    float masterScale = soundMuted_ ? 0.0f : static_cast<float>(pendingMasterVolume) / 100.0f;
                    audio::AudioEngine::instance().setMasterVolume(masterScale);
                    if (auto* music = renderer->getMusicManager()) {
                        music->setVolume(pendingMusicVolume);
                    }
                    if (auto* ambient = renderer->getAmbientSoundManager()) {
                        ambient->setVolumeScale(pendingAmbientVolume / 100.0f);
                    }
                    if (auto* ui = renderer->getUiSoundManager()) {
                        ui->setVolumeScale(pendingUiVolume / 100.0f);
                    }
                    if (auto* combat = renderer->getCombatSoundManager()) {
                        combat->setVolumeScale(pendingCombatVolume / 100.0f);
                    }
                    if (auto* spell = renderer->getSpellSoundManager()) {
                        spell->setVolumeScale(pendingSpellVolume / 100.0f);
                    }
                    if (auto* movement = renderer->getMovementSoundManager()) {
                        movement->setVolumeScale(pendingMovementVolume / 100.0f);
                    }
                    if (auto* footstep = renderer->getFootstepManager()) {
                        footstep->setVolumeScale(pendingFootstepVolume / 100.0f);
                    }
                    if (auto* npcVoice = renderer->getNpcVoiceManager()) {
                        npcVoice->setVolumeScale(pendingNpcVoiceVolume / 100.0f);
                    }
                    if (auto* mount = renderer->getMountSoundManager()) {
                        mount->setVolumeScale(pendingMountVolume / 100.0f);
                    }
                    if (auto* activity = renderer->getActivitySoundManager()) {
                        activity->setVolumeScale(pendingActivityVolume / 100.0f);
                    }
                    saveSettings();
                };

                ImGui::Text("Master Volume");
                if (ImGui::SliderInt("##MasterVolume", &pendingMasterVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::Separator();

                if (ImGui::Checkbox("Enable WoWee Music", &pendingUseOriginalSoundtrack)) {
                    if (renderer) {
                        if (auto* zm = renderer->getZoneManager()) {
                            zm->setUseOriginalSoundtrack(pendingUseOriginalSoundtrack);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Include WoWee music tracks in zone music rotation");
                ImGui::Separator();

                ImGui::Text("Music");
                if (ImGui::SliderInt("##MusicVolume", &pendingMusicVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }

                ImGui::Spacing();
                ImGui::Text("Ambient Sounds");
                if (ImGui::SliderInt("##AmbientVolume", &pendingAmbientVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Weather, zones, cities, emitters");

                ImGui::Spacing();
                ImGui::Text("UI Sounds");
                if (ImGui::SliderInt("##UiVolume", &pendingUiVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Buttons, loot, quest complete");

                ImGui::Spacing();
                ImGui::Text("Combat Sounds");
                if (ImGui::SliderInt("##CombatVolume", &pendingCombatVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Weapon swings, impacts, grunts");

                ImGui::Spacing();
                ImGui::Text("Spell Sounds");
                if (ImGui::SliderInt("##SpellVolume", &pendingSpellVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Magic casting and impacts");

                ImGui::Spacing();
                ImGui::Text("Movement Sounds");
                if (ImGui::SliderInt("##MovementVolume", &pendingMovementVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Water splashes, jump/land");

                ImGui::Spacing();
                ImGui::Text("Footsteps");
                if (ImGui::SliderInt("##FootstepVolume", &pendingFootstepVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }

                ImGui::Spacing();
                ImGui::Text("NPC Voices");
                if (ImGui::SliderInt("##NpcVoiceVolume", &pendingNpcVoiceVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }

                ImGui::Spacing();
                ImGui::Text("Mount Sounds");
                if (ImGui::SliderInt("##MountVolume", &pendingMountVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }

                ImGui::Spacing();
                ImGui::Text("Activity Sounds");
                if (ImGui::SliderInt("##ActivityVolume", &pendingActivityVolume, 0, 100, "%d%%")) {
                    applyAudioSettings();
                }
                ImGui::TextWrapped("Swimming, eating, drinking");

                ImGui::EndChild();

                if (ImGui::Button("Restore Audio Defaults", ImVec2(-1, 0))) {
                    pendingMasterVolume = 100;
                    pendingMusicVolume = kDefaultMusicVolume;
                    pendingAmbientVolume = 100;
                    pendingUiVolume = 100;
                    pendingCombatVolume = 100;
                    pendingSpellVolume = 100;
                    pendingMovementVolume = 100;
                    pendingFootstepVolume = 100;
                    pendingNpcVoiceVolume = 100;
                    pendingMountVolume = 100;
                    pendingActivityVolume = 100;
                    applyAudioSettings();
                }

                ImGui::EndTabItem();
            }

            // ============================================================
            // GAMEPLAY TAB
            // ============================================================
            if (ImGui::BeginTabItem("Gameplay")) {
                ImGui::Spacing();

                ImGui::Text("Controls");
                ImGui::Separator();
                if (ImGui::SliderFloat("Mouse Sensitivity", &pendingMouseSensitivity, 0.05f, 1.0f, "%.2f")) {
                    if (renderer) {
                        if (auto* cameraController = renderer->getCameraController()) {
                            cameraController->setMouseSensitivity(pendingMouseSensitivity);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("Invert Mouse", &pendingInvertMouse)) {
                    if (renderer) {
                        if (auto* cameraController = renderer->getCameraController()) {
                            cameraController->setInvertMouse(pendingInvertMouse);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("Extended Camera Zoom", &pendingExtendedZoom)) {
                    if (renderer) {
                        if (auto* cameraController = renderer->getCameraController()) {
                            cameraController->setExtendedZoom(pendingExtendedZoom);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Allow the camera to zoom out further than normal");

                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::Text("Interface");
                ImGui::Separator();
                if (ImGui::SliderInt("UI Opacity", &pendingUiOpacity, 20, 100, "%d%%")) {
                    uiOpacity_ = static_cast<float>(pendingUiOpacity) / 100.0f;
                    saveSettings();
                }
                if (ImGui::Checkbox("Rotate Minimap", &pendingMinimapRotate)) {
                    // Force north-up minimap.
                    minimapRotate_ = false;
                    pendingMinimapRotate = false;
                    if (renderer) {
                        if (auto* minimap = renderer->getMinimap()) {
                            minimap->setRotateWithCamera(false);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("Square Minimap", &pendingMinimapSquare)) {
                    minimapSquare_ = pendingMinimapSquare;
                    if (renderer) {
                        if (auto* minimap = renderer->getMinimap()) {
                            minimap->setSquareShape(minimapSquare_);
                        }
                    }
                    saveSettings();
                }
                if (ImGui::Checkbox("Show Nearby NPC Dots", &pendingMinimapNpcDots)) {
                    minimapNpcDots_ = pendingMinimapNpcDots;
                    saveSettings();
                }
                // Zoom controls
                ImGui::Text("Minimap Zoom:");
                ImGui::SameLine();
                if (ImGui::Button("  -  ")) {
                    if (renderer) {
                        if (auto* minimap = renderer->getMinimap()) {
                            minimap->zoomOut();
                            saveSettings();
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("  +  ")) {
                    if (renderer) {
                        if (auto* minimap = renderer->getMinimap()) {
                            minimap->zoomIn();
                            saveSettings();
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::Text("Loot");
                ImGui::Separator();
                if (ImGui::Checkbox("Auto Loot", &pendingAutoLoot)) {
                    saveSettings();  // per-frame sync applies pendingAutoLoot to gameHandler
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Automatically pick up all items when looting");

                ImGui::Spacing();
                ImGui::Text("Bags");
                ImGui::Separator();
                if (ImGui::Checkbox("Separate Bag Windows", &pendingSeparateBags)) {
                    inventoryScreen.setSeparateBags(pendingSeparateBags);
                    saveSettings();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restore Gameplay Defaults", ImVec2(-1, 0))) {
                    pendingMouseSensitivity = kDefaultMouseSensitivity;
                    pendingInvertMouse = kDefaultInvertMouse;
                    pendingExtendedZoom = false;
                    pendingUiOpacity = 65;
                    pendingMinimapRotate = false;
                    pendingMinimapSquare = false;
                    pendingMinimapNpcDots = false;
                    pendingSeparateBags = true;
                    inventoryScreen.setSeparateBags(true);
                    uiOpacity_ = 0.65f;
                    minimapRotate_ = false;
                    minimapSquare_ = false;
                    minimapNpcDots_ = false;
                    if (renderer) {
                        if (auto* cameraController = renderer->getCameraController()) {
                            cameraController->setMouseSensitivity(pendingMouseSensitivity);
                            cameraController->setInvertMouse(pendingInvertMouse);
                            cameraController->setExtendedZoom(pendingExtendedZoom);
                        }
                        if (auto* minimap = renderer->getMinimap()) {
                            minimap->setRotateWithCamera(minimapRotate_);
                            minimap->setSquareShape(minimapSquare_);
                        }
                    }
                    saveSettings();
                }

                ImGui::EndTabItem();
            }

            // ============================================================
            // CHAT TAB
            // ============================================================
            if (ImGui::BeginTabItem("Chat")) {
                ImGui::Spacing();

                ImGui::Text("Appearance");
                ImGui::Separator();

                if (ImGui::Checkbox("Show Timestamps", &chatShowTimestamps_)) {
                    saveSettings();
                }
                ImGui::SetItemTooltip("Show [HH:MM] before each chat message");

                const char* fontSizes[] = { "Small", "Medium", "Large" };
                if (ImGui::Combo("Chat Font Size", &chatFontSize_, fontSizes, 3)) {
                    saveSettings();
                }

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Text("Auto-Join Channels");
                ImGui::Separator();

                if (ImGui::Checkbox("General", &chatAutoJoinGeneral_)) saveSettings();
                if (ImGui::Checkbox("Trade", &chatAutoJoinTrade_)) saveSettings();
                if (ImGui::Checkbox("LocalDefense", &chatAutoJoinLocalDefense_)) saveSettings();
                if (ImGui::Checkbox("LookingForGroup", &chatAutoJoinLFG_)) saveSettings();
                if (ImGui::Checkbox("Local", &chatAutoJoinLocal_)) saveSettings();

                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::Text("Joined Channels");
                ImGui::Separator();

                ImGui::TextDisabled("Use /join and /leave commands in chat to manage channels.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restore Chat Defaults", ImVec2(-1, 0))) {
                    chatShowTimestamps_ = false;
                    chatFontSize_ = 1;
                    chatAutoJoinGeneral_ = true;
                    chatAutoJoinTrade_ = true;
                    chatAutoJoinLocalDefense_ = true;
                    chatAutoJoinLFG_ = true;
                    chatAutoJoinLocal_ = true;
                    saveSettings();
                }

                ImGui::EndTabItem();
            }

            // ============================================================
            // ABOUT TAB
            // ============================================================
            if (ImGui::BeginTabItem("About")) {
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::TextWrapped("WoWee - World of Warcraft Client Emulator");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Developer");
                ImGui::Indent();
                ImGui::Text("Kelsi Davis");
                ImGui::Unindent();
                ImGui::Spacing();

                ImGui::Text("GitHub");
                ImGui::Indent();
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "https://github.com/Kelsidavis/WoWee");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Click to copy");
                }
                if (ImGui::IsItemClicked()) {
                    ImGui::SetClipboardText("https://github.com/Kelsidavis/WoWee");
                }
                ImGui::Unindent();
                ImGui::Spacing();

                ImGui::Text("Contact");
                ImGui::Indent();
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "github.com/Kelsidavis");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Click to copy");
                }
                if (ImGui::IsItemClicked()) {
                    ImGui::SetClipboardText("https://github.com/Kelsidavis");
                }
                ImGui::Unindent();

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextWrapped("A multi-expansion WoW client supporting Classic, TBC, and WotLK (3.3.5a).");
                ImGui::Spacing();
                ImGui::TextDisabled("Built with Vulkan, SDL2, and ImGui");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 10.0f));
        if (ImGui::Button("Back to Game", ImVec2(-1, 0))) {
            showSettingsWindow = false;
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

void GameScreen::renderQuestMarkers(game::GameHandler& gameHandler) {
    const auto& statuses = gameHandler.getNpcQuestStatuses();
    if (statuses.empty()) return;

    auto* renderer = core::Application::getInstance().getRenderer();
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* window = core::Application::getInstance().getWindow();
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
    auto* renderer = core::Application::getInstance().getRenderer();
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    auto* minimap = renderer ? renderer->getMinimap() : nullptr;
    auto* window = core::Application::getInstance().getWindow();
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
        bearing = std::atan2(-fwd.x, fwd.y);
        cosB = std::cos(bearing);
        sinB = std::sin(bearing);
    }

    auto* drawList = ImGui::GetForegroundDrawList();

    auto projectToMinimap = [&](const glm::vec3& worldRenderPos, float& sx, float& sy) -> bool {
        float dx = worldRenderPos.x - playerRender.x;
        float dy = worldRenderPos.y - playerRender.y;

        // Match minimap shader transform exactly.
        // Render axes: +X=west, +Y=north. Minimap screen axes: +X=right(east), +Y=down(south).
        float rx = -dx * cosB + dy * sinB;
        float ry = -dx * sinB - dy * cosB;

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

    // Optional base nearby NPC dots (independent of quest status packets).
    if (minimapNpcDots_) {
        for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
            if (!entity || entity->getType() != game::ObjectType::UNIT) continue;

            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (!unit || unit->getHealth() == 0) continue;

            glm::vec3 npcRender = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            float sx = 0.0f, sy = 0.0f;
            if (!projectToMinimap(npcRender, sx, sy)) continue;

            ImU32 baseDot = unit->isHostile() ? IM_COL32(220, 70, 70, 220) : IM_COL32(245, 245, 245, 210);
            drawList->AddCircleFilled(ImVec2(sx, sy), 1.0f, baseDot);
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

            ImU32 dotColor = (member.guid == leaderGuid)
                ? IM_COL32(255, 210, 0, 235)
                : IM_COL32(100, 180, 255, 235);
            drawList->AddCircleFilled(ImVec2(sx, sy), 4.0f, dotColor);
            drawList->AddCircle(ImVec2(sx, sy), 4.0f, IM_COL32(255, 255, 255, 160), 12, 1.0f);

            ImVec2 cursorPos = ImGui::GetMousePos();
            float mdx = cursorPos.x - sx, mdy = cursorPos.y - sy;
            if (!member.name.empty() && (mdx * mdx + mdy * mdy) < 64.0f) {
                ImGui::SetTooltip("%s", member.name.c_str());
            }
        }
    }

    auto applyMuteState = [&]() {
        auto* activeRenderer = core::Application::getInstance().getRenderer();
        float masterScale = soundMuted_ ? 0.0f : static_cast<float>(pendingMasterVolume) / 100.0f;
        audio::AudioEngine::instance().setMasterVolume(masterScale);
        if (!activeRenderer) return;
        if (auto* music = activeRenderer->getMusicManager()) {
            music->setVolume(pendingMusicVolume);
        }
        if (auto* ambient = activeRenderer->getAmbientSoundManager()) {
            ambient->setVolumeScale(pendingAmbientVolume / 100.0f);
        }
        if (auto* ui = activeRenderer->getUiSoundManager()) {
            ui->setVolumeScale(pendingUiVolume / 100.0f);
        }
        if (auto* combat = activeRenderer->getCombatSoundManager()) {
            combat->setVolumeScale(pendingCombatVolume / 100.0f);
        }
        if (auto* spell = activeRenderer->getSpellSoundManager()) {
            spell->setVolumeScale(pendingSpellVolume / 100.0f);
        }
        if (auto* movement = activeRenderer->getMovementSoundManager()) {
            movement->setVolumeScale(pendingMovementVolume / 100.0f);
        }
        if (auto* footstep = activeRenderer->getFootstepManager()) {
            footstep->setVolumeScale(pendingFootstepVolume / 100.0f);
        }
        if (auto* npcVoice = activeRenderer->getNpcVoiceManager()) {
            npcVoice->setVolumeScale(pendingNpcVoiceVolume / 100.0f);
        }
        if (auto* mount = activeRenderer->getMountSoundManager()) {
            mount->setVolumeScale(pendingMountVolume / 100.0f);
        }
        if (auto* activity = activeRenderer->getActivitySoundManager()) {
            activity->setVolumeScale(pendingActivityVolume / 100.0f);
        }
    };

    // Zone name label above the minimap (centered, WoW-style)
    {
        const std::string& zoneName = renderer ? renderer->getCurrentZoneName() : std::string{};
        if (!zoneName.empty()) {
            auto* fgDl = ImGui::GetForegroundDrawList();
            float zoneTextY = centerY - mapRadius - 16.0f;
            ImFont* font = ImGui::GetFont();
            ImVec2 tsz = font->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, zoneName.c_str());
            float tzx = centerX - tsz.x * 0.5f;
            fgDl->AddText(font, 12.0f, ImVec2(tzx + 1.0f, zoneTextY + 1.0f),
                IM_COL32(0, 0, 0, 180), zoneName.c_str());
            fgDl->AddText(font, 12.0f, ImVec2(tzx, zoneTextY),
                IM_COL32(255, 220, 120, 230), zoneName.c_str());
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
            soundMuted_ = !soundMuted_;
            if (soundMuted_) {
                preMuteVolume_ = audio::AudioEngine::instance().getMasterVolume();
            }
            applyMuteState();
            saveSettings();
        }
        bool hovered = ImGui::IsItemHovered();
        ImU32 bg = soundMuted_ ? IM_COL32(135, 42, 42, 230) : IM_COL32(38, 38, 38, 210);
        if (hovered) bg = soundMuted_ ? IM_COL32(160, 58, 58, 230) : IM_COL32(65, 65, 65, 220);
        ImU32 fg = IM_COL32(255, 255, 255, 245);
        draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, 4.0f);
        draw->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(p.x + size.x - 0.5f, p.y + size.y - 0.5f),
                      IM_COL32(255, 255, 255, 42), 4.0f);
        draw->AddRectFilled(ImVec2(p.x + 4.0f, p.y + 8.0f), ImVec2(p.x + 7.0f, p.y + 12.0f), fg, 1.0f);
        draw->AddTriangleFilled(ImVec2(p.x + 7.0f, p.y + 7.0f),
                                ImVec2(p.x + 7.0f, p.y + 13.0f),
                                ImVec2(p.x + 11.8f, p.y + 10.0f), fg);
        if (soundMuted_) {
            draw->AddLine(ImVec2(p.x + 13.5f, p.y + 6.2f), ImVec2(p.x + 17.2f, p.y + 13.8f), fg, 1.8f);
            draw->AddLine(ImVec2(p.x + 17.2f, p.y + 6.2f), ImVec2(p.x + 13.5f, p.y + 13.8f), fg, 1.8f);
        } else {
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 3.6f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.4f);
            draw->PathArcTo(ImVec2(p.x + 11.8f, p.y + 10.0f), 5.5f, -0.7f, 0.7f, 12);
            draw->PathStroke(fg, 0, 1.2f);
        }
        if (hovered) ImGui::SetTooltip(soundMuted_ ? "Unmute" : "Mute");
    }
    ImGui::End();

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

    // "New Mail" indicator below the minimap
    if (gameHandler.hasNewMail()) {
        float indicatorX = centerX - mapRadius;
        float indicatorY = centerY + mapRadius + 4.0f;
        ImGui::SetNextWindowPos(ImVec2(indicatorX, indicatorY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(mapRadius * 2.0f, 22), ImGuiCond_Always);
        ImGuiWindowFlags mailFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##NewMailIndicator", nullptr, mailFlags)) {
            // Pulsing effect
            float pulse = 0.7f + 0.3f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, pulse), "New Mail!");
        }
        ImGui::End();
    }
}

std::string GameScreen::getSettingsPath() {
    std::string dir;
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    dir = appdata ? std::string(appdata) + "\\wowee" : ".";
#else
    const char* home = std::getenv("HOME");
    dir = home ? std::string(home) + "/.wowee" : ".";
#endif
    return dir + "/settings.cfg";
}

std::string GameScreen::replaceGenderPlaceholders(const std::string& text, game::GameHandler& gameHandler) {
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

    // Replace simple placeholders.
    // $n = player name
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

void GameScreen::renderChatBubbles(game::GameHandler& gameHandler) {
    if (chatBubbles_.empty()) return;

    auto* renderer = core::Application::getInstance().getRenderer();
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = core::Application::getInstance().getWindow();
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
        float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * screenH;  // Flip Y

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

void GameScreen::saveSettings() {
    std::string path = getSettingsPath();
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save settings to ", path);
        return;
    }

    // Interface
    out << "ui_opacity=" << pendingUiOpacity << "\n";
    out << "minimap_rotate=" << (pendingMinimapRotate ? 1 : 0) << "\n";
    out << "minimap_square=" << (pendingMinimapSquare ? 1 : 0) << "\n";
    out << "minimap_npc_dots=" << (pendingMinimapNpcDots ? 1 : 0) << "\n";
    out << "separate_bags=" << (pendingSeparateBags ? 1 : 0) << "\n";
    out << "show_action_bar2=" << (pendingShowActionBar2 ? 1 : 0) << "\n";
    out << "action_bar2_offset_x=" << pendingActionBar2OffsetX << "\n";
    out << "action_bar2_offset_y=" << pendingActionBar2OffsetY << "\n";
    out << "show_right_bar=" << (pendingShowRightBar ? 1 : 0) << "\n";
    out << "show_left_bar=" << (pendingShowLeftBar ? 1 : 0) << "\n";
    out << "right_bar_offset_y=" << pendingRightBarOffsetY << "\n";
    out << "left_bar_offset_y=" << pendingLeftBarOffsetY << "\n";

    // Audio
    out << "sound_muted=" << (soundMuted_ ? 1 : 0) << "\n";
    out << "use_original_soundtrack=" << (pendingUseOriginalSoundtrack ? 1 : 0) << "\n";
    out << "master_volume=" << pendingMasterVolume << "\n";
    out << "music_volume=" << pendingMusicVolume << "\n";
    out << "ambient_volume=" << pendingAmbientVolume << "\n";
    out << "ui_volume=" << pendingUiVolume << "\n";
    out << "combat_volume=" << pendingCombatVolume << "\n";
    out << "spell_volume=" << pendingSpellVolume << "\n";
    out << "movement_volume=" << pendingMovementVolume << "\n";
    out << "footstep_volume=" << pendingFootstepVolume << "\n";
    out << "npc_voice_volume=" << pendingNpcVoiceVolume << "\n";
    out << "mount_volume=" << pendingMountVolume << "\n";
    out << "activity_volume=" << pendingActivityVolume << "\n";

    // Gameplay
    out << "auto_loot=" << (pendingAutoLoot ? 1 : 0) << "\n";
    out << "ground_clutter_density=" << pendingGroundClutterDensity << "\n";
    out << "shadows=" << (pendingShadows ? 1 : 0) << "\n";
    out << "shadow_distance=" << pendingShadowDistance << "\n";
    out << "water_refraction=" << (pendingWaterRefraction ? 1 : 0) << "\n";
    out << "antialiasing=" << pendingAntiAliasing << "\n";
    out << "normal_mapping=" << (pendingNormalMapping ? 1 : 0) << "\n";
    out << "normal_map_strength=" << pendingNormalMapStrength << "\n";
    out << "pom=" << (pendingPOM ? 1 : 0) << "\n";
    out << "pom_quality=" << pendingPOMQuality << "\n";
    out << "upscaling_mode=" << pendingUpscalingMode << "\n";
    out << "fsr=" << (pendingFSR ? 1 : 0) << "\n";
    out << "fsr_quality=" << pendingFSRQuality << "\n";
    out << "fsr_sharpness=" << pendingFSRSharpness << "\n";
    out << "fsr2_jitter_sign=" << pendingFSR2JitterSign << "\n";
    out << "fsr2_mv_scale_x=" << pendingFSR2MotionVecScaleX << "\n";
    out << "fsr2_mv_scale_y=" << pendingFSR2MotionVecScaleY << "\n";
    out << "amd_fsr3_framegen=" << (pendingAMDFramegen ? 1 : 0) << "\n";

    // Controls
    out << "mouse_sensitivity=" << pendingMouseSensitivity << "\n";
    out << "invert_mouse=" << (pendingInvertMouse ? 1 : 0) << "\n";
    out << "extended_zoom=" << (pendingExtendedZoom ? 1 : 0) << "\n";

    // Chat
    out << "chat_active_tab=" << activeChatTab_ << "\n";
    out << "chat_timestamps=" << (chatShowTimestamps_ ? 1 : 0) << "\n";
    out << "chat_font_size=" << chatFontSize_ << "\n";
    out << "chat_autojoin_general=" << (chatAutoJoinGeneral_ ? 1 : 0) << "\n";
    out << "chat_autojoin_trade=" << (chatAutoJoinTrade_ ? 1 : 0) << "\n";
    out << "chat_autojoin_localdefense=" << (chatAutoJoinLocalDefense_ ? 1 : 0) << "\n";
    out << "chat_autojoin_lfg=" << (chatAutoJoinLFG_ ? 1 : 0) << "\n";
    out << "chat_autojoin_local=" << (chatAutoJoinLocal_ ? 1 : 0) << "\n";

    LOG_INFO("Settings saved to ", path);
}

void GameScreen::loadSettings() {
    std::string path = getSettingsPath();
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
                    pendingUiOpacity = v;
                    uiOpacity_ = static_cast<float>(v) / 100.0f;
                }
            } else if (key == "minimap_rotate") {
                // Ignore persisted rotate state; keep north-up.
                minimapRotate_ = false;
                pendingMinimapRotate = false;
            } else if (key == "minimap_square") {
                int v = std::stoi(val);
                minimapSquare_ = (v != 0);
                pendingMinimapSquare = minimapSquare_;
            } else if (key == "minimap_npc_dots") {
                int v = std::stoi(val);
                minimapNpcDots_ = (v != 0);
                pendingMinimapNpcDots = minimapNpcDots_;
            } else if (key == "separate_bags") {
                pendingSeparateBags = (std::stoi(val) != 0);
                inventoryScreen.setSeparateBags(pendingSeparateBags);
            } else if (key == "show_action_bar2") {
                pendingShowActionBar2 = (std::stoi(val) != 0);
            } else if (key == "action_bar2_offset_x") {
                pendingActionBar2OffsetX = std::clamp(std::stof(val), -600.0f, 600.0f);
            } else if (key == "action_bar2_offset_y") {
                pendingActionBar2OffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "show_right_bar") {
                pendingShowRightBar = (std::stoi(val) != 0);
            } else if (key == "show_left_bar") {
                pendingShowLeftBar = (std::stoi(val) != 0);
            } else if (key == "right_bar_offset_y") {
                pendingRightBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            } else if (key == "left_bar_offset_y") {
                pendingLeftBarOffsetY = std::clamp(std::stof(val), -400.0f, 400.0f);
            }
            // Audio
            else if (key == "sound_muted") {
                soundMuted_ = (std::stoi(val) != 0);
                if (soundMuted_) {
                    // Apply mute on load; preMuteVolume_ will be set when AudioEngine is available
                    audio::AudioEngine::instance().setMasterVolume(0.0f);
                }
            }
            else if (key == "use_original_soundtrack") pendingUseOriginalSoundtrack = (std::stoi(val) != 0);
            else if (key == "master_volume") pendingMasterVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "music_volume") pendingMusicVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ambient_volume") pendingAmbientVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "ui_volume") pendingUiVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "combat_volume") pendingCombatVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "spell_volume") pendingSpellVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "movement_volume") pendingMovementVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "footstep_volume") pendingFootstepVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "npc_voice_volume") pendingNpcVoiceVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "mount_volume") pendingMountVolume = std::clamp(std::stoi(val), 0, 100);
            else if (key == "activity_volume") pendingActivityVolume = std::clamp(std::stoi(val), 0, 100);
            // Gameplay
            else if (key == "auto_loot") pendingAutoLoot = (std::stoi(val) != 0);
            else if (key == "ground_clutter_density") pendingGroundClutterDensity = std::clamp(std::stoi(val), 0, 150);
            else if (key == "shadows") pendingShadows = (std::stoi(val) != 0);
            else if (key == "shadow_distance") pendingShadowDistance = std::clamp(std::stof(val), 40.0f, 500.0f);
            else if (key == "water_refraction") pendingWaterRefraction = (std::stoi(val) != 0);
            else if (key == "antialiasing") pendingAntiAliasing = std::clamp(std::stoi(val), 0, 3);
            else if (key == "normal_mapping") pendingNormalMapping = (std::stoi(val) != 0);
            else if (key == "normal_map_strength") pendingNormalMapStrength = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "pom") pendingPOM = (std::stoi(val) != 0);
            else if (key == "pom_quality") pendingPOMQuality = std::clamp(std::stoi(val), 0, 2);
            else if (key == "upscaling_mode") {
                pendingUpscalingMode = std::clamp(std::stoi(val), 0, 2);
                pendingFSR = (pendingUpscalingMode == 1);
            } else if (key == "fsr") {
                pendingFSR = (std::stoi(val) != 0);
                // Backward compatibility: old configs only had fsr=0/1.
                if (pendingUpscalingMode == 0 && pendingFSR) pendingUpscalingMode = 1;
            }
            else if (key == "fsr_quality") pendingFSRQuality = std::clamp(std::stoi(val), 0, 3);
            else if (key == "fsr_sharpness") pendingFSRSharpness = std::clamp(std::stof(val), 0.0f, 2.0f);
            else if (key == "fsr2_jitter_sign") pendingFSR2JitterSign = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_x") pendingFSR2MotionVecScaleX = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "fsr2_mv_scale_y") pendingFSR2MotionVecScaleY = std::clamp(std::stof(val), -2.0f, 2.0f);
            else if (key == "amd_fsr3_framegen") pendingAMDFramegen = (std::stoi(val) != 0);
            // Controls
            else if (key == "mouse_sensitivity") pendingMouseSensitivity = std::clamp(std::stof(val), 0.05f, 1.0f);
            else if (key == "invert_mouse") pendingInvertMouse = (std::stoi(val) != 0);
            else if (key == "extended_zoom") pendingExtendedZoom = (std::stoi(val) != 0);
            // Chat
            else if (key == "chat_active_tab") activeChatTab_ = std::clamp(std::stoi(val), 0, 3);
            else if (key == "chat_timestamps") chatShowTimestamps_ = (std::stoi(val) != 0);
            else if (key == "chat_font_size") chatFontSize_ = std::clamp(std::stoi(val), 0, 2);
            else if (key == "chat_autojoin_general") chatAutoJoinGeneral_ = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_trade") chatAutoJoinTrade_ = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_localdefense") chatAutoJoinLocalDefense_ = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_lfg") chatAutoJoinLFG_ = (std::stoi(val) != 0);
            else if (key == "chat_autojoin_local") chatAutoJoinLocal_ = (std::stoi(val) != 0);
        } catch (...) {}
    }
    LOG_INFO("Settings loaded from ", path);
}

// ============================================================
// Mail Window
// ============================================================

void GameScreen::renderMailWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isMailboxOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 250, 80), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::Begin("Mailbox", &open)) {
        const auto& inbox = gameHandler.getMailInbox();

        // Top bar: money + compose button
        uint64_t money = gameHandler.getMoneyCopper();
        uint32_t mg = static_cast<uint32_t>(money / 10000);
        uint32_t ms = static_cast<uint32_t>((money / 100) % 100);
        uint32_t mc = static_cast<uint32_t>(money % 100);
        ImGui::Text("Your money: %ug %us %uc", mg, ms, mc);
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        if (ImGui::Button("Compose")) {
            mailRecipientBuffer_[0] = '\0';
            mailSubjectBuffer_[0] = '\0';
            mailBodyBuffer_[0] = '\0';
            mailComposeMoney_[0] = 0;
            mailComposeMoney_[1] = 0;
            mailComposeMoney_[2] = 0;
            gameHandler.openMailCompose();
        }
        ImGui::Separator();

        if (inbox.empty()) {
            ImGui::TextDisabled("No mail.");
        } else {
            // Two-panel layout: left = mail list, right = selected mail detail
            float listWidth = 220.0f;

            // Left panel - mail list
            ImGui::BeginChild("MailList", ImVec2(listWidth, 0), true);
            for (size_t i = 0; i < inbox.size(); ++i) {
                const auto& mail = inbox[i];
                ImGui::PushID(static_cast<int>(i));

                bool selected = (gameHandler.getSelectedMailIndex() == static_cast<int>(i));
                std::string label = mail.subject.empty() ? "(No Subject)" : mail.subject;

                // Unread indicator
                if (!mail.read) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.5f, 1.0f));
                }

                if (ImGui::Selectable(label.c_str(), selected)) {
                    gameHandler.setSelectedMailIndex(static_cast<int>(i));
                    // Mark as read
                    if (!mail.read) {
                        gameHandler.mailMarkAsRead(mail.messageId);
                    }
                }

                if (!mail.read) {
                    ImGui::PopStyleColor();
                }

                // Sub-info line
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  From: %s", mail.senderName.c_str());
                if (mail.money > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), " [G]");
                }
                if (!mail.attachments.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), " [A]");
                }

                ImGui::PopID();
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // Right panel - selected mail detail
            ImGui::BeginChild("MailDetail", ImVec2(0, 0), true);
            int sel = gameHandler.getSelectedMailIndex();
            if (sel >= 0 && sel < static_cast<int>(inbox.size())) {
                const auto& mail = inbox[sel];

                ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "%s",
                    mail.subject.empty() ? "(No Subject)" : mail.subject.c_str());
                ImGui::Text("From: %s", mail.senderName.c_str());

                if (mail.messageType == 2) {
                    ImGui::TextColored(ImVec4(0.8f, 0.6f, 0.2f, 1.0f), "[Auction House]");
                }
                ImGui::Separator();

                // Body text
                if (!mail.body.empty()) {
                    ImGui::TextWrapped("%s", mail.body.c_str());
                    ImGui::Separator();
                }

                // Money
                if (mail.money > 0) {
                    uint32_t g = mail.money / 10000;
                    uint32_t s = (mail.money / 100) % 100;
                    uint32_t c = mail.money % 100;
                    ImGui::Text("Money: %ug %us %uc", g, s, c);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Take Money")) {
                        gameHandler.mailTakeMoney(mail.messageId);
                    }
                }

                // COD warning
                if (mail.cod > 0) {
                    uint32_t g = mail.cod / 10000;
                    uint32_t s = (mail.cod / 100) % 100;
                    uint32_t c = mail.cod % 100;
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                        "COD: %ug %us %uc (you pay this to take items)", g, s, c);
                }

                // Attachments
                if (!mail.attachments.empty()) {
                    ImGui::Text("Attachments: %zu", mail.attachments.size());
                    for (size_t j = 0; j < mail.attachments.size(); ++j) {
                        const auto& att = mail.attachments[j];
                        ImGui::PushID(static_cast<int>(j));

                        auto* info = gameHandler.getItemInfo(att.itemId);
                        if (info && info->valid) {
                            ImGui::BulletText("%s x%u", info->name.c_str(), att.stackCount);
                        } else {
                            ImGui::BulletText("Item %u x%u", att.itemId, att.stackCount);
                            gameHandler.ensureItemInfo(att.itemId);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Take")) {
                            gameHandler.mailTakeItem(mail.messageId, att.slot);
                        }

                        ImGui::PopID();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();

                // Action buttons
                if (ImGui::Button("Delete")) {
                    gameHandler.mailDelete(mail.messageId);
                }
                ImGui::SameLine();
                if (mail.messageType == 0 && ImGui::Button("Reply")) {
                    // Pre-fill compose with sender as recipient
                    strncpy(mailRecipientBuffer_, mail.senderName.c_str(), sizeof(mailRecipientBuffer_) - 1);
                    mailRecipientBuffer_[sizeof(mailRecipientBuffer_) - 1] = '\0';
                    std::string reSubject = "Re: " + mail.subject;
                    strncpy(mailSubjectBuffer_, reSubject.c_str(), sizeof(mailSubjectBuffer_) - 1);
                    mailSubjectBuffer_[sizeof(mailSubjectBuffer_) - 1] = '\0';
                    mailBodyBuffer_[0] = '\0';
                    mailComposeMoney_[0] = 0;
                    mailComposeMoney_[1] = 0;
                    mailComposeMoney_[2] = 0;
                    gameHandler.openMailCompose();
                }
            } else {
                ImGui::TextDisabled("Select a mail to read.");
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeMailbox();
    }
}

void GameScreen::renderMailComposeWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isMailComposeOpen()) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW / 2 - 190, screenH / 2 - 250), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_Appearing);

    bool open = true;
    if (ImGui::Begin("Send Mail", &open)) {
        ImGui::Text("To:");
        ImGui::SameLine(60);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##MailTo", mailRecipientBuffer_, sizeof(mailRecipientBuffer_));

        ImGui::Text("Subject:");
        ImGui::SameLine(60);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##MailSubject", mailSubjectBuffer_, sizeof(mailSubjectBuffer_));

        ImGui::Text("Body:");
        ImGui::InputTextMultiline("##MailBody", mailBodyBuffer_, sizeof(mailBodyBuffer_),
                                   ImVec2(-1, 120));

        // Attachments section
        int attachCount = gameHandler.getMailAttachmentCount();
        ImGui::Text("Attachments (%d/12):", attachCount);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Right-click items in bags to attach");

        const auto& attachments = gameHandler.getMailAttachments();
        // Show attachment slots in a grid (6 per row)
        for (int i = 0; i < game::GameHandler::MAIL_MAX_ATTACHMENTS; ++i) {
            if (i % 6 != 0) ImGui::SameLine();
            ImGui::PushID(i + 5000);
            const auto& att = attachments[i];
            if (att.occupied()) {
                // Show item with quality color border
                ImVec4 qualColor = ui::InventoryScreen::getQualityColor(att.item.quality);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(qualColor.x * 0.3f, qualColor.y * 0.3f, qualColor.z * 0.3f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(qualColor.x * 0.5f, qualColor.y * 0.5f, qualColor.z * 0.5f, 0.9f));

                // Try to show icon
                VkDescriptorSet icon = inventoryScreen.getItemIcon(att.item.displayInfoId);
                bool clicked = false;
                if (icon) {
                    clicked = ImGui::ImageButton("##att", (ImTextureID)icon, ImVec2(30, 30));
                } else {
                    // Truncate name to fit
                    std::string label = att.item.name.substr(0, 4);
                    clicked = ImGui::Button(label.c_str(), ImVec2(36, 36));
                }
                ImGui::PopStyleColor(2);

                if (clicked) {
                    gameHandler.detachMailAttachment(i);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(qualColor, "%s", att.item.name.c_str());
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Click to remove");
                    ImGui::EndTooltip();
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.5f));
                ImGui::Button("##empty", ImVec2(36, 36));
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Text("Money:");
        ImGui::SameLine(60);
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("##MailGold", &mailComposeMoney_[0], 0, 0);
        if (mailComposeMoney_[0] < 0) mailComposeMoney_[0] = 0;
        ImGui::SameLine();
        ImGui::Text("g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40);
        ImGui::InputInt("##MailSilver", &mailComposeMoney_[1], 0, 0);
        if (mailComposeMoney_[1] < 0) mailComposeMoney_[1] = 0;
        if (mailComposeMoney_[1] > 99) mailComposeMoney_[1] = 99;
        ImGui::SameLine();
        ImGui::Text("s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(40);
        ImGui::InputInt("##MailCopper", &mailComposeMoney_[2], 0, 0);
        if (mailComposeMoney_[2] < 0) mailComposeMoney_[2] = 0;
        if (mailComposeMoney_[2] > 99) mailComposeMoney_[2] = 99;
        ImGui::SameLine();
        ImGui::Text("c");

        uint32_t totalMoney = static_cast<uint32_t>(mailComposeMoney_[0]) * 10000 +
                              static_cast<uint32_t>(mailComposeMoney_[1]) * 100 +
                              static_cast<uint32_t>(mailComposeMoney_[2]);

        uint32_t sendCost = attachCount > 0 ? static_cast<uint32_t>(30 * attachCount) : 30u;
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Sending cost: %uc", sendCost);

        ImGui::Spacing();
        bool canSend = (strlen(mailRecipientBuffer_) > 0);
        if (!canSend) ImGui::BeginDisabled();
        if (ImGui::Button("Send", ImVec2(80, 0))) {
            gameHandler.sendMail(mailRecipientBuffer_, mailSubjectBuffer_,
                                 mailBodyBuffer_, totalMoney);
        }
        if (!canSend) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            gameHandler.closeMailCompose();
        }
    }
    ImGui::End();

    if (!open) {
        gameHandler.closeMailCompose();
    }
}

// ============================================================
// Bank Window
// ============================================================

void GameScreen::renderBankWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isBankOpen()) return;

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(480, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bank", &open)) {
        ImGui::End();
        if (!open) gameHandler.closeBank();
        return;
    }

    auto& inv = gameHandler.getInventory();
    bool isHolding = inventoryScreen.isHoldingItem();
    constexpr float SLOT_SIZE = 42.0f;
    static constexpr float kBankPickupHold = 0.10f; // seconds
    // Persistent pickup tracking for bank (mirrors inventory_screen's pickupPending_)
    static bool bankPickupPending = false;
    static float bankPickupPressTime = 0.0f;
    static int bankPickupType = 0; // 0=main bank, 1=bank bag slot, 2=bank bag equip slot
    static int bankPickupIndex = -1;
    static int bankPickupBagIndex = -1;
    static int bankPickupBagSlotIndex = -1;

    // Helper: render a bank item slot with icon, click-and-hold pickup, drop, tooltip
    auto renderBankItemSlot = [&](const game::ItemSlot& slot, int pickType, int mainIdx,
                                   int bagIdx, int bagSlotIdx, uint8_t dstBag, uint8_t dstSlot) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();

        if (slot.empty()) {
            ImU32 bgCol = IM_COL32(30, 30, 30, 200);
            ImU32 borderCol = IM_COL32(60, 60, 60, 200);
            if (isHolding) {
                bgCol = IM_COL32(20, 50, 20, 200);
                borderCol = IM_COL32(0, 180, 0, 200);
            }
            drawList->AddRectFilled(pos, ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE), bgCol);
            drawList->AddRect(pos, ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE), borderCol);
            ImGui::InvisibleButton("slot", ImVec2(SLOT_SIZE, SLOT_SIZE));
            if (isHolding && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                inventoryScreen.dropIntoBankSlot(gameHandler, dstBag, dstSlot);
            }
        } else {
            const auto& item = slot.item;
            ImVec4 qc = InventoryScreen::getQualityColor(item.quality);
            ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(qc);
            VkDescriptorSet iconTex = inventoryScreen.getItemIcon(item.displayInfoId);

            if (iconTex) {
                drawList->AddImage((ImTextureID)(uintptr_t)iconTex, pos,
                                   ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE));
                drawList->AddRect(pos, ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE),
                                  borderCol, 0.0f, 0, 2.0f);
            } else {
                ImU32 bgCol = IM_COL32(40, 35, 30, 220);
                drawList->AddRectFilled(pos, ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE), bgCol);
                drawList->AddRect(pos, ImVec2(pos.x + SLOT_SIZE, pos.y + SLOT_SIZE),
                                  borderCol, 0.0f, 0, 2.0f);
                if (!item.name.empty()) {
                    char abbr[3] = { item.name[0], item.name.size() > 1 ? item.name[1] : '\0', '\0' };
                    float tw = ImGui::CalcTextSize(abbr).x;
                    drawList->AddText(ImVec2(pos.x + (SLOT_SIZE - tw) * 0.5f, pos.y + 2.0f),
                                      ImGui::ColorConvertFloat4ToU32(qc), abbr);
                }
            }

            if (item.stackCount > 1) {
                char countStr[16];
                snprintf(countStr, sizeof(countStr), "%u", item.stackCount);
                float cw = ImGui::CalcTextSize(countStr).x;
                drawList->AddText(ImVec2(pos.x + SLOT_SIZE - cw - 2.0f, pos.y + SLOT_SIZE - 14.0f),
                                  IM_COL32(255, 255, 255, 220), countStr);
            }

            ImGui::InvisibleButton("slot", ImVec2(SLOT_SIZE, SLOT_SIZE));

            if (!isHolding) {
                // Start pickup tracking on mouse press
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    bankPickupPending = true;
                    bankPickupPressTime = ImGui::GetTime();
                    bankPickupType = pickType;
                    bankPickupIndex = mainIdx;
                    bankPickupBagIndex = bagIdx;
                    bankPickupBagSlotIndex = bagSlotIdx;
                }
                // Check if held long enough to pick up
                if (bankPickupPending && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                    (ImGui::GetTime() - bankPickupPressTime) >= kBankPickupHold) {
                    bool sameSlot = (bankPickupType == pickType);
                    if (pickType == 0)
                        sameSlot = sameSlot && (bankPickupIndex == mainIdx);
                    else if (pickType == 1)
                        sameSlot = sameSlot && (bankPickupBagIndex == bagIdx) && (bankPickupBagSlotIndex == bagSlotIdx);
                    else if (pickType == 2)
                        sameSlot = sameSlot && (bankPickupIndex == mainIdx);

                    if (sameSlot && ImGui::IsItemHovered()) {
                        bankPickupPending = false;
                        if (pickType == 0) {
                            inventoryScreen.pickupFromBank(inv, mainIdx);
                        } else if (pickType == 1) {
                            inventoryScreen.pickupFromBankBag(inv, bagIdx, bagSlotIdx);
                        } else if (pickType == 2) {
                            inventoryScreen.pickupFromBankBagEquip(inv, mainIdx);
                        }
                    }
                }
            } else {
                // Drop/swap on mouse release
                if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    inventoryScreen.dropIntoBankSlot(gameHandler, dstBag, dstSlot);
                }
            }

            // Tooltip
            if (ImGui::IsItemHovered() && !isHolding) {
                ImGui::BeginTooltip();
                ImGui::TextColored(qc, "%s", item.name.c_str());
                if (item.stackCount > 1) ImGui::Text("Count: %u", item.stackCount);
                ImGui::EndTooltip();
            }
        }
    };

    // Main bank slots (24 for Classic, 28 for TBC/WotLK)
    int bankSlotCount = gameHandler.getEffectiveBankSlots();
    int bankBagCount = gameHandler.getEffectiveBankBagSlots();
    ImGui::Text("Bank Slots");
    ImGui::Separator();
    for (int i = 0; i < bankSlotCount; i++) {
        if (i % 7 != 0) ImGui::SameLine();
        ImGui::PushID(i + 1000);
        renderBankItemSlot(inv.getBankSlot(i), 0, i, -1, -1, 0xFF, static_cast<uint8_t>(39 + i));
        ImGui::PopID();
    }

    // Bank bag equip slots — show bag icon with pickup/drop, or "Buy Slot"
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Bank Bags");
    uint8_t purchased = inv.getPurchasedBankBagSlots();
    for (int i = 0; i < bankBagCount; i++) {
        if (i > 0) ImGui::SameLine();
        ImGui::PushID(i + 2000);

        int bagSize = inv.getBankBagSize(i);
        if (i < purchased || bagSize > 0) {
            const auto& bagSlot = inv.getBankBagItem(i);
            // Render as an item slot: icon with pickup/drop (pickType=2 for bag equip)
            renderBankItemSlot(bagSlot, 2, i, -1, -1, 0xFF, static_cast<uint8_t>(67 + i));
        } else {
            if (ImGui::Button("Buy Slot", ImVec2(50, 30))) {
                gameHandler.buyBankSlot();
            }
        }
        ImGui::PopID();
    }

    // Show expanded bank bag contents
    for (int bagIdx = 0; bagIdx < bankBagCount; bagIdx++) {
        int bagSize = inv.getBankBagSize(bagIdx);
        if (bagSize <= 0) continue;

        ImGui::Spacing();
        ImGui::Text("Bank Bag %d (%d slots)", bagIdx + 1, bagSize);
        for (int s = 0; s < bagSize; s++) {
            if (s % 7 != 0) ImGui::SameLine();
            ImGui::PushID(3000 + bagIdx * 100 + s);
            renderBankItemSlot(inv.getBankBagSlot(bagIdx, s), 1, -1, bagIdx, s,
                               static_cast<uint8_t>(67 + bagIdx), static_cast<uint8_t>(s));
            ImGui::PopID();
        }
    }

    ImGui::End();

    if (!open) gameHandler.closeBank();
}

// ============================================================
// Guild Bank Window
// ============================================================

void GameScreen::renderGuildBankWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isGuildBankOpen()) return;

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Guild Bank", &open)) {
        ImGui::End();
        if (!open) gameHandler.closeGuildBank();
        return;
    }

    const auto& data = gameHandler.getGuildBankData();
    uint8_t activeTab = gameHandler.getGuildBankActiveTab();

    // Money display
    uint32_t gold = static_cast<uint32_t>(data.money / 10000);
    uint32_t silver = static_cast<uint32_t>((data.money / 100) % 100);
    uint32_t copper = static_cast<uint32_t>(data.money % 100);
    ImGui::Text("Guild Bank Money: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "%ug %us %uc", gold, silver, copper);

    // Tab bar
    if (!data.tabs.empty()) {
        for (size_t i = 0; i < data.tabs.size(); i++) {
            if (i > 0) ImGui::SameLine();
            bool selected = (i == activeTab);
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
            std::string tabLabel = data.tabs[i].tabName.empty() ? ("Tab " + std::to_string(i + 1)) : data.tabs[i].tabName;
            if (ImGui::Button(tabLabel.c_str())) {
                gameHandler.queryGuildBankTab(static_cast<uint8_t>(i));
            }
            if (selected) ImGui::PopStyleColor();
        }
    }

    // Buy tab button
    if (data.tabs.size() < 6) {
        ImGui::SameLine();
        if (ImGui::Button("Buy Tab")) {
            gameHandler.buyGuildBankTab();
        }
    }

    ImGui::Separator();

    // Tab items (98 slots = 14 columns × 7 rows)
    for (size_t i = 0; i < data.tabItems.size(); i++) {
        if (i % 14 != 0) ImGui::SameLine();
        const auto& item = data.tabItems[i];
        ImGui::PushID(static_cast<int>(i) + 5000);

        if (item.itemEntry == 0) {
            ImGui::Button("##gb", ImVec2(34, 34));
        } else {
            auto* info = gameHandler.getItemInfo(item.itemEntry);
            game::ItemQuality quality = game::ItemQuality::COMMON;
            std::string name = "Item " + std::to_string(item.itemEntry);
            if (info) {
                quality = static_cast<game::ItemQuality>(info->quality);
                name = info->name;
            }
            ImVec4 qc = InventoryScreen::getQualityColor(quality);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(qc.x * 0.3f, qc.y * 0.3f, qc.z * 0.3f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(qc.x * 0.5f, qc.y * 0.5f, qc.z * 0.5f, 0.9f));
            std::string lbl = item.stackCount > 1 ? std::to_string(item.stackCount) : ("##gi" + std::to_string(i));
            if (ImGui::Button(lbl.c_str(), ImVec2(34, 34))) {
                // Withdraw: auto-store to first free bag slot
                gameHandler.guildBankWithdrawItem(activeTab, item.slotId, 0xFF, 0);
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextColored(qc, "%s", name.c_str());
                if (item.stackCount > 1) ImGui::Text("Count: %u", item.stackCount);
                ImGui::EndTooltip();
            }
        }
        ImGui::PopID();
    }

    // Money deposit/withdraw
    ImGui::Separator();
    ImGui::Text("Money:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("##gbg", &guildBankMoneyInput_[0], 0); ImGui::SameLine(); ImGui::Text("g");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    ImGui::InputInt("##gbs", &guildBankMoneyInput_[1], 0); ImGui::SameLine(); ImGui::Text("s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(40);
    ImGui::InputInt("##gbc", &guildBankMoneyInput_[2], 0); ImGui::SameLine(); ImGui::Text("c");

    ImGui::SameLine();
    if (ImGui::Button("Deposit")) {
        uint32_t amount = guildBankMoneyInput_[0] * 10000 + guildBankMoneyInput_[1] * 100 + guildBankMoneyInput_[2];
        if (amount > 0) gameHandler.depositGuildBankMoney(amount);
    }
    ImGui::SameLine();
    if (ImGui::Button("Withdraw")) {
        uint32_t amount = guildBankMoneyInput_[0] * 10000 + guildBankMoneyInput_[1] * 100 + guildBankMoneyInput_[2];
        if (amount > 0) gameHandler.withdrawGuildBankMoney(amount);
    }

    if (data.withdrawAmount >= 0) {
        ImGui::Text("Remaining withdrawals: %d", data.withdrawAmount);
    }

    ImGui::End();

    if (!open) gameHandler.closeGuildBank();
}

// ============================================================
// Auction House Window
// ============================================================

void GameScreen::renderAuctionHouseWindow(game::GameHandler& gameHandler) {
    if (!gameHandler.isAuctionHouseOpen()) return;

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(650, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Auction House", &open)) {
        ImGui::End();
        if (!open) gameHandler.closeAuctionHouse();
        return;
    }

    int tab = gameHandler.getAuctionActiveTab();

    // Tab buttons
    const char* tabNames[] = {"Browse", "Bids", "Auctions"};
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        bool selected = (tab == i);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(tabNames[i], ImVec2(100, 0))) {
            gameHandler.setAuctionActiveTab(i);
            if (i == 1) gameHandler.auctionListBidderItems();
            else if (i == 2) gameHandler.auctionListOwnerItems();
        }
        if (selected) ImGui::PopStyleColor();
    }

    ImGui::Separator();

    if (tab == 0) {
        // Browse tab - Search filters

        // --- Helper: resolve current UI filter state into wire-format search params ---
        // WoW 3.3.5a item class IDs:
        //   0=Consumable, 1=Container, 2=Weapon, 3=Gem, 4=Armor,
        //   7=Projectile/TradeGoods, 9=Recipe, 11=Quiver, 15=Miscellaneous
        struct AHClassMapping { const char* label; uint32_t classId; };
        static const AHClassMapping classMappings[] = {
            {"All",         0xFFFFFFFF},
            {"Weapon",      2},
            {"Armor",       4},
            {"Container",   1},
            {"Consumable",  0},
            {"Trade Goods", 7},
            {"Gem",         3},
            {"Recipe",      9},
            {"Quiver",      11},
            {"Miscellaneous", 15},
        };
        static constexpr int NUM_CLASSES = 10;

        // Weapon subclass IDs (WoW 3.3.5a)
        struct AHSubMapping { const char* label; uint32_t subId; };
        static const AHSubMapping weaponSubs[] = {
            {"All", 0xFFFFFFFF}, {"Axe (1H)", 0}, {"Axe (2H)", 1}, {"Bow", 2},
            {"Gun", 3}, {"Mace (1H)", 4}, {"Mace (2H)", 5}, {"Polearm", 6},
            {"Sword (1H)", 7}, {"Sword (2H)", 8}, {"Staff", 10},
            {"Fist Weapon", 13}, {"Dagger", 15}, {"Thrown", 16},
            {"Crossbow", 18}, {"Wand", 19},
        };
        static constexpr int NUM_WEAPON_SUBS = 16;

        // Armor subclass IDs
        static const AHSubMapping armorSubs[] = {
            {"All", 0xFFFFFFFF}, {"Cloth", 1}, {"Leather", 2}, {"Mail", 3},
            {"Plate", 4}, {"Shield", 6}, {"Miscellaneous", 0},
        };
        static constexpr int NUM_ARMOR_SUBS = 7;

        auto getSearchClassId = [&]() -> uint32_t {
            if (auctionItemClass_ < 0 || auctionItemClass_ >= NUM_CLASSES) return 0xFFFFFFFF;
            return classMappings[auctionItemClass_].classId;
        };

        auto getSearchSubClassId = [&]() -> uint32_t {
            if (auctionItemSubClass_ < 0) return 0xFFFFFFFF;
            uint32_t cid = getSearchClassId();
            if (cid == 2 && auctionItemSubClass_ < NUM_WEAPON_SUBS)
                return weaponSubs[auctionItemSubClass_].subId;
            if (cid == 4 && auctionItemSubClass_ < NUM_ARMOR_SUBS)
                return armorSubs[auctionItemSubClass_].subId;
            return 0xFFFFFFFF;
        };

        auto doSearch = [&](uint32_t offset) {
            auctionBrowseOffset_ = offset;
            if (auctionLevelMin_ < 0) auctionLevelMin_ = 0;
            if (auctionLevelMax_ < 0) auctionLevelMax_ = 0;
            uint32_t q = auctionQuality_ > 0 ? static_cast<uint32_t>(auctionQuality_ - 1) : 0xFFFFFFFF;
            gameHandler.auctionSearch(auctionSearchName_,
                static_cast<uint8_t>(auctionLevelMin_),
                static_cast<uint8_t>(auctionLevelMax_),
                q, getSearchClassId(), getSearchSubClassId(), 0, 0, offset);
        };

        // Row 1: Name + Level range
        ImGui::SetNextItemWidth(200);
        bool enterPressed = ImGui::InputText("Name", auctionSearchName_, sizeof(auctionSearchName_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputInt("Min Lv", &auctionLevelMin_, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputInt("Max Lv", &auctionLevelMax_, 0);

        // Row 2: Quality + Category + Subcategory + Search button
        const char* qualities[] = {"All", "Poor", "Common", "Uncommon", "Rare", "Epic", "Legendary"};
        ImGui::SetNextItemWidth(100);
        ImGui::Combo("Quality", &auctionQuality_, qualities, 7);

        ImGui::SameLine();
        // Build class label list from mappings
        const char* classLabels[NUM_CLASSES];
        for (int c = 0; c < NUM_CLASSES; c++) classLabels[c] = classMappings[c].label;
        ImGui::SetNextItemWidth(120);
        int classIdx = auctionItemClass_ < 0 ? 0 : auctionItemClass_;
        if (ImGui::Combo("Category", &classIdx, classLabels, NUM_CLASSES)) {
            if (classIdx != auctionItemClass_) auctionItemSubClass_ = -1;
            auctionItemClass_ = classIdx;
        }

        // Subcategory (only for Weapon and Armor)
        uint32_t curClassId = getSearchClassId();
        if (curClassId == 2 || curClassId == 4) {
            const AHSubMapping* subs = (curClassId == 2) ? weaponSubs : armorSubs;
            int numSubs = (curClassId == 2) ? NUM_WEAPON_SUBS : NUM_ARMOR_SUBS;
            const char* subLabels[20];
            for (int s = 0; s < numSubs && s < 20; s++) subLabels[s] = subs[s].label;
            int subIdx = auctionItemSubClass_ + 1;  // -1 → 0 ("All")
            if (subIdx < 0 || subIdx >= numSubs) subIdx = 0;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("Subcat", &subIdx, subLabels, numSubs)) {
                auctionItemSubClass_ = subIdx - 1;  // 0 → -1 ("All")
            }
        }

        ImGui::SameLine();
        float delay = gameHandler.getAuctionSearchDelay();
        if (delay > 0.0f) {
            char delayBuf[32];
            snprintf(delayBuf, sizeof(delayBuf), "Search (%.0fs)", delay);
            ImGui::BeginDisabled();
            ImGui::Button(delayBuf);
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Search") || enterPressed) {
                doSearch(0);
            }
        }

        ImGui::Separator();

        // Results table
        const auto& results = gameHandler.getAuctionBrowseResults();
        constexpr uint32_t AH_PAGE_SIZE = 50;
        ImGui::Text("%zu results (of %u total)", results.auctions.size(), results.totalCount);

        // Pagination
        if (results.totalCount > AH_PAGE_SIZE) {
            ImGui::SameLine();
            uint32_t page = auctionBrowseOffset_ / AH_PAGE_SIZE + 1;
            uint32_t totalPages = (results.totalCount + AH_PAGE_SIZE - 1) / AH_PAGE_SIZE;

            if (auctionBrowseOffset_ == 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("< Prev")) {
                uint32_t newOff = (auctionBrowseOffset_ >= AH_PAGE_SIZE) ? auctionBrowseOffset_ - AH_PAGE_SIZE : 0;
                doSearch(newOff);
            }
            if (auctionBrowseOffset_ == 0) ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::Text("Page %u/%u", page, totalPages);

            ImGui::SameLine();
            if (auctionBrowseOffset_ + AH_PAGE_SIZE >= results.totalCount) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Next >")) {
                doSearch(auctionBrowseOffset_ + AH_PAGE_SIZE);
            }
            if (auctionBrowseOffset_ + AH_PAGE_SIZE >= results.totalCount) ImGui::EndDisabled();
        }

        if (ImGui::BeginChild("AuctionResults", ImVec2(0, -110), true)) {
            if (ImGui::BeginTable("AuctionTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Bid", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Buyout", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < results.auctions.size(); i++) {
                    const auto& auction = results.auctions[i];
                    auto* info = gameHandler.getItemInfo(auction.itemEntry);
                    std::string name = info ? info->name : ("Item #" + std::to_string(auction.itemEntry));
                    game::ItemQuality quality = info ? static_cast<game::ItemQuality>(info->quality) : game::ItemQuality::COMMON;
                    ImVec4 qc = InventoryScreen::getQualityColor(quality);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    // Item icon
                    if (info && info->valid && info->displayInfoId != 0) {
                        VkDescriptorSet iconTex = inventoryScreen.getItemIcon(info->displayInfoId);
                        if (iconTex) {
                            ImGui::Image((void*)(intptr_t)iconTex, ImVec2(16, 16));
                            ImGui::SameLine();
                        }
                    }
                    ImGui::TextColored(qc, "%s", name.c_str());
                    // Item tooltip on hover
                    if (ImGui::IsItemHovered() && info && info->valid) {
                        ImGui::BeginTooltip();
                        ImGui::TextColored(qc, "%s", info->name.c_str());
                        if (info->inventoryType > 0) {
                            if (!info->subclassName.empty())
                                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", info->subclassName.c_str());
                        }
                        if (info->armor > 0) ImGui::Text("%d Armor", info->armor);
                        if (info->damageMax > 0.0f && info->delayMs > 0) {
                            float speed = static_cast<float>(info->delayMs) / 1000.0f;
                            ImGui::Text("%.0f - %.0f Damage  Speed %.2f", info->damageMin, info->damageMax, speed);
                        }
                        ImVec4 green(0.0f, 1.0f, 0.0f, 1.0f);
                        std::string bonusLine;
                        auto appendStat = [](std::string& out, int32_t val, const char* n) {
                            if (val <= 0) return;
                            if (!out.empty()) out += "  ";
                            out += "+" + std::to_string(val) + " " + n;
                        };
                        appendStat(bonusLine, info->strength, "Str");
                        appendStat(bonusLine, info->agility, "Agi");
                        appendStat(bonusLine, info->stamina, "Sta");
                        appendStat(bonusLine, info->intellect, "Int");
                        appendStat(bonusLine, info->spirit, "Spi");
                        if (!bonusLine.empty()) ImGui::TextColored(green, "%s", bonusLine.c_str());
                        if (info->sellPrice > 0) {
                            ImGui::TextColored(ImVec4(1, 0.84f, 0, 1), "Sell: %ug %us %uc",
                                info->sellPrice / 10000, (info->sellPrice / 100) % 100, info->sellPrice % 100);
                        }
                        ImGui::EndTooltip();
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", auction.stackCount);

                    ImGui::TableSetColumnIndex(2);
                    // Time left display
                    uint32_t mins = auction.timeLeftMs / 60000;
                    if (mins > 720) ImGui::Text("Long");
                    else if (mins > 120) ImGui::Text("Medium");
                    else ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Short");

                    ImGui::TableSetColumnIndex(3);
                    {
                        uint32_t bid = auction.currentBid > 0 ? auction.currentBid : auction.startBid;
                        ImGui::Text("%ug%us%uc", bid / 10000, (bid / 100) % 100, bid % 100);
                    }

                    ImGui::TableSetColumnIndex(4);
                    if (auction.buyoutPrice > 0) {
                        ImGui::Text("%ug%us%uc", auction.buyoutPrice / 10000,
                                    (auction.buyoutPrice / 100) % 100, auction.buyoutPrice % 100);
                    } else {
                        ImGui::TextDisabled("--");
                    }

                    ImGui::TableSetColumnIndex(5);
                    ImGui::PushID(static_cast<int>(i) + 7000);
                    if (auction.buyoutPrice > 0 && ImGui::SmallButton("Buy")) {
                        gameHandler.auctionBuyout(auction.auctionId, auction.buyoutPrice);
                    }
                    if (auction.buyoutPrice > 0) ImGui::SameLine();
                    if (ImGui::SmallButton("Bid")) {
                        uint32_t bidAmt = auction.currentBid > 0
                            ? auction.currentBid + auction.minBidIncrement
                            : auction.startBid;
                        gameHandler.auctionPlaceBid(auction.auctionId, bidAmt);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        // Sell section
        ImGui::Separator();
        ImGui::Text("Sell Item:");

        // Item picker from backpack
        {
            auto& inv = gameHandler.getInventory();
            // Build list of non-empty backpack slots
            std::string preview = (auctionSellSlotIndex_ >= 0)
                ? ([&]() -> std::string {
                    const auto& slot = inv.getBackpackSlot(auctionSellSlotIndex_);
                    if (!slot.empty()) {
                        std::string s = slot.item.name;
                        if (slot.item.stackCount > 1) s += " x" + std::to_string(slot.item.stackCount);
                        return s;
                    }
                    return "Select item...";
                })()
                : "Select item...";

            ImGui::SetNextItemWidth(250);
            if (ImGui::BeginCombo("##sellitem", preview.c_str())) {
                for (int i = 0; i < game::Inventory::BACKPACK_SLOTS; i++) {
                    const auto& slot = inv.getBackpackSlot(i);
                    if (slot.empty()) continue;
                    ImGui::PushID(i + 9000);
                    // Item icon
                    if (slot.item.displayInfoId != 0) {
                        VkDescriptorSet sIcon = inventoryScreen.getItemIcon(slot.item.displayInfoId);
                        if (sIcon) {
                            ImGui::Image((void*)(intptr_t)sIcon, ImVec2(16, 16));
                            ImGui::SameLine();
                        }
                    }
                    std::string label = slot.item.name;
                    if (slot.item.stackCount > 1) label += " x" + std::to_string(slot.item.stackCount);
                    ImVec4 iqc = InventoryScreen::getQualityColor(slot.item.quality);
                    ImGui::PushStyleColor(ImGuiCol_Text, iqc);
                    if (ImGui::Selectable(label.c_str(), auctionSellSlotIndex_ == i)) {
                        auctionSellSlotIndex_ = i;
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Text("Bid:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputInt("##sbg", &auctionSellBid_[0], 0); ImGui::SameLine(); ImGui::Text("g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(35);
        ImGui::InputInt("##sbs", &auctionSellBid_[1], 0); ImGui::SameLine(); ImGui::Text("s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(35);
        ImGui::InputInt("##sbc", &auctionSellBid_[2], 0); ImGui::SameLine(); ImGui::Text("c");

        ImGui::SameLine(0, 20);
        ImGui::Text("Buyout:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputInt("##sbog", &auctionSellBuyout_[0], 0); ImGui::SameLine(); ImGui::Text("g");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(35);
        ImGui::InputInt("##sbos", &auctionSellBuyout_[1], 0); ImGui::SameLine(); ImGui::Text("s");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(35);
        ImGui::InputInt("##sboc", &auctionSellBuyout_[2], 0); ImGui::SameLine(); ImGui::Text("c");

        const char* durations[] = {"12 hours", "24 hours", "48 hours"};
        ImGui::SetNextItemWidth(90);
        ImGui::Combo("##dur", &auctionSellDuration_, durations, 3);
        ImGui::SameLine();

        // Create Auction button
        bool canCreate = auctionSellSlotIndex_ >= 0 &&
                         !gameHandler.getInventory().getBackpackSlot(auctionSellSlotIndex_).empty() &&
                         (auctionSellBid_[0] > 0 || auctionSellBid_[1] > 0 || auctionSellBid_[2] > 0);
        if (!canCreate) ImGui::BeginDisabled();
        if (ImGui::Button("Create Auction")) {
            uint32_t bidCopper = static_cast<uint32_t>(auctionSellBid_[0]) * 10000
                               + static_cast<uint32_t>(auctionSellBid_[1]) * 100
                               + static_cast<uint32_t>(auctionSellBid_[2]);
            uint32_t buyoutCopper = static_cast<uint32_t>(auctionSellBuyout_[0]) * 10000
                                  + static_cast<uint32_t>(auctionSellBuyout_[1]) * 100
                                  + static_cast<uint32_t>(auctionSellBuyout_[2]);
            const uint32_t durationMins[] = {720, 1440, 2880};
            uint32_t dur = durationMins[auctionSellDuration_];
            uint64_t itemGuid = gameHandler.getBackpackItemGuid(auctionSellSlotIndex_);
            const auto& slot = gameHandler.getInventory().getBackpackSlot(auctionSellSlotIndex_);
            uint32_t stackCount = slot.item.stackCount;
            if (itemGuid != 0) {
                gameHandler.auctionSellItem(itemGuid, stackCount, bidCopper, buyoutCopper, dur);
                // Clear sell inputs
                auctionSellSlotIndex_ = -1;
                auctionSellBid_[0] = auctionSellBid_[1] = auctionSellBid_[2] = 0;
                auctionSellBuyout_[0] = auctionSellBuyout_[1] = auctionSellBuyout_[2] = 0;
            }
        }
        if (!canCreate) ImGui::EndDisabled();

    } else if (tab == 1) {
        // Bids tab
        const auto& results = gameHandler.getAuctionBidderResults();
        ImGui::Text("Your Bids: %zu items", results.auctions.size());

        if (ImGui::BeginTable("BidTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Your Bid", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Buyout", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (size_t bi = 0; bi < results.auctions.size(); bi++) {
                const auto& a = results.auctions[bi];
                auto* info = gameHandler.getItemInfo(a.itemEntry);
                std::string name = info ? info->name : ("Item #" + std::to_string(a.itemEntry));
                game::ItemQuality quality = info ? static_cast<game::ItemQuality>(info->quality) : game::ItemQuality::COMMON;
                ImVec4 bqc = InventoryScreen::getQualityColor(quality);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (info && info->valid && info->displayInfoId != 0) {
                    VkDescriptorSet bIcon = inventoryScreen.getItemIcon(info->displayInfoId);
                    if (bIcon) {
                        ImGui::Image((void*)(intptr_t)bIcon, ImVec2(16, 16));
                        ImGui::SameLine();
                    }
                }
                ImGui::TextColored(bqc, "%s", name.c_str());
                // Tooltip
                if (ImGui::IsItemHovered() && info && info->valid) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(bqc, "%s", info->name.c_str());
                    if (info->armor > 0) ImGui::Text("%d Armor", info->armor);
                    if (info->damageMax > 0.0f && info->delayMs > 0) {
                        float speed = static_cast<float>(info->delayMs) / 1000.0f;
                        ImGui::Text("%.0f - %.0f Damage  Speed %.2f", info->damageMin, info->damageMax, speed);
                    }
                    std::string bl;
                    auto appS = [](std::string& o, int32_t v, const char* n) {
                        if (v <= 0) return;
                        if (!o.empty()) o += "  ";
                        o += "+" + std::to_string(v) + " " + n;
                    };
                    appS(bl, info->strength, "Str"); appS(bl, info->agility, "Agi");
                    appS(bl, info->stamina, "Sta"); appS(bl, info->intellect, "Int");
                    appS(bl, info->spirit, "Spi");
                    if (!bl.empty()) ImGui::TextColored(ImVec4(0,1,0,1), "%s", bl.c_str());
                    if (info->sellPrice > 0)
                        ImGui::TextColored(ImVec4(1,0.84f,0,1), "Sell: %ug %us %uc",
                            info->sellPrice/10000, (info->sellPrice/100)%100, info->sellPrice%100);
                    ImGui::EndTooltip();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", a.stackCount);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%ug%us%uc", a.currentBid / 10000, (a.currentBid / 100) % 100, a.currentBid % 100);
                ImGui::TableSetColumnIndex(3);
                if (a.buyoutPrice > 0)
                    ImGui::Text("%ug%us%uc", a.buyoutPrice / 10000, (a.buyoutPrice / 100) % 100, a.buyoutPrice % 100);
                else
                    ImGui::TextDisabled("--");
                ImGui::TableSetColumnIndex(4);
                uint32_t mins = a.timeLeftMs / 60000;
                if (mins > 720) ImGui::Text("Long");
                else if (mins > 120) ImGui::Text("Medium");
                else ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Short");

                ImGui::TableSetColumnIndex(5);
                ImGui::PushID(static_cast<int>(bi) + 7500);
                if (a.buyoutPrice > 0 && ImGui::SmallButton("Buy")) {
                    gameHandler.auctionBuyout(a.auctionId, a.buyoutPrice);
                }
                if (a.buyoutPrice > 0) ImGui::SameLine();
                if (ImGui::SmallButton("Bid")) {
                    uint32_t bidAmt = a.currentBid > 0
                        ? a.currentBid + a.minBidIncrement
                        : a.startBid;
                    gameHandler.auctionPlaceBid(a.auctionId, bidAmt);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

    } else if (tab == 2) {
        // Auctions tab (your listings)
        const auto& results = gameHandler.getAuctionOwnerResults();
        ImGui::Text("Your Auctions: %zu items", results.auctions.size());

        if (ImGui::BeginTable("OwnerTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Bid", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Buyout", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("##cancel", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < results.auctions.size(); i++) {
                const auto& a = results.auctions[i];
                auto* info = gameHandler.getItemInfo(a.itemEntry);
                std::string name = info ? info->name : ("Item #" + std::to_string(a.itemEntry));
                game::ItemQuality quality = info ? static_cast<game::ItemQuality>(info->quality) : game::ItemQuality::COMMON;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImVec4 oqc = InventoryScreen::getQualityColor(quality);
                if (info && info->valid && info->displayInfoId != 0) {
                    VkDescriptorSet oIcon = inventoryScreen.getItemIcon(info->displayInfoId);
                    if (oIcon) {
                        ImGui::Image((void*)(intptr_t)oIcon, ImVec2(16, 16));
                        ImGui::SameLine();
                    }
                }
                ImGui::TextColored(oqc, "%s", name.c_str());
                if (ImGui::IsItemHovered() && info && info->valid) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(oqc, "%s", info->name.c_str());
                    if (info->armor > 0) ImGui::Text("%d Armor", info->armor);
                    if (info->damageMax > 0.0f && info->delayMs > 0) {
                        float speed = static_cast<float>(info->delayMs) / 1000.0f;
                        ImGui::Text("%.0f - %.0f Damage  Speed %.2f", info->damageMin, info->damageMax, speed);
                    }
                    std::string ol;
                    auto appO = [](std::string& o, int32_t v, const char* n) {
                        if (v <= 0) return;
                        if (!o.empty()) o += "  ";
                        o += "+" + std::to_string(v) + " " + n;
                    };
                    appO(ol, info->strength, "Str"); appO(ol, info->agility, "Agi");
                    appO(ol, info->stamina, "Sta"); appO(ol, info->intellect, "Int");
                    appO(ol, info->spirit, "Spi");
                    if (!ol.empty()) ImGui::TextColored(ImVec4(0,1,0,1), "%s", ol.c_str());
                    if (info->sellPrice > 0)
                        ImGui::TextColored(ImVec4(1,0.84f,0,1), "Sell: %ug %us %uc",
                            info->sellPrice/10000, (info->sellPrice/100)%100, info->sellPrice%100);
                    ImGui::EndTooltip();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", a.stackCount);
                ImGui::TableSetColumnIndex(2);
                {
                    uint32_t bid = a.currentBid > 0 ? a.currentBid : a.startBid;
                    ImGui::Text("%ug%us%uc", bid / 10000, (bid / 100) % 100, bid % 100);
                }
                ImGui::TableSetColumnIndex(3);
                if (a.buyoutPrice > 0)
                    ImGui::Text("%ug%us%uc", a.buyoutPrice / 10000, (a.buyoutPrice / 100) % 100, a.buyoutPrice % 100);
                else
                    ImGui::TextDisabled("--");
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(static_cast<int>(i) + 8000);
                if (ImGui::SmallButton("Cancel")) {
                    gameHandler.auctionCancelItem(a.auctionId);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();

    if (!open) gameHandler.closeAuctionHouse();
}

// ============================================================
// Level-Up Ding Animation
// ============================================================

void GameScreen::triggerDing(uint32_t newLevel) {
    dingTimer_ = DING_DURATION;
    dingLevel_ = newLevel;

    auto* renderer = core::Application::getInstance().getRenderer();
    if (renderer) {
        if (auto* sfx = renderer->getUiSoundManager()) {
            sfx->playLevelUp();
        }
        renderer->playEmote("cheer");
    }
}

void GameScreen::renderDingEffect() {
    if (dingTimer_ <= 0.0f) return;

    float dt = ImGui::GetIO().DeltaTime;
    dingTimer_ -= dt;
    if (dingTimer_ < 0.0f) dingTimer_ = 0.0f;

    // Show "You have reached level X!" for the first 2.5s, fade out over last 0.5s.
    // The 3D visual effect is handled by Renderer::triggerLevelUpEffect (LevelUp.m2).
    constexpr float kFadeTime = 0.5f;
    float alpha = dingTimer_ < kFadeTime ? (dingTimer_ / kFadeTime) : 1.0f;
    if (alpha <= 0.0f) return;

    ImGuiIO& io = ImGui::GetIO();
    float cx = io.DisplaySize.x * 0.5f;
    float cy = io.DisplaySize.y * 0.38f;  // Upper-center, like WoW

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float baseSize = ImGui::GetFontSize();
    float fontSize = baseSize * 1.8f;

    char buf[64];
    snprintf(buf, sizeof(buf), "You have reached level %u!", dingLevel_);

    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);
    float tx = cx - sz.x * 0.5f;
    float ty = cy - sz.y * 0.5f;

    // Slight black outline for readability
    draw->AddText(font, fontSize, ImVec2(tx + 2, ty + 2),
                  IM_COL32(0, 0, 0, (int)(alpha * 180)), buf);
    // Gold text
    draw->AddText(font, fontSize, ImVec2(tx, ty),
                  IM_COL32(255, 210, 0, (int)(alpha * 255)), buf);
}

void GameScreen::triggerAchievementToast(uint32_t achievementId) {
    achievementToastId_    = achievementId;
    achievementToastTimer_ = ACHIEVEMENT_TOAST_DURATION;

    // Play a UI sound if available
    auto* renderer = core::Application::getInstance().getRenderer();
    if (renderer) {
        if (auto* sfx = renderer->getUiSoundManager()) {
            sfx->playAchievementAlert();
        }
    }
}

void GameScreen::renderAchievementToast() {
    if (achievementToastTimer_ <= 0.0f) return;

    float dt = ImGui::GetIO().DeltaTime;
    achievementToastTimer_ -= dt;
    if (achievementToastTimer_ < 0.0f) achievementToastTimer_ = 0.0f;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth())  : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    // Slide in from the right — fully visible for most of the duration, slides out at end
    constexpr float SLIDE_TIME = 0.4f;
    float  slideIn  = std::min(achievementToastTimer_, ACHIEVEMENT_TOAST_DURATION - achievementToastTimer_);
    float  slideFrac = (ACHIEVEMENT_TOAST_DURATION > 0.0f && SLIDE_TIME > 0.0f)
                         ? std::min(slideIn / SLIDE_TIME, 1.0f)
                         : 1.0f;

    constexpr float TOAST_W = 280.0f;
    constexpr float TOAST_H =  60.0f;
    float xFull   = screenW - TOAST_W - 20.0f;
    float xHidden = screenW + 10.0f;
    float toastX  = xHidden + (xFull - xHidden) * slideFrac;
    float toastY  = screenH - TOAST_H - 80.0f;  // above action bar area

    float alpha = std::min(1.0f, achievementToastTimer_ / 0.5f);  // fade at very end

    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Background panel (gold border, dark fill)
    ImVec2 tl(toastX,            toastY);
    ImVec2 br(toastX + TOAST_W,  toastY + TOAST_H);
    draw->AddRectFilled(tl, br, IM_COL32(30, 20, 10, (int)(alpha * 230)), 6.0f);
    draw->AddRect(tl, br, IM_COL32(200, 170, 50, (int)(alpha * 255)), 6.0f, 0, 2.0f);

    // Title
    ImFont* font = ImGui::GetFont();
    float   titleSize = 14.0f;
    float   bodySize  = 12.0f;
    const char* title = "Achievement Earned!";
    float titleW = font->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title).x;
    float titleX = toastX + (TOAST_W - titleW) * 0.5f;
    draw->AddText(font, titleSize, ImVec2(titleX + 1, toastY + 8 + 1),
                  IM_COL32(0, 0, 0, (int)(alpha * 180)), title);
    draw->AddText(font, titleSize, ImVec2(titleX, toastY + 8),
                  IM_COL32(255, 215, 0, (int)(alpha * 255)), title);

    // Achievement ID line (until we have Achievement.dbc name lookup)
    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "Achievement #%u", achievementToastId_);
    float idW = font->CalcTextSizeA(bodySize, FLT_MAX, 0.0f, idBuf).x;
    float idX = toastX + (TOAST_W - idW) * 0.5f;
    draw->AddText(font, bodySize, ImVec2(idX, toastY + 28),
                  IM_COL32(220, 200, 150, (int)(alpha * 255)), idBuf);
}

// ---------------------------------------------------------------------------
// Zone discovery text — "Entering: <ZoneName>" fades in/out at screen centre
// ---------------------------------------------------------------------------

void GameScreen::renderZoneText() {
    // Poll the renderer for zone name changes
    auto* appRenderer = core::Application::getInstance().getRenderer();
    if (appRenderer) {
        const std::string& zoneName = appRenderer->getCurrentZoneName();
        if (!zoneName.empty() && zoneName != lastKnownZoneName_) {
            lastKnownZoneName_ = zoneName;
            zoneTextName_  = zoneName;
            zoneTextTimer_ = ZONE_TEXT_DURATION;
        }
    }

    if (zoneTextTimer_ <= 0.0f || zoneTextName_.empty()) return;

    float dt = ImGui::GetIO().DeltaTime;
    zoneTextTimer_ -= dt;
    if (zoneTextTimer_ < 0.0f) zoneTextTimer_ = 0.0f;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth())  : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    // Fade: ramp up in first 0.5 s, hold, fade out in last 1.0 s
    float alpha;
    if (zoneTextTimer_ > ZONE_TEXT_DURATION - 0.5f)
        alpha = 1.0f - (zoneTextTimer_ - (ZONE_TEXT_DURATION - 0.5f)) / 0.5f;
    else if (zoneTextTimer_ < 1.0f)
        alpha = zoneTextTimer_;
    else
        alpha = 1.0f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    ImFont* font = ImGui::GetFont();

    // "Entering:" header
    const char* header = "Entering:";
    float headerSize = 16.0f;
    float nameSize   = 26.0f;

    ImVec2 headerDim = font->CalcTextSizeA(headerSize, FLT_MAX, 0.0f, header);
    ImVec2 nameDim   = font->CalcTextSizeA(nameSize,   FLT_MAX, 0.0f, zoneTextName_.c_str());

    float centreY = screenH * 0.30f;  // upper third, like WoW
    float headerX = (screenW - headerDim.x) * 0.5f;
    float nameX   = (screenW - nameDim.x)   * 0.5f;
    float headerY = centreY;
    float nameY   = centreY + headerDim.y + 4.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // "Entering:" in gold
    draw->AddText(font, headerSize, ImVec2(headerX + 1, headerY + 1),
                  IM_COL32(0, 0, 0, (int)(alpha * 160)), header);
    draw->AddText(font, headerSize, ImVec2(headerX, headerY),
                  IM_COL32(255, 215, 0, (int)(alpha * 255)), header);

    // Zone name in white
    draw->AddText(font, nameSize, ImVec2(nameX + 1, nameY + 1),
                  IM_COL32(0, 0, 0, (int)(alpha * 160)), zoneTextName_.c_str());
    draw->AddText(font, nameSize, ImVec2(nameX, nameY),
                  IM_COL32(255, 255, 255, (int)(alpha * 255)), zoneTextName_.c_str());
}

// ---------------------------------------------------------------------------
// Dungeon Finder window (toggle with hotkey or bag-bar button)
// ---------------------------------------------------------------------------
void GameScreen::renderDungeonFinderWindow(game::GameHandler& gameHandler) {
    // Toggle on I key when not typing
    if (!chatInputActive && ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        showDungeonFinder_ = !showDungeonFinder_;
    }

    if (!showDungeonFinder_) return;

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth())  : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) :  720.0f;

    ImGui::SetNextWindowPos(ImVec2(screenW * 0.5f - 175.0f, screenH * 0.2f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_Always);

    bool open = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("Dungeon Finder", &open, flags)) {
        ImGui::End();
        if (!open) showDungeonFinder_ = false;
        return;
    }
    if (!open) {
        ImGui::End();
        showDungeonFinder_ = false;
        return;
    }

    using LfgState = game::GameHandler::LfgState;
    LfgState state = gameHandler.getLfgState();

    // ---- Status banner ----
    switch (state) {
        case LfgState::None:
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Status: Not queued");
            break;
        case LfgState::RoleCheck:
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Status: Role check in progress...");
            break;
        case LfgState::Queued: {
            int32_t avgSec  = gameHandler.getLfgAvgWaitSec();
            uint32_t qMs    = gameHandler.getLfgTimeInQueueMs();
            int      qMin   = static_cast<int>(qMs / 60000);
            int      qSec   = static_cast<int>((qMs % 60000) / 1000);
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Status: In queue (%d:%02d)", qMin, qSec);
            if (avgSec >= 0) {
                int aMin = avgSec / 60;
                int aSec = avgSec % 60;
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                                   "Avg wait: %d:%02d", aMin, aSec);
            }
            break;
        }
        case LfgState::Proposal:
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "Status: Group found!");
            break;
        case LfgState::Boot:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Status: Vote kick in progress");
            break;
        case LfgState::InDungeon:
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Status: In dungeon");
            break;
        case LfgState::FinishedDungeon:
            ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "Status: Dungeon complete");
            break;
        case LfgState::RaidBrowser:
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Status: Raid browser");
            break;
    }

    ImGui::Separator();

    // ---- Proposal accept/decline ----
    if (state == LfgState::Proposal) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                           "A group has been found for your dungeon!");
        ImGui::Spacing();
        if (ImGui::Button("Accept", ImVec2(120, 0))) {
            gameHandler.lfgAcceptProposal(gameHandler.getLfgProposalId(), true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(120, 0))) {
            gameHandler.lfgAcceptProposal(gameHandler.getLfgProposalId(), false);
        }
        ImGui::Separator();
    }

    // ---- Vote-to-kick buttons ----
    if (state == LfgState::Boot) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Vote to kick in progress:");
        ImGui::Spacing();
        if (ImGui::Button("Vote Yes (kick)", ImVec2(140, 0))) {
            gameHandler.lfgSetBootVote(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Vote No (keep)", ImVec2(140, 0))) {
            gameHandler.lfgSetBootVote(false);
        }
        ImGui::Separator();
    }

    // ---- Teleport button (in dungeon) ----
    if (state == LfgState::InDungeon) {
        if (ImGui::Button("Teleport to Dungeon", ImVec2(-1, 0))) {
            gameHandler.lfgTeleport(true);
        }
        ImGui::Separator();
    }

    // ---- Role selection (only when not queued/in dungeon) ----
    bool canConfigure = (state == LfgState::None || state == LfgState::FinishedDungeon);

    if (canConfigure) {
        ImGui::Text("Role:");
        ImGui::SameLine();
        bool isTank   = (lfgRoles_ & 0x02) != 0;
        bool isHealer = (lfgRoles_ & 0x04) != 0;
        bool isDps    = (lfgRoles_ & 0x08) != 0;
        if (ImGui::Checkbox("Tank",   &isTank))   lfgRoles_ = (lfgRoles_ & ~0x02) | (isTank   ? 0x02 : 0);
        ImGui::SameLine();
        if (ImGui::Checkbox("Healer", &isHealer)) lfgRoles_ = (lfgRoles_ & ~0x04) | (isHealer ? 0x04 : 0);
        ImGui::SameLine();
        if (ImGui::Checkbox("DPS",    &isDps))    lfgRoles_ = (lfgRoles_ & ~0x08) | (isDps    ? 0x08 : 0);

        ImGui::Spacing();

        // ---- Dungeon selection ----
        ImGui::Text("Dungeon:");

        struct DungeonEntry { uint32_t id; const char* name; };
        static const DungeonEntry kDungeons[] = {
            { 861, "Random Dungeon" },
            { 862, "Random Heroic" },
            // Vanilla classics
            {  36, "Deadmines" },
            {  43, "Ragefire Chasm" },
            {  47, "Razorfen Kraul" },
            {  48, "Blackfathom Deeps" },
            {  52, "Uldaman" },
            {  57, "Dire Maul: East" },
            {  70, "Onyxia's Lair" },
            // TBC heroics
            { 264, "The Blood Furnace" },
            { 269, "The Shattered Halls" },
            // WotLK normals/heroics
            { 576, "The Nexus" },
            { 578, "The Oculus" },
            { 595, "The Culling of Stratholme" },
            { 599, "Halls of Stone" },
            { 600, "Drak'Tharon Keep" },
            { 601, "Azjol-Nerub" },
            { 604, "Gundrak" },
            { 608, "Violet Hold" },
            { 619, "Ahn'kahet: Old Kingdom" },
            { 623, "Halls of Lightning" },
            { 632, "The Forge of Souls" },
            { 650, "Trial of the Champion" },
            { 658, "Pit of Saron" },
            { 668, "Halls of Reflection" },
        };

        // Find current index
        int curIdx = 0;
        for (int i = 0; i < (int)(sizeof(kDungeons)/sizeof(kDungeons[0])); ++i) {
            if (kDungeons[i].id == lfgSelectedDungeon_) { curIdx = i; break; }
        }

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##dungeon", kDungeons[curIdx].name)) {
            for (int i = 0; i < (int)(sizeof(kDungeons)/sizeof(kDungeons[0])); ++i) {
                bool selected = (kDungeons[i].id == lfgSelectedDungeon_);
                if (ImGui::Selectable(kDungeons[i].name, selected))
                    lfgSelectedDungeon_ = kDungeons[i].id;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // ---- Join button ----
        bool rolesOk = (lfgRoles_ != 0);
        if (!rolesOk) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Join Dungeon Finder", ImVec2(-1, 0))) {
            gameHandler.lfgJoin(lfgSelectedDungeon_, lfgRoles_);
        }
        if (!rolesOk) {
            ImGui::EndDisabled();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Select at least one role.");
        }
    }

    // ---- Leave button (when queued or role check) ----
    if (state == LfgState::Queued || state == LfgState::RoleCheck) {
        if (ImGui::Button("Leave Queue", ImVec2(-1, 0))) {
            gameHandler.lfgLeave();
        }
    }

    ImGui::End();
}

// ============================================================
// Instance Lockouts
// ============================================================

void GameScreen::renderInstanceLockouts(game::GameHandler& gameHandler) {
    if (!showInstanceLockouts_) return;

    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().DisplaySize.x / 2 - 240, 140), ImGuiCond_Appearing);

    if (!ImGui::Begin("Instance Lockouts", &showInstanceLockouts_,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    const auto& lockouts = gameHandler.getInstanceLockouts();

    if (lockouts.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No active instance lockouts.");
    } else {
        // Build map name lookup from Map.dbc (cached after first call)
        static std::unordered_map<uint32_t, std::string> sMapNames;
        static bool sMapNamesLoaded = false;
        if (!sMapNamesLoaded) {
            sMapNamesLoaded = true;
            if (auto* am = core::Application::getInstance().getAssetManager()) {
                if (auto dbc = am->loadDBC("Map.dbc"); dbc && dbc->isLoaded()) {
                    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                        uint32_t id = dbc->getUInt32(i, 0);
                        // Field 2 = MapName_enUS (first localized), field 1 = InternalName
                        std::string name = dbc->getString(i, 2);
                        if (name.empty()) name = dbc->getString(i, 1);
                        if (!name.empty()) sMapNames[id] = std::move(name);
                    }
                }
            }
        }

        auto difficultyLabel = [](uint32_t diff) -> const char* {
            switch (diff) {
                case 0: return "Normal";
                case 1: return "Heroic";
                case 2: return "25-Man";
                case 3: return "25-Man Heroic";
                default: return "Unknown";
            }
        };

        // Current UTC time for reset countdown
        auto nowSec = static_cast<uint64_t>(std::time(nullptr));

        if (ImGui::BeginTable("lockouts", 4,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupColumn("Instance",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Difficulty", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Resets In",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Status",     ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableHeadersRow();

            for (const auto& lo : lockouts) {
                ImGui::TableNextRow();

                // Instance name
                ImGui::TableSetColumnIndex(0);
                auto it = sMapNames.find(lo.mapId);
                if (it != sMapNames.end()) {
                    ImGui::TextUnformatted(it->second.c_str());
                } else {
                    ImGui::Text("Map %u", lo.mapId);
                }

                // Difficulty
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(difficultyLabel(lo.difficulty));

                // Reset countdown
                ImGui::TableSetColumnIndex(2);
                if (lo.resetTime > nowSec) {
                    uint64_t remaining = lo.resetTime - nowSec;
                    uint64_t days  = remaining / 86400;
                    uint64_t hours = (remaining % 86400) / 3600;
                    if (days > 0) {
                        ImGui::Text("%llud %lluh",
                            static_cast<unsigned long long>(days),
                            static_cast<unsigned long long>(hours));
                    } else {
                        uint64_t mins = (remaining % 3600) / 60;
                        ImGui::Text("%lluh %llum",
                            static_cast<unsigned long long>(hours),
                            static_cast<unsigned long long>(mins));
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Expired");
                }

                // Locked / Extended status
                ImGui::TableSetColumnIndex(3);
                if (lo.extended) {
                    ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), "Ext");
                } else if (lo.locked) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Locked");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "Open");
                }
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();
}

// ============================================================================
// Battleground score frame
//
// Displays the current score for the player's battleground using world states.
// Shown in the top-centre of the screen whenever SMSG_INIT_WORLD_STATES has
// been received for a known BG map.  The layout adapts per battleground:
//
//   WSG  489 – Alliance / Horde flag captures (max 3)
//   AB   529 – Alliance / Horde resource scores (max 1600)
//   AV    30 – Alliance / Horde reinforcements
//   EotS 566 – Alliance / Horde resource scores (max 1600)
// ============================================================================
void GameScreen::renderBattlegroundScore(game::GameHandler& gameHandler) {
    // Only show when in a recognised battleground map
    uint32_t mapId = gameHandler.getWorldStateMapId();

    // World state key sets per battleground
    // Keys from the WoW 3.3.5a WorldState.dbc / client source
    struct BgScoreDef {
        uint32_t mapId;
        const char* name;
        uint32_t allianceKey;   // world state key for Alliance value
        uint32_t hordeKey;      // world state key for Horde value
        uint32_t maxKey;        // max score world state key (0 = use hardcoded)
        uint32_t hardcodedMax;  // used when maxKey == 0
        const char* unit;       // suffix label (e.g. "flags", "resources")
    };

    static constexpr BgScoreDef kBgDefs[] = {
        // Warsong Gulch: 3 flag captures wins
        { 489, "Warsong Gulch", 1581, 1582, 0, 3, "flags" },
        // Arathi Basin: 1600 resources wins
        { 529, "Arathi Basin",  1218, 1219, 0, 1600, "resources" },
        // Alterac Valley: reinforcements count down from 600 / 800 etc.
        {  30, "Alterac Valley", 1322, 1323, 0, 600, "reinforcements" },
        // Eye of the Storm: 1600 resources wins
        { 566, "Eye of the Storm", 2757, 2758, 0, 1600, "resources" },
        // Strand of the Ancients (WotLK)
        { 607, "Strand of the Ancients", 3476, 3477, 0, 4, "" },
    };

    const BgScoreDef* def = nullptr;
    for (const auto& d : kBgDefs) {
        if (d.mapId == mapId) { def = &d; break; }
    }
    if (!def) return;

    auto allianceOpt = gameHandler.getWorldState(def->allianceKey);
    auto hordeOpt    = gameHandler.getWorldState(def->hordeKey);
    if (!allianceOpt && !hordeOpt) return;

    uint32_t allianceScore = allianceOpt.value_or(0);
    uint32_t hordeScore    = hordeOpt.value_or(0);
    uint32_t maxScore      = def->hardcodedMax;
    if (def->maxKey != 0) {
        if (auto mv = gameHandler.getWorldState(def->maxKey)) maxScore = *mv;
    }

    auto* window = core::Application::getInstance().getWindow();
    float screenW = window ? static_cast<float>(window->getWidth())  : 1280.0f;

    // Width scales with screen but stays reasonable
    float frameW = 260.0f;
    float frameH = 60.0f;
    float posX   = screenW / 2.0f - frameW / 2.0f;
    float posY   = 4.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(frameW, frameH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(6.0f, 4.0f));

    if (ImGui::Begin("##BGScore", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoSavedSettings)) {

        // BG name centred at top
        float nameW = ImGui::CalcTextSize(def->name).x;
        ImGui::SetCursorPosX((frameW - nameW) / 2.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "%s", def->name);

        // Alliance score | separator | Horde score
        float innerW  = frameW - 12.0f;
        float halfW   = innerW / 2.0f - 4.0f;

        ImGui::SetCursorPosX(6.0f);
        ImGui::BeginGroup();
        {
            // Alliance (blue)
            char aBuf[32];
            if (maxScore > 0 && strlen(def->unit) > 0)
                snprintf(aBuf, sizeof(aBuf), "\xF0\x9F\x94\xB5 %u / %u", allianceScore, maxScore);
            else
                snprintf(aBuf, sizeof(aBuf), "\xF0\x9F\x94\xB5 %u", allianceScore);
            ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", aBuf);
        }
        ImGui::EndGroup();

        ImGui::SameLine(halfW + 16.0f);

        ImGui::BeginGroup();
        {
            // Horde (red)
            char hBuf[32];
            if (maxScore > 0 && strlen(def->unit) > 0)
                snprintf(hBuf, sizeof(hBuf), "\xF0\x9F\x94\xB4 %u / %u", hordeScore, maxScore);
            else
                snprintf(hBuf, sizeof(hBuf), "\xF0\x9F\x94\xB4 %u", hordeScore);
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", hBuf);
        }
        ImGui::EndGroup();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}} // namespace wowee::ui
