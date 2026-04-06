#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/chat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/social_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/warden_handler.hpp"
#include "game/packet_parsers.hpp"
#include "game/transport_manager.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_memory.hpp"
#include "game/warden_module.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/expansion_profile.hpp"
#include "rendering/renderer.hpp"
#include "rendering/spell_visual_system.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "auth/crypto.hpp"
#include "core/coordinates.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "game/protocol_constants.hpp"
#include "rendering/animation/animation_ids.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <random>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <openssl/sha.h>
#include <openssl/hmac.h>

namespace wowee {
namespace game {

namespace {
const char* worldStateName(WorldState state) {
    switch (state) {
        case WorldState::DISCONNECTED: return "DISCONNECTED";
        case WorldState::CONNECTING: return "CONNECTING";
        case WorldState::CONNECTED: return "CONNECTED";
        case WorldState::CHALLENGE_RECEIVED: return "CHALLENGE_RECEIVED";
        case WorldState::AUTH_SENT: return "AUTH_SENT";
        case WorldState::AUTHENTICATED: return "AUTHENTICATED";
        case WorldState::READY: return "READY";
        case WorldState::CHAR_LIST_REQUESTED: return "CHAR_LIST_REQUESTED";
        case WorldState::CHAR_LIST_RECEIVED: return "CHAR_LIST_RECEIVED";
        case WorldState::ENTERING_WORLD: return "ENTERING_WORLD";
        case WorldState::IN_WORLD: return "IN_WORLD";
        case WorldState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

} // end anonymous namespace

std::string formatCopperAmount(uint32_t amount) {
    uint32_t gold = amount / game::COPPER_PER_GOLD;
    uint32_t silver = (amount / game::COPPER_PER_SILVER) % 100;
    uint32_t copper = amount % game::COPPER_PER_SILVER;

    std::ostringstream oss;
    bool wrote = false;
    if (gold > 0) {
        oss << gold << "g";
        wrote = true;
    }
    if (silver > 0) {
        if (wrote) oss << " ";
        oss << silver << "s";
        wrote = true;
    }
    if (copper > 0 || !wrote) {
        if (wrote) oss << " ";
        oss << copper << "c";
    }
    return oss.str();
}

// Registration helpers for common dispatch table patterns
void GameHandler::registerSkipHandler(LogicalOpcode op) {
    dispatchTable_[op] = [](network::Packet& packet) { packet.skipAll(); };
}
void GameHandler::registerErrorHandler(LogicalOpcode op, const char* msg) {
    dispatchTable_[op] = [this, msg](network::Packet&) {
        addUIError(msg);
        addSystemChatMessage(msg);
    };
}
void GameHandler::registerHandler(LogicalOpcode op, void (GameHandler::*handler)(network::Packet&)) {
    dispatchTable_[op] = [this, handler](network::Packet& packet) { (this->*handler)(packet); };
}
void GameHandler::registerWorldHandler(LogicalOpcode op, void (GameHandler::*handler)(network::Packet&)) {
    dispatchTable_[op] = [this, handler](network::Packet& packet) {
        if (state == WorldState::IN_WORLD) (this->*handler)(packet);
    };
}

GameHandler::GameHandler(GameServices& services)
    : services_(services) {
    LOG_DEBUG("GameHandler created");

    setActiveOpcodeTable(&opcodeTable_);
    setActiveUpdateFieldTable(&updateFieldTable_);

    // Initialize packet parsers (WotLK default, may be replaced for other expansions)
    packetParsers_ = std::make_unique<WotlkPacketParsers>();

    // Initialize transport manager
    transportManager_ = std::make_unique<TransportManager>();

    // Initialize Warden module manager
    wardenModuleManager_ = std::make_unique<WardenModuleManager>();

    // Initialize domain handlers
    entityController_ = std::make_unique<EntityController>(*this);
    chatHandler_      = std::make_unique<ChatHandler>(*this);
    movementHandler_  = std::make_unique<MovementHandler>(*this);
    combatHandler_    = std::make_unique<CombatHandler>(*this);
    spellHandler_     = std::make_unique<SpellHandler>(*this);
    inventoryHandler_ = std::make_unique<InventoryHandler>(*this);
    socialHandler_    = std::make_unique<SocialHandler>(*this);
    questHandler_     = std::make_unique<QuestHandler>(*this);
    wardenHandler_    = std::make_unique<WardenHandler>(*this);
    wardenHandler_->initModuleManager();

    // Default action bar layout
    actionBar[0].type = ActionBarSlot::SPELL;
    actionBar[0].id = game::SPELL_ID_ATTACK;   // Attack in slot 1
    actionBar[11].type = ActionBarSlot::SPELL;
    actionBar[11].id = game::SPELL_ID_HEARTHSTONE;  // Hearthstone in slot 12

    // Build the opcode dispatch table (replaces switch(*logicalOp) in handlePacket)
    registerOpcodeHandlers();
}

GameHandler::~GameHandler() {
    disconnect();
}

void GameHandler::setPacketParsers(std::unique_ptr<PacketParsers> parsers) {
    packetParsers_ = std::move(parsers);
}

bool GameHandler::connect(const std::string& host,
                          uint16_t port,
                          const std::vector<uint8_t>& sessionKey,
                          const std::string& accountName,
                          uint32_t build,
                          uint32_t realmId) {

    if (sessionKey.size() != 40) {
        LOG_ERROR("Invalid session key size: ", sessionKey.size(), " (expected 40)");
        fail("Invalid session key");
        return false;
    }

    LOG_INFO("========================================");
    LOG_INFO("   CONNECTING TO WORLD SERVER");
    LOG_INFO("========================================");
    LOG_INFO("Host: ", host);
    LOG_INFO("Port: ", port);
    LOG_INFO("Account: ", accountName);
    LOG_INFO("Build: ", build);

    // Store authentication data
    this->sessionKey = sessionKey;
    this->accountName = accountName;
    this->build = build;
    this->realmId_ = realmId;

    // Diagnostic: dump session key for AUTH_REJECT debugging
    LOG_INFO("GameHandler session key (", sessionKey.size(), "): ",
             core::toHexString(sessionKey.data(), sessionKey.size()));
    resetWardenState();

    // Generate random client seed
    this->clientSeed = generateClientSeed();
    LOG_DEBUG("Generated client seed: 0x", std::hex, clientSeed, std::dec);

    // Create world socket
    socket = std::make_unique<network::WorldSocket>();

    // Set up packet callback
    socket->setPacketCallback([this](const network::Packet& packet) {
        enqueueIncomingPacket(packet);
    });

    // Connect to world server
    setState(WorldState::CONNECTING);

    if (!socket->connect(host, port)) {
        LOG_ERROR("Failed to connect to world server");
        fail("Connection failed");
        return false;
    }

    setState(WorldState::CONNECTED);
    LOG_INFO("Connected to world server, waiting for SMSG_AUTH_CHALLENGE...");

    return true;
}

void GameHandler::resetWardenState() {
    requiresWarden_ = false;
    wardenGateSeen_ = false;
    wardenGateElapsed_ = 0.0f;
    wardenGateNextStatusLog_ = 2.0f;
    wardenPacketsAfterGate_ = 0;
    wardenCharEnumBlockedLogged_ = false;
    wardenCrypto_.reset();
    wardenState_ = WardenState::WAIT_MODULE_USE;
    wardenModuleHash_.clear();
    wardenModuleKey_.clear();
    wardenModuleSize_ = 0;
    wardenModuleData_.clear();
    wardenLoadedModule_.reset();
}

void GameHandler::disconnect() {
    if (onTaxiFlight_) {
        taxiRecoverPending_ = true;
    } else {
        taxiRecoverPending_ = false;
    }
    if (socket) {
        socket->disconnect();
        socket.reset();
    }
    activeCharacterGuid_ = 0;
    guildNameCache_.clear();
    pendingGuildNameQueries_.clear();
    friendGuids_.clear();
    contacts_.clear();
    transportAttachments_.clear();
    resetWardenState();
    pendingIncomingPackets_.clear();
    // Fire despawn callbacks so the renderer releases M2/character model resources.
    for (const auto& [guid, entity] : entityController_->getEntityManager().getEntities()) {
        if (guid == playerGuid) continue;
        if (entity->getType() == ObjectType::UNIT && creatureDespawnCallback_)
            creatureDespawnCallback_(guid);
        else if (entity->getType() == ObjectType::PLAYER && playerDespawnCallback_)
            playerDespawnCallback_(guid);
        else if (entity->getType() == ObjectType::GAMEOBJECT && gameObjectDespawnCallback_)
            gameObjectDespawnCallback_(guid);
    }
    otherPlayerVisibleItemEntries_.clear();
    otherPlayerVisibleDirty_.clear();
    otherPlayerMoveTimeMs_.clear();
    if (spellHandler_) spellHandler_->clearUnitCastStates();
    if (spellHandler_) spellHandler_->clearUnitAurasCache();
    if (combatHandler_) combatHandler_->clearCombatText();
    entityController_->clearAll();
    setState(WorldState::DISCONNECTED);
    LOG_INFO("Disconnected from world server");
}

void GameHandler::resetDbcCaches() {
    spellNameCacheLoaded_ = false;
    spellNameCache_.clear();
    skillLineDbcLoaded_ = false;
    skillLineNames_.clear();
    skillLineCategories_.clear();
    skillLineAbilityLoaded_ = false;
    spellToSkillLine_.clear();
    taxiDbcLoaded_ = false;
    taxiNodes_.clear();
    taxiPathEdges_.clear();
    taxiPathNodes_.clear();
    areaTriggerDbcLoaded_ = false;
    areaTriggers_.clear();
    activeAreaTriggers_.clear();
    talentDbcLoaded_ = false;
    talentCache_.clear();
    talentTabCache_.clear();
    // Clear the AssetManager DBC file cache so that expansion-specific DBCs
    // (CharSections, ItemDisplayInfo, etc.) are reloaded from the new expansion's
    // MPQ files instead of returning stale data from a previous session/expansion.
    auto* am = services_.assetManager;
    if (am) {
        am->clearDBCCache();
    }
    LOG_INFO("GameHandler: DBC caches cleared for expansion switch");
}

bool GameHandler::isConnected() const {
    return socket && socket->isConnected();
}

void GameHandler::updateNetworking(float deltaTime) {
    // Reset per-tick monster-move budget tracking (Classic/Turtle flood protection).
    if (movementHandler_) {
        movementHandler_->monsterMovePacketsThisTickRef() = 0;
        movementHandler_->monsterMovePacketsDroppedThisTickRef() = 0;
    }

    // Update socket (processes incoming data and triggers callbacks)
    if (socket) {
        auto socketStart = std::chrono::steady_clock::now();
        socket->update();
        float socketMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - socketStart).count();
        if (socketMs > 3.0f) {
            LOG_WARNING("SLOW socket->update: ", socketMs, "ms");
        }
    }

    {
        auto packetStart = std::chrono::steady_clock::now();
        processQueuedIncomingPackets();
        float packetMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - packetStart).count();
        if (packetMs > 3.0f) {
            LOG_WARNING("SLOW queued packet handling: ", packetMs, "ms");
        }
    }

    // Drain pending async Warden response (built on background thread to avoid 5s stalls)
    if (wardenResponsePending_) {
        auto status = wardenPendingEncrypted_.wait_for(std::chrono::milliseconds(0));
        if (status == std::future_status::ready) {
            auto plaintext = wardenPendingEncrypted_.get();
            wardenResponsePending_ = false;
            if (!plaintext.empty() && wardenCrypto_) {
                std::vector<uint8_t> encrypted = wardenCrypto_->encrypt(plaintext);
                network::Packet response(wireOpcode(Opcode::CMSG_WARDEN_DATA));
                for (uint8_t byte : encrypted) {
                    response.writeUInt8(byte);
                }
                if (socket && socket->isConnected()) {
                    socket->send(response);
                    LOG_WARNING("Warden: Sent async CHEAT_CHECKS_RESULT (", plaintext.size(), " bytes plaintext)");
                }
            }
        }
    }

    // Detect RX silence (server stopped sending packets but TCP still open)
    if (isInWorld() && socket->isConnected() &&
        lastRxTime_.time_since_epoch().count() > 0) {
        auto silenceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastRxTime_).count();
        if (silenceMs > game::RX_SILENCE_WARNING_MS && !rxSilenceLogged_) {
            rxSilenceLogged_ = true;
            LOG_WARNING("RX SILENCE: No packets from server for ", silenceMs, "ms — possible soft disconnect");
        }
        if (silenceMs > game::RX_SILENCE_CRITICAL_MS && !rxSilence15sLogged_) {
            rxSilence15sLogged_ = true;
            LOG_WARNING("RX SILENCE: 15s — server appears to have stopped sending");
        }
    }

    // Detect server-side disconnect (socket closed during update)
    if (socket && !socket->isConnected() && state != WorldState::DISCONNECTED) {
        if (pendingIncomingPackets_.empty() && !entityController_->hasPendingUpdateObjectWork()) {
            LOG_WARNING("Server closed connection in state: ", worldStateName(state));
            disconnect();
            return;
        }
        LOG_DEBUG("World socket closed with ", pendingIncomingPackets_.size(),
                  " queued packet(s) and update-object batch(es) pending dispatch");
    }

    // Post-gate visibility: determine whether server goes silent or closes after Warden requirement.
    if (wardenGateSeen_ && socket && socket->isConnected()) {
        wardenGateElapsed_ += deltaTime;
        if (wardenGateElapsed_ >= wardenGateNextStatusLog_) {
            LOG_DEBUG("Warden gate status: elapsed=", wardenGateElapsed_,
                     "s connected=", socket->isConnected() ? "yes" : "no",
                     " packetsAfterGate=", wardenPacketsAfterGate_);
            wardenGateNextStatusLog_ += game::WARDEN_GATE_LOG_INTERVAL_SEC;
        }
    }
}

void GameHandler::updateTaxiAndMountState(float deltaTime) {
// Update taxi landing cooldown
if (taxiLandingCooldown_ > 0.0f) {
    taxiLandingCooldown_ -= deltaTime;
}
if (taxiStartGrace_ > 0.0f) {
    taxiStartGrace_ -= deltaTime;
}
if (playerTransportStickyTimer_ > 0.0f) {
    playerTransportStickyTimer_ -= deltaTime;
    if (playerTransportStickyTimer_ <= 0.0f) {
        playerTransportStickyTimer_ = 0.0f;
        playerTransportStickyGuid_ = 0;
    }
}

// Detect taxi flight landing: UNIT_FLAG_TAXI_FLIGHT (0x00000100) cleared
if (onTaxiFlight_) {
    updateClientTaxi(deltaTime);
    auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
    auto unit = std::dynamic_pointer_cast<Unit>(playerEntity);
    if (unit &&
        (unit->getUnitFlags() & game::UNIT_FLAG_TAXI_FLIGHT) == 0 &&
        !taxiClientActive_ &&
        !taxiActivatePending_ &&
        taxiStartGrace_ <= 0.0f) {
        onTaxiFlight_ = false;
        taxiLandingCooldown_ = 2.0f;  // 2 second cooldown to prevent re-entering
        if (taxiMountActive_ && mountCallback_) {
            mountCallback_(0);
        }
        taxiMountActive_ = false;
        taxiMountDisplayId_ = 0;
        currentMountDisplayId_ = 0;
        taxiClientActive_ = false;
        taxiClientPath_.clear();
        taxiRecoverPending_ = false;
        movementInfo.flags = 0;
        movementInfo.flags2 = 0;
        if (socket) {
            sendMovement(Opcode::MSG_MOVE_STOP);
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        }
        LOG_INFO("Taxi flight landed");
    }
}

// Safety: if taxi flight ended but mount is still active, force dismount.
// Guard against transient taxi-state flicker.
if (!onTaxiFlight_ && taxiMountActive_) {
    bool serverStillTaxi = false;
    auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
    auto playerUnit = std::dynamic_pointer_cast<Unit>(playerEntity);
    if (playerUnit) {
        serverStillTaxi = (playerUnit->getUnitFlags() & game::UNIT_FLAG_TAXI_FLIGHT) != 0;
    }

    if (taxiStartGrace_ > 0.0f || serverStillTaxi || taxiClientActive_ || taxiActivatePending_) {
        onTaxiFlight_ = true;
    } else {
        if (mountCallback_) mountCallback_(0);
        taxiMountActive_ = false;
        taxiMountDisplayId_ = 0;
        currentMountDisplayId_ = 0;
        movementInfo.flags = 0;
        movementInfo.flags2 = 0;
        if (socket) {
            sendMovement(Opcode::MSG_MOVE_STOP);
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        }
        LOG_INFO("Taxi dismount cleanup");
    }
}

// Keep non-taxi mount state server-authoritative.
// Some server paths don't emit explicit mount field updates in lockstep
// with local visual state changes, so reconcile continuously.
if (!onTaxiFlight_ && !taxiMountActive_) {
    auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
    auto playerUnit = std::dynamic_pointer_cast<Unit>(playerEntity);
    if (playerUnit) {
        uint32_t serverMountDisplayId = playerUnit->getMountDisplayId();
        if (serverMountDisplayId != currentMountDisplayId_) {
            LOG_INFO("Mount reconcile: server=", serverMountDisplayId,
                     " local=", currentMountDisplayId_);
            currentMountDisplayId_ = serverMountDisplayId;
            if (mountCallback_) {
                mountCallback_(serverMountDisplayId);
            }
        }
    }
}

if (taxiRecoverPending_ && state == WorldState::IN_WORLD) {
    auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
    if (playerEntity) {
        playerEntity->setPosition(taxiRecoverPos_.x, taxiRecoverPos_.y,
                                  taxiRecoverPos_.z, movementInfo.orientation);
        movementInfo.x = taxiRecoverPos_.x;
        movementInfo.y = taxiRecoverPos_.y;
        movementInfo.z = taxiRecoverPos_.z;
        if (socket) {
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        }
        taxiRecoverPending_ = false;
        LOG_INFO("Taxi recovery applied");
    }
}

if (taxiActivatePending_) {
    taxiActivateTimer_ += deltaTime;
    if (taxiActivateTimer_ > 5.0f) {
        // If client taxi simulation is already active, server reply may be missing/late.
        // Do not cancel the flight in that case; clear pending state and continue.
        if (onTaxiFlight_ || taxiClientActive_ || taxiMountActive_) {
            taxiActivatePending_ = false;
            taxiActivateTimer_ = 0.0f;
        } else {
        taxiActivatePending_ = false;
        taxiActivateTimer_ = 0.0f;
        if (taxiMountActive_ && mountCallback_) {
            mountCallback_(0);
        }
        taxiMountActive_ = false;
        taxiMountDisplayId_ = 0;
        taxiClientActive_ = false;
        taxiClientPath_.clear();
        onTaxiFlight_ = false;
        LOG_WARNING("Taxi activation timed out");
        }
    }
}
}

void GameHandler::updateAutoAttack(float deltaTime) {
    if (combatHandler_) combatHandler_->updateAutoAttack(deltaTime);

// Close NPC windows if player walks too far (15 units)
}

void GameHandler::updateEntityInterpolation(float deltaTime) {
// Update entity movement interpolation (keeps targeting in sync with visuals)
// Only update entities within reasonable distance for performance
const float updateRadiusSq = game::ENTITY_UPDATE_RADIUS * game::ENTITY_UPDATE_RADIUS;  // 150 unit radius
auto playerEntity = entityController_->getEntityManager().getEntity(playerGuid);
glm::vec3 playerPos = playerEntity ? glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ()) : glm::vec3(0.0f);

for (auto& [guid, entity] : entityController_->getEntityManager().getEntities()) {
    // Always update player
    if (guid == playerGuid) {
        entity->updateMovement(deltaTime);
        continue;
    }
    // Keep selected/engaged target interpolation exact for UI targeting circle.
    if (guid == targetGuid || (combatHandler_ && guid == combatHandler_->getAutoAttackTargetGuid())) {
        entity->updateMovement(deltaTime);
        continue;
    }

    // Distance cull other entities (use latest position to avoid culling by stale origin)
    glm::vec3 entityPos(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
    float distSq = glm::dot(entityPos - playerPos, entityPos - playerPos);
    if (distSq < updateRadiusSq) {
        entity->updateMovement(deltaTime);
    }
}
}

void GameHandler::updateTimers(float deltaTime) {
    if (spellHandler_) spellHandler_->updateTimers(deltaTime);

    // Periodically clear stale pending item queries so they can be retried.
    // Without this, a lost/malformed response leaves the entry stuck forever.
    pendingItemQueryTimer_ += deltaTime;
    if (pendingItemQueryTimer_ >= 5.0f) {
        pendingItemQueryTimer_ = 0.0f;
        if (!pendingItemQueries_.empty()) {
            LOG_DEBUG("Clearing ", pendingItemQueries_.size(), " stale pending item queries");
            pendingItemQueries_.clear();
        }
    }

    if (auctionSearchDelayTimer_ > 0.0f) {
        auctionSearchDelayTimer_ -= deltaTime;
        if (auctionSearchDelayTimer_ < 0.0f) auctionSearchDelayTimer_ = 0.0f;
    }

    for (auto it = pendingQuestAcceptTimeouts_.begin(); it != pendingQuestAcceptTimeouts_.end();) {
        it->second -= deltaTime;
        if (it->second <= 0.0f) {
            const uint32_t questId = it->first;
            const uint64_t npcGuid = pendingQuestAcceptNpcGuids_.count(questId) != 0
                ? pendingQuestAcceptNpcGuids_[questId] : 0;
            triggerQuestAcceptResync(questId, npcGuid, "timeout");
            it = pendingQuestAcceptTimeouts_.erase(it);
            pendingQuestAcceptNpcGuids_.erase(questId);
        } else {
            ++it;
        }
    }

    if (pendingMoneyDeltaTimer_ > 0.0f) {
        pendingMoneyDeltaTimer_ -= deltaTime;
        if (pendingMoneyDeltaTimer_ <= 0.0f) {
            pendingMoneyDeltaTimer_ = 0.0f;
            pendingMoneyDelta_ = 0;
        }
    }
    // autoAttackRangeWarnCooldown_ decrement moved into CombatHandler::updateAutoAttack()

    if (pendingLoginQuestResync_) {
        pendingLoginQuestResyncTimeout_ -= deltaTime;
        if (resyncQuestLogFromServerSlots(true)) {
            pendingLoginQuestResync_ = false;
            pendingLoginQuestResyncTimeout_ = 0.0f;
        } else if (pendingLoginQuestResyncTimeout_ <= 0.0f) {
            pendingLoginQuestResync_ = false;
            pendingLoginQuestResyncTimeout_ = 0.0f;
            LOG_WARNING("Quest login resync timed out waiting for player quest slot fields");
        }
    }

    for (auto it = pendingGameObjectLootRetries_.begin(); it != pendingGameObjectLootRetries_.end();) {
        it->timer -= deltaTime;
        if (it->timer <= 0.0f) {
            if (it->remainingRetries > 0 && isInWorld()) {
                // Keep server-side position/facing fresh before retrying GO use.
                sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
                auto usePacket = GameObjectUsePacket::build(it->guid);
                socket->send(usePacket);
                if (it->sendLoot) {
                    auto lootPacket = LootPacket::build(it->guid);
                    socket->send(lootPacket);
                }
                --it->remainingRetries;
                it->timer = 0.20f;
            }
        }
        if (it->remainingRetries == 0) {
            it = pendingGameObjectLootRetries_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = pendingGameObjectLootOpens_.begin(); it != pendingGameObjectLootOpens_.end();) {
        it->timer -= deltaTime;
        if (it->timer <= 0.0f) {
            if (isInWorld()) {
                // Avoid sending CMSG_LOOT while a timed cast is active (e.g. gathering).
                // handleSpellGo will trigger loot after the cast completes.
                if (spellHandler_ && spellHandler_->isCasting() && spellHandler_->getCurrentCastSpellId() != 0) {
                    it->timer = 0.20f;
                    ++it;
                    continue;
                }
                lootTarget(it->guid);
            }
            it = pendingGameObjectLootOpens_.erase(it);
        } else {
            ++it;
        }
    }

    // Periodically re-query names for players whose initial CMSG_NAME_QUERY was
    // lost (server didn't respond) or whose entity was recreated while the query
    // was still pending. Runs every 5 seconds to keep overhead minimal.
    if (isInWorld()) {
        static float nameResyncTimer = 0.0f;
        nameResyncTimer += deltaTime;
        if (nameResyncTimer >= 5.0f) {
            nameResyncTimer = 0.0f;
            for (const auto& [guid, entity] : entityController_->getEntityManager().getEntities()) {
                if (!entity || entity->getType() != ObjectType::PLAYER) continue;
                if (guid == playerGuid) continue;
                auto player = std::static_pointer_cast<Player>(entity);
                if (!player->getName().empty()) continue;
                // Player entity exists with empty name and no pending query — resend.
                entityController_->queryPlayerName(guid);
            }
        }
    }

    if (pendingLootMoneyNotifyTimer_ > 0.0f) {
        pendingLootMoneyNotifyTimer_ -= deltaTime;
        if (pendingLootMoneyNotifyTimer_ <= 0.0f) {
            pendingLootMoneyNotifyTimer_ = 0.0f;
            bool alreadyAnnounced = false;
            if (pendingLootMoneyGuid_ != 0) {
                auto it = localLootState_.find(pendingLootMoneyGuid_);
                if (it != localLootState_.end()) {
                    alreadyAnnounced = it->second.moneyTaken;
                    it->second.moneyTaken = true;
                }
            }
            if (!alreadyAnnounced && pendingLootMoneyAmount_ > 0) {
                addSystemChatMessage("Looted: " + formatCopperAmount(pendingLootMoneyAmount_));
                auto* ac = services_.audioCoordinator;
                if (ac) {
                    if (auto* sfx = ac->getUiSoundManager()) {
                        if (pendingLootMoneyAmount_ >= 10000) {
                            sfx->playLootCoinLarge();
                        } else {
                            sfx->playLootCoinSmall();
                        }
                    }
                }
                if (pendingLootMoneyGuid_ != 0) {
                    recentLootMoneyAnnounceCooldowns_[pendingLootMoneyGuid_] = 1.5f;
                }
            }
            pendingLootMoneyGuid_ = 0;
            pendingLootMoneyAmount_ = 0;
        }
    }

    for (auto it = recentLootMoneyAnnounceCooldowns_.begin(); it != recentLootMoneyAnnounceCooldowns_.end();) {
        it->second -= deltaTime;
        if (it->second <= 0.0f) {
            it = recentLootMoneyAnnounceCooldowns_.erase(it);
        } else {
            ++it;
        }
    }

    // Auto-inspect throttling (fallback for player equipment visuals).
    if (inspectRateLimit_ > 0.0f) {
        inspectRateLimit_ = std::max(0.0f, inspectRateLimit_ - deltaTime);
    }
    if (isInWorld() && inspectRateLimit_ <= 0.0f && !pendingAutoInspect_.empty()) {
        uint64_t guid = *pendingAutoInspect_.begin();
        pendingAutoInspect_.erase(pendingAutoInspect_.begin());
        if (guid != 0 && guid != playerGuid && entityController_->getEntityManager().hasEntity(guid)) {
            auto pkt = InspectPacket::build(guid);
            socket->send(pkt);
            inspectRateLimit_ = 2.0f; // throttle to avoid compositing stutter
            LOG_DEBUG("Sent CMSG_INSPECT for player 0x", std::hex, guid, std::dec);
        }
    }
}

void GameHandler::update(float deltaTime) {
    // Fire deferred char-create callback (outside ImGui render)
    if (pendingCharCreateResult_) {
        pendingCharCreateResult_ = false;
        if (charCreateCallback_) {
            charCreateCallback_(pendingCharCreateSuccess_, pendingCharCreateMsg_);
        }
    }

    if (!socket) {
        return;
    }

    updateNetworking(deltaTime);
    if (!socket) return;  // disconnect() may have been called

    // Validate target still exists
    if (targetGuid != 0 && !entityController_->getEntityManager().hasEntity(targetGuid)) {
        clearTarget();
    }

    // Update auto-follow: refresh render position or cancel if entity disappeared
    if (followTargetGuid_ != 0) {
        auto followEnt = entityController_->getEntityManager().getEntity(followTargetGuid_);
        if (followEnt) {
            followRenderPos_ = core::coords::canonicalToRender(
                glm::vec3(followEnt->getX(), followEnt->getY(), followEnt->getZ()));
        } else {
            cancelFollow();
        }
    }

    // Detect combat state transitions → fire PLAYER_REGEN_DISABLED / PLAYER_REGEN_ENABLED
    {
        bool combatNow = isInCombat();
        if (combatNow != wasCombat_) {
            wasCombat_ = combatNow;
                fireAddonEvent(combatNow ? "PLAYER_REGEN_DISABLED" : "PLAYER_REGEN_ENABLED", {});
        }
    }

    updateTimers(deltaTime);

    // Send periodic heartbeat if in world
    if (state == WorldState::IN_WORLD) {
        timeSinceLastPing += deltaTime;
        if (movementHandler_) movementHandler_->timeSinceLastMoveHeartbeatRef() += deltaTime;

        const float currentPingInterval =
            (isPreWotlk()) ? game::CLASSIC_PING_INTERVAL_SEC : pingInterval;
        if (timeSinceLastPing >= currentPingInterval) {
            if (socket) {
                sendPing();
            }
            timeSinceLastPing = 0.0f;
        }

        const bool classicLikeCombatSync =
            (combatHandler_ && combatHandler_->hasAutoAttackIntent()) && (isPreWotlk());
        // Must match the locomotion bitmask in movement_handler.cpp so both
        // sites agree on what constitutes "moving" for heartbeat throttling.
        const uint32_t locomotionFlags =
            static_cast<uint32_t>(MovementFlags::FORWARD) |
            static_cast<uint32_t>(MovementFlags::BACKWARD) |
            static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
            static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
            static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
            static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
            static_cast<uint32_t>(MovementFlags::ASCENDING) |
            static_cast<uint32_t>(MovementFlags::DESCENDING) |
            static_cast<uint32_t>(MovementFlags::SWIMMING) |
            static_cast<uint32_t>(MovementFlags::FALLING) |
            static_cast<uint32_t>(MovementFlags::FALLINGFAR);
        const bool classicLikeStationaryCombatSync =
            classicLikeCombatSync &&
            !onTaxiFlight_ &&
            !taxiActivatePending_ &&
            !taxiClientActive_ &&
            (movementInfo.flags & locomotionFlags) == 0;
        float heartbeatInterval = (onTaxiFlight_ || taxiActivatePending_ || taxiClientActive_)
                                      ? game::HEARTBEAT_INTERVAL_TAXI
                                      : (classicLikeStationaryCombatSync ? game::HEARTBEAT_INTERVAL_STATIONARY_COMBAT
                                                                         : (classicLikeCombatSync ? game::HEARTBEAT_INTERVAL_MOVING_COMBAT
                                                                                                  : moveHeartbeatInterval_));
        if (movementHandler_ && movementHandler_->timeSinceLastMoveHeartbeatRef() >= heartbeatInterval) {
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
            movementHandler_->timeSinceLastMoveHeartbeatRef() = 0.0f;
        }

        // Check area triggers (instance portals, tavern rests, etc.)
        areaTriggerCheckTimer_ += deltaTime;
        if (areaTriggerCheckTimer_ >= game::AREA_TRIGGER_CHECK_INTERVAL) {
            areaTriggerCheckTimer_ = 0.0f;
            checkAreaTriggers();
        }

        // Cancel GO interaction cast if player enters combat (auto-attack).
        if (pendingGameObjectInteractGuid_ != 0 &&
            combatHandler_ && (combatHandler_->isAutoAttacking() || combatHandler_->hasAutoAttackIntent())) {
            pendingGameObjectInteractGuid_ = 0;
            if (spellHandler_) spellHandler_->resetCastState();
            addUIError("Interrupted.");
            addSystemChatMessage("Interrupted.");
        }
        // Check if client-side cast timer expired (tick-down is in SpellHandler::updateTimers).
        // Two paths depending on whether this is a GO interaction cast:
        if (spellHandler_ && spellHandler_->isCasting() && spellHandler_->getCastTimeRemaining() <= 0.0f) {
            if (pendingGameObjectInteractGuid_ != 0) {
                // GO interaction cast: do NOT call resetCastState() here. The server
                // sends SMSG_SPELL_GO when the cast completes server-side (~50-200ms
                // after the client timer expires due to float precision/frame timing).
                // handleSpellGo checks `wasInTimedCast = casting_ && spellId == currentCastSpellId_`
                // — if we clear those fields now, wasInTimedCast is false and the loot
                // path (CMSG_LOOT via lastInteractedGoGuid_) never fires.
                // Let the cast bar sit at 100% until SMSG_SPELL_GO arrives to clean up.
                pendingGameObjectInteractGuid_ = 0;
            } else {
                // Regular cast with no GO pending: clean up immediately.
                spellHandler_->resetCastState();
            }
        }

        // Unit cast states and spell cooldowns are ticked by SpellHandler::updateTimers()
        // (called from GameHandler::updateTimers above). No duplicate tick-down here.

        // Update action bar cooldowns
        for (auto& slot : actionBar) {
            if (slot.cooldownRemaining > 0.0f) {
                slot.cooldownRemaining -= deltaTime;
                if (slot.cooldownRemaining < 0.0f) slot.cooldownRemaining = 0.0f;
            }
        }

        // Update combat text (Phase 2)
        updateCombatText(deltaTime);
        tickMinimapPings(deltaTime);

        // Tick logout countdown
        if (socialHandler_) socialHandler_->updateLogoutCountdown(deltaTime);

        updateTaxiAndMountState(deltaTime);

        // Update transport manager
        if (transportManager_) {
            transportManager_->update(deltaTime);
            updateAttachedTransportChildren(deltaTime);
        }

        updateAutoAttack(deltaTime);
        auto closeIfTooFar = [&](bool windowOpen, uint64_t npcGuid, auto closeFn, const char* label) {
            if (!windowOpen || npcGuid == 0) return;
            auto npc = entityController_->getEntityManager().getEntity(npcGuid);
            if (!npc) return;
            float dx = movementInfo.x - npc->getX();
            float dy = movementInfo.y - npc->getY();
            if (std::sqrt(dx * dx + dy * dy) > game::NPC_INTERACT_MAX_DISTANCE) {
                closeFn();
                LOG_INFO(label, " closed: walked too far from NPC");
            }
        };
        closeIfTooFar(isVendorWindowOpen(), getVendorItems().vendorGuid, [this]{ closeVendor(); }, "Vendor");
        closeIfTooFar(isGossipWindowOpen(), getCurrentGossip().npcGuid, [this]{ closeGossip(); }, "Gossip");
        closeIfTooFar(isTaxiWindowOpen(), taxiNpcGuid_, [this]{ closeTaxi(); }, "Taxi window");
        closeIfTooFar(isTrainerWindowOpen(), getTrainerSpells().trainerGuid, [this]{ closeTrainer(); }, "Trainer");

        updateEntityInterpolation(deltaTime);

    }
}

// ============================================================
// Single-player local combat
// ============================================================

// ============================================================
// XP tracking
// ============================================================

// WotLK 3.3.5a XP-to-next-level table (from player_xp_for_level)
static constexpr uint32_t XP_TABLE[] = {
    0,       // level 0 (unused)
    400,     900,     1400,    2100,    2800,    3600,    4500,    5400,    6500,    7600,     // 1-10
    8700,    9800,    11000,   12300,   13600,   15000,   16400,   17800,   19300,   20800,    // 11-20
    22400,   24000,   25500,   27200,   28900,   30500,   32200,   33900,   36300,   38800,    // 21-30
    41600,   44600,   48000,   51400,   55000,   58700,   62400,   66200,   70200,   74300,    // 31-40
    78500,   82800,   87100,   91600,   96300,   101000,  105800,  110700,  115700,  120900,   // 41-50
    126100,  131500,  137000,  142500,  148200,  154000,  159900,  165800,  172000,  290000,   // 51-60
    317000,  349000,  386000,  428000,  475000,  527000,  585000,  648000,  717000,  1523800,  // 61-70
    1539600, 1555700, 1571800, 1587900, 1604200, 1620700, 1637400, 1653900, 1670800           // 71-79
};
static constexpr uint32_t XP_TABLE_SIZE = sizeof(XP_TABLE) / sizeof(XP_TABLE[0]);

uint32_t GameHandler::xpForLevel(uint32_t level) {
    if (level == 0 || level >= XP_TABLE_SIZE) return 0;
    return XP_TABLE[level];
}

uint32_t GameHandler::killXp(uint32_t playerLevel, uint32_t victimLevel) {
    return CombatHandler::killXp(playerLevel, victimLevel);
}

void GameHandler::handleXpGain(network::Packet& packet) {
    if (combatHandler_) combatHandler_->handleXpGain(packet);
}

void GameHandler::addMoneyCopper(uint32_t amount) {
    if (inventoryHandler_) inventoryHandler_->addMoneyCopper(amount);
}

void GameHandler::addSystemChatMessage(const std::string& message) {
    if (chatHandler_) chatHandler_->addSystemChatMessage(message);
}

// ============================================================
// Taxi / Flight Path Handlers
// ============================================================

void GameHandler::updateClientTaxi(float deltaTime) {
    if (movementHandler_) movementHandler_->updateClientTaxi(deltaTime);
}

void GameHandler::closeTaxi() {
    if (movementHandler_) movementHandler_->closeTaxi();
}

uint32_t GameHandler::getTaxiCostTo(uint32_t destNodeId) const {
    if (movementHandler_) return movementHandler_->getTaxiCostTo(destNodeId);
    return 0;
}

void GameHandler::activateTaxi(uint32_t destNodeId) {
    if (movementHandler_) movementHandler_->activateTaxi(destNodeId);
}

// ============================================================
// Server Info Command Handlers
// ============================================================

void GameHandler::handleQueryTimeResponse(network::Packet& packet) {
    QueryTimeResponseData data;
    if (!QueryTimeResponseParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_QUERY_TIME_RESPONSE");
        return;
    }

    // Convert Unix timestamp to readable format
    time_t serverTime = static_cast<time_t>(data.serverTime);
    struct tm* timeInfo = localtime(&serverTime);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);

    std::string msg = "Server time: " + std::string(timeStr);
    addSystemChatMessage(msg);
    LOG_INFO("Server time: ", data.serverTime, " (", timeStr, ")");
}

uint32_t GameHandler::generateClientSeed() {
    // Generate cryptographically random seed
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFF);
    return dis(gen);
}

void GameHandler::setState(WorldState newState) {
    if (state != newState) {
        LOG_DEBUG("World state: ", static_cast<int>(state), " -> ", static_cast<int>(newState));
        state = newState;
    }
}

void GameHandler::fail(const std::string& reason) {
    LOG_ERROR("World connection failed: ", reason);
    setState(WorldState::FAILED);

    if (onFailure) {
        onFailure(reason);
    }
}

// ============================================================
// Player Skills
// ============================================================

static const std::string kEmptySkillName;

const std::string& GameHandler::getSkillName(uint32_t skillId) const {
    auto it = skillLineNames_.find(skillId);
    return (it != skillLineNames_.end()) ? it->second : kEmptySkillName;
}

uint32_t GameHandler::getSkillCategory(uint32_t skillId) const {
    auto it = skillLineCategories_.find(skillId);
    return (it != skillLineCategories_.end()) ? it->second : 0;
}

bool GameHandler::isProfessionSpell(uint32_t spellId) const {
    auto slIt = spellToSkillLine_.find(spellId);
    if (slIt == spellToSkillLine_.end()) return false;
    auto catIt = skillLineCategories_.find(slIt->second);
    if (catIt == skillLineCategories_.end()) return false;
    // Category 11 = profession (Blacksmithing, etc.), 9 = secondary (Cooking, First Aid, Fishing)
    return catIt->second == 11 || catIt->second == 9;
}

void GameHandler::loadSkillLineDbc() {
    if (spellHandler_) spellHandler_->loadSkillLineDbc();
}

void GameHandler::extractSkillFields(const std::map<uint16_t, uint32_t>& fields) {
    if (spellHandler_) spellHandler_->extractSkillFields(fields);
}

void GameHandler::extractExploredZoneFields(const std::map<uint16_t, uint32_t>& fields) {
    if (spellHandler_) spellHandler_->extractExploredZoneFields(fields);
}

std::string GameHandler::getCharacterConfigDir() {
    std::string dir;
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    dir = appdata ? std::string(appdata) + "\\wowee\\characters" : "characters";
#else
    const char* home = std::getenv("HOME");
    dir = home ? std::string(home) + "/.wowee/characters" : "characters";
#endif
    return dir;
}

static const std::string EMPTY_MACRO_TEXT;

const std::string& GameHandler::getMacroText(uint32_t macroId) const {
    auto it = macros_.find(macroId);
    return (it != macros_.end()) ? it->second : EMPTY_MACRO_TEXT;
}

void GameHandler::setMacroText(uint32_t macroId, const std::string& text) {
    if (text.empty())
        macros_.erase(macroId);
    else
        macros_[macroId] = text;
    saveCharacterConfig();
}

void GameHandler::saveCharacterConfig() {
    const Character* ch = getActiveCharacter();
    if (!ch || ch->name.empty()) return;

    std::string dir = getCharacterConfigDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string path = dir + "/" + ch->name + ".cfg";
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save character config to ", path);
        return;
    }

    out << "character_guid=" << playerGuid << "\n";
    out << "gender=" << static_cast<int>(ch->gender) << "\n";
    // For male/female, derive from gender; only nonbinary has a meaningful separate choice
    bool saveUseFemaleModel = (ch->gender == Gender::NONBINARY) ? ch->useFemaleModel
                                                                 : (ch->gender == Gender::FEMALE);
    out << "use_female_model=" << (saveUseFemaleModel ? 1 : 0) << "\n";
    for (int i = 0; i < ACTION_BAR_SLOTS; i++) {
        out << "action_bar_" << i << "_type=" << static_cast<int>(actionBar[i].type) << "\n";
        out << "action_bar_" << i << "_id=" << actionBar[i].id << "\n";
    }

    // Save client-side macro text (escape newlines as \n literal)
    for (const auto& [id, text] : macros_) {
        if (!text.empty()) {
            std::string escaped;
            escaped.reserve(text.size());
            for (char c : text) {
                if (c == '\n') { escaped += "\\n"; }
                else if (c == '\r') { /* skip CR */ }
                else if (c == '\\') { escaped += "\\\\"; }
                else { escaped += c; }
            }
            out << "macro_" << id << "_text=" << escaped << "\n";
        }
    }

    // Save quest log
    out << "quest_log_count=" << questLog_.size() << "\n";
    for (size_t i = 0; i < questLog_.size(); i++) {
        const auto& quest = questLog_[i];
        out << "quest_" << i << "_id=" << quest.questId << "\n";
        out << "quest_" << i << "_title=" << quest.title << "\n";
        out << "quest_" << i << "_complete=" << (quest.complete ? 1 : 0) << "\n";
    }

    // Save tracked quest IDs so the quest tracker restores on login
    if (!trackedQuestIds_.empty()) {
        std::string ids;
        for (uint32_t qid : trackedQuestIds_) {
            if (!ids.empty()) ids += ',';
            ids += std::to_string(qid);
        }
        out << "tracked_quests=" << ids << "\n";
    }

    LOG_INFO("Character config saved to ", path);
}

void GameHandler::loadCharacterConfig() {
    const Character* ch = getActiveCharacter();
    if (!ch || ch->name.empty()) return;

    std::string path = getCharacterConfigDir() + "/" + ch->name + ".cfg";
    std::ifstream in(path);
    if (!in.is_open()) return;

    uint64_t savedGuid = 0;
    std::array<int, ACTION_BAR_SLOTS> types{};
    std::array<uint32_t, ACTION_BAR_SLOTS> ids{};
    bool hasSlots = false;
    int savedGender = -1;
    int savedUseFemaleModel = -1;

    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "character_guid") {
            try { savedGuid = std::stoull(val); } catch (...) {}
        } else if (key == "gender") {
            try { savedGender = std::stoi(val); } catch (...) {}
        } else if (key == "use_female_model") {
            try { savedUseFemaleModel = std::stoi(val); } catch (...) {}
        } else if (key.rfind("macro_", 0) == 0) {
            // Parse macro_N_text
            size_t firstUnder = 6; // length of "macro_"
            size_t secondUnder = key.find('_', firstUnder);
            if (secondUnder == std::string::npos) continue;
            uint32_t macroId = 0;
            try { macroId = static_cast<uint32_t>(std::stoul(key.substr(firstUnder, secondUnder - firstUnder))); } catch (...) { continue; }
            if (key.substr(secondUnder + 1) == "text" && !val.empty()) {
                // Unescape \n and \\ sequences
                std::string unescaped;
                unescaped.reserve(val.size());
                for (size_t i = 0; i < val.size(); ++i) {
                    if (val[i] == '\\' && i + 1 < val.size()) {
                        if (val[i+1] == 'n')  { unescaped += '\n'; ++i; }
                        else if (val[i+1] == '\\') { unescaped += '\\'; ++i; }
                        else { unescaped += val[i]; }
                    } else {
                        unescaped += val[i];
                    }
                }
                macros_[macroId] = std::move(unescaped);
            }
        } else if (key == "tracked_quests" && !val.empty()) {
            // Parse comma-separated quest IDs
            trackedQuestIds_.clear();
            size_t tqPos = 0;
            while (tqPos <= val.size()) {
                size_t comma = val.find(',', tqPos);
                std::string idStr = (comma != std::string::npos)
                    ? val.substr(tqPos, comma - tqPos) : val.substr(tqPos);
                try {
                    uint32_t qid = static_cast<uint32_t>(std::stoul(idStr));
                    if (qid != 0) trackedQuestIds_.insert(qid);
                } catch (...) {}
                if (comma == std::string::npos) break;
                tqPos = comma + 1;
            }
        } else if (key.rfind("action_bar_", 0) == 0) {
            // Parse action_bar_N_type or action_bar_N_id
            size_t firstUnderscore = 11; // length of "action_bar_"
            size_t secondUnderscore = key.find('_', firstUnderscore);
            if (secondUnderscore == std::string::npos) continue;
            int slot = -1;
            try { slot = std::stoi(key.substr(firstUnderscore, secondUnderscore - firstUnderscore)); } catch (...) { continue; }
            if (slot < 0 || slot >= ACTION_BAR_SLOTS) continue;
            std::string suffix = key.substr(secondUnderscore + 1);
            try {
                if (suffix == "type") {
                    types[slot] = std::stoi(val);
                    hasSlots = true;
                } else if (suffix == "id") {
                    ids[slot] = static_cast<uint32_t>(std::stoul(val));
                    hasSlots = true;
                }
            } catch (...) {}
        }
    }

    // Validate guid matches current character
    if (savedGuid != 0 && savedGuid != playerGuid) {
        LOG_WARNING("Character config guid mismatch for ", ch->name, ", using defaults");
        return;
    }

    // Apply saved gender and body type (allows nonbinary to persist even though server only stores male/female)
    if (savedGender >= 0 && savedGender <= 2) {
        for (auto& character : characters) {
            if (character.guid == playerGuid) {
                character.gender = static_cast<Gender>(savedGender);
                if (character.gender == Gender::NONBINARY) {
                    // Only nonbinary characters have a meaningful body type choice
                    if (savedUseFemaleModel >= 0) {
                        character.useFemaleModel = (savedUseFemaleModel != 0);
                    }
                } else {
                    // Male/female always use the model matching their gender
                    character.useFemaleModel = (character.gender == Gender::FEMALE);
                }
                LOG_INFO("Applied saved gender: ", getGenderName(character.gender),
                         ", body type: ", (character.useFemaleModel ? "feminine" : "masculine"));
                break;
            }
        }
    }

    if (hasSlots) {
        for (int i = 0; i < ACTION_BAR_SLOTS; i++) {
            actionBar[i].type = static_cast<ActionBarSlot::Type>(types[i]);
            actionBar[i].id = ids[i];
        }
        LOG_INFO("Character config loaded from ", path);
    }
}

void GameHandler::setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                         const glm::vec3& localOffset, bool hasLocalOrientation,
                                         float localOrientation) {
    if (movementHandler_) movementHandler_->setTransportAttachment(childGuid, type, transportGuid, localOffset, hasLocalOrientation, localOrientation);
}

void GameHandler::clearTransportAttachment(uint64_t childGuid) {
    if (movementHandler_) movementHandler_->clearTransportAttachment(childGuid);
}

void GameHandler::updateAttachedTransportChildren(float deltaTime) {
    if (movementHandler_) movementHandler_->updateAttachedTransportChildren(deltaTime);
}

// ============================================================
// Mail System
// ============================================================

void GameHandler::openMailbox(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->openMailbox(guid);
}

void GameHandler::closeMailbox() {
    if (inventoryHandler_) inventoryHandler_->closeMailbox();
}

void GameHandler::refreshMailList() {
    if (inventoryHandler_) inventoryHandler_->refreshMailList();
}

void GameHandler::sendMail(const std::string& recipient, const std::string& subject,
                           const std::string& body, uint64_t money, uint64_t cod) {
    if (inventoryHandler_) inventoryHandler_->sendMail(recipient, subject, body, money, cod);
}

bool GameHandler::attachItemFromBackpack(int backpackIndex) {
    return inventoryHandler_ && inventoryHandler_->attachItemFromBackpack(backpackIndex);
}

bool GameHandler::attachItemFromBag(int bagIndex, int slotIndex) {
    return inventoryHandler_ && inventoryHandler_->attachItemFromBag(bagIndex, slotIndex);
}

bool GameHandler::detachMailAttachment(int attachIndex) {
    return inventoryHandler_ && inventoryHandler_->detachMailAttachment(attachIndex);
}

void GameHandler::clearMailAttachments() {
    if (inventoryHandler_) inventoryHandler_->clearMailAttachments();
}

int GameHandler::getMailAttachmentCount() const {
    if (inventoryHandler_) return inventoryHandler_->getMailAttachmentCount();
    return 0;
}

void GameHandler::mailTakeMoney(uint32_t mailId) {
    if (inventoryHandler_) inventoryHandler_->mailTakeMoney(mailId);
}

void GameHandler::mailTakeItem(uint32_t mailId, uint32_t itemGuidLow) {
    if (inventoryHandler_) inventoryHandler_->mailTakeItem(mailId, itemGuidLow);
}

void GameHandler::mailDelete(uint32_t mailId) {
    if (inventoryHandler_) inventoryHandler_->mailDelete(mailId);
}

void GameHandler::mailMarkAsRead(uint32_t mailId) {
    if (inventoryHandler_) inventoryHandler_->mailMarkAsRead(mailId);
}

glm::vec3 GameHandler::getComposedWorldPosition() {
    if (playerTransportGuid_ != 0 && transportManager_) {
        auto* tr = transportManager_->getTransport(playerTransportGuid_);
        if (tr) {
            return transportManager_->getPlayerWorldPosition(playerTransportGuid_, playerTransportOffset_);
        }
        // Transport not tracked — fall through to normal position
    }
    // Not on transport, return normal movement position
    return glm::vec3(movementInfo.x, movementInfo.y, movementInfo.z);
}

// ============================================================
// Bank System
// ============================================================

void GameHandler::openBank(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->openBank(guid);
}

void GameHandler::closeBank() {
    if (inventoryHandler_) inventoryHandler_->closeBank();
}

void GameHandler::buyBankSlot() {
    if (inventoryHandler_) inventoryHandler_->buyBankSlot();
}

void GameHandler::depositItem(uint8_t srcBag, uint8_t srcSlot) {
    if (inventoryHandler_) inventoryHandler_->depositItem(srcBag, srcSlot);
}

void GameHandler::withdrawItem(uint8_t srcBag, uint8_t srcSlot) {
    if (inventoryHandler_) inventoryHandler_->withdrawItem(srcBag, srcSlot);
}

// ============================================================
// Guild Bank System
// ============================================================

void GameHandler::openGuildBank(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->openGuildBank(guid);
}

void GameHandler::closeGuildBank() {
    if (inventoryHandler_) inventoryHandler_->closeGuildBank();
}

void GameHandler::queryGuildBankTab(uint8_t tabId) {
    if (inventoryHandler_) inventoryHandler_->queryGuildBankTab(tabId);
}

void GameHandler::buyGuildBankTab() {
    if (inventoryHandler_) inventoryHandler_->buyGuildBankTab();
}

void GameHandler::depositGuildBankMoney(uint32_t amount) {
    if (inventoryHandler_) inventoryHandler_->depositGuildBankMoney(amount);
}

void GameHandler::withdrawGuildBankMoney(uint32_t amount) {
    if (inventoryHandler_) inventoryHandler_->withdrawGuildBankMoney(amount);
}

void GameHandler::guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag, uint8_t destSlot) {
    if (inventoryHandler_) inventoryHandler_->guildBankWithdrawItem(tabId, bankSlot, destBag, destSlot);
}

void GameHandler::guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot) {
    if (inventoryHandler_) inventoryHandler_->guildBankDepositItem(tabId, bankSlot, srcBag, srcSlot);
}

// ============================================================
// Auction House System
// ============================================================

void GameHandler::openAuctionHouse(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->openAuctionHouse(guid);
}

void GameHandler::closeAuctionHouse() {
    if (inventoryHandler_) inventoryHandler_->closeAuctionHouse();
}

void GameHandler::auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                                 uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                                 uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset) {
    if (inventoryHandler_) inventoryHandler_->auctionSearch(name, levelMin, levelMax, quality, itemClass, itemSubClass, invTypeMask, usableOnly, offset);
}

void GameHandler::auctionSellItem(int backpackIndex, uint32_t bid,
                                    uint32_t buyout, uint32_t duration) {
    if (inventoryHandler_) inventoryHandler_->auctionSellItem(backpackIndex, bid, buyout, duration);
}

void GameHandler::auctionPlaceBid(uint32_t auctionId, uint32_t amount) {
    if (inventoryHandler_) inventoryHandler_->auctionPlaceBid(auctionId, amount);
}

void GameHandler::auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice) {
    if (inventoryHandler_) inventoryHandler_->auctionBuyout(auctionId, buyoutPrice);
}

void GameHandler::auctionCancelItem(uint32_t auctionId) {
    if (inventoryHandler_) inventoryHandler_->auctionCancelItem(auctionId);
}

void GameHandler::auctionListOwnerItems(uint32_t offset) {
    if (inventoryHandler_) inventoryHandler_->auctionListOwnerItems(offset);
}

void GameHandler::auctionListBidderItems(uint32_t offset) {
    if (inventoryHandler_) inventoryHandler_->auctionListBidderItems(offset);
}

// ---------------------------------------------------------------------------
// Item text (SMSG_ITEM_TEXT_QUERY_RESPONSE)
//   uint64 itemGuid + uint8 isEmpty + string text (when !isEmpty)
// ---------------------------------------------------------------------------

void GameHandler::queryItemText(uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->queryItemText(itemGuid);
}

// ---------------------------------------------------------------------------
// SMSG_QUEST_CONFIRM_ACCEPT (shared quest from group member)
//   uint32 questId + string questTitle + uint64 sharerGuid
// ---------------------------------------------------------------------------

void GameHandler::acceptSharedQuest() {
    if (questHandler_) questHandler_->acceptSharedQuest();
}

void GameHandler::declineSharedQuest() {
    if (questHandler_) questHandler_->declineSharedQuest();
}

// ---------------------------------------------------------------------------
// SMSG_SUMMON_REQUEST
//   uint64 summonerGuid + uint32 zoneId + uint32 timeoutMs
// ---------------------------------------------------------------------------

void GameHandler::handleSummonRequest(network::Packet& packet) {
    if (socialHandler_) socialHandler_->handleSummonRequest(packet);
}

void GameHandler::acceptSummon() {
    if (socialHandler_) socialHandler_->acceptSummon();
}

void GameHandler::declineSummon() {
    if (socialHandler_) socialHandler_->declineSummon();
}

// ---------------------------------------------------------------------------
// Trade (SMSG_TRADE_STATUS / SMSG_TRADE_STATUS_EXTENDED)
// WotLK 3.3.5a status values:
//   0=busy, 1=begin_trade(+guid), 2=open_window, 3=cancelled, 4=accepted,
//   5=busy2, 6=no_target, 7=back_to_trade, 8=complete, 9=rejected,
//   10=too_far, 11=wrong_faction, 12=close_window, 13=ignore,
//   14-19=stun/dead/logout, 20=trial, 21=conjured_only
// ---------------------------------------------------------------------------

void GameHandler::acceptTradeRequest() {
    if (inventoryHandler_) inventoryHandler_->acceptTradeRequest();
}

void GameHandler::declineTradeRequest() {
    if (inventoryHandler_) inventoryHandler_->declineTradeRequest();
}

void GameHandler::acceptTrade() {
    if (inventoryHandler_) inventoryHandler_->acceptTrade();
}

void GameHandler::cancelTrade() {
    if (inventoryHandler_) inventoryHandler_->cancelTrade();
}

void GameHandler::setTradeItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) {
    if (inventoryHandler_) inventoryHandler_->setTradeItem(tradeSlot, bag, bagSlot);
}

void GameHandler::clearTradeItem(uint8_t tradeSlot) {
    if (inventoryHandler_) inventoryHandler_->clearTradeItem(tradeSlot);
}

void GameHandler::setTradeGold(uint64_t copper) {
    if (inventoryHandler_) inventoryHandler_->setTradeGold(copper);
}

void GameHandler::resetTradeState() {
    if (inventoryHandler_) inventoryHandler_->resetTradeState();
}

// ---------------------------------------------------------------------------
// Group loot roll (SMSG_LOOT_ROLL / SMSG_LOOT_ROLL_WON / CMSG_LOOT_ROLL)
// ---------------------------------------------------------------------------

void GameHandler::sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType) {
    if (inventoryHandler_) inventoryHandler_->sendLootRoll(objectGuid, slot, rollType);
}

// ---------------------------------------------------------------------------
// SMSG_ACHIEVEMENT_EARNED (WotLK 3.3.5a wire 0x4AB)
//   uint64 guid          — player who earned it (may be another player)
//   uint32 achievementId — Achievement.dbc ID
//   PackedTime date      — uint32 bitfield (seconds since epoch)
//   uint32 realmFirst    — how many on realm also got it (0 = realm first)
// ---------------------------------------------------------------------------
void GameHandler::loadTitleNameCache() const {
    if (titleNameCacheLoaded_) return;
    titleNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("CharTitles.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 5) return;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharTitles") : nullptr;

    uint32_t titleField = layout ? layout->field("Title")    : 2;
    uint32_t bitField   = layout ? layout->field("TitleBit") : 36;
    if (titleField == 0xFFFFFFFF) titleField = 2;
    if (bitField   == 0xFFFFFFFF) bitField   = static_cast<uint32_t>(dbc->getFieldCount() - 1);

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t bit = dbc->getUInt32(i, bitField);
        if (bit == 0) continue;
        std::string name = dbc->getString(i, titleField);
        if (!name.empty()) titleNameCache_[bit] = std::move(name);
    }
    LOG_INFO("CharTitles: loaded ", titleNameCache_.size(), " title names from DBC");
}

std::string GameHandler::getFormattedTitle(uint32_t bit) const {
    loadTitleNameCache();
    auto it = titleNameCache_.find(bit);
    if (it == titleNameCache_.end() || it->second.empty()) return {};

    const auto& ln2 = lookupName(playerGuid);
    static const std::string kUnknown = "unknown";
    const std::string& pName = ln2.empty() ? kUnknown : ln2;

    const std::string& fmt = it->second;
    size_t pos = fmt.find("%s");
    if (pos != std::string::npos) {
        return fmt.substr(0, pos) + pName + fmt.substr(pos + 2);
    }
    return fmt;
}

void GameHandler::sendSetTitle(int32_t bit) {
    if (!isInWorld()) return;
    auto packet = SetTitlePacket::build(bit);
    socket->send(packet);
    chosenTitleBit_ = bit;
    LOG_INFO("sendSetTitle: bit=", bit);
}

void GameHandler::loadAchievementNameCache() {
    if (achievementNameCacheLoaded_) return;
    achievementNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("Achievement.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 22) return;

    const auto* achL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Achievement") : nullptr;
    uint32_t titleField = achL ? achL->field("Title") : 4;
    if (titleField == 0xFFFFFFFF) titleField = 4;
    uint32_t descField = achL ? achL->field("Description") : 0xFFFFFFFF;
    uint32_t ptsField  = achL ? achL->field("Points")      : 0xFFFFFFFF;

    uint32_t fieldCount = dbc->getFieldCount();
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, 0);
        if (id == 0) continue;
        std::string title = dbc->getString(i, titleField);
        if (!title.empty()) achievementNameCache_[id] = std::move(title);
        if (descField != 0xFFFFFFFF && descField < fieldCount) {
            std::string desc = dbc->getString(i, descField);
            if (!desc.empty()) achievementDescCache_[id] = std::move(desc);
        }
        if (ptsField != 0xFFFFFFFF && ptsField < fieldCount) {
            uint32_t pts = dbc->getUInt32(i, ptsField);
            if (pts > 0) achievementPointsCache_[id] = pts;
        }
    }
    LOG_INFO("Achievement: loaded ", achievementNameCache_.size(), " names from Achievement.dbc");
}

// ---------------------------------------------------------------------------
// SMSG_ALL_ACHIEVEMENT_DATA (WotLK 3.3.5a)
//   Achievement records: repeated { uint32 id, uint32 packedDate } until 0xFFFFFFFF sentinel
//   Criteria records:    repeated { uint32 id, uint64 counter, uint32 packedDate, ... } until 0xFFFFFFFF
// ---------------------------------------------------------------------------
void GameHandler::handleAllAchievementData(network::Packet& packet) {
    loadAchievementNameCache();
    earnedAchievements_.clear();
    achievementDates_.clear();

    // Parse achievement entries (id + packedDate pairs, sentinel 0xFFFFFFFF)
    while (packet.hasRemaining(4)) {
        uint32_t id = packet.readUInt32();
        if (id == 0xFFFFFFFF) break;
        if (!packet.hasRemaining(4)) break;
        uint32_t date = packet.readUInt32();
        earnedAchievements_.insert(id);
        achievementDates_[id] = date;
    }

    // Parse criteria block: id + uint64 counter + uint32 date + uint32 flags, sentinel 0xFFFFFFFF
    criteriaProgress_.clear();
    while (packet.hasRemaining(4)) {
        uint32_t id = packet.readUInt32();
        if (id == 0xFFFFFFFF) break;
        // counter(8) + date(4) + unknown(4) = 16 bytes
        if (!packet.hasRemaining(16)) break;
        uint64_t counter = packet.readUInt64();
        packet.readUInt32();  // date
        packet.readUInt32();  // unknown / flags
        criteriaProgress_[id] = counter;
    }

    LOG_INFO("SMSG_ALL_ACHIEVEMENT_DATA: loaded ", earnedAchievements_.size(),
             " achievements, ", criteriaProgress_.size(), " criteria");
}

// ---------------------------------------------------------------------------
// SMSG_RESPOND_INSPECT_ACHIEVEMENTS (WotLK 3.3.5a)
//   Wire format: packed_guid (inspected player) + same achievement/criteria
//   blocks as SMSG_ALL_ACHIEVEMENT_DATA:
//     Achievement records: repeated { uint32 id, uint32 packedDate } until 0xFFFFFFFF sentinel
//     Criteria records:    repeated { uint32 id, uint64 counter, uint32 date, uint32 unk }
//                          until 0xFFFFFFFF sentinel
//   We store only the earned achievement IDs (not criteria) per inspected player.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Faction name cache (lazily loaded from Faction.dbc)
// ---------------------------------------------------------------------------

void GameHandler::loadFactionNameCache() const {
    if (factionNameCacheLoaded_) return;
    factionNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("Faction.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    // Faction.dbc WotLK 3.3.5a field layout:
    //   0: ID
    //   1: ReputationListID  (-1 / 0xFFFFFFFF = no reputation tracking)
    //   2-5:  ReputationRaceMask[4]
    //   6-9:  ReputationClassMask[4]
    //  10-13: ReputationBase[4]
    //  14-17: ReputationFlags[4]
    //  18:    ParentFactionID
    //  19-20: SpilloverRateIn, SpilloverRateOut (floats)
    //  21-22: SpilloverMaxRankIn, SpilloverMaxRankOut
    //  23:    Name (English locale, string ref)
    constexpr uint32_t ID_FIELD      = 0;
    constexpr uint32_t REPLIST_FIELD = 1;
    constexpr uint32_t NAME_FIELD    = 23;  // enUS name string

    // Classic/TBC have fewer fields; fall back gracefully
    const bool hasRepListField = dbc->getFieldCount() > REPLIST_FIELD;
    if (dbc->getFieldCount() <= NAME_FIELD) {
        LOG_WARNING("Faction.dbc: unexpected field count ", dbc->getFieldCount());
        // Don't abort — still try to load names from a shorter layout
    }
    const uint32_t nameField = (dbc->getFieldCount() > NAME_FIELD) ? NAME_FIELD : 22u;

    uint32_t count = dbc->getRecordCount();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t factionId = dbc->getUInt32(i, ID_FIELD);
        if (factionId == 0) continue;
        if (dbc->getFieldCount() > nameField) {
            std::string name = dbc->getString(i, nameField);
            if (!name.empty()) {
                factionNameCache_[factionId] = std::move(name);
            }
        }
        // Build repListId ↔ factionId mapping (WotLK field 1)
        if (hasRepListField) {
            uint32_t repListId = dbc->getUInt32(i, REPLIST_FIELD);
            if (repListId != 0xFFFFFFFFu) {
                factionRepListToId_[repListId] = factionId;
                factionIdToRepList_[factionId] = repListId;
            }
        }
    }
    LOG_INFO("Faction.dbc: loaded ", factionNameCache_.size(), " faction names, ",
             factionRepListToId_.size(), " with reputation tracking");
}

uint32_t GameHandler::getFactionIdByRepListId(uint32_t repListId) const {
    loadFactionNameCache();
    auto it = factionRepListToId_.find(repListId);
    return (it != factionRepListToId_.end()) ? it->second : 0u;
}

uint32_t GameHandler::getRepListIdByFactionId(uint32_t factionId) const {
    loadFactionNameCache();
    auto it = factionIdToRepList_.find(factionId);
    return (it != factionIdToRepList_.end()) ? it->second : 0xFFFFFFFFu;
}

void GameHandler::setWatchedFactionId(uint32_t factionId) {
    watchedFactionId_ = factionId;
    if (!isInWorld()) return;
    // CMSG_SET_WATCHED_FACTION: int32 repListId (-1 = unwatch)
    int32_t repListId = -1;
    if (factionId != 0) {
        uint32_t rl = getRepListIdByFactionId(factionId);
        if (rl != 0xFFFFFFFFu) repListId = static_cast<int32_t>(rl);
    }
    network::Packet pkt(wireOpcode(Opcode::CMSG_SET_WATCHED_FACTION));
    pkt.writeUInt32(static_cast<uint32_t>(repListId));
    socket->send(pkt);
    LOG_DEBUG("CMSG_SET_WATCHED_FACTION: repListId=", repListId, " (factionId=", factionId, ")");
}

std::string GameHandler::getFactionName(uint32_t factionId) const {
    auto it = factionNameCache_.find(factionId);
    if (it != factionNameCache_.end()) return it->second;
    return "faction #" + std::to_string(factionId);
}

const std::string& GameHandler::getFactionNamePublic(uint32_t factionId) const {
    loadFactionNameCache();
    auto it = factionNameCache_.find(factionId);
    if (it != factionNameCache_.end()) return it->second;
    static const std::string empty;
    return empty;
}

// ---------------------------------------------------------------------------
// Area name cache (lazy-loaded from WorldMapArea.dbc)
// ---------------------------------------------------------------------------

void GameHandler::loadAreaNameCache() const {
    if (areaNameCacheLoaded_) return;
    areaNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    // AreaTable.dbc has the canonical zone/area names keyed by AreaID.
    // Field 0 = ID, field 11 = AreaName (enUS locale).
    auto areaDbc = am->loadDBC("AreaTable.dbc");
    if (areaDbc && areaDbc->isLoaded() && areaDbc->getFieldCount() > 11) {
        for (uint32_t i = 0; i < areaDbc->getRecordCount(); ++i) {
            uint32_t areaId = areaDbc->getUInt32(i, 0);
            if (areaId == 0) continue;
            std::string name = areaDbc->getString(i, 11);
            if (!name.empty()) {
                areaNameCache_[areaId] = std::move(name);
            }
        }
    }

    // WorldMapArea.dbc supplements with map-UI area names (different ID space).
    auto dbc = am->loadDBC("WorldMapArea.dbc");
    if (dbc && dbc->isLoaded()) {
        const auto* layout = pipeline::getActiveDBCLayout()
            ? pipeline::getActiveDBCLayout()->getLayout("WorldMapArea") : nullptr;
        const uint32_t areaIdField   = layout ? (*layout)["AreaID"]   : 2;
        const uint32_t areaNameField = layout ? (*layout)["AreaName"] : 3;

        if (dbc->getFieldCount() > areaNameField) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                uint32_t areaId = dbc->getUInt32(i, areaIdField);
                if (areaId == 0) continue;
                std::string name = dbc->getString(i, areaNameField);
                // Don't overwrite AreaTable names — those are authoritative
                if (!name.empty() && !areaNameCache_.count(areaId)) {
                    areaNameCache_[areaId] = std::move(name);
                }
            }
        }
    }

    LOG_INFO("Area name cache: loaded ", areaNameCache_.size(), " entries");
}

std::string GameHandler::getAreaName(uint32_t areaId) const {
    if (areaId == 0) return {};
    loadAreaNameCache();
    auto it = areaNameCache_.find(areaId);
    return (it != areaNameCache_.end()) ? it->second : std::string{};
}

void GameHandler::loadMapNameCache() const {
    if (mapNameCacheLoaded_) return;
    mapNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("Map.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    // Map.dbc layout: 0=ID, 1=InternalName, 2=InstanceType, 3=Flags,
    // 4=MapName_enUS (display name), fields 5+ = other locales
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, 0);
        std::string name = dbc->getString(i, 4);
        if (name.empty()) name = dbc->getString(i, 1); // internal name fallback
        if (!name.empty() && !mapNameCache_.count(id)) {
            mapNameCache_[id] = std::move(name);
        }
    }
    LOG_INFO("Map.dbc: loaded ", mapNameCache_.size(), " map names");
}

std::string GameHandler::getMapName(uint32_t mapId) const {
    loadMapNameCache();
    auto it = mapNameCache_.find(mapId);
    return (it != mapNameCache_.end()) ? it->second : std::string{};
}

// ---------------------------------------------------------------------------
// LFG dungeon name cache (WotLK: LFGDungeons.dbc)
// ---------------------------------------------------------------------------

void GameHandler::loadLfgDungeonDbc() const {
    if (lfgDungeonNameCacheLoaded_) return;
    lfgDungeonNameCacheLoaded_ = true;

    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("LFGDungeons.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("LFGDungeons") : nullptr;
    const uint32_t idField   = layout ? (*layout)["ID"]   : 0;
    const uint32_t nameField = layout ? (*layout)["Name"] : 1;

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, idField);
        if (id == 0) continue;
        std::string name = dbc->getString(i, nameField);
        if (!name.empty())
            lfgDungeonNameCache_[id] = std::move(name);
    }
    LOG_INFO("LFGDungeons.dbc: loaded ", lfgDungeonNameCache_.size(), " dungeon names");
}

std::string GameHandler::getLfgDungeonName(uint32_t dungeonId) const {
    if (dungeonId == 0) return {};
    loadLfgDungeonDbc();
    auto it = lfgDungeonNameCache_.find(dungeonId);
    return (it != lfgDungeonNameCache_.end()) ? it->second : std::string{};
}

// ---------------------------------------------------------------------------
// Aura duration update
// ---------------------------------------------------------------------------

void GameHandler::handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs) {
    if (spellHandler_) spellHandler_->handleUpdateAuraDuration(slot, durationMs);
}

// ---------------------------------------------------------------------------
// Equipment set list
// ---------------------------------------------------------------------------

// ---- Battlefield Manager (WotLK Wintergrasp / outdoor battlefields) ----

void GameHandler::acceptBfMgrInvite() {
    if (socialHandler_) socialHandler_->acceptBfMgrInvite();
}

void GameHandler::declineBfMgrInvite() {
    if (socialHandler_) socialHandler_->declineBfMgrInvite();
}

// ---- WotLK Calendar ----

void GameHandler::requestCalendar() {
    if (socialHandler_) socialHandler_->requestCalendar();
}

// ============================================================
// Delegating getters — SocialHandler owns the canonical state
// ============================================================

uint32_t GameHandler::getTotalTimePlayed() const {
    return socialHandler_ ? socialHandler_->getTotalTimePlayed() : 0;
}

uint32_t GameHandler::getLevelTimePlayed() const {
    return socialHandler_ ? socialHandler_->getLevelTimePlayed() : 0;
}

const std::vector<GameHandler::WhoEntry>& GameHandler::getWhoResults() const {
    if (socialHandler_) return socialHandler_->getWhoResults();
    static const std::vector<WhoEntry> empty;
    return empty;
}

uint32_t GameHandler::getWhoOnlineCount() const {
    return socialHandler_ ? socialHandler_->getWhoOnlineCount() : 0;
}

const std::array<GameHandler::BgQueueSlot, 3>& GameHandler::getBgQueues() const {
    if (socialHandler_) return socialHandler_->getBgQueues();
    static const std::array<BgQueueSlot, 3> empty{};
    return empty;
}

const std::vector<GameHandler::AvailableBgInfo>& GameHandler::getAvailableBgs() const {
    if (socialHandler_) return socialHandler_->getAvailableBgs();
    static const std::vector<AvailableBgInfo> empty;
    return empty;
}

const GameHandler::BgScoreboardData* GameHandler::getBgScoreboard() const {
    return socialHandler_ ? socialHandler_->getBgScoreboard() : nullptr;
}

const std::vector<GameHandler::BgPlayerPosition>& GameHandler::getBgPlayerPositions() const {
    if (socialHandler_) return socialHandler_->getBgPlayerPositions();
    static const std::vector<BgPlayerPosition> empty;
    return empty;
}

bool GameHandler::isLoggingOut() const {
    return socialHandler_ ? socialHandler_->isLoggingOut() : false;
}

float GameHandler::getLogoutCountdown() const {
    return socialHandler_ ? socialHandler_->getLogoutCountdown() : 0.0f;
}

bool GameHandler::isInGuild() const {
    if (socialHandler_) return socialHandler_->isInGuild();
    const Character* ch = getActiveCharacter();
    return ch && ch->hasGuild();
}

bool GameHandler::hasPendingGroupInvite() const {
    return socialHandler_ ? socialHandler_->hasPendingGroupInvite() : pendingGroupInvite;
}
const std::string& GameHandler::getPendingInviterName() const {
    if (socialHandler_) return socialHandler_->getPendingInviterName();
    return pendingInviterName;
}

const std::string& GameHandler::getGuildName() const {
    if (socialHandler_) return socialHandler_->getGuildName();
    static const std::string empty;
    return empty;
}

const GuildRosterData& GameHandler::getGuildRoster() const {
    if (socialHandler_) return socialHandler_->getGuildRoster();
    static const GuildRosterData empty;
    return empty;
}

bool GameHandler::hasGuildRoster() const {
    return socialHandler_ ? socialHandler_->hasGuildRoster() : false;
}

const std::vector<std::string>& GameHandler::getGuildRankNames() const {
    if (socialHandler_) return socialHandler_->getGuildRankNames();
    static const std::vector<std::string> empty;
    return empty;
}

bool GameHandler::hasPendingGuildInvite() const {
    return socialHandler_ ? socialHandler_->hasPendingGuildInvite() : false;
}

const std::string& GameHandler::getPendingGuildInviterName() const {
    if (socialHandler_) return socialHandler_->getPendingGuildInviterName();
    static const std::string empty;
    return empty;
}

const std::string& GameHandler::getPendingGuildInviteGuildName() const {
    if (socialHandler_) return socialHandler_->getPendingGuildInviteGuildName();
    static const std::string empty;
    return empty;
}

const GuildInfoData& GameHandler::getGuildInfoData() const {
    if (socialHandler_) return socialHandler_->getGuildInfoData();
    static const GuildInfoData empty;
    return empty;
}

const GuildQueryResponseData& GameHandler::getGuildQueryData() const {
    if (socialHandler_) return socialHandler_->getGuildQueryData();
    static const GuildQueryResponseData empty;
    return empty;
}

bool GameHandler::hasGuildInfoData() const {
    return socialHandler_ ? socialHandler_->hasGuildInfoData() : false;
}

bool GameHandler::hasPetitionShowlist() const {
    return socialHandler_ ? socialHandler_->hasPetitionShowlist() : false;
}

uint32_t GameHandler::getPetitionCost() const {
    return socialHandler_ ? socialHandler_->getPetitionCost() : 0;
}

uint64_t GameHandler::getPetitionNpcGuid() const {
    return socialHandler_ ? socialHandler_->getPetitionNpcGuid() : 0;
}

const GameHandler::PetitionInfo& GameHandler::getPetitionInfo() const {
    if (socialHandler_) return socialHandler_->getPetitionInfo();
    static const PetitionInfo empty;
    return empty;
}

bool GameHandler::hasPetitionSignaturesUI() const {
    return socialHandler_ ? socialHandler_->hasPetitionSignaturesUI() : false;
}

bool GameHandler::hasPendingReadyCheck() const {
    return socialHandler_ ? socialHandler_->hasPendingReadyCheck() : false;
}

const std::string& GameHandler::getReadyCheckInitiator() const {
    if (socialHandler_) return socialHandler_->getReadyCheckInitiator();
    static const std::string empty;
    return empty;
}

const std::vector<GameHandler::ReadyCheckResult>& GameHandler::getReadyCheckResults() const {
    if (socialHandler_) return socialHandler_->getReadyCheckResults();
    static const std::vector<ReadyCheckResult> empty;
    return empty;
}

uint32_t GameHandler::getInstanceDifficulty() const {
    return socialHandler_ ? socialHandler_->getInstanceDifficulty() : 0;
}

bool GameHandler::isInstanceHeroic() const {
    return socialHandler_ ? socialHandler_->isInstanceHeroic() : false;
}

bool GameHandler::isInInstance() const {
    return socialHandler_ ? socialHandler_->isInInstance() : false;
}

bool GameHandler::hasPendingDuelRequest() const {
    return socialHandler_ ? socialHandler_->hasPendingDuelRequest() : false;
}

const std::string& GameHandler::getDuelChallengerName() const {
    if (socialHandler_) return socialHandler_->getDuelChallengerName();
    static const std::string empty;
    return empty;
}

float GameHandler::getDuelCountdownRemaining() const {
    return socialHandler_ ? socialHandler_->getDuelCountdownRemaining() : 0.0f;
}

const std::vector<GameHandler::InstanceLockout>& GameHandler::getInstanceLockouts() const {
    if (socialHandler_) return socialHandler_->getInstanceLockouts();
    static const std::vector<InstanceLockout> empty;
    return empty;
}

GameHandler::LfgState GameHandler::getLfgState() const {
    return socialHandler_ ? socialHandler_->getLfgState() : LfgState::None;
}

bool GameHandler::isLfgQueued() const {
    return socialHandler_ ? socialHandler_->isLfgQueued() : false;
}

bool GameHandler::isLfgInDungeon() const {
    return socialHandler_ ? socialHandler_->isLfgInDungeon() : false;
}

uint32_t GameHandler::getLfgDungeonId() const {
    return socialHandler_ ? socialHandler_->getLfgDungeonId() : 0;
}

std::string GameHandler::getCurrentLfgDungeonName() const {
    return socialHandler_ ? socialHandler_->getCurrentLfgDungeonName() : std::string{};
}

uint32_t GameHandler::getLfgProposalId() const {
    return socialHandler_ ? socialHandler_->getLfgProposalId() : 0;
}

int32_t GameHandler::getLfgAvgWaitSec() const {
    return socialHandler_ ? socialHandler_->getLfgAvgWaitSec() : -1;
}

uint32_t GameHandler::getLfgTimeInQueueMs() const {
    return socialHandler_ ? socialHandler_->getLfgTimeInQueueMs() : 0;
}

uint32_t GameHandler::getLfgBootVotes() const {
    return socialHandler_ ? socialHandler_->getLfgBootVotes() : 0;
}

uint32_t GameHandler::getLfgBootTotal() const {
    return socialHandler_ ? socialHandler_->getLfgBootTotal() : 0;
}

uint32_t GameHandler::getLfgBootTimeLeft() const {
    return socialHandler_ ? socialHandler_->getLfgBootTimeLeft() : 0;
}

uint32_t GameHandler::getLfgBootNeeded() const {
    return socialHandler_ ? socialHandler_->getLfgBootNeeded() : 0;
}

const std::string& GameHandler::getLfgBootTargetName() const {
    if (socialHandler_) return socialHandler_->getLfgBootTargetName();
    static const std::string empty;
    return empty;
}

const std::string& GameHandler::getLfgBootReason() const {
    if (socialHandler_) return socialHandler_->getLfgBootReason();
    static const std::string empty;
    return empty;
}

const std::vector<GameHandler::ArenaTeamStats>& GameHandler::getArenaTeamStats() const {
    if (socialHandler_) return socialHandler_->getArenaTeamStats();
    static const std::vector<ArenaTeamStats> empty;
    return empty;
}

// ---- SpellHandler delegating getters ----

int GameHandler::getCraftQueueRemaining() const {
    return spellHandler_ ? spellHandler_->getCraftQueueRemaining() : 0;
}
uint32_t GameHandler::getCraftQueueSpellId() const {
    return spellHandler_ ? spellHandler_->getCraftQueueSpellId() : 0;
}
uint32_t GameHandler::getQueuedSpellId() const {
    return spellHandler_ ? spellHandler_->getQueuedSpellId() : 0;
}
const std::unordered_map<uint32_t, TalentEntry>& GameHandler::getAllTalents() const {
    if (spellHandler_) return spellHandler_->getAllTalents();
    static const std::unordered_map<uint32_t, TalentEntry> empty;
    return empty;
}
const std::unordered_map<uint32_t, TalentTabEntry>& GameHandler::getAllTalentTabs() const {
    if (spellHandler_) return spellHandler_->getAllTalentTabs();
    static const std::unordered_map<uint32_t, TalentTabEntry> empty;
    return empty;
}
float GameHandler::getGCDTotal() const {
    return spellHandler_ ? spellHandler_->getGCDTotal() : 0.0f;
}
bool GameHandler::showTalentWipeConfirmDialog() const {
    return spellHandler_ ? spellHandler_->showTalentWipeConfirmDialog() : false;
}
uint32_t GameHandler::getTalentWipeCost() const {
    return spellHandler_ ? spellHandler_->getTalentWipeCost() : 0;
}
void GameHandler::cancelTalentWipe() {
    if (spellHandler_) spellHandler_->cancelTalentWipe();
}
bool GameHandler::showPetUnlearnDialog() const {
    return spellHandler_ ? spellHandler_->showPetUnlearnDialog() : false;
}
uint32_t GameHandler::getPetUnlearnCost() const {
    return spellHandler_ ? spellHandler_->getPetUnlearnCost() : 0;
}
void GameHandler::cancelPetUnlearn() {
    if (spellHandler_) spellHandler_->cancelPetUnlearn();
}

// ---- QuestHandler delegating getters ----

bool GameHandler::isGossipWindowOpen() const {
    return questHandler_ ? questHandler_->isGossipWindowOpen() : gossipWindowOpen;
}
const GossipMessageData& GameHandler::getCurrentGossip() const {
    if (questHandler_) return questHandler_->getCurrentGossip();
    return currentGossip;
}
bool GameHandler::isQuestDetailsOpen() {
    if (questHandler_) return questHandler_->isQuestDetailsOpen();
    return questDetailsOpen;
}
const QuestDetailsData& GameHandler::getQuestDetails() const {
    if (questHandler_) return questHandler_->getQuestDetails();
    return currentQuestDetails;
}

const std::vector<GossipPoi>& GameHandler::getGossipPois() const {
    if (questHandler_) return questHandler_->getGossipPois();
    static const std::vector<GossipPoi> empty;
    return empty;
}
const std::unordered_map<uint64_t, QuestGiverStatus>& GameHandler::getNpcQuestStatuses() const {
    if (questHandler_) return questHandler_->getNpcQuestStatuses();
    static const std::unordered_map<uint64_t, QuestGiverStatus> empty;
    return empty;
}
const std::vector<GameHandler::QuestLogEntry>& GameHandler::getQuestLog() const {
    if (questHandler_) return questHandler_->getQuestLog();
    static const std::vector<QuestLogEntry> empty;
    return empty;
}
bool GameHandler::isQuestOfferRewardOpen() const {
    return questHandler_ ? questHandler_->isQuestOfferRewardOpen() : false;
}
const QuestOfferRewardData& GameHandler::getQuestOfferReward() const {
    if (questHandler_) return questHandler_->getQuestOfferReward();
    static const QuestOfferRewardData empty;
    return empty;
}
bool GameHandler::isQuestRequestItemsOpen() const {
    return questHandler_ ? questHandler_->isQuestRequestItemsOpen() : false;
}
const QuestRequestItemsData& GameHandler::getQuestRequestItems() const {
    if (questHandler_) return questHandler_->getQuestRequestItems();
    static const QuestRequestItemsData empty;
    return empty;
}
int GameHandler::getSelectedQuestLogIndex() const {
    return questHandler_ ? questHandler_->getSelectedQuestLogIndex() : 0;
}
uint32_t GameHandler::getSharedQuestId() const {
    return questHandler_ ? questHandler_->getSharedQuestId() : 0;
}
const std::string& GameHandler::getSharedQuestSharerName() const {
    if (questHandler_) return questHandler_->getSharedQuestSharerName();
    static const std::string empty;
    return empty;
}
const std::string& GameHandler::getSharedQuestTitle() const {
    if (questHandler_) return questHandler_->getSharedQuestTitle();
    static const std::string empty;
    return empty;
}
const std::unordered_set<uint32_t>& GameHandler::getTrackedQuestIds() const {
    if (questHandler_) return questHandler_->getTrackedQuestIds();
    static const std::unordered_set<uint32_t> empty;
    return empty;
}
bool GameHandler::hasPendingSharedQuest() const {
    return questHandler_ ? questHandler_->hasPendingSharedQuest() : false;
}

// ---- MovementHandler delegating getters ----

float GameHandler::getServerRunSpeed() const {
    return movementHandler_ ? movementHandler_->getServerRunSpeed() : 7.0f;
}
float GameHandler::getServerWalkSpeed() const {
    return movementHandler_ ? movementHandler_->getServerWalkSpeed() : 2.5f;
}
float GameHandler::getServerSwimSpeed() const {
    return movementHandler_ ? movementHandler_->getServerSwimSpeed() : 4.722f;
}
float GameHandler::getServerSwimBackSpeed() const {
    return movementHandler_ ? movementHandler_->getServerSwimBackSpeed() : 2.5f;
}
float GameHandler::getServerFlightSpeed() const {
    return movementHandler_ ? movementHandler_->getServerFlightSpeed() : 7.0f;
}
float GameHandler::getServerFlightBackSpeed() const {
    return movementHandler_ ? movementHandler_->getServerFlightBackSpeed() : 4.5f;
}
float GameHandler::getServerRunBackSpeed() const {
    return movementHandler_ ? movementHandler_->getServerRunBackSpeed() : 4.5f;
}
float GameHandler::getServerTurnRate() const {
    return movementHandler_ ? movementHandler_->getServerTurnRate() : 3.14159f;
}
bool GameHandler::isTaxiWindowOpen() const {
    return movementHandler_ ? movementHandler_->isTaxiWindowOpen() : false;
}
bool GameHandler::isOnTaxiFlight() const {
    return movementHandler_ ? movementHandler_->isOnTaxiFlight() : false;
}
bool GameHandler::isTaxiMountActive() const {
    return movementHandler_ ? movementHandler_->isTaxiMountActive() : false;
}
bool GameHandler::isTaxiActivationPending() const {
    return movementHandler_ ? movementHandler_->isTaxiActivationPending() : false;
}
const std::string& GameHandler::getTaxiDestName() const {
    if (movementHandler_) return movementHandler_->getTaxiDestName();
    static const std::string empty;
    return empty;
}
const ShowTaxiNodesData& GameHandler::getTaxiData() const {
    if (movementHandler_) return movementHandler_->getTaxiData();
    static const ShowTaxiNodesData empty;
    return empty;
}
uint32_t GameHandler::getTaxiCurrentNode() const {
    if (movementHandler_) return movementHandler_->getTaxiData().nearestNode;
    return 0;
}
const std::unordered_map<uint32_t, GameHandler::TaxiNode>& GameHandler::getTaxiNodes() const {
    if (movementHandler_) return movementHandler_->getTaxiNodes();
    static const std::unordered_map<uint32_t, TaxiNode> empty;
    return empty;
}

} // namespace game
} // namespace wowee
