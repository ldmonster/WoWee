#include "game/entity_controller.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/packet_parsers.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "game/chat_handler.hpp"
#include "game/transport_manager.hpp"
#include "core/logger.hpp"
#include "core/coordinates.hpp"
#include "network/world_socket.hpp"
#include <algorithm>
#include <cstring>
#include <zlib.h>

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

bool envFlagEnabled(const char* key, bool defaultValue = false) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    return !(raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F' ||
             raw[0] == 'n' || raw[0] == 'N');
}

int parseEnvIntClamped(const char* key, int defaultValue, int minValue, int maxValue) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    char* end = nullptr;
    long parsed = std::strtol(raw, &end, 10);
    if (end == raw) return defaultValue;
    return static_cast<int>(std::clamp<long>(parsed, minValue, maxValue));
}

int updateObjectBlocksBudgetPerUpdate(WorldState state) {
    static const int inWorldBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_UPDATE_OBJECT_BLOCKS", 24, 1, 2048);
    static const int loginBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_UPDATE_OBJECT_BLOCKS_LOGIN", 128, 1, 4096);
    return state == WorldState::IN_WORLD ? inWorldBudget : loginBudget;
}

float slowUpdateObjectBlockLogThresholdMs() {
    static const int thresholdMs =
        parseEnvIntClamped("WOWEE_NET_SLOW_UPDATE_BLOCK_LOG_MS", 10, 1, 60000);
    return static_cast<float>(thresholdMs);
}

} // anonymous namespace

EntityController::EntityController(GameHandler& owner)
    : owner_(owner) { initTypeHandlers(); }

void EntityController::registerOpcodes(DispatchTable& table) {
    // World object updates
    table[Opcode::SMSG_UPDATE_OBJECT] = [this](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_UPDATE_OBJECT, state=", static_cast<int>(owner_.state), " size=", packet.getSize());
        if (owner_.state == WorldState::IN_WORLD) handleUpdateObject(packet);
    };
    table[Opcode::SMSG_COMPRESSED_UPDATE_OBJECT] = [this](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_COMPRESSED_UPDATE_OBJECT, state=", static_cast<int>(owner_.state), " size=", packet.getSize());
        if (owner_.state == WorldState::IN_WORLD) handleCompressedUpdateObject(packet);
    };
    table[Opcode::SMSG_DESTROY_OBJECT] = [this](network::Packet& packet) {
        if (owner_.state == WorldState::IN_WORLD) handleDestroyObject(packet);
    };

    // Entity queries
    table[Opcode::SMSG_NAME_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleNameQueryResponse(packet);
    };
    table[Opcode::SMSG_CREATURE_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleCreatureQueryResponse(packet);
    };
    table[Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handleGameObjectQueryResponse(packet);
    };
    table[Opcode::SMSG_GAMEOBJECT_PAGETEXT] = [this](network::Packet& packet) {
        handleGameObjectPageText(packet);
    };
    table[Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE] = [this](network::Packet& packet) {
        handlePageTextQueryResponse(packet);
    };
}

void EntityController::clearAll() {
    pendingUpdateObjectWork_.clear();
    playerNameCache.clear();
    playerClassRaceCache_.clear();
    pendingNameQueries.clear();
    creatureInfoCache.clear();
    pendingCreatureQueries.clear();
    gameObjectInfoCache_.clear();
    pendingGameObjectQueries_.clear();
    transportGuids_.clear();
    serverUpdatedTransportGuids_.clear();
    entityManager.clear();
}

// ============================================================
// Update Object Pipeline
// ============================================================

void EntityController::enqueueUpdateObjectWork(UpdateObjectData&& data) {
    pendingUpdateObjectWork_.push_back(PendingUpdateObjectWork{std::move(data)});
}
void EntityController::processPendingUpdateObjectWork(const std::chrono::steady_clock::time_point& start,
                                                 float budgetMs) {
    if (pendingUpdateObjectWork_.empty()) {
        return;
    }

    const int maxBlocksThisUpdate = updateObjectBlocksBudgetPerUpdate(owner_.state);
    int processedBlocks = 0;

    while (!pendingUpdateObjectWork_.empty() && processedBlocks < maxBlocksThisUpdate) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= budgetMs) {
            break;
        }

        auto& work = pendingUpdateObjectWork_.front();
        if (!work.outOfRangeProcessed) {
            auto outOfRangeStart = std::chrono::steady_clock::now();
            processOutOfRangeObjects(work.data.outOfRangeGuids);
            float outOfRangeMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - outOfRangeStart).count();
            if (outOfRangeMs > slowUpdateObjectBlockLogThresholdMs()) {
                LOG_WARNING("SLOW update-object out-of-range handling: ", outOfRangeMs,
                            "ms guidCount=", work.data.outOfRangeGuids.size());
            }
            work.outOfRangeProcessed = true;
        }

        while (work.nextBlockIndex < work.data.blocks.size() && processedBlocks < maxBlocksThisUpdate) {
            elapsedMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsedMs >= budgetMs) {
                break;
            }

            const UpdateBlock& block = work.data.blocks[work.nextBlockIndex];
            auto blockStart = std::chrono::steady_clock::now();
            applyUpdateObjectBlock(block, work.newItemCreated);
            float blockMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - blockStart).count();
            if (blockMs > slowUpdateObjectBlockLogThresholdMs()) {
                LOG_WARNING("SLOW update-object block apply: ", blockMs,
                            "ms index=", work.nextBlockIndex,
                            " type=", static_cast<int>(block.updateType),
                            " guid=0x", std::hex, block.guid, std::dec,
                            " objectType=", static_cast<int>(block.objectType),
                            " fieldCount=", block.fields.size(),
                            " hasMovement=", block.hasMovement ? 1 : 0);
            }
            ++work.nextBlockIndex;
            ++processedBlocks;
        }

        if (work.nextBlockIndex >= work.data.blocks.size()) {
            finalizeUpdateObjectBatch(work.newItemCreated);
            pendingUpdateObjectWork_.pop_front();
            continue;
        }
        break;
    }

    if (!pendingUpdateObjectWork_.empty()) {
        const auto& work = pendingUpdateObjectWork_.front();
        LOG_DEBUG("GameHandler update-object budget reached (remainingBatches=",
                  pendingUpdateObjectWork_.size(), ", nextBlockIndex=", work.nextBlockIndex,
                  "/", work.data.blocks.size(), ", owner_.state=", worldStateName(owner_.state), ")");
    }
}
void EntityController::handleUpdateObject(network::Packet& packet) {
    UpdateObjectData data;
    if (!owner_.packetParsers_->parseUpdateObject(packet, data)) {
        static int updateObjErrors = 0;
        if (++updateObjErrors <= 5)
            LOG_WARNING("Failed to parse SMSG_UPDATE_OBJECT");
        if (data.blocks.empty()) return;
        // Fall through: process any blocks that were successfully parsed before the failure.
    }

    enqueueUpdateObjectWork(std::move(data));
}

void EntityController::processOutOfRangeObjects(const std::vector<uint64_t>& guids) {
    // Process out-of-range objects first
    for (uint64_t guid : guids) {
        auto entity = entityManager.getEntity(guid);
        if (!entity) continue;

        const bool isKnownTransport = transportGuids_.count(guid) > 0;
        if (isKnownTransport) {
            // Keep transports alive across out-of-range flapping.
            // Boats/zeppelins are global movers and removing them here can make
            // them disappear until a later movement snapshot happens to recreate them.
            const bool playerAboardNow = (owner_.playerTransportGuid_ == guid);
            const bool stickyAboard = (owner_.playerTransportStickyGuid_ == guid && owner_.playerTransportStickyTimer_ > 0.0f);
            const bool movementSaysAboard = (owner_.movementInfo.transportGuid == guid);
            LOG_INFO("Preserving transport on out-of-range: 0x",
                     std::hex, guid, std::dec,
                     " now=", playerAboardNow,
                     " sticky=", stickyAboard,
                     " movement=", movementSaysAboard);
            continue;
        }

        LOG_DEBUG("Entity went out of range: 0x", std::hex, guid, std::dec);
        // Trigger despawn callbacks before removing entity
        if (entity->getType() == ObjectType::UNIT && owner_.creatureDespawnCallback_) {
            owner_.creatureDespawnCallback_(guid);
        } else if (entity->getType() == ObjectType::PLAYER && owner_.playerDespawnCallback_) {
            owner_.playerDespawnCallback_(guid);
            owner_.otherPlayerVisibleItemEntries_.erase(guid);
            owner_.otherPlayerVisibleDirty_.erase(guid);
            owner_.otherPlayerMoveTimeMs_.erase(guid);
            owner_.inspectedPlayerItemEntries_.erase(guid);
            owner_.pendingAutoInspect_.erase(guid);
            // Clear pending name query so the query is re-sent when this player
            // comes back into range (entity is recreated as a new object).
            pendingNameQueries.erase(guid);
        } else if (entity->getType() == ObjectType::GAMEOBJECT && owner_.gameObjectDespawnCallback_) {
            owner_.gameObjectDespawnCallback_(guid);
        }
        transportGuids_.erase(guid);
        serverUpdatedTransportGuids_.erase(guid);
        owner_.clearTransportAttachment(guid);
        if (owner_.playerTransportGuid_ == guid) {
            owner_.clearPlayerTransport();
        }
        entityManager.removeEntity(guid);
    }

}

// ============================================================
// Phase 1: Extracted helper methods
// ============================================================

bool EntityController::extractPlayerAppearance(const std::map<uint16_t, uint32_t>& fields,
                                               uint8_t& outRace,
                                               uint8_t& outGender,
                                               uint32_t& outAppearanceBytes,
                                               uint8_t& outFacial) const {
    outRace = 0;
    outGender = 0;
    outAppearanceBytes = 0;
    outFacial = 0;

    auto readField = [&](uint16_t idx, uint32_t& out) -> bool {
        if (idx == 0xFFFF) return false;
        auto it = fields.find(idx);
        if (it == fields.end()) return false;
        out = it->second;
        return true;
    };

    uint32_t bytes0 = 0;
    uint32_t pbytes = 0;
    uint32_t pbytes2 = 0;

    const uint16_t ufBytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
    const uint16_t ufPbytes = fieldIndex(UF::PLAYER_BYTES);
    const uint16_t ufPbytes2 = fieldIndex(UF::PLAYER_BYTES_2);

    bool haveBytes0 = readField(ufBytes0, bytes0);
    bool havePbytes = readField(ufPbytes, pbytes);
    bool havePbytes2 = readField(ufPbytes2, pbytes2);

    // Heuristic fallback: Turtle can run with unusual build numbers; if the JSON table is missing,
    // try to locate plausible packed fields by scanning.
    if (!haveBytes0) {
        for (const auto& [idx, v] : fields) {
            uint8_t race = static_cast<uint8_t>(v & 0xFF);
            uint8_t cls = static_cast<uint8_t>((v >> 8) & 0xFF);
            uint8_t gender = static_cast<uint8_t>((v >> 16) & 0xFF);
            uint8_t power = static_cast<uint8_t>((v >> 24) & 0xFF);
            if (race >= 1 && race <= 20 &&
                cls >= 1 && cls <= 20 &&
                gender <= 1 &&
                power <= 10) {
                bytes0 = v;
                haveBytes0 = true;
                break;
            }
        }
    }
    if (!havePbytes) {
        for (const auto& [idx, v] : fields) {
            uint8_t skin = static_cast<uint8_t>(v & 0xFF);
            uint8_t face = static_cast<uint8_t>((v >> 8) & 0xFF);
            uint8_t hair = static_cast<uint8_t>((v >> 16) & 0xFF);
            uint8_t color = static_cast<uint8_t>((v >> 24) & 0xFF);
            if (skin <= 50 && face <= 50 && hair <= 100 && color <= 50) {
                pbytes = v;
                havePbytes = true;
                break;
            }
        }
    }
    if (!havePbytes2) {
        for (const auto& [idx, v] : fields) {
            uint8_t facial = static_cast<uint8_t>(v & 0xFF);
            if (facial <= 100) {
                pbytes2 = v;
                havePbytes2 = true;
                break;
            }
        }
    }

    if (!haveBytes0 || !havePbytes) return false;

    outRace = static_cast<uint8_t>(bytes0 & 0xFF);
    outGender = static_cast<uint8_t>((bytes0 >> 16) & 0xFF);
    outAppearanceBytes = pbytes;
    outFacial = havePbytes2 ? static_cast<uint8_t>(pbytes2 & 0xFF) : 0;
    return true;
}

void EntityController::maybeDetectCoinageIndex(const std::map<uint16_t, uint32_t>& oldFields,
                                               const std::map<uint16_t, uint32_t>& newFields) {
    if (owner_.pendingMoneyDelta_ == 0 || owner_.pendingMoneyDeltaTimer_ <= 0.0f) return;
    if (oldFields.empty() || newFields.empty()) return;

    constexpr uint32_t kMaxPlausibleCoinage = 2147483647u;
    std::vector<uint16_t> candidates;
    candidates.reserve(8);

    for (const auto& [idx, newVal] : newFields) {
        auto itOld = oldFields.find(idx);
        if (itOld == oldFields.end()) continue;
        uint32_t oldVal = itOld->second;
        if (newVal < oldVal) continue;
        uint32_t delta = newVal - oldVal;
        if (delta != owner_.pendingMoneyDelta_) continue;
        if (newVal > kMaxPlausibleCoinage) continue;
        candidates.push_back(idx);
    }

    if (candidates.empty()) return;

    uint16_t current = fieldIndex(UF::PLAYER_FIELD_COINAGE);
    uint16_t chosen = candidates[0];
    if (std::find(candidates.begin(), candidates.end(), current) != candidates.end()) {
        chosen = current;
    } else {
        std::sort(candidates.begin(), candidates.end());
        chosen = candidates[0];
    }

    if (chosen != current && current != 0xFFFF) {
        owner_.updateFieldTable_.setIndex(UF::PLAYER_FIELD_COINAGE, chosen);
        LOG_WARNING("Auto-detected PLAYER_FIELD_COINAGE index: ", chosen, " (was ", current, ")");
    }

    owner_.pendingMoneyDelta_ = 0;
    owner_.pendingMoneyDeltaTimer_ = 0.0f;
}

// ============================================================
// Phase 2: Update type dispatch
// ============================================================

void EntityController::applyUpdateObjectBlock(const UpdateBlock& block, bool& newItemCreated) {
    switch (block.updateType) {
        case UpdateType::CREATE_OBJECT:
        case UpdateType::CREATE_OBJECT2:
            handleCreateObject(block, newItemCreated);
            break;
        case UpdateType::VALUES:
            handleValuesUpdate(block);
            break;
        case UpdateType::MOVEMENT:
            handleMovementUpdate(block);
            break;
        default:
            break;
    }
}

// ============================================================
// Phase 3: Concern-specific helpers
// ============================================================

// 3i: Non-player transport child attachment — identical in CREATE/VALUES/MOVEMENT
void EntityController::updateNonPlayerTransportAttachment(const UpdateBlock& block,
                                                           const std::shared_ptr<Entity>& entity,
                                                           ObjectType entityType) {
    if (block.guid == owner_.playerGuid) return;
    if (entityType != ObjectType::UNIT && entityType != ObjectType::GAMEOBJECT) return;

    if (block.onTransport && block.transportGuid != 0) {
        glm::vec3 localOffset = core::coords::serverToCanonical(
            glm::vec3(block.transportX, block.transportY, block.transportZ));
        const bool hasLocalOrientation = (block.updateFlags & 0x0020) != 0; // UPDATEFLAG_LIVING
        float localOriCanonical = core::coords::normalizeAngleRad(-block.transportO);
        owner_.setTransportAttachment(block.guid, entityType, block.transportGuid,
                               localOffset, hasLocalOrientation, localOriCanonical);
        if (owner_.transportManager_ && owner_.transportManager_->getTransport(block.transportGuid)) {
            glm::vec3 composed = owner_.transportManager_->getPlayerWorldPosition(block.transportGuid, localOffset);
            entity->setPosition(composed.x, composed.y, composed.z, entity->getOrientation());
        }
    } else {
        owner_.clearTransportAttachment(block.guid);
    }
}

// 3f: Rebuild playerAuras from UNIT_FIELD_AURAS (Classic/vanilla only).
//     blockFields is used to check if any aura field was updated in this packet.
//     entity->getFields() is used for reading the full accumulated state.
//     Normalises Classic harmful bit (0x02) to WotLK debuff bit (0x80) so
//     downstream code checking for 0x80 works consistently across expansions.
void EntityController::syncClassicAurasFromFields(const std::shared_ptr<Entity>& entity) {
    if (!isClassicLikeExpansion() || !owner_.spellHandler_) return;

    const uint16_t ufAuras     = fieldIndex(UF::UNIT_FIELD_AURAS);
    const uint16_t ufAuraFlags = fieldIndex(UF::UNIT_FIELD_AURAFLAGS);
    if (ufAuras == 0xFFFF) return;

    const auto& allFields = entity->getFields();
    bool hasAuraField = false;
    for (const auto& [fk, fv] : allFields) {
        if (fk >= ufAuras && fk < ufAuras + 48) { hasAuraField = true; break; }
    }
    if (!hasAuraField) return;

    owner_.spellHandler_->playerAuras_.clear();
    owner_.spellHandler_->playerAuras_.resize(48);
    uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    for (int slot = 0; slot < 48; ++slot) {
        auto it = allFields.find(static_cast<uint16_t>(ufAuras + slot));
        if (it != allFields.end() && it->second != 0) {
            AuraSlot& a = owner_.spellHandler_->playerAuras_[slot];
            a.spellId = it->second;
            // Read aura flag byte: packed 4-per-uint32 at ufAuraFlags
            uint8_t aFlag = 0;
            if (ufAuraFlags != 0xFFFF) {
                auto fit = allFields.find(static_cast<uint16_t>(ufAuraFlags + slot / 4));
                if (fit != allFields.end())
                    aFlag = static_cast<uint8_t>((fit->second >> ((slot % 4) * 8)) & 0xFF);
            }
            // Normalize Classic harmful bit (0x02) to WotLK debuff bit (0x80)
            // so downstream code checking for 0x80 works consistently.
            if (aFlag & 0x02)
                aFlag = (aFlag & ~0x02) | 0x80;
            a.flags = aFlag;
            a.durationMs = -1;
            a.maxDurationMs = -1;
            a.casterGuid = owner_.playerGuid;
            a.receivedAtMs = nowMs;
        }
    }
    LOG_DEBUG("[Classic] Rebuilt playerAuras from UNIT_FIELD_AURAS");
    pendingEvents_.emit("UNIT_AURA", {"player"});
}

// 3h: Detect player mount/dismount from UNIT_FIELD_MOUNTDISPLAYID changes
void EntityController::detectPlayerMountChange(uint32_t newMountDisplayId,
                                                const std::map<uint16_t, uint32_t>& blockFields) {
    uint32_t old = owner_.currentMountDisplayId_;
    owner_.currentMountDisplayId_ = newMountDisplayId;
    if (newMountDisplayId != old && owner_.mountCallback_) owner_.mountCallback_(newMountDisplayId);
    if (newMountDisplayId != old)
        pendingEvents_.emit("UNIT_MODEL_CHANGED", {"player"});
    if (old == 0 && newMountDisplayId != 0) {
        // Just mounted — find the mount aura (indefinite duration, self-cast)
        owner_.mountAuraSpellId_ = 0;
        if (owner_.spellHandler_) for (const auto& a : owner_.spellHandler_->playerAuras_) {
            if (!a.isEmpty() && a.maxDurationMs < 0 && a.casterGuid == owner_.playerGuid) {
                owner_.mountAuraSpellId_ = a.spellId;
            }
        }
        // Classic/vanilla fallback: scan UNIT_FIELD_AURAS from same update block
        if (owner_.mountAuraSpellId_ == 0) {
            const uint16_t ufAuras = fieldIndex(UF::UNIT_FIELD_AURAS);
            if (ufAuras != 0xFFFF) {
                for (const auto& [fk, fv] : blockFields) {
                    if (fk >= ufAuras && fk < ufAuras + 48 && fv != 0) {
                        owner_.mountAuraSpellId_ = fv;
                        break;
                    }
                }
            }
        }
        LOG_INFO("Mount detected: displayId=", newMountDisplayId, " auraSpellId=", owner_.mountAuraSpellId_);
    }
    if (old != 0 && newMountDisplayId == 0) {
        // Only clear the specific mount aura, not all indefinite auras.
        // Previously this cleared every aura with maxDurationMs < 0, which
        // would strip racial passives, tracking, and zone buffs on dismount.
        uint32_t mountSpell = owner_.mountAuraSpellId_;
        owner_.mountAuraSpellId_ = 0;
        if (mountSpell != 0 && owner_.spellHandler_) {
            for (auto& a : owner_.spellHandler_->playerAuras_) {
                if (!a.isEmpty() && a.spellId == mountSpell) {
                    a = AuraSlot{};
                    break;
                }
            }
        }
    }
}

// Phase 4: Resolve cached field indices once per handler call.
EntityController::UnitFieldIndices EntityController::UnitFieldIndices::resolve() {
    return UnitFieldIndices{
        fieldIndex(UF::UNIT_FIELD_HEALTH),
        fieldIndex(UF::UNIT_FIELD_MAXHEALTH),
        fieldIndex(UF::UNIT_FIELD_POWER1),
        fieldIndex(UF::UNIT_FIELD_MAXPOWER1),
        fieldIndex(UF::UNIT_FIELD_LEVEL),
        fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE),
        fieldIndex(UF::UNIT_FIELD_FLAGS),
        fieldIndex(UF::UNIT_DYNAMIC_FLAGS),
        fieldIndex(UF::UNIT_FIELD_DISPLAYID),
        fieldIndex(UF::UNIT_FIELD_MOUNTDISPLAYID),
        fieldIndex(UF::UNIT_NPC_FLAGS),
        fieldIndex(UF::UNIT_FIELD_BYTES_0),
        fieldIndex(UF::UNIT_FIELD_BYTES_1)
    };
}

EntityController::PlayerFieldIndices EntityController::PlayerFieldIndices::resolve() {
    return PlayerFieldIndices{
        fieldIndex(UF::PLAYER_XP),
        fieldIndex(UF::PLAYER_NEXT_LEVEL_XP),
        fieldIndex(UF::PLAYER_REST_STATE_EXPERIENCE),
        fieldIndex(UF::UNIT_FIELD_LEVEL),
        fieldIndex(UF::PLAYER_FIELD_COINAGE),
        fieldIndex(UF::PLAYER_FIELD_HONOR_CURRENCY),
        fieldIndex(UF::PLAYER_FIELD_ARENA_CURRENCY),
        fieldIndex(UF::PLAYER_FLAGS),
        fieldIndex(UF::UNIT_FIELD_RESISTANCES),
        fieldIndex(UF::PLAYER_BYTES),
        fieldIndex(UF::PLAYER_BYTES_2),
        fieldIndex(UF::PLAYER_CHOSEN_TITLE),
        {fieldIndex(UF::UNIT_FIELD_STAT0), fieldIndex(UF::UNIT_FIELD_STAT1),
         fieldIndex(UF::UNIT_FIELD_STAT2), fieldIndex(UF::UNIT_FIELD_STAT3),
         fieldIndex(UF::UNIT_FIELD_STAT4)},
        fieldIndex(UF::UNIT_FIELD_ATTACK_POWER),
        fieldIndex(UF::UNIT_FIELD_RANGED_ATTACK_POWER),
        fieldIndex(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS),
        fieldIndex(UF::PLAYER_FIELD_MOD_HEALING_DONE_POS),
        fieldIndex(UF::PLAYER_BLOCK_PERCENTAGE),
        fieldIndex(UF::PLAYER_DODGE_PERCENTAGE),
        fieldIndex(UF::PLAYER_PARRY_PERCENTAGE),
        fieldIndex(UF::PLAYER_CRIT_PERCENTAGE),
        fieldIndex(UF::PLAYER_RANGED_CRIT_PERCENTAGE),
        fieldIndex(UF::PLAYER_SPELL_CRIT_PERCENTAGE1),
        fieldIndex(UF::PLAYER_FIELD_COMBAT_RATING_1)
    };
}

// 3a: Create the appropriate Entity subclass from the block's object type.
std::shared_ptr<Entity> EntityController::createEntityFromBlock(const UpdateBlock& block) {
    switch (block.objectType) {
        case ObjectType::PLAYER:
            return std::make_shared<Player>(block.guid);
        case ObjectType::UNIT:
            return std::make_shared<Unit>(block.guid);
        case ObjectType::GAMEOBJECT:
            return std::make_shared<GameObject>(block.guid);
        default: {
            auto entity = std::make_shared<Entity>(block.guid);
            entity->setType(block.objectType);
            return entity;
        }
    }
}

// 3b: Track player-on-transport state from movement blocks.
//     Consolidates near-identical logic from CREATE and MOVEMENT handlers.
//     When updateMovementInfoPos is true (MOVEMENT), movementInfo.x/y/z are set
//     to the raw canonical position when not on a resolved transport.
//     When false (CREATE), movementInfo is only set for resolved transport positions.
void EntityController::applyPlayerTransportState(const UpdateBlock& block,
                                                   const std::shared_ptr<Entity>& entity,
                                                   const glm::vec3& canonicalPos, float oCanonical,
                                                   bool updateMovementInfoPos) {
    if (block.onTransport) {
        // Convert transport offset from server → canonical coordinates
        glm::vec3 serverOffset(block.transportX, block.transportY, block.transportZ);
        glm::vec3 canonicalOffset = core::coords::serverToCanonical(serverOffset);
        owner_.setPlayerOnTransport(block.transportGuid, canonicalOffset);
        if (owner_.transportManager_ && owner_.transportManager_->getTransport(owner_.playerTransportGuid_)) {
            glm::vec3 composed = owner_.transportManager_->getPlayerWorldPosition(owner_.playerTransportGuid_, owner_.playerTransportOffset_);
            entity->setPosition(composed.x, composed.y, composed.z, oCanonical);
            owner_.movementInfo.x = composed.x;
            owner_.movementInfo.y = composed.y;
            owner_.movementInfo.z = composed.z;
        } else if (updateMovementInfoPos) {
            owner_.movementInfo.x = canonicalPos.x;
            owner_.movementInfo.y = canonicalPos.y;
            owner_.movementInfo.z = canonicalPos.z;
        }
        LOG_INFO("Player on transport: 0x", std::hex, owner_.playerTransportGuid_, std::dec,
                " offset=(", owner_.playerTransportOffset_.x, ", ", owner_.playerTransportOffset_.y,
                ", ", owner_.playerTransportOffset_.z, ")");
    } else {
        if (updateMovementInfoPos) {
            owner_.movementInfo.x = canonicalPos.x;
            owner_.movementInfo.y = canonicalPos.y;
            owner_.movementInfo.z = canonicalPos.z;
        }
        // Don't clear client-side M2 transport boarding (trams) —
        // the server doesn't know about client-detected transport attachment.
        bool isClientM2Transport = false;
        if (owner_.playerTransportGuid_ != 0 && owner_.transportManager_) {
            auto* tr = owner_.transportManager_->getTransport(owner_.playerTransportGuid_);
            isClientM2Transport = (tr && tr->isM2);
        }
        if (owner_.playerTransportGuid_ != 0 && !isClientM2Transport) {
            LOG_INFO("Player left transport");
            owner_.clearPlayerTransport();
        }
    }
}

// 3c: Apply unit fields during CREATE — sets health/power/level/flags/displayId/etc.
//     Returns true if the entity is initially dead (health=0 or DYNFLAG_DEAD).
bool EntityController::applyUnitFieldsOnCreate(const UpdateBlock& block,
                                                 std::shared_ptr<Unit>& unit,
                                                 const UnitFieldIndices& ufi) {
    bool unitInitiallyDead = false;
    constexpr uint32_t UNIT_DYNFLAG_DEAD = 0x0008;
    constexpr uint32_t UNIT_DYNFLAG_LOOTABLE = 0x0001;

    for (const auto& [key, val] : block.fields) {
        // Check all specific fields BEFORE power/maxpower range checks.
        // In Classic, power indices (23-27) are adjacent to maxHealth (28),
        // and maxPower indices (29-33) are adjacent to level (34) and faction (35).
        // A range check like "key >= powerBase && key < powerBase+7" would
        // incorrectly capture maxHealth/level/faction in Classic's tight layout.
        if (key == ufi.health) {
            unit->setHealth(val);
            if (block.objectType == ObjectType::UNIT && val == 0) {
                unitInitiallyDead = true;
            }
            if (block.guid == owner_.playerGuid && val == 0) {
                owner_.playerDead_ = true;
                LOG_INFO("Player logged in dead");
            }
        } else if (key == ufi.maxHealth) { unit->setMaxHealth(val); }
        else if (key == ufi.level) {
            unit->setLevel(val);
        } else if (key == ufi.faction) {
            unit->setFactionTemplate(val);
            if (owner_.addonEventCallback_) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_FACTION", {uid});
            }
        }
        else if (key == ufi.flags) {
            unit->setUnitFlags(val);
            if (owner_.addonEventCallback_) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_FLAGS", {uid});
            }
        }
        else if (key == ufi.bytes0) {
            unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
        } else if (key == ufi.displayId) {
            unit->setDisplayId(val);
            if (owner_.addonEventCallback_) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_MODEL_CHANGED", {uid});
            }
        }
        else if (key == ufi.npcFlags) { unit->setNpcFlags(val); }
        else if (key == ufi.dynFlags) {
            unit->setDynamicFlags(val);
            if (block.objectType == ObjectType::UNIT &&
                ((val & UNIT_DYNFLAG_DEAD) != 0 || (val & UNIT_DYNFLAG_LOOTABLE) != 0)) {
                unitInitiallyDead = true;
            }
        }
        // Power/maxpower range checks AFTER all specific fields
        else if (key >= ufi.powerBase && key < ufi.powerBase + 7) {
            unit->setPowerByType(static_cast<uint8_t>(key - ufi.powerBase), val);
        } else if (key >= ufi.maxPowerBase && key < ufi.maxPowerBase + 7) {
            unit->setMaxPowerByType(static_cast<uint8_t>(key - ufi.maxPowerBase), val);
        }
        else if (key == ufi.mountDisplayId) {
            if (block.guid == owner_.playerGuid) {
                detectPlayerMountChange(val, block.fields);
            }
            unit->setMountDisplayId(val);
        }
    }
    return unitInitiallyDead;
}

// Consolidates player-death state into one place so both the health==0 and
// dynFlags UNIT_DYNFLAG_DEAD paths share the same corpse-caching logic.
// Classic WoW does not send SMSG_DEATH_RELEASE_LOC, so this cached position
// is the primary source for canReclaimCorpse().
void EntityController::markPlayerDead(const char* source) {
    owner_.playerDead_ = true;
    owner_.releasedSpirit_ = false;
    // owner_.movementInfo is canonical (x=north, y=west); corpseX_/Y_ are
    // raw server coords (x=west, y=north) — swap axes.
    owner_.corpseX_     = owner_.movementInfo.y;
    owner_.corpseY_     = owner_.movementInfo.x;
    owner_.corpseZ_     = owner_.movementInfo.z;
    owner_.corpseMapId_ = owner_.currentMapId_;
    LOG_INFO("Player died (", source, "). Corpse cached at server=(",
             owner_.corpseX_, ",", owner_.corpseY_, ",", owner_.corpseZ_,
             ") map=", owner_.corpseMapId_);
}

// 3c: Apply unit fields during VALUES update — tracks health/power/display changes
//     and fires events for transitions (death, resurrect, level up, etc.).
EntityController::UnitFieldUpdateResult EntityController::applyUnitFieldsOnUpdate(
        const UpdateBlock& block, const std::shared_ptr<Entity>& entity,
        std::shared_ptr<Unit>& unit, const UnitFieldIndices& ufi) {
    UnitFieldUpdateResult result;
    result.oldDisplayId = unit->getDisplayId();
    uint32_t oldHealth = unit->getHealth();
    constexpr uint32_t UNIT_DYNFLAG_DEAD = 0x0008;

    for (const auto& [key, val] : block.fields) {
        if (key == ufi.health) {
            unit->setHealth(val);
            result.healthChanged = true;
            if (val == 0) {
                if (owner_.combatHandler_ && block.guid == owner_.combatHandler_->getAutoAttackTargetGuid()) {
                    owner_.stopAutoAttack();
                }
                if (owner_.combatHandler_) owner_.combatHandler_->removeHostileAttacker(block.guid);
                if (block.guid == owner_.playerGuid) {
                    markPlayerDead("health=0");
                    owner_.stopAutoAttack();
                    pendingEvents_.emit("PLAYER_DEAD", {});
                }
                if ((entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) && owner_.npcDeathCallback_) {
                    owner_.npcDeathCallback_(block.guid);
                    result.npcDeathNotified = true;
                }
            } else if (oldHealth == 0 && val > 0) {
                if (block.guid == owner_.playerGuid) {
                    bool wasGhost = owner_.releasedSpirit_;
                    owner_.playerDead_ = false;
                    if (!wasGhost) {
                        LOG_INFO("Player resurrected!");
                        pendingEvents_.emit("PLAYER_ALIVE", {});
                    } else {
                        LOG_INFO("Player entered ghost form");
                        owner_.releasedSpirit_ = false;
                        pendingEvents_.emit("PLAYER_UNGHOST", {});
                    }
                }
                if ((entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) && owner_.npcRespawnCallback_) {
                    owner_.npcRespawnCallback_(block.guid);
                    result.npcRespawnNotified = true;
                }
            }
        // Specific fields checked BEFORE power/maxpower range checks
        // (Classic packs maxHealth/level/faction adjacent to power indices)
        } else if (key == ufi.maxHealth) { unit->setMaxHealth(val); result.healthChanged = true; }
        else if (key == ufi.bytes0) {
            uint8_t oldPT = unit->getPowerType();
            unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
            if (unit->getPowerType() != oldPT) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_DISPLAYPOWER", {uid});
            }
        } else if (key == ufi.flags) { unit->setUnitFlags(val); }
        else if (ufi.bytes1 != 0xFFFF && key == ufi.bytes1 && block.guid == owner_.playerGuid) {
            uint8_t newForm = static_cast<uint8_t>((val >> 24) & 0xFF);
            if (newForm != owner_.shapeshiftFormId_) {
                owner_.shapeshiftFormId_ = newForm;
                LOG_INFO("Shapeshift form changed: ", static_cast<int>(newForm));
                    pendingEvents_.emit("UPDATE_SHAPESHIFT_FORM", {});
                    pendingEvents_.emit("UPDATE_SHAPESHIFT_FORMS", {});
            }
        }
        else if (key == ufi.dynFlags) {
            uint32_t oldDyn = unit->getDynamicFlags();
            unit->setDynamicFlags(val);
            if (block.guid == owner_.playerGuid) {
                bool wasDead = (oldDyn & UNIT_DYNFLAG_DEAD) != 0;
                bool nowDead = (val & UNIT_DYNFLAG_DEAD) != 0;
                if (!wasDead && nowDead) {
                    markPlayerDead("dynFlags");
                } else if (wasDead && !nowDead) {
                    owner_.playerDead_ = false;
                    owner_.releasedSpirit_ = false;
                    owner_.selfResAvailable_ = false;
                    LOG_INFO("Player resurrected (dynamic flags)");
                }
            } else if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                bool wasDead = (oldDyn & UNIT_DYNFLAG_DEAD) != 0;
                bool nowDead = (val & UNIT_DYNFLAG_DEAD) != 0;
                if (!wasDead && nowDead) {
                    if (!result.npcDeathNotified && owner_.npcDeathCallback_) {
                        owner_.npcDeathCallback_(block.guid);
                        result.npcDeathNotified = true;
                    }
                } else if (wasDead && !nowDead) {
                    if (!result.npcRespawnNotified && owner_.npcRespawnCallback_) {
                        owner_.npcRespawnCallback_(block.guid);
                        result.npcRespawnNotified = true;
                    }
                }
            }
        } else if (key == ufi.level) {
            uint32_t oldLvl = unit->getLevel();
            unit->setLevel(val);
            if (val != oldLvl) {
                auto uid = owner_.guidToUnitId(block.guid);
                if (!uid.empty())
                    pendingEvents_.emit("UNIT_LEVEL", {uid});
            }
            if (block.guid != owner_.playerGuid &&
                entity->getType() == ObjectType::PLAYER &&
                val > oldLvl && oldLvl > 0 &&
                owner_.otherPlayerLevelUpCallback_) {
                owner_.otherPlayerLevelUpCallback_(block.guid, val);
            }
        }
        else if (key == ufi.faction) {
            unit->setFactionTemplate(val);
            unit->setHostile(owner_.isHostileFaction(val));
        } else if (key == ufi.displayId) {
            if (val != unit->getDisplayId()) {
                unit->setDisplayId(val);
                result.displayIdChanged = true;
            }
        } else if (key == ufi.mountDisplayId) {
            if (block.guid == owner_.playerGuid) {
                detectPlayerMountChange(val, block.fields);
            }
            unit->setMountDisplayId(val);
        } else if (key == ufi.npcFlags) { unit->setNpcFlags(val); }
        // Power/maxpower range checks AFTER all specific fields
        else if (key >= ufi.powerBase && key < ufi.powerBase + 7) {
            unit->setPowerByType(static_cast<uint8_t>(key - ufi.powerBase), val);
            result.powerChanged = true;
        } else if (key >= ufi.maxPowerBase && key < ufi.maxPowerBase + 7) {
            unit->setMaxPowerByType(static_cast<uint8_t>(key - ufi.maxPowerBase), val);
            result.powerChanged = true;
        }
    }

    // Fire UNIT_HEALTH / UNIT_POWER events for Lua addons
    if ((result.healthChanged || result.powerChanged)) {
        auto unitId = owner_.guidToUnitId(block.guid);
        if (!unitId.empty()) {
            if (result.healthChanged) pendingEvents_.emit("UNIT_HEALTH", {unitId});
            if (result.powerChanged) {
                pendingEvents_.emit("UNIT_POWER", {unitId});
                // When player power changes, action bar usability may change
                if (block.guid == owner_.playerGuid) {
                    pendingEvents_.emit("ACTIONBAR_UPDATE_USABLE", {});
                    pendingEvents_.emit("SPELL_UPDATE_USABLE", {});
                }
            }
        }
    }

    return result;
}

// 3d: Apply player stat fields (XP, coinage, combat stats, etc.).
//     Shared between CREATE and VALUES — isCreate controls event firing differences.
bool EntityController::applyPlayerStatFields(const std::map<uint16_t, uint32_t>& fields,
                                               const PlayerFieldIndices& pfi,
                                               bool isCreate) {
    bool slotsChanged = false;
    for (const auto& [key, val] : fields) {
        if (key == pfi.xp) {
            owner_.playerXp_ = val;
            if (!isCreate) {
                LOG_DEBUG("XP updated: ", val);
                pendingEvents_.emit("PLAYER_XP_UPDATE", {std::to_string(val)});
            }
        }
        else if (key == pfi.nextXp) {
            owner_.playerNextLevelXp_ = val;
            if (!isCreate) LOG_DEBUG("Next level XP updated: ", val);
        }
        else if (pfi.restedXp != 0xFFFF && key == pfi.restedXp) {
            owner_.playerRestedXp_ = val;
            if (!isCreate) pendingEvents_.emit("UPDATE_EXHAUSTION", {});
        }
        else if (key == pfi.level) {
            owner_.serverPlayerLevel_ = val;
            if (!isCreate) LOG_DEBUG("Level updated: ", val);
            for (auto& ch : owner_.characters) {
                if (ch.guid == owner_.playerGuid) { ch.level = val; break; }
            }
        }
        else if (key == pfi.coinage) {
            uint64_t oldMoney = owner_.playerMoneyCopper_;
            owner_.playerMoneyCopper_ = val;
            LOG_DEBUG("Money ", isCreate ? "set from update fields: " : "updated via VALUES: ", val, " copper");
            if (val != oldMoney)
                pendingEvents_.emit("PLAYER_MONEY", {});
        }
        else if (pfi.honor != 0xFFFF && key == pfi.honor) {
            owner_.playerHonorPoints_ = val;
            LOG_DEBUG("Honor points ", isCreate ? "from update fields: " : "updated: ", val);
        }
        else if (pfi.arena != 0xFFFF && key == pfi.arena) {
            owner_.playerArenaPoints_ = val;
            LOG_DEBUG("Arena points ", isCreate ? "from update fields: " : "updated: ", val);
        }
        else if (pfi.armor != 0xFFFF && key == pfi.armor) {
            owner_.playerArmorRating_ = static_cast<int32_t>(val);
            if (isCreate) LOG_DEBUG("Armor rating from update fields: ", owner_.playerArmorRating_);
        }
        else if (pfi.armor != 0xFFFF && key > pfi.armor && key <= pfi.armor + 6) {
            owner_.playerResistances_[key - pfi.armor - 1] = static_cast<int32_t>(val);
        }
        else if (pfi.pBytes2 != 0xFFFF && key == pfi.pBytes2) {
            uint8_t bankBagSlots = static_cast<uint8_t>((val >> 16) & 0xFF);
            owner_.inventory.setPurchasedBankBagSlots(bankBagSlots);
            // Byte 3 (bits 24-31): REST_STATE
            // 0 = not resting, 1 = REST_TYPE_IN_TAVERN, 2 = REST_TYPE_IN_CITY
            uint8_t restStateByte = static_cast<uint8_t>((val >> 24) & 0xFF);
            if (isCreate) {
                LOG_WARNING("PLAYER_BYTES_2 (CREATE): raw=0x", std::hex, val, std::dec,
                           " bankBagSlots=", static_cast<int>(bankBagSlots));
                bool wasResting = owner_.isResting_;
                owner_.isResting_ = (restStateByte != 0);
                if (owner_.isResting_ != wasResting) {
                    pendingEvents_.emit("UPDATE_EXHAUSTION", {});
                    pendingEvents_.emit("PLAYER_UPDATE_RESTING", {});
                }
            } else {
                // Byte 0 (bits 0-7): facial hair / piercings
                uint8_t facialHair = static_cast<uint8_t>(val & 0xFF);
                for (auto& ch : owner_.characters) {
                    if (ch.guid == owner_.playerGuid) { ch.facialFeatures = facialHair; break; }
                }
                LOG_DEBUG("PLAYER_BYTES_2 (VALUES): raw=0x", std::hex, val, std::dec,
                           " bankBagSlots=", static_cast<int>(bankBagSlots),
                           " facial=", static_cast<int>(facialHair));
                owner_.isResting_ = (restStateByte != 0);
                if (owner_.appearanceChangedCallback_)
                    owner_.appearanceChangedCallback_();
            }
        }
        else if (pfi.chosenTitle != 0xFFFF && key == pfi.chosenTitle) {
            owner_.chosenTitleBit_ = static_cast<int32_t>(val);
            LOG_DEBUG("PLAYER_CHOSEN_TITLE ", isCreate ? "from update fields: " : "updated: ",
                      owner_.chosenTitleBit_);
        }
        // VALUES-only fields: PLAYER_BYTES (appearance) and PLAYER_FLAGS (ghost state)
        else if (!isCreate && pfi.pBytes != 0xFFFF && key == pfi.pBytes) {
            // PLAYER_BYTES changed (barber shop, polymorph, etc.)
            for (auto& ch : owner_.characters) {
                if (ch.guid == owner_.playerGuid) { ch.appearanceBytes = val; break; }
            }
            if (owner_.appearanceChangedCallback_)
                owner_.appearanceChangedCallback_();
        }
        else if (!isCreate && key == pfi.playerFlags) {
            constexpr uint32_t PLAYER_FLAGS_GHOST = 0x00000010;
            bool wasGhost = owner_.releasedSpirit_;
            bool nowGhost = (val & PLAYER_FLAGS_GHOST) != 0;
            if (!wasGhost && nowGhost) {
                owner_.releasedSpirit_ = true;
                LOG_INFO("Player entered ghost form (PLAYER_FLAGS)");
                if (owner_.ghostStateCallback_) owner_.ghostStateCallback_(true);
            } else if (wasGhost && !nowGhost) {
                owner_.releasedSpirit_ = false;
                owner_.playerDead_ = false;
                owner_.repopPending_ = false;
                owner_.resurrectPending_ = false;
                owner_.selfResAvailable_ = false;
                owner_.corpseMapId_ = 0;  // corpse reclaimed
                owner_.corpseGuid_ = 0;
                owner_.corpseReclaimAvailableMs_ = 0;
                LOG_INFO("Player resurrected (PLAYER_FLAGS ghost cleared)");
                pendingEvents_.emit("PLAYER_ALIVE", {});
                if (owner_.ghostStateCallback_) owner_.ghostStateCallback_(false);
            }
            pendingEvents_.emit("PLAYER_FLAGS_CHANGED", {});
        }
        else if (pfi.meleeAP  != 0xFFFF && key == pfi.meleeAP)  { owner_.playerMeleeAP_  = static_cast<int32_t>(val); }
        else if (pfi.rangedAP != 0xFFFF && key == pfi.rangedAP) { owner_.playerRangedAP_ = static_cast<int32_t>(val); }
        else if (pfi.spDmg1   != 0xFFFF && key >= pfi.spDmg1 && key < pfi.spDmg1 + 7) {
            owner_.playerSpellDmgBonus_[key - pfi.spDmg1] = static_cast<int32_t>(val);
        }
        else if (pfi.healBonus != 0xFFFF && key == pfi.healBonus) { owner_.playerHealBonus_ = static_cast<int32_t>(val); }
        // Percentage stats are stored as IEEE 754 floats packed into uint32 update fields.
        // memcpy reinterprets the bits; clamp to [0..100] to guard against NaN/Inf from
        // corrupted packets reaching the UI (display-only, no gameplay logic depends on these).
        else if (pfi.blockPct != 0xFFFF && key == pfi.blockPct) { std::memcpy(&owner_.playerBlockPct_, &val, 4); owner_.playerBlockPct_ = std::clamp(owner_.playerBlockPct_, 0.0f, 100.0f); }
        else if (pfi.dodgePct != 0xFFFF && key == pfi.dodgePct) { std::memcpy(&owner_.playerDodgePct_, &val, 4); owner_.playerDodgePct_ = std::clamp(owner_.playerDodgePct_, 0.0f, 100.0f); }
        else if (pfi.parryPct != 0xFFFF && key == pfi.parryPct) { std::memcpy(&owner_.playerParryPct_, &val, 4); owner_.playerParryPct_ = std::clamp(owner_.playerParryPct_, 0.0f, 100.0f); }
        else if (pfi.critPct  != 0xFFFF && key == pfi.critPct)  { std::memcpy(&owner_.playerCritPct_,  &val, 4); owner_.playerCritPct_  = std::clamp(owner_.playerCritPct_,  0.0f, 100.0f); }
        else if (pfi.rangedCritPct != 0xFFFF && key == pfi.rangedCritPct) { std::memcpy(&owner_.playerRangedCritPct_, &val, 4); owner_.playerRangedCritPct_ = std::clamp(owner_.playerRangedCritPct_, 0.0f, 100.0f); }
        else if (pfi.sCrit1   != 0xFFFF && key >= pfi.sCrit1 && key < pfi.sCrit1 + 7) {
            std::memcpy(&owner_.playerSpellCritPct_[key - pfi.sCrit1], &val, 4);
        }
        else if (pfi.rating1  != 0xFFFF && key >= pfi.rating1 && key < pfi.rating1 + 25) {
            owner_.playerCombatRatings_[key - pfi.rating1] = static_cast<int32_t>(val);
        }
        else {
            for (int si = 0; si < 5; ++si) {
                if (pfi.stats[si] != 0xFFFF && key == pfi.stats[si]) {
                    owner_.playerStats_[si] = static_cast<int32_t>(val);
                    break;
                }
            }
        }
        // Do not synthesize quest-log entries from raw update-field slots.
        // Slot layouts differ on some classic-family realms and can produce
        // phantom "already accepted" quests that block quest acceptance.
    }
    if (owner_.applyInventoryFields(fields)) slotsChanged = true;
    return slotsChanged;
}

// 3e: Dispatch entity spawn callbacks for units/players.
//     Consolidates player/creature spawn callback invocation from CREATE and VALUES handlers.
//     isDead = unitInitiallyDead (CREATE) or computed isDeadNow && !npcDeathNotified (VALUES).
void EntityController::dispatchEntitySpawn(uint64_t guid, ObjectType objectType,
                                            const std::shared_ptr<Entity>& entity,
                                            const std::shared_ptr<Unit>& unit,
                                            bool isDead) {
    if (objectType == ObjectType::PLAYER && guid == owner_.playerGuid) {
        return;  // Skip local player — spawned separately via spawnPlayerCharacter()
    }
    if (objectType == ObjectType::PLAYER) {
        if (owner_.playerSpawnCallback_) {
            uint8_t race = 0, gender = 0, facial = 0;
            uint32_t appearanceBytes = 0;
            if (extractPlayerAppearance(entity->getFields(), race, gender, appearanceBytes, facial)) {
                owner_.playerSpawnCallback_(guid, unit->getDisplayId(), race, gender,
                                    appearanceBytes, facial,
                                    unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation());
            } else {
                LOG_WARNING("[Spawn] PLAYER guid=0x", std::hex, guid, std::dec,
                          " displayId=", unit->getDisplayId(), " appearance extraction failed — model will not render");
            }
        }
    } else if (owner_.creatureSpawnCallback_) {
        LOG_DEBUG("[Spawn] UNIT guid=0x", std::hex, guid, std::dec,
                  " displayId=", unit->getDisplayId(), " at (",
                  unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
        float unitScale = 1.0f;
        uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
        if (scaleIdx != 0xFFFF) {
            // raw == 0 means the field was never populated (IEEE 754 0.0f is all-zero bits).
            // Keep the default 1.0f rather than setting scale to 0 and making the entity invisible.
            uint32_t raw = entity->getField(scaleIdx);
            if (raw != 0) {
                std::memcpy(&unitScale, &raw, sizeof(float));
                if (unitScale <= 0.01f || unitScale > 100.0f) unitScale = 1.0f;
            }
        }
        owner_.creatureSpawnCallback_(guid, unit->getDisplayId(),
            unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation(), unitScale);
    }
    if (isDead && owner_.npcDeathCallback_) {
        owner_.npcDeathCallback_(guid);
    }
    // Query quest giver status for NPCs with questgiver flag (0x02)
    if (objectType == ObjectType::UNIT && (unit->getNpcFlags() & 0x02) && owner_.socket) {
        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
        qsPkt.writeUInt64(guid);
        owner_.socket->send(qsPkt);
    }
}

// 3g: Track online item/container objects during CREATE.
void EntityController::trackItemOnCreate(const UpdateBlock& block, bool& newItemCreated) {
    auto entryIt = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
    auto stackIt = block.fields.find(fieldIndex(UF::ITEM_FIELD_STACK_COUNT));
    auto durIt   = block.fields.find(fieldIndex(UF::ITEM_FIELD_DURABILITY));
    auto maxDurIt= block.fields.find(fieldIndex(UF::ITEM_FIELD_MAXDURABILITY));
    const uint16_t enchBase = (fieldIndex(UF::ITEM_FIELD_STACK_COUNT) != 0xFFFF)
        ? static_cast<uint16_t>(fieldIndex(UF::ITEM_FIELD_STACK_COUNT) + 8u) : 0xFFFFu;
    auto permEnchIt  = (enchBase != 0xFFFF) ? block.fields.find(enchBase)       : block.fields.end();
    auto tempEnchIt  = (enchBase != 0xFFFF) ? block.fields.find(enchBase + 3u)  : block.fields.end();
    auto sock1EnchIt = (enchBase != 0xFFFF) ? block.fields.find(enchBase + 6u)  : block.fields.end();
    auto sock2EnchIt = (enchBase != 0xFFFF) ? block.fields.find(enchBase + 9u)  : block.fields.end();
    auto sock3EnchIt = (enchBase != 0xFFFF) ? block.fields.find(enchBase + 12u) : block.fields.end();
    if (entryIt != block.fields.end() && entryIt->second != 0) {
        // Preserve existing info when doing partial updates
        GameHandler::OnlineItemInfo info = owner_.onlineItems_.count(block.guid)
            ? owner_.onlineItems_[block.guid] : GameHandler::OnlineItemInfo{};
        info.entry = entryIt->second;
        if (stackIt    != block.fields.end()) info.stackCount            = stackIt->second;
        if (durIt      != block.fields.end()) info.curDurability         = durIt->second;
        if (maxDurIt   != block.fields.end()) info.maxDurability         = maxDurIt->second;
        if (permEnchIt != block.fields.end()) info.permanentEnchantId    = permEnchIt->second;
        if (tempEnchIt != block.fields.end()) info.temporaryEnchantId    = tempEnchIt->second;
        if (sock1EnchIt != block.fields.end()) info.socketEnchantIds[0]  = sock1EnchIt->second;
        if (sock2EnchIt != block.fields.end()) info.socketEnchantIds[1]  = sock2EnchIt->second;
        if (sock3EnchIt != block.fields.end()) info.socketEnchantIds[2]  = sock3EnchIt->second;
        auto [itemIt, isNew] = owner_.onlineItems_.insert_or_assign(block.guid, info);
        if (isNew) newItemCreated = true;
        owner_.queryItemInfo(info.entry, block.guid);
    }
    // Extract container slot GUIDs for bags
    if (block.objectType == ObjectType::CONTAINER) {
        owner_.extractContainerFields(block.guid, block.fields);
    }
}

// 3g: Update item stack count / durability / enchants for existing items during VALUES.
void EntityController::updateItemOnValuesUpdate(const UpdateBlock& block,
                                                  const std::shared_ptr<Entity>& entity) {
    bool inventoryChanged = false;
    const uint16_t itemStackField   = fieldIndex(UF::ITEM_FIELD_STACK_COUNT);
    const uint16_t itemDurField     = fieldIndex(UF::ITEM_FIELD_DURABILITY);
    const uint16_t itemMaxDurField  = fieldIndex(UF::ITEM_FIELD_MAXDURABILITY);
    const uint16_t containerNumSlotsField = fieldIndex(UF::CONTAINER_FIELD_NUM_SLOTS);
    const uint16_t containerSlot1Field = fieldIndex(UF::CONTAINER_FIELD_SLOT_1);
    // ITEM_FIELD_ENCHANTMENT starts 8 fields after ITEM_FIELD_STACK_COUNT (fixed offset
    // across all expansions: +DURATION, +5×SPELL_CHARGES, +FLAGS = +8).
    // Slot 0 = permanent (field +0), slot 1 = temp (+3), slots 2-4 = sockets (+6,+9,+12).
    const uint16_t itemEnchBase      = (itemStackField != 0xFFFF) ? (itemStackField + 8u)  : 0xFFFF;
    const uint16_t itemPermEnchField = itemEnchBase;
    const uint16_t itemTempEnchField = (itemEnchBase != 0xFFFF) ? (itemEnchBase + 3u)  : 0xFFFF;
    const uint16_t itemSock1EnchField= (itemEnchBase != 0xFFFF) ? (itemEnchBase + 6u)  : 0xFFFF;
    const uint16_t itemSock2EnchField= (itemEnchBase != 0xFFFF) ? (itemEnchBase + 9u)  : 0xFFFF;
    const uint16_t itemSock3EnchField= (itemEnchBase != 0xFFFF) ? (itemEnchBase + 12u) : 0xFFFF;

    auto it = owner_.onlineItems_.find(block.guid);
    bool isItemInInventory = (it != owner_.onlineItems_.end());

    for (const auto& [key, val] : block.fields) {
        if (key == itemStackField && isItemInInventory) {
            if (it->second.stackCount != val) {
                it->second.stackCount = val;
                inventoryChanged = true;
            }
        } else if (key == itemDurField && isItemInInventory) {
            if (it->second.curDurability != val) {
                const uint32_t prevDur = it->second.curDurability;
                it->second.curDurability = val;
                inventoryChanged = true;
                // Warn once when durability drops below 20% for an equipped item.
                const uint32_t maxDur = it->second.maxDurability;
                if (maxDur > 0 && val < maxDur / 5u && prevDur >= maxDur / 5u) {
                    // Check if this item is in an equip slot (not bag inventory).
                    bool isEquipped = false;
                    for (uint64_t slotGuid : owner_.equipSlotGuids_) {
                        if (slotGuid == block.guid) { isEquipped = true; break; }
                    }
                    if (isEquipped) {
                        std::string itemName;
                        const auto* info = owner_.getItemInfo(it->second.entry);
                        if (info) itemName = info->name;
                        char buf[128];
                        if (!itemName.empty())
                            std::snprintf(buf, sizeof(buf), "%s is about to break!", itemName.c_str());
                        else
                            std::snprintf(buf, sizeof(buf), "An equipped item is about to break!");
                        owner_.addUIError(buf);
                        owner_.addSystemChatMessage(buf);
                    }
                }
            }
        } else if (key == itemMaxDurField && isItemInInventory) {
            if (it->second.maxDurability != val) {
                it->second.maxDurability = val;
                inventoryChanged = true;
            }
        } else if (isItemInInventory && itemPermEnchField != 0xFFFF && key == itemPermEnchField) {
            if (it->second.permanentEnchantId != val) {
                it->second.permanentEnchantId = val;
                inventoryChanged = true;
            }
        } else if (isItemInInventory && itemTempEnchField != 0xFFFF && key == itemTempEnchField) {
            if (it->second.temporaryEnchantId != val) {
                it->second.temporaryEnchantId = val;
                inventoryChanged = true;
            }
        } else if (isItemInInventory && itemSock1EnchField != 0xFFFF && key == itemSock1EnchField) {
            if (it->second.socketEnchantIds[0] != val) {
                it->second.socketEnchantIds[0] = val;
                inventoryChanged = true;
            }
        } else if (isItemInInventory && itemSock2EnchField != 0xFFFF && key == itemSock2EnchField) {
            if (it->second.socketEnchantIds[1] != val) {
                it->second.socketEnchantIds[1] = val;
                inventoryChanged = true;
            }
        } else if (isItemInInventory && itemSock3EnchField != 0xFFFF && key == itemSock3EnchField) {
            if (it->second.socketEnchantIds[2] != val) {
                it->second.socketEnchantIds[2] = val;
                inventoryChanged = true;
            }
        }
    }
    // Update container slot GUIDs on bag content changes
    if (entity->getType() == ObjectType::CONTAINER) {
        for (const auto& [key, _] : block.fields) {
            if ((containerNumSlotsField != 0xFFFF && key == containerNumSlotsField) ||
                (containerSlot1Field != 0xFFFF && key >= containerSlot1Field && key < containerSlot1Field + 72)) {
                inventoryChanged = true;
                break;
            }
        }
        owner_.extractContainerFields(block.guid, block.fields);
    }
    if (inventoryChanged) {
        owner_.rebuildOnlineInventory();
        pendingEvents_.emit("BAG_UPDATE", {});
        pendingEvents_.emit("UNIT_INVENTORY_CHANGED", {"player"});
    }
}

// ============================================================
// Phase 5: Object-type handler struct definitions
// ============================================================

struct EntityController::UnitTypeHandler : EntityController::IObjectTypeHandler {
    EntityController& ctl_;
    explicit UnitTypeHandler(EntityController& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateUnit(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateUnit(block, entity); }
};

struct EntityController::PlayerTypeHandler : EntityController::IObjectTypeHandler {
    EntityController& ctl_;
    explicit PlayerTypeHandler(EntityController& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreatePlayer(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdatePlayer(block, entity); }
};

struct EntityController::GameObjectTypeHandler : EntityController::IObjectTypeHandler {
    EntityController& ctl_;
    explicit GameObjectTypeHandler(EntityController& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateGameObject(block, entity); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateGameObject(block, entity); }
};

struct EntityController::ItemTypeHandler : EntityController::IObjectTypeHandler {
    EntityController& ctl_;
    explicit ItemTypeHandler(EntityController& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool& newItemCreated) override { ctl_.onCreateItem(block, newItemCreated); }
    void onValuesUpdate(const UpdateBlock& block, std::shared_ptr<Entity>& entity) override { ctl_.onValuesUpdateItem(block, entity); }
};

struct EntityController::CorpseTypeHandler : EntityController::IObjectTypeHandler {
    EntityController& ctl_;
    explicit CorpseTypeHandler(EntityController& c) : ctl_(c) {}
    void onCreate(const UpdateBlock& block, std::shared_ptr<Entity>& entity, bool&) override { ctl_.onCreateCorpse(block); }
};

// ============================================================
// Phase 5: Handler registry infrastructure
// ============================================================

void EntityController::initTypeHandlers() {
    typeHandlers_[static_cast<uint8_t>(ObjectType::UNIT)] = std::make_unique<UnitTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::PLAYER)] = std::make_unique<PlayerTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::GAMEOBJECT)] = std::make_unique<GameObjectTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::ITEM)] = std::make_unique<ItemTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::CONTAINER)] = std::make_unique<ItemTypeHandler>(*this);
    typeHandlers_[static_cast<uint8_t>(ObjectType::CORPSE)] = std::make_unique<CorpseTypeHandler>(*this);
}

EntityController::IObjectTypeHandler* EntityController::getTypeHandler(ObjectType type) const {
    auto it = typeHandlers_.find(static_cast<uint8_t>(type));
    return it != typeHandlers_.end() ? it->second.get() : nullptr;
}

// ============================================================
// Phase 6: Deferred event bus flush
// ============================================================

void EntityController::flushPendingEvents() {
    for (const auto& [name, args] : pendingEvents_.events) {
        owner_.fireAddonEvent(name, args);
    }
    pendingEvents_.clear();
}

// ============================================================
// Phase 5: Type-specific CREATE handlers
// ============================================================

void EntityController::onCreateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    // Name query for creatures
    auto it = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
    if (it != block.fields.end() && it->second != 0) {
        auto unit = std::static_pointer_cast<Unit>(entity);
        unit->setEntry(it->second);
        std::string cached = getCachedCreatureName(it->second);
        if (!cached.empty()) {
            unit->setName(cached);
        }
        queryCreatureInfo(it->second, block.guid);
    }

    // Unit fields
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    bool unitInitiallyDead = applyUnitFieldsOnCreate(block, unit, ufi);

    // Hostility
    unit->setHostile(owner_.isHostileFaction(unit->getFactionTemplate()));

    // Spawn dispatch
    if (unit->getDisplayId() == 0) {
        LOG_WARNING("[Spawn] UNIT guid=0x", std::hex, block.guid, std::dec,
                  " has displayId=0 — no spawn (entry=", unit->getEntry(),
                  " at ", unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
    }
    if (unit->getDisplayId() != 0) {
        dispatchEntitySpawn(block.guid, block.objectType, entity, unit, unitInitiallyDead);
        if (block.hasMovement && block.moveFlags != 0 && owner_.unitMoveFlagsCallback_ &&
            block.guid != owner_.playerGuid) {
            owner_.unitMoveFlagsCallback_(block.guid, block.moveFlags);
        }
    }
}

void EntityController::onCreatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    static const bool kVerboseUpdateObject = envFlagEnabled("WOWEE_LOG_UPDATE_OBJECT_VERBOSE", false);

    // For the local player, capture the full initial field state
    if (block.guid == owner_.playerGuid) {
        owner_.lastPlayerFields_ = entity->getFields();
        owner_.maybeDetectVisibleItemLayout();
    }

    // Name query + visible items
    queryPlayerName(block.guid);
    if (block.guid != owner_.playerGuid) {
        owner_.updateOtherPlayerVisibleItems(block.guid, entity->getFields());
    }

    // Unit fields (PLAYER is a unit)
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    bool unitInitiallyDead = applyUnitFieldsOnCreate(block, unit, ufi);

    // Self-player post-unit-field handling
    if (block.guid == owner_.playerGuid) {
        constexpr uint32_t UNIT_FLAG_TAXI_FLIGHT = 0x00000100;
        if ((unit->getUnitFlags() & UNIT_FLAG_TAXI_FLIGHT) != 0 && !owner_.onTaxiFlight_ && owner_.taxiLandingCooldown_ <= 0.0f) {
            owner_.onTaxiFlight_ = true;
            owner_.taxiStartGrace_ = std::max(owner_.taxiStartGrace_, 2.0f);
            owner_.sanitizeMovementForTaxi();
            if (owner_.movementHandler_) owner_.movementHandler_->applyTaxiMountForCurrentNode();
        }
    }
    if (block.guid == owner_.playerGuid &&
        (unit->getDynamicFlags() & 0x0008 /*UNIT_DYNFLAG_DEAD*/) != 0) {
        owner_.playerDead_ = true;
        LOG_INFO("Player logged in dead (dynamic flags)");
    }
    // Detect ghost state on login via PLAYER_FLAGS
    if (block.guid == owner_.playerGuid) {
        constexpr uint32_t PLAYER_FLAGS_GHOST = 0x00000010;
        auto pfIt = block.fields.find(fieldIndex(UF::PLAYER_FLAGS));
        if (pfIt != block.fields.end() && (pfIt->second & PLAYER_FLAGS_GHOST) != 0) {
            owner_.releasedSpirit_ = true;
            owner_.playerDead_ = true;
            LOG_INFO("Player logged in as ghost (PLAYER_FLAGS)");
            if (owner_.ghostStateCallback_) owner_.ghostStateCallback_(true);
            // Query corpse position so minimap marker is accurate on reconnect
            if (owner_.socket) {
                network::Packet cq(wireOpcode(Opcode::MSG_CORPSE_QUERY));
                owner_.socket->send(cq);
            }
        }
    }
    // 3f: Classic aura sync on initial object create
    if (block.guid == owner_.playerGuid) {
        syncClassicAurasFromFields(entity);
    }

    // Hostility
    unit->setHostile(owner_.isHostileFaction(unit->getFactionTemplate()));

    // Spawn dispatch
    if (unit->getDisplayId() != 0) {
        dispatchEntitySpawn(block.guid, block.objectType, entity, unit, unitInitiallyDead);
        if (block.hasMovement && block.moveFlags != 0 && owner_.unitMoveFlagsCallback_ &&
            block.guid != owner_.playerGuid) {
            owner_.unitMoveFlagsCallback_(block.guid, block.moveFlags);
        }
    }

    // 3d: Player stat fields (self only)
    if (block.guid == owner_.playerGuid) {
        // Auto-detect coinage index using the previous snapshot vs this full snapshot.
        maybeDetectCoinageIndex(owner_.lastPlayerFields_, block.fields);

        owner_.lastPlayerFields_ = block.fields;
        owner_.detectInventorySlotBases(block.fields);

        if (kVerboseUpdateObject) {
            uint16_t maxField = 0;
            for (const auto& [key, _val] : block.fields) {
                if (key > maxField) maxField = key;
            }
            LOG_INFO("Player update with ", block.fields.size(),
                     " fields (max index=", maxField, ")");
        }

        PlayerFieldIndices pfi = PlayerFieldIndices::resolve();
        bool slotsChanged = applyPlayerStatFields(block.fields, pfi, true);
        if (slotsChanged) owner_.rebuildOnlineInventory();
        owner_.maybeDetectVisibleItemLayout();
        owner_.extractSkillFields(owner_.lastPlayerFields_);
        owner_.extractExploredZoneFields(owner_.lastPlayerFields_);
        owner_.applyQuestStateFromFields(owner_.lastPlayerFields_);
    }
}

void EntityController::onCreateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    auto go = std::static_pointer_cast<GameObject>(entity);
    auto itDisp = block.fields.find(fieldIndex(UF::GAMEOBJECT_DISPLAYID));
    if (itDisp != block.fields.end()) {
        go->setDisplayId(itDisp->second);
    }
    auto itEntry = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
    if (itEntry != block.fields.end() && itEntry->second != 0) {
        go->setEntry(itEntry->second);
        auto cacheIt = gameObjectInfoCache_.find(itEntry->second);
        if (cacheIt != gameObjectInfoCache_.end()) {
            go->setName(cacheIt->second.name);
        }
        queryGameObjectInfo(itEntry->second, block.guid);
    }
    // Detect transport GameObjects via UPDATEFLAG_TRANSPORT (0x0002)
    LOG_DEBUG("GameObject CREATE: guid=0x", std::hex, block.guid, std::dec,
             " entry=", go->getEntry(), " displayId=", go->getDisplayId(),
             " updateFlags=0x", std::hex, block.updateFlags, std::dec,
             " pos=(", go->getX(), ", ", go->getY(), ", ", go->getZ(), ")");
    if (block.updateFlags & 0x0002) {
        transportGuids_.insert(block.guid);
        LOG_INFO("Detected transport GameObject: 0x", std::hex, block.guid, std::dec,
                 " entry=", go->getEntry(),
                 " displayId=", go->getDisplayId(),
                 " pos=(", go->getX(), ", ", go->getY(), ", ", go->getZ(), ")");
        // Note: TransportSpawnCallback will be invoked from Application after WMO instance is created
    }
    if (go->getDisplayId() != 0 && owner_.gameObjectSpawnCallback_) {
        float goScale = 1.0f;
        {
            uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
            if (scaleIdx != 0xFFFF) {
                uint32_t raw = entity->getField(scaleIdx);
                if (raw != 0) {
                    std::memcpy(&goScale, &raw, sizeof(float));
                    if (goScale <= 0.01f || goScale > 100.0f) goScale = 1.0f;
                }
            }
        }
        owner_.gameObjectSpawnCallback_(block.guid, go->getEntry(), go->getDisplayId(),
            go->getX(), go->getY(), go->getZ(), go->getOrientation(), goScale);
    }
    // Fire transport move callback for transports (position update on re-creation)
    if (transportGuids_.count(block.guid) && owner_.transportMoveCallback_) {
        serverUpdatedTransportGuids_.insert(block.guid);
        owner_.transportMoveCallback_(block.guid,
            go->getX(), go->getY(), go->getZ(), go->getOrientation());
    }
}

void EntityController::onCreateItem(const UpdateBlock& block, bool& newItemCreated) {
    trackItemOnCreate(block, newItemCreated);
}

void EntityController::onCreateCorpse(const UpdateBlock& block) {
    // Detect player's own corpse object so we have the position even when
    // SMSG_DEATH_RELEASE_LOC hasn't been received (e.g. login as ghost).
    if (block.hasMovement) {
        // CORPSE_FIELD_OWNER is at index 6 (uint64, low word at 6, high at 7)
        uint16_t ownerLowIdx = 6;
        auto ownerLowIt = block.fields.find(ownerLowIdx);
        uint32_t ownerLow = (ownerLowIt != block.fields.end()) ? ownerLowIt->second : 0;
        auto ownerHighIt = block.fields.find(ownerLowIdx + 1);
        uint32_t ownerHigh = (ownerHighIt != block.fields.end()) ? ownerHighIt->second : 0;
        uint64_t ownerGuid = (static_cast<uint64_t>(ownerHigh) << 32) | ownerLow;
        if (ownerGuid == owner_.playerGuid || ownerLow == static_cast<uint32_t>(owner_.playerGuid)) {
            // Server coords from movement block
            owner_.corpseGuid_  = block.guid;
            owner_.corpseX_     = block.x;
            owner_.corpseY_     = block.y;
            owner_.corpseZ_     = block.z;
            owner_.corpseMapId_ = owner_.currentMapId_;
            LOG_INFO("Corpse object detected: guid=0x", std::hex, owner_.corpseGuid_, std::dec,
                     " server=(", block.x, ", ", block.y, ", ", block.z,
                     ") map=", owner_.corpseMapId_);
        }
    }
}

// ============================================================
// Phase 5: Type-specific VALUES UPDATE handlers
// ============================================================

void EntityController::handleDisplayIdChange(const UpdateBlock& block,
                                              const std::shared_ptr<Entity>& entity,
                                              const std::shared_ptr<Unit>& unit,
                                              const UnitFieldUpdateResult& result) {
    if (!result.displayIdChanged || unit->getDisplayId() == 0 ||
        unit->getDisplayId() == result.oldDisplayId)
        return;

    constexpr uint32_t UNIT_DYNFLAG_DEAD = 0x0008;
    constexpr uint32_t UNIT_DYNFLAG_LOOTABLE = 0x0001;
    bool isDeadNow = (unit->getHealth() == 0) ||
        ((unit->getDynamicFlags() & (UNIT_DYNFLAG_DEAD | UNIT_DYNFLAG_LOOTABLE)) != 0);
    dispatchEntitySpawn(block.guid, entity->getType(), entity, unit,
                        isDeadNow && !result.npcDeathNotified);
    if (owner_.addonEventCallback_) {
        std::string uid;
        if (block.guid == owner_.targetGuid) uid = "target";
        else if (block.guid == owner_.focusGuid) uid = "focus";
        else if (block.guid == owner_.petGuid_) uid = "pet";
        if (!uid.empty())
            pendingEvents_.emit("UNIT_MODEL_CHANGED", {uid});
    }
}

void EntityController::onValuesUpdateUnit(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    UnitFieldUpdateResult result = applyUnitFieldsOnUpdate(block, entity, unit, ufi);
    handleDisplayIdChange(block, entity, unit, result);
}

void EntityController::onValuesUpdatePlayer(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    // Other player visible items
    if (block.guid != owner_.playerGuid) {
        owner_.updateOtherPlayerVisibleItems(block.guid, entity->getFields());
    }

    // Unit field update (player IS a unit)
    auto unit = std::static_pointer_cast<Unit>(entity);
    UnitFieldIndices ufi = UnitFieldIndices::resolve();
    UnitFieldUpdateResult result = applyUnitFieldsOnUpdate(block, entity, unit, ufi);

    // 3f: Classic aura sync from UNIT_FIELD_AURAS when those fields are updated
    if (block.guid == owner_.playerGuid) {
        syncClassicAurasFromFields(entity);
    }

    // 3e: Display ID changed — re-spawn/model-change
    handleDisplayIdChange(block, entity, unit, result);

    // 3d: Self-player stat/inventory/quest field updates
    if (block.guid == owner_.playerGuid) {
        const bool needCoinageDetectSnapshot =
            (owner_.pendingMoneyDelta_ != 0 && owner_.pendingMoneyDeltaTimer_ > 0.0f);
        std::map<uint16_t, uint32_t> oldFieldsSnapshot;
        if (needCoinageDetectSnapshot) {
            oldFieldsSnapshot = owner_.lastPlayerFields_;
        }
        if (block.hasMovement && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
            owner_.serverRunSpeed_ = block.runSpeed;
            // Some server dismount paths update run speed without updating mount display field.
            if (!owner_.onTaxiFlight_ && !owner_.taxiMountActive_ &&
                owner_.currentMountDisplayId_ != 0 && block.runSpeed <= 8.5f) {
                LOG_INFO("Auto-clearing mount from movement speed update: speed=", block.runSpeed,
                         " displayId=", owner_.currentMountDisplayId_);
                owner_.currentMountDisplayId_ = 0;
                if (owner_.mountCallback_) {
                    owner_.mountCallback_(0);
                }
            }
        }
        auto mergeHint = owner_.lastPlayerFields_.end();
        for (const auto& [key, val] : block.fields) {
            mergeHint = owner_.lastPlayerFields_.insert_or_assign(mergeHint, key, val);
        }
        if (needCoinageDetectSnapshot) {
            maybeDetectCoinageIndex(oldFieldsSnapshot, owner_.lastPlayerFields_);
        }
        owner_.maybeDetectVisibleItemLayout();
        owner_.detectInventorySlotBases(block.fields);

        PlayerFieldIndices pfi = PlayerFieldIndices::resolve();
        bool slotsChanged = applyPlayerStatFields(block.fields, pfi, false);
        if (slotsChanged) {
            owner_.rebuildOnlineInventory();
            pendingEvents_.emit("PLAYER_EQUIPMENT_CHANGED", {});
        }
        owner_.extractSkillFields(owner_.lastPlayerFields_);
        owner_.extractExploredZoneFields(owner_.lastPlayerFields_);
        owner_.applyQuestStateFromFields(owner_.lastPlayerFields_);
    }
}

void EntityController::onValuesUpdateItem(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    updateItemOnValuesUpdate(block, entity);
}

void EntityController::onValuesUpdateGameObject(const UpdateBlock& block, std::shared_ptr<Entity>& entity) {
    if (block.hasMovement) {
        if (transportGuids_.count(block.guid) && owner_.transportMoveCallback_) {
            serverUpdatedTransportGuids_.insert(block.guid);
            owner_.transportMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                   entity->getZ(), entity->getOrientation());
        } else if (owner_.gameObjectMoveCallback_) {
            owner_.gameObjectMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                    entity->getZ(), entity->getOrientation());
        }
    }
}

// ============================================================
// Phase 2: Update type handlers (refactored with Phase 5 dispatch)
// ============================================================

void EntityController::handleCreateObject(const UpdateBlock& block, bool& newItemCreated) {
    pendingEvents_.clear();

    // 3a: Create entity from block type
    std::shared_ptr<Entity> entity = createEntityFromBlock(block);

    // Set position from movement block (server → canonical)
    if (block.hasMovement) {
        glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
        float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
        entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
        LOG_DEBUG("  Position: (", pos.x, ", ", pos.y, ", ", pos.z, ")");
        if (block.guid == owner_.playerGuid && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
            owner_.serverRunSpeed_ = block.runSpeed;
        }
        // 3b: Track player-on-transport state
        if (block.guid == owner_.playerGuid) {
            applyPlayerTransportState(block, entity, pos, oCanonical, false);
        }
        // 3i: Track transport-relative children so they follow parent transport motion.
        updateNonPlayerTransportAttachment(block, entity, block.objectType);
    }

    // Set fields
    for (const auto& field : block.fields) {
        entity->setField(field.first, field.second);
    }

    // Add to manager
    entityManager.addEntity(block.guid, entity);

    // Phase 5: Dispatch to type-specific handler
    auto* handler = getTypeHandler(block.objectType);
    if (handler) handler->onCreate(block, entity, newItemCreated);

    flushPendingEvents();
}

void EntityController::handleValuesUpdate(const UpdateBlock& block) {
    auto entity = entityManager.getEntity(block.guid);
    if (!entity) return;
    pendingEvents_.clear();

    // Position update (common)
    if (block.hasMovement) {
        glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
        float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
        entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
        updateNonPlayerTransportAttachment(block, entity, entity->getType());
    }

    // Set fields (common)
    for (const auto& field : block.fields) {
        entity->setField(field.first, field.second);
    }

    // Phase 5: Dispatch to type-specific handler
    auto* handler = getTypeHandler(entity->getType());
    if (handler) handler->onValuesUpdate(block, entity);

    flushPendingEvents();
    LOG_DEBUG("Updated entity fields: 0x", std::hex, block.guid, std::dec);
}

void EntityController::handleMovementUpdate(const UpdateBlock& block) {
            // Diagnostic: Log if we receive MOVEMENT blocks for transports
            if (transportGuids_.count(block.guid)) {
                LOG_INFO("MOVEMENT update for transport 0x", std::hex, block.guid, std::dec,
                         " pos=(", block.x, ", ", block.y, ", ", block.z, ")");
            }

            // Update entity position (server → canonical)
            auto entity = entityManager.getEntity(block.guid);
            if (entity) {
                glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
                float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
                entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
                LOG_DEBUG("Updated entity position: 0x", std::hex, block.guid, std::dec);

                updateNonPlayerTransportAttachment(block, entity, entity->getType());

                // 3b: Track player-on-transport state from MOVEMENT updates
                if (block.guid == owner_.playerGuid) {
                    owner_.movementInfo.orientation = oCanonical;
                    applyPlayerTransportState(block, entity, pos, oCanonical, true);
                }

                // Fire transport move callback if this is a known transport
                if (transportGuids_.count(block.guid) && owner_.transportMoveCallback_) {
                    serverUpdatedTransportGuids_.insert(block.guid);
                    owner_.transportMoveCallback_(block.guid, pos.x, pos.y, pos.z, oCanonical);
                }
                // Fire move callback for non-transport gameobjects.
                if (entity->getType() == ObjectType::GAMEOBJECT &&
                    transportGuids_.count(block.guid) == 0 &&
                    owner_.gameObjectMoveCallback_) {
                    owner_.gameObjectMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                            entity->getZ(), entity->getOrientation());
                }
                // Fire move callback for non-player units (creatures).
                // SMSG_MONSTER_MOVE handles smooth interpolated movement, but many
                // servers (especially vanilla/Turtle WoW) communicate NPC positions
                // via MOVEMENT blocks instead. Use duration=0 for an instant snap.
                if (block.guid != owner_.playerGuid &&
                    entity->getType() == ObjectType::UNIT &&
                    transportGuids_.count(block.guid) == 0 &&
                    owner_.creatureMoveCallback_) {
                    owner_.creatureMoveCallback_(block.guid, pos.x, pos.y, pos.z, 0);
                }
            } else {
                LOG_WARNING("MOVEMENT update for unknown entity: 0x", std::hex, block.guid, std::dec);
            }
}

void EntityController::finalizeUpdateObjectBatch(bool newItemCreated) {
    owner_.tabCycleStale = true;
    // Entity count logging disabled

    // Deferred rebuild: if new item objects were created in this packet, rebuild
    // owner_.inventory so that slot GUIDs updated earlier in the same packet can resolve.
    if (newItemCreated) {
        owner_.rebuildOnlineInventory();
    }

    // Late owner_.inventory base detection once items are known
    if (owner_.playerGuid != 0 && owner_.invSlotBase_ < 0 && !owner_.lastPlayerFields_.empty() && !owner_.onlineItems_.empty()) {
        owner_.detectInventorySlotBases(owner_.lastPlayerFields_);
        if (owner_.invSlotBase_ >= 0) {
            if (owner_.applyInventoryFields(owner_.lastPlayerFields_)) {
                owner_.rebuildOnlineInventory();
            }
        }
    }
}

void EntityController::handleCompressedUpdateObject(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_COMPRESSED_UPDATE_OBJECT, packet size: ", packet.getSize());

    // First 4 bytes = decompressed size
    if (packet.getSize() < 4) {
        LOG_WARNING("SMSG_COMPRESSED_UPDATE_OBJECT too small");
        return;
    }

    uint32_t decompressedSize = packet.readUInt32();
    LOG_DEBUG("  Decompressed size: ", decompressedSize);

    // Capital cities and large raids can produce very large update packets.
    // The real WoW client handles up to ~10MB; 5MB covers all practical cases.
    if (decompressedSize == 0 || decompressedSize > 5 * 1024 * 1024) {
        LOG_WARNING("Invalid decompressed size: ", decompressedSize);
        return;
    }

    // Remaining data is zlib compressed
    size_t compressedSize = packet.getRemainingSize();
    const uint8_t* compressedData = packet.getData().data() + packet.getReadPos();

    // Decompress
    std::vector<uint8_t> decompressed(decompressedSize);
    uLongf destLen = decompressedSize;
    int ret = uncompress(decompressed.data(), &destLen, compressedData, compressedSize);

    if (ret != Z_OK) {
        LOG_WARNING("Failed to decompress UPDATE_OBJECT: zlib error ", ret);
        return;
    }

    // Create packet from decompressed data and parse it
    network::Packet decompressedPacket(wireOpcode(Opcode::SMSG_UPDATE_OBJECT), decompressed);
    handleUpdateObject(decompressedPacket);
}
void EntityController::handleDestroyObject(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_DESTROY_OBJECT");

    DestroyObjectData data;
    if (!DestroyObjectParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_DESTROY_OBJECT");
        return;
    }

    // Remove entity
    if (entityManager.hasEntity(data.guid)) {
        if (transportGuids_.count(data.guid) > 0) {
            const bool playerAboardNow = (owner_.playerTransportGuid_ == data.guid);
            const bool stickyAboard = (owner_.playerTransportStickyGuid_ == data.guid && owner_.playerTransportStickyTimer_ > 0.0f);
            const bool movementSaysAboard = (owner_.movementInfo.transportGuid == data.guid);
            if (playerAboardNow || stickyAboard || movementSaysAboard) {
                serverUpdatedTransportGuids_.erase(data.guid);
                LOG_INFO("Preserving in-use transport on destroy: 0x", std::hex, data.guid, std::dec,
                         " now=", playerAboardNow,
                         " sticky=", stickyAboard,
                         " movement=", movementSaysAboard);
                return;
            }
        }
        // Mirror out-of-range handling: invoke render-layer despawn callbacks before entity removal.
        auto entity = entityManager.getEntity(data.guid);
        if (entity) {
            if (entity->getType() == ObjectType::UNIT && owner_.creatureDespawnCallback_) {
                owner_.creatureDespawnCallback_(data.guid);
            } else if (entity->getType() == ObjectType::PLAYER && owner_.playerDespawnCallback_) {
                // Player entities also need renderer cleanup on DESTROY_OBJECT, not just out-of-range.
                owner_.playerDespawnCallback_(data.guid);
                owner_.otherPlayerVisibleItemEntries_.erase(data.guid);
                owner_.otherPlayerVisibleDirty_.erase(data.guid);
                owner_.otherPlayerMoveTimeMs_.erase(data.guid);
                owner_.inspectedPlayerItemEntries_.erase(data.guid);
                owner_.pendingAutoInspect_.erase(data.guid);
                pendingNameQueries.erase(data.guid);
            } else if (entity->getType() == ObjectType::GAMEOBJECT && owner_.gameObjectDespawnCallback_) {
                owner_.gameObjectDespawnCallback_(data.guid);
            }
        }
        if (transportGuids_.count(data.guid) > 0) {
            transportGuids_.erase(data.guid);
            serverUpdatedTransportGuids_.erase(data.guid);
            if (owner_.playerTransportGuid_ == data.guid) {
                owner_.clearPlayerTransport();
            }
        }
        owner_.clearTransportAttachment(data.guid);
        entityManager.removeEntity(data.guid);
        LOG_INFO("Destroyed entity: 0x", std::hex, data.guid, std::dec,
                 " (", (data.isDeath ? "death" : "despawn"), ")");
    } else {
        LOG_DEBUG("Destroy object for unknown entity: 0x", std::hex, data.guid, std::dec);
    }

    // Clean up auto-attack and target if destroyed entity was our target
    if (owner_.combatHandler_ && data.guid == owner_.combatHandler_->getAutoAttackTargetGuid()) {
        owner_.stopAutoAttack();
    }
    if (data.guid == owner_.targetGuid) {
        owner_.targetGuid = 0;
    }
    if (owner_.combatHandler_) owner_.combatHandler_->removeHostileAttacker(data.guid);

    // Remove online item/container tracking
    owner_.containerContents_.erase(data.guid);
    if (owner_.onlineItems_.erase(data.guid)) {
        owner_.rebuildOnlineInventory();
    }

    // Clean up quest giver status
    owner_.npcQuestStatus_.erase(data.guid);

    // Remove combat text entries referencing the destroyed entity so floating
    // damage numbers don't linger after the source/target despawns.
    if (owner_.combatHandler_) owner_.combatHandler_->removeCombatTextForGuid(data.guid);

    // Clean up unit cast owner_.state (cast bar) for the destroyed unit
    if (owner_.spellHandler_) owner_.spellHandler_->unitCastStates_.erase(data.guid);
    // Clean up cached auras
    if (owner_.spellHandler_) owner_.spellHandler_->unitAurasCache_.erase(data.guid);

    owner_.tabCycleStale = true;
}

// Name Queries
// ============================================================

void EntityController::queryPlayerName(uint64_t guid) {
    // If already cached, apply the name to the entity (handles entity recreation after
    // moving out/in range — the entity object is new but the cached name is valid).
    auto cacheIt = playerNameCache.find(guid);
    if (cacheIt != playerNameCache.end()) {
        auto entity = entityManager.getEntity(guid);
        if (entity && entity->getType() == ObjectType::PLAYER) {
            auto player = std::static_pointer_cast<Player>(entity);
            if (player->getName().empty()) {
                player->setName(cacheIt->second);
            }
        }
        return;
    }
    if (pendingNameQueries.count(guid)) return;
    if (!owner_.isInWorld()) {
        LOG_INFO("queryPlayerName: skipped guid=0x", std::hex, guid, std::dec,
                 " owner_.state=", worldStateName(owner_.state), " owner_.socket=", (owner_.socket ? "yes" : "no"));
        return;
    }

    LOG_INFO("queryPlayerName: sending CMSG_NAME_QUERY for guid=0x", std::hex, guid, std::dec);
    pendingNameQueries.insert(guid);
    auto packet = NameQueryPacket::build(guid);
    owner_.socket->send(packet);
}

void EntityController::queryCreatureInfo(uint32_t entry, uint64_t guid) {
    if (creatureInfoCache.count(entry) || pendingCreatureQueries.count(entry)) return;
    if (!owner_.isInWorld()) return;

    pendingCreatureQueries.insert(entry);
    auto packet = CreatureQueryPacket::build(entry, guid);
    owner_.socket->send(packet);
}

void EntityController::queryGameObjectInfo(uint32_t entry, uint64_t guid) {
    if (gameObjectInfoCache_.count(entry) || pendingGameObjectQueries_.count(entry)) return;
    if (!owner_.isInWorld()) return;

    pendingGameObjectQueries_.insert(entry);
    auto packet = GameObjectQueryPacket::build(entry, guid);
    owner_.socket->send(packet);
}

std::string EntityController::getCachedPlayerName(uint64_t guid) const {
    return std::string(lookupName(guid));
}

std::string EntityController::getCachedCreatureName(uint32_t entry) const {
    auto it = creatureInfoCache.find(entry);
    return (it != creatureInfoCache.end()) ? it->second.name : "";
}
void EntityController::handleNameQueryResponse(network::Packet& packet) {
    NameQueryResponseData data;
    if (!owner_.packetParsers_ || !owner_.packetParsers_->parseNameQueryResponse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_NAME_QUERY_RESPONSE (size=", packet.getSize(), ")");
        return;
    }

    pendingNameQueries.erase(data.guid);

    LOG_INFO("Name query response: guid=0x", std::hex, data.guid, std::dec,
             " found=", static_cast<int>(data.found), " name='", data.name, "'",
             " race=", static_cast<int>(data.race), " class=", static_cast<int>(data.classId));

    if (data.isValid()) {
        playerNameCache[data.guid] = data.name;
        // Cache class/race from name query for UnitClass/UnitRace fallback
        if (data.classId != 0 || data.race != 0) {
            playerClassRaceCache_[data.guid] = {data.classId, data.race};
        }
        // Update entity name
        auto entity = entityManager.getEntity(data.guid);
        if (entity && entity->getType() == ObjectType::PLAYER) {
            auto player = std::static_pointer_cast<Player>(entity);
            player->setName(data.name);
        }

        // Backfill chat history entries that arrived before we knew the name.
        if (owner_.chatHandler_) {
            for (auto& msg : owner_.chatHandler_->getChatHistory()) {
                if (msg.senderGuid == data.guid && msg.senderName.empty()) {
                    msg.senderName = data.name;
                }
            }
        }

        // Backfill mail inbox sender names
        for (auto& mail : owner_.mailInbox_) {
            if (mail.messageType == 0 && mail.senderGuid == data.guid) {
                mail.senderName = data.name;
            }
        }

        // Backfill friend list: if this GUID came from a friend list packet,
        // register the name in owner_.friendsCache now that we know it.
        if (owner_.friendGuids_.count(data.guid)) {
            owner_.friendsCache[data.name] = data.guid;
        }

        // Backfill ignore list: SMSG_IGNORE_LIST only contains GUIDs, so
        // ignoreCache (name→guid for UI) is populated here once names resolve.
        if (owner_.ignoreListGuids_.count(data.guid)) {
            owner_.ignoreCache[data.name] = data.guid;
        }

        // Fire UNIT_NAME_UPDATE so nameplate/unit frame addons know the name is available
        if (owner_.addonEventCallback_) {
            std::string unitId;
            if (data.guid == owner_.targetGuid) unitId = "target";
            else if (data.guid == owner_.focusGuid) unitId = "focus";
            else if (data.guid == owner_.playerGuid) unitId = "player";
            if (!unitId.empty())
                owner_.fireAddonEvent("UNIT_NAME_UPDATE", {unitId});
        }
    }
}

void EntityController::handleCreatureQueryResponse(network::Packet& packet) {
    CreatureQueryResponseData data;
    if (!owner_.packetParsers_->parseCreatureQueryResponse(packet, data)) return;

    pendingCreatureQueries.erase(data.entry);

    if (data.isValid()) {
        creatureInfoCache[data.entry] = data;
        // Update all unit entities with this entry
        for (auto& [guid, entity] : entityManager.getEntities()) {
            if (entity->getType() == ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<Unit>(entity);
                if (unit->getEntry() == data.entry) {
                    unit->setName(data.name);
                }
            }
        }
    }
}

// ============================================================
// GameObject Query
// ============================================================

void EntityController::handleGameObjectQueryResponse(network::Packet& packet) {
    GameObjectQueryResponseData data;
    bool ok = owner_.packetParsers_ ? owner_.packetParsers_->parseGameObjectQueryResponse(packet, data)
                             : GameObjectQueryResponseParser::parse(packet, data);
    if (!ok) return;

    pendingGameObjectQueries_.erase(data.entry);

    if (data.isValid()) {
        gameObjectInfoCache_[data.entry] = data;
        // Update all gameobject entities with this entry
        for (auto& [guid, entity] : entityManager.getEntities()) {
            if (entity->getType() == ObjectType::GAMEOBJECT) {
                auto go = std::static_pointer_cast<GameObject>(entity);
                if (go->getEntry() == data.entry) {
                    go->setName(data.name);
                }
            }
        }

        // MO_TRANSPORT (type 15): assign TaxiPathNode path if available
        if (data.type == 15 && data.hasData && data.data[0] != 0 && owner_.transportManager_) {
            uint32_t taxiPathId = data.data[0];
            if (owner_.transportManager_->hasTaxiPath(taxiPathId)) {
                if (owner_.transportManager_->assignTaxiPathToTransport(data.entry, taxiPathId)) {
                    LOG_DEBUG("MO_TRANSPORT entry=", data.entry, " assigned TaxiPathNode path ", taxiPathId);
                }
            } else {
                LOG_DEBUG("MO_TRANSPORT entry=", data.entry, " taxiPathId=", taxiPathId,
                         " not found in TaxiPathNode.dbc");
            }
        }
    }
}

void EntityController::handleGameObjectPageText(network::Packet& packet) {
    if (!packet.hasRemaining(8)) return;
    uint64_t guid = packet.readUInt64();
    auto entity = entityManager.getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return;

    auto go = std::static_pointer_cast<GameObject>(entity);
    uint32_t entry = go->getEntry();
    if (entry == 0) return;

    auto cacheIt = gameObjectInfoCache_.find(entry);
    if (cacheIt == gameObjectInfoCache_.end()) {
        queryGameObjectInfo(entry, guid);
        return;
    }

    const GameObjectQueryResponseData& info = cacheIt->second;
    uint32_t pageId = 0;
    // AzerothCore layout:
    // type 9 (TEXT): data[0]=pageID
    // type 10 (GOOBER): data[7]=pageId
    if (info.type == 9) pageId = info.data[0];
    else if (info.type == 10) pageId = info.data[7];

    if (pageId != 0 && owner_.socket && owner_.state == WorldState::IN_WORLD) {
        owner_.bookPages_.clear();  // start a fresh book for this interaction
        auto req = PageTextQueryPacket::build(pageId, guid);
        owner_.socket->send(req);
        return;
    }

    if (!info.name.empty()) {
        owner_.addSystemChatMessage(info.name);
    }
}

void EntityController::handlePageTextQueryResponse(network::Packet& packet) {
    PageTextQueryResponseData data;
    if (!PageTextQueryResponseParser::parse(packet, data)) return;

    if (!data.isValid()) return;

    // Append page if not already collected
    bool alreadyHave = false;
    for (const auto& bp : owner_.bookPages_) {
        if (bp.pageId == data.pageId) { alreadyHave = true; break; }
    }
    if (!alreadyHave) {
        owner_.bookPages_.push_back({data.pageId, data.text});
    }

    // Follow the chain: if there's a next page we haven't fetched yet, request it
    if (data.nextPageId != 0) {
        bool nextHave = false;
        for (const auto& bp : owner_.bookPages_) {
            if (bp.pageId == data.nextPageId) { nextHave = true; break; }
        }
        if (!nextHave && owner_.socket && owner_.state == WorldState::IN_WORLD) {
            auto req = PageTextQueryPacket::build(data.nextPageId, owner_.playerGuid);
            owner_.socket->send(req);
        }
    }
    LOG_DEBUG("handlePageTextQueryResponse: pageId=", data.pageId,
              " nextPage=", data.nextPageId,
              " totalPages=", owner_.bookPages_.size());
}


} // namespace game
} // namespace wowee
