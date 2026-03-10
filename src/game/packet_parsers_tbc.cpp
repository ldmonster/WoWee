#include "game/packet_parsers.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace game {

// ============================================================================
// TBC 2.4.3 movement flag constants (shifted relative to WotLK 3.3.5a)
// ============================================================================
namespace TbcMoveFlags {
    constexpr uint32_t ON_TRANSPORT     = 0x00000200;  // Gates transport data (same as WotLK)
    constexpr uint32_t JUMPING          = 0x00002000;  // Gates jump data (WotLK: FALLING=0x1000)
    constexpr uint32_t SWIMMING         = 0x00200000;  // Same as WotLK
    constexpr uint32_t FLYING           = 0x01000000;  // WotLK: 0x02000000
    constexpr uint32_t ONTRANSPORT      = 0x02000000;  // Secondary pitch check
    constexpr uint32_t SPLINE_ELEVATION = 0x04000000;  // Same as WotLK
    constexpr uint32_t SPLINE_ENABLED   = 0x08000000;  // Same as WotLK
}

// ============================================================================
// TBC parseMovementBlock
// Key differences from WotLK:
// - UpdateFlags is uint8 (not uint16)
// - No VEHICLE (0x0080), POSITION (0x0100), ROTATION (0x0200) flags
// - moveFlags2 is uint8 (not uint16)
// - No transport seat byte
// - No interpolated movement (flags2 & 0x0200) check
// - Pitch check: SWIMMING, else ONTRANSPORT(0x02000000)
// - Spline data: has splineId, no durationMod/durationModNext/verticalAccel/effectStartTime/splineMode
// - Flag 0x08 (HIGH_GUID) reads 2 u32s (Classic: 1 u32)
// ============================================================================
bool TbcPacketParsers::parseMovementBlock(network::Packet& packet, UpdateBlock& block) {
    // TBC 2.4.3: UpdateFlags is uint8 (1 byte)
    uint8_t updateFlags = packet.readUInt8();
    block.updateFlags = static_cast<uint16_t>(updateFlags);

    LOG_DEBUG("  [TBC] UpdateFlags: 0x", std::hex, (int)updateFlags, std::dec);

    // TBC UpdateFlag bit values (same as lower byte of WotLK):
    // 0x01 = SELF
    // 0x02 = TRANSPORT
    // 0x04 = HAS_TARGET
    // 0x08 = LOWGUID
    // 0x10 = HIGHGUID
    // 0x20 = LIVING
    // 0x40 = HAS_POSITION (stationary)
    const uint8_t UPDATEFLAG_LIVING              = 0x20;
    const uint8_t UPDATEFLAG_HAS_POSITION        = 0x40;
    const uint8_t UPDATEFLAG_HAS_TARGET          = 0x04;
    const uint8_t UPDATEFLAG_TRANSPORT           = 0x02;
    const uint8_t UPDATEFLAG_LOWGUID             = 0x08;
    const uint8_t UPDATEFLAG_HIGHGUID            = 0x10;

    if (updateFlags & UPDATEFLAG_LIVING) {
        // Full movement block for living units
        uint32_t moveFlags = packet.readUInt32();
        uint8_t moveFlags2 = packet.readUInt8();  // TBC: uint8, not uint16
        (void)moveFlags2;
        /*uint32_t time =*/ packet.readUInt32();

        // Position
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [TBC] LIVING: (", block.x, ", ", block.y, ", ", block.z,
                  "), o=", block.orientation, " moveFlags=0x", std::hex, moveFlags, std::dec);

        // Transport data
        if (moveFlags & TbcMoveFlags::ON_TRANSPORT) {
            block.onTransport = true;
            block.transportGuid = UpdateObjectParser::readPackedGuid(packet);
            block.transportX = packet.readFloat();
            block.transportY = packet.readFloat();
            block.transportZ = packet.readFloat();
            block.transportO = packet.readFloat();
            /*uint32_t tTime =*/ packet.readUInt32();
            // TBC: NO transport seat byte
            // TBC: NO interpolated movement check
        }

        // Pitch: SWIMMING, or else ONTRANSPORT (TBC-specific secondary pitch)
        if (moveFlags & TbcMoveFlags::SWIMMING) {
            /*float pitch =*/ packet.readFloat();
        } else if (moveFlags & TbcMoveFlags::ONTRANSPORT) {
            /*float pitch =*/ packet.readFloat();
        }

        // Fall time (always present)
        /*uint32_t fallTime =*/ packet.readUInt32();

        // Jumping (TBC: JUMPING=0x2000, WotLK: FALLING=0x1000)
        if (moveFlags & TbcMoveFlags::JUMPING) {
            /*float jumpVelocity =*/ packet.readFloat();
            /*float jumpSinAngle =*/ packet.readFloat();
            /*float jumpCosAngle =*/ packet.readFloat();
            /*float jumpXYSpeed =*/ packet.readFloat();
        }

        // Spline elevation (TBC: 0x02000000, WotLK: 0x04000000)
        if (moveFlags & TbcMoveFlags::SPLINE_ELEVATION) {
            /*float splineElevation =*/ packet.readFloat();
        }

        // Speeds (TBC: 8 values — walk, run, runBack, swim, fly, flyBack, swimBack, turn)
        // WotLK adds pitchRate (9 total)
        /*float walkSpeed =*/ packet.readFloat();
        float runSpeed = packet.readFloat();
        /*float runBackSpeed =*/ packet.readFloat();
        /*float swimSpeed =*/ packet.readFloat();
        /*float flySpeed =*/ packet.readFloat();
        /*float flyBackSpeed =*/ packet.readFloat();
        /*float swimBackSpeed =*/ packet.readFloat();
        /*float turnRate =*/ packet.readFloat();

        block.runSpeed = runSpeed;
        block.moveFlags = moveFlags;

        // Spline data (TBC/WotLK: SPLINE_ENABLED = 0x08000000)
        if (moveFlags & TbcMoveFlags::SPLINE_ENABLED) {
            uint32_t splineFlags = packet.readUInt32();
            LOG_DEBUG("  [TBC] Spline: flags=0x", std::hex, splineFlags, std::dec);

            if (splineFlags & 0x00010000) { // FINAL_POINT
                /*float finalX =*/ packet.readFloat();
                /*float finalY =*/ packet.readFloat();
                /*float finalZ =*/ packet.readFloat();
            } else if (splineFlags & 0x00020000) { // FINAL_TARGET
                /*uint64_t finalTarget =*/ packet.readUInt64();
            } else if (splineFlags & 0x00040000) { // FINAL_ANGLE
                /*float finalAngle =*/ packet.readFloat();
            }

            // TBC spline: timePassed, duration, id, nodes, finalNode
            // (no durationMod, durationModNext, verticalAccel, effectStartTime, splineMode)
            /*uint32_t timePassed =*/ packet.readUInt32();
            /*uint32_t duration =*/ packet.readUInt32();
            /*uint32_t splineId =*/ packet.readUInt32();

            uint32_t pointCount = packet.readUInt32();
            if (pointCount > 256) {
                static uint32_t badTbcSplineCount = 0;
                ++badTbcSplineCount;
                if (badTbcSplineCount <= 5 || (badTbcSplineCount % 100) == 0) {
                    LOG_WARNING("  [TBC] Spline pointCount=", pointCount,
                                " exceeds max, capping (occurrence=", badTbcSplineCount, ")");
                }
                pointCount = 0;
            }
            for (uint32_t i = 0; i < pointCount; i++) {
                /*float px =*/ packet.readFloat();
                /*float py =*/ packet.readFloat();
                /*float pz =*/ packet.readFloat();
            }

            // TBC: NO splineMode byte (WotLK adds it)
            /*float endPointX =*/ packet.readFloat();
            /*float endPointY =*/ packet.readFloat();
            /*float endPointZ =*/ packet.readFloat();
        }
    }
    else if (updateFlags & UPDATEFLAG_HAS_POSITION) {
        // TBC: Simple stationary position (same as WotLK STATIONARY)
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [TBC] STATIONARY: (", block.x, ", ", block.y, ", ", block.z, ")");
    }
    // TBC: No UPDATEFLAG_POSITION (0x0100) code path

    // Target GUID
    if (updateFlags & UPDATEFLAG_HAS_TARGET) {
        /*uint64_t targetGuid =*/ UpdateObjectParser::readPackedGuid(packet);
    }

    // Transport time
    if (updateFlags & UPDATEFLAG_TRANSPORT) {
        /*uint32_t transportTime =*/ packet.readUInt32();
    }

    // TBC: No VEHICLE flag (WotLK 0x0080)
    // TBC: No ROTATION flag (WotLK 0x0200)

    // HIGH_GUID (0x08) — TBC has 2 u32s, Classic has 1 u32
    if (updateFlags & UPDATEFLAG_LOWGUID) {
        /*uint32_t unknown0 =*/ packet.readUInt32();
        /*uint32_t unknown1 =*/ packet.readUInt32();
    }

    // ALL (0x10)
    if (updateFlags & UPDATEFLAG_HIGHGUID) {
        /*uint32_t unknown2 =*/ packet.readUInt32();
    }

    return true;
}

// ============================================================================
// TBC writeMovementPayload
// Key differences from WotLK:
// - flags2 is uint8 (not uint16)
// - No transport seat byte
// - No interpolated movement (flags2 & 0x0200) write
// - Pitch check uses TBC flag positions
// ============================================================================
void TbcPacketParsers::writeMovementPayload(network::Packet& packet, const MovementInfo& info) {
    // Movement flags (uint32, same as WotLK)
    packet.writeUInt32(info.flags);

    // TBC: flags2 is uint8 (WotLK: uint16)
    packet.writeUInt8(static_cast<uint8_t>(info.flags2 & 0xFF));

    // Timestamp
    packet.writeUInt32(info.time);

    // Position
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.x), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.y), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.z), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.orientation), sizeof(float));

    // Transport data (TBC ON_TRANSPORT = 0x200, same bit as WotLK)
    if (info.flags & TbcMoveFlags::ON_TRANSPORT) {
        // Packed transport GUID
        uint8_t transMask = 0;
        uint8_t transGuidBytes[8];
        int transGuidByteCount = 0;
        for (int i = 0; i < 8; i++) {
            uint8_t byte = static_cast<uint8_t>((info.transportGuid >> (i * 8)) & 0xFF);
            if (byte != 0) {
                transMask |= (1 << i);
                transGuidBytes[transGuidByteCount++] = byte;
            }
        }
        packet.writeUInt8(transMask);
        for (int i = 0; i < transGuidByteCount; i++) {
            packet.writeUInt8(transGuidBytes[i]);
        }

        // Transport local position
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.transportX), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.transportY), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.transportZ), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.transportO), sizeof(float));

        // Transport time
        packet.writeUInt32(info.transportTime);

        // TBC: NO transport seat byte
        // TBC: NO interpolated movement time
    }

    // Pitch: SWIMMING or else ONTRANSPORT (TBC flag positions)
    if (info.flags & TbcMoveFlags::SWIMMING) {
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.pitch), sizeof(float));
    } else if (info.flags & TbcMoveFlags::ONTRANSPORT) {
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.pitch), sizeof(float));
    }

    // Fall time (always present)
    packet.writeUInt32(info.fallTime);

    // Jump data (TBC JUMPING = 0x2000, WotLK FALLING = 0x1000)
    if (info.flags & TbcMoveFlags::JUMPING) {
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpVelocity), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpSinAngle), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpCosAngle), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpXYSpeed), sizeof(float));
    }
}

// ============================================================================
// TBC buildMovementPacket
// Classic/TBC: client movement packets do NOT include PackedGuid prefix
// (WotLK added PackedGuid to client packets)
// ============================================================================
network::Packet TbcPacketParsers::buildMovementPacket(LogicalOpcode opcode,
                                                       const MovementInfo& info,
                                                       uint64_t /*playerGuid*/) {
    network::Packet packet(wireOpcode(opcode));

    // TBC: NO PackedGuid prefix for client packets
    writeMovementPayload(packet, info);

    return packet;
}

// ============================================================================
// TBC parseCharEnum
// Differences from WotLK:
// - After flags: uint8 firstLogin (not uint32 customization + uint8 unknown)
// - Equipment: 20 items (not 23)
// ============================================================================
bool TbcPacketParsers::parseCharEnum(network::Packet& packet, CharEnumResponse& response) {
    uint8_t count = packet.readUInt8();

    LOG_INFO("[TBC] Parsing SMSG_CHAR_ENUM: ", (int)count, " characters");

    response.characters.clear();
    response.characters.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        Character character;

        // GUID (8 bytes)
        character.guid = packet.readUInt64();

        // Name (null-terminated string)
        character.name = packet.readString();

        // Race, class, gender
        character.race = static_cast<Race>(packet.readUInt8());
        character.characterClass = static_cast<Class>(packet.readUInt8());
        character.gender = static_cast<Gender>(packet.readUInt8());

        // Appearance (5 bytes: skin, face, hairStyle, hairColor packed + facialFeatures)
        character.appearanceBytes = packet.readUInt32();
        character.facialFeatures = packet.readUInt8();

        // Level
        character.level = packet.readUInt8();

        // Location
        character.zoneId = packet.readUInt32();
        character.mapId = packet.readUInt32();
        character.x = packet.readFloat();
        character.y = packet.readFloat();
        character.z = packet.readFloat();

        // Guild ID
        character.guildId = packet.readUInt32();

        // Flags
        character.flags = packet.readUInt32();

        // TBC: uint8 firstLogin (WotLK: uint32 customization + uint8 unknown)
        /*uint8_t firstLogin =*/ packet.readUInt8();

        // Pet data (always present)
        character.pet.displayModel = packet.readUInt32();
        character.pet.level = packet.readUInt32();
        character.pet.family = packet.readUInt32();

        // Equipment (TBC: 20 items, WotLK: 23 items)
        character.equipment.reserve(20);
        for (int j = 0; j < 20; ++j) {
            EquipmentItem item;
            item.displayModel = packet.readUInt32();
            item.inventoryType = packet.readUInt8();
            item.enchantment = packet.readUInt32();
            character.equipment.push_back(item);
        }

        LOG_DEBUG("  Character ", (int)(i + 1), ": ", character.name,
                  " (", getRaceName(character.race), " ", getClassName(character.characterClass),
                  " level ", (int)character.level, " zone ", character.zoneId, ")");

        response.characters.push_back(character);
    }

    LOG_INFO("[TBC] Parsed ", response.characters.size(), " characters");
    return true;
}

// ============================================================================
// TBC parseUpdateObject
// Key difference from WotLK: u8 has_transport byte after blockCount
// (WotLK removed this field)
// ============================================================================
bool TbcPacketParsers::parseUpdateObject(network::Packet& packet, UpdateObjectData& data) {
    constexpr uint32_t kMaxReasonableUpdateBlocks = 4096;
    auto parseWithLayout = [&](bool withHasTransportByte, UpdateObjectData& out) -> bool {
        out = UpdateObjectData{};
        size_t start = packet.getReadPos();
        if (packet.getSize() - start < 4) return false;

        out.blockCount = packet.readUInt32();
        if (out.blockCount > kMaxReasonableUpdateBlocks) {
            packet.setReadPos(start);
            return false;
        }

        if (withHasTransportByte) {
            if (packet.getReadPos() >= packet.getSize()) {
                packet.setReadPos(start);
                return false;
            }
            /*uint8_t hasTransport =*/ packet.readUInt8();
        }

        if (packet.getReadPos() + 1 <= packet.getSize()) {
            uint8_t firstByte = packet.readUInt8();
            if (firstByte == static_cast<uint8_t>(UpdateType::OUT_OF_RANGE_OBJECTS)) {
                if (packet.getReadPos() + 4 > packet.getSize()) {
                    packet.setReadPos(start);
                    return false;
                }
                uint32_t count = packet.readUInt32();
                if (count > kMaxReasonableUpdateBlocks) {
                    packet.setReadPos(start);
                    return false;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    if (packet.getReadPos() >= packet.getSize()) {
                        packet.setReadPos(start);
                        return false;
                    }
                    uint64_t guid = UpdateObjectParser::readPackedGuid(packet);
                    out.outOfRangeGuids.push_back(guid);
                }
            } else {
                packet.setReadPos(packet.getReadPos() - 1);
            }
        }

        out.blocks.reserve(out.blockCount);
        for (uint32_t i = 0; i < out.blockCount; ++i) {
            if (packet.getReadPos() >= packet.getSize()) {
                packet.setReadPos(start);
                return false;
            }

            UpdateBlock block;
            uint8_t updateTypeVal = packet.readUInt8();
            if (updateTypeVal > static_cast<uint8_t>(UpdateType::NEAR_OBJECTS)) {
                packet.setReadPos(start);
                return false;
            }
            block.updateType = static_cast<UpdateType>(updateTypeVal);

            bool ok = false;
            switch (block.updateType) {
                case UpdateType::VALUES: {
                    block.guid = UpdateObjectParser::readPackedGuid(packet);
                    ok = UpdateObjectParser::parseUpdateFields(packet, block);
                    break;
                }
                case UpdateType::MOVEMENT: {
                    block.guid = UpdateObjectParser::readPackedGuid(packet);
                    ok = this->parseMovementBlock(packet, block);
                    break;
                }
                case UpdateType::CREATE_OBJECT:
                case UpdateType::CREATE_OBJECT2: {
                    block.guid = UpdateObjectParser::readPackedGuid(packet);
                    if (packet.getReadPos() >= packet.getSize()) {
                        ok = false;
                        break;
                    }
                    uint8_t objectTypeVal = packet.readUInt8();
                    block.objectType = static_cast<ObjectType>(objectTypeVal);
                    ok = this->parseMovementBlock(packet, block);
                    if (ok) ok = UpdateObjectParser::parseUpdateFields(packet, block);
                    break;
                }
                case UpdateType::OUT_OF_RANGE_OBJECTS:
                case UpdateType::NEAR_OBJECTS:
                    ok = true;
                    break;
                default:
                    ok = false;
                    break;
            }

            if (!ok) {
                packet.setReadPos(start);
                return false;
            }
            out.blocks.push_back(block);
        }
        return true;
    };

    size_t startPos = packet.getReadPos();
    UpdateObjectData parsed;
    if (parseWithLayout(true, parsed)) {
        data = std::move(parsed);
        return true;
    }

    packet.setReadPos(startPos);
    if (parseWithLayout(false, parsed)) {
        LOG_DEBUG("[TBC] SMSG_UPDATE_OBJECT parsed without has_transport byte fallback");
        data = std::move(parsed);
        return true;
    }

    packet.setReadPos(startPos);
    return false;
}

// ============================================================================
// TBC 2.4.3 SMSG_GOSSIP_MESSAGE
// Identical to WotLK except each quest entry lacks questFlags(u32) and
// isRepeatable(u8) that WotLK added. Without this override the WotLK parser
// reads those 5 bytes as part of the quest title, corrupting all gossip quests.
// ============================================================================
bool TbcPacketParsers::parseGossipMessage(network::Packet& packet, GossipMessageData& data) {
    if (packet.getSize() - packet.getReadPos() < 16) return false;

    data.npcGuid = packet.readUInt64();
    data.menuId = packet.readUInt32();      // TBC added menuId (Classic doesn't have it)
    data.titleTextId = packet.readUInt32();
    uint32_t optionCount = packet.readUInt32();

    data.options.clear();
    data.options.reserve(optionCount);
    for (uint32_t i = 0; i < optionCount; ++i) {
        GossipOption opt;
        opt.id = packet.readUInt32();
        opt.icon = packet.readUInt8();
        opt.isCoded = (packet.readUInt8() != 0);
        opt.boxMoney = packet.readUInt32();
        opt.text = packet.readString();
        opt.boxText = packet.readString();
        data.options.push_back(opt);
    }

    uint32_t questCount = packet.readUInt32();
    data.quests.clear();
    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        GossipQuestItem quest;
        quest.questId = packet.readUInt32();
        quest.questIcon = packet.readUInt32();
        quest.questLevel = static_cast<int32_t>(packet.readUInt32());
        // TBC 2.4.3: NO questFlags(u32) and NO isRepeatable(u8) here
        // WotLK adds these 5 bytes — reading them from TBC garbles the quest title
        quest.questFlags = 0;
        quest.isRepeatable = 0;
        quest.title = normalizeWowTextTokens(packet.readString());
        data.quests.push_back(quest);
    }

    LOG_DEBUG("[TBC] Gossip: ", optionCount, " options, ", questCount, " quests");
    return true;
}

// ============================================================================
// TBC 2.4.3 SMSG_MONSTER_MOVE
// Identical to WotLK except WotLK added a uint8 unk byte immediately after the
// packed GUID (toggles MOVEMENTFLAG2_UNK7). TBC does NOT have this byte.
// Without this override, all NPC movement positions/durations are offset by 1
// byte and parse as garbage.
// ============================================================================
bool TbcPacketParsers::parseMonsterMove(network::Packet& packet, MonsterMoveData& data) {
    data.guid = UpdateObjectParser::readPackedGuid(packet);
    if (data.guid == 0) return false;
    // No unk byte here in TBC 2.4.3

    if (packet.getReadPos() + 12 > packet.getSize()) return false;
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();

    if (packet.getReadPos() + 4 > packet.getSize()) return false;
    packet.readUInt32(); // splineId

    if (packet.getReadPos() >= packet.getSize()) return false;
    data.moveType = packet.readUInt8();

    if (data.moveType == 1) {
        data.destX = data.x;
        data.destY = data.y;
        data.destZ = data.z;
        data.hasDest = false;
        return true;
    }

    if (data.moveType == 2) {
        if (packet.getReadPos() + 12 > packet.getSize()) return false;
        packet.readFloat(); packet.readFloat(); packet.readFloat();
    } else if (data.moveType == 3) {
        if (packet.getReadPos() + 8 > packet.getSize()) return false;
        data.facingTarget = packet.readUInt64();
    } else if (data.moveType == 4) {
        if (packet.getReadPos() + 4 > packet.getSize()) return false;
        data.facingAngle = packet.readFloat();
    }

    if (packet.getReadPos() + 4 > packet.getSize()) return false;
    data.splineFlags = packet.readUInt32();

    // TBC 2.4.3 SplineFlags animation bit is same as WotLK: 0x00400000
    if (data.splineFlags & 0x00400000) {
        if (packet.getReadPos() + 5 > packet.getSize()) return false;
        packet.readUInt8();  // animationType
        packet.readUInt32(); // effectStartTime
    }

    if (packet.getReadPos() + 4 > packet.getSize()) return false;
    data.duration = packet.readUInt32();

    if (data.splineFlags & 0x00000800) {
        if (packet.getReadPos() + 8 > packet.getSize()) return false;
        packet.readFloat();  // verticalAcceleration
        packet.readUInt32(); // effectStartTime
    }

    if (packet.getReadPos() + 4 > packet.getSize()) return false;
    uint32_t pointCount = packet.readUInt32();
    if (pointCount == 0) return true;
    if (pointCount > 16384) return false;

    bool uncompressed = (data.splineFlags & (0x00080000 | 0x00002000)) != 0;
    if (uncompressed) {
        for (uint32_t i = 0; i < pointCount - 1; i++) {
            if (packet.getReadPos() + 12 > packet.getSize()) return true;
            packet.readFloat(); packet.readFloat(); packet.readFloat();
        }
        if (packet.getReadPos() + 12 > packet.getSize()) return true;
        data.destX = packet.readFloat();
        data.destY = packet.readFloat();
        data.destZ = packet.readFloat();
        data.hasDest = true;
    } else {
        if (packet.getReadPos() + 12 > packet.getSize()) return true;
        data.destX = packet.readFloat();
        data.destY = packet.readFloat();
        data.destZ = packet.readFloat();
        data.hasDest = true;
    }

    LOG_DEBUG("[TBC] MonsterMove: guid=0x", std::hex, data.guid, std::dec,
              " type=", (int)data.moveType, " dur=", data.duration, "ms",
              " dest=(", data.destX, ",", data.destY, ",", data.destZ, ")");
    return true;
}

// ============================================================================
// TBC 2.4.3 CMSG_CAST_SPELL
// Format: castCount(u8) + spellId(u32) + SpellCastTargets
// WotLK 3.3.5a adds castFlags(u8) between spellId and targets — TBC does NOT.
// ============================================================================
network::Packet TbcPacketParsers::buildCastSpell(uint32_t spellId, uint64_t targetGuid, uint8_t castCount) {
    network::Packet packet(wireOpcode(LogicalOpcode::CMSG_CAST_SPELL));
    packet.writeUInt8(castCount);
    packet.writeUInt32(spellId);
    // No castFlags byte in TBC 2.4.3

    if (targetGuid != 0) {
        packet.writeUInt32(0x02); // TARGET_FLAG_UNIT
        // Write packed GUID
        uint8_t mask = 0;
        uint8_t bytes[8];
        int byteCount = 0;
        uint64_t g = targetGuid;
        for (int i = 0; i < 8; ++i) {
            uint8_t b = g & 0xFF;
            if (b != 0) {
                mask |= (1 << i);
                bytes[byteCount++] = b;
            }
            g >>= 8;
        }
        packet.writeUInt8(mask);
        for (int i = 0; i < byteCount; ++i)
            packet.writeUInt8(bytes[i]);
    } else {
        packet.writeUInt32(0x00); // TARGET_FLAG_SELF
    }

    return packet;
}

// ============================================================================
// TBC 2.4.3 CMSG_USE_ITEM
// Format: bag(u8) + slot(u8) + castCount(u8) + spellId(u32) + itemGuid(u64) +
//         castFlags(u8) + SpellCastTargets
// WotLK 3.3.5a adds glyphIndex(u32) between itemGuid and castFlags — TBC does NOT.
// ============================================================================
network::Packet TbcPacketParsers::buildUseItem(uint8_t bagIndex, uint8_t slotIndex, uint64_t itemGuid, uint32_t spellId) {
    network::Packet packet(wireOpcode(LogicalOpcode::CMSG_USE_ITEM));
    packet.writeUInt8(bagIndex);
    packet.writeUInt8(slotIndex);
    packet.writeUInt8(0);          // cast count
    packet.writeUInt32(spellId);   // on-use spell id
    packet.writeUInt64(itemGuid);  // full 8-byte GUID
    // No glyph index field in TBC 2.4.3
    packet.writeUInt8(0);          // cast flags
    packet.writeUInt32(0x00);      // SpellCastTargets: TARGET_FLAG_SELF
    return packet;
}

network::Packet TbcPacketParsers::buildAcceptQuestPacket(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    // TBC servers generally expect guid + questId only.
    return packet;
}

// ============================================================================
// TBC 2.4.3 SMSG_QUESTGIVER_QUEST_DETAILS
//
// TBC and Classic share the same format — neither has the WotLK-specific fields
// (informUnit GUID, flags uint32, isFinished uint8) that were added in 3.x.
//
// Format:
//   npcGuid(8) + questId(4) + title + details + objectives
//   + activateAccept(1) + suggestedPlayers(4)
//   + emoteCount(4) + [delay(4)+type(4)] × emoteCount
//   + choiceCount(4) + [itemId(4)+count(4)+displayInfo(4)] × choiceCount
//   + rewardCount(4) + [itemId(4)+count(4)+displayInfo(4)] × rewardCount
//   + rewardMoney(4) + rewardXp(4)
// ============================================================================
bool TbcPacketParsers::parseQuestDetails(network::Packet& packet, QuestDetailsData& data) {
    if (packet.getSize() < 16) return false;

    data.npcGuid = packet.readUInt64();
    data.questId = packet.readUInt32();
    data.title      = normalizeWowTextTokens(packet.readString());
    data.details    = normalizeWowTextTokens(packet.readString());
    data.objectives = normalizeWowTextTokens(packet.readString());

    if (packet.getReadPos() + 5 > packet.getSize()) {
        LOG_DEBUG("Quest details tbc/classic (short): id=", data.questId, " title='", data.title, "'");
        return !data.title.empty() || data.questId != 0;
    }

    /*activateAccept*/ packet.readUInt8();
    data.suggestedPlayers = packet.readUInt32();

    // TBC/Classic: emote section before reward items
    if (packet.getReadPos() + 4 <= packet.getSize()) {
        uint32_t emoteCount = packet.readUInt32();
        for (uint32_t i = 0; i < emoteCount && packet.getReadPos() + 8 <= packet.getSize(); ++i) {
            packet.readUInt32(); // delay
            packet.readUInt32(); // type
        }
    }

    // Choice reward items (variable count, up to QUEST_REWARD_CHOICES_COUNT)
    if (packet.getReadPos() + 4 <= packet.getSize()) {
        uint32_t choiceCount = packet.readUInt32();
        for (uint32_t i = 0; i < choiceCount && packet.getReadPos() + 12 <= packet.getSize(); ++i) {
            packet.readUInt32(); // itemId
            packet.readUInt32(); // count
            packet.readUInt32(); // displayInfo
        }
    }

    // Fixed reward items (variable count, up to QUEST_REWARDS_COUNT)
    if (packet.getReadPos() + 4 <= packet.getSize()) {
        uint32_t rewardCount = packet.readUInt32();
        for (uint32_t i = 0; i < rewardCount && packet.getReadPos() + 12 <= packet.getSize(); ++i) {
            packet.readUInt32(); // itemId
            packet.readUInt32(); // count
            packet.readUInt32(); // displayInfo
        }
    }

    if (packet.getReadPos() + 4 <= packet.getSize())
        data.rewardMoney = packet.readUInt32();
    if (packet.getReadPos() + 4 <= packet.getSize())
        data.rewardXp = packet.readUInt32();

    LOG_DEBUG("Quest details tbc/classic: id=", data.questId, " title='", data.title, "'");
    return true;
}

// ============================================================================
// TBC 2.4.3 CMSG_QUESTGIVER_QUERY_QUEST
//
// WotLK adds a trailing uint8 isDialogContinued byte; TBC does not.
// TBC format: guid(8) + questId(4) = 12 bytes.
// ============================================================================
network::Packet TbcPacketParsers::buildQueryQuestPacket(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_QUERY_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    // No isDialogContinued byte (WotLK-only addition)
    return packet;
}

// ============================================================================
// TBC parseAuraUpdate - SMSG_AURA_UPDATE doesn't exist in TBC
// TBC uses inline aura update fields + SMSG_INIT_EXTRA_AURA_INFO_OBSOLETE (0x3A3) /
// SMSG_SET_EXTRA_AURA_INFO_OBSOLETE (0x3A4) instead
// ============================================================================
bool TbcPacketParsers::parseAuraUpdate(network::Packet& /*packet*/, AuraUpdateData& /*data*/, bool /*isAll*/) {
    LOG_DEBUG("[TBC] parseAuraUpdate called but SMSG_AURA_UPDATE does not exist in TBC 2.4.3");
    return false;
}

// ============================================================================
// TBC/Classic parseNameQueryResponse
//
// WotLK uses: packedGuid + uint8 found + name + realmName + u8 race + u8 gender + u8 class
// Classic/TBC commonly use: uint64 guid + [optional uint8 found] + CString name + uint32 race + uint32 gender + uint32 class
//
// Implement a robust parser that handles both classic-era variants.
// ============================================================================
static bool hasNullWithin(const network::Packet& p, size_t start, size_t maxLen) {
    const auto& d = p.getData();
    size_t end = std::min(d.size(), start + maxLen);
    for (size_t i = start; i < end; i++) {
        if (d[i] == 0) return true;
    }
    return false;
}

bool TbcPacketParsers::parseNameQueryResponse(network::Packet& packet, NameQueryResponseData& data) {
    // Default all fields
    data = NameQueryResponseData{};

    size_t start = packet.getReadPos();
    if (packet.getSize() - start < 8) return false;

    // Variant A: guid(u64) + name + race(u32) + gender(u32) + class(u32)
    {
        packet.setReadPos(start);
        data.guid = packet.readUInt64();
        data.found = 0;
        data.name = packet.readString();
        if (!data.name.empty() && (packet.getSize() - packet.getReadPos()) >= 12) {
            uint32_t race = packet.readUInt32();
            uint32_t gender = packet.readUInt32();
            uint32_t cls = packet.readUInt32();
            data.race = static_cast<uint8_t>(race & 0xFF);
            data.gender = static_cast<uint8_t>(gender & 0xFF);
            data.classId = static_cast<uint8_t>(cls & 0xFF);
            data.realmName.clear();
            return true;
        }
    }

    // Variant B: guid(u64) + found(u8) + [if found==0: name + race(u32)+gender(u32)+class(u32)]
    {
        packet.setReadPos(start);
        data.guid = packet.readUInt64();
        if (packet.getSize() - packet.getReadPos() < 1) {
            packet.setReadPos(start);
            return false;
        }
        uint8_t found = packet.readUInt8();
        // Guard: only treat it as a found flag if a CString likely follows.
        if ((found == 0 || found == 1) && hasNullWithin(packet, packet.getReadPos(), 64)) {
            data.found = found;
            if (data.found != 0) return true;
            data.name = packet.readString();
            if (!data.name.empty() && (packet.getSize() - packet.getReadPos()) >= 12) {
                uint32_t race = packet.readUInt32();
                uint32_t gender = packet.readUInt32();
                uint32_t cls = packet.readUInt32();
                data.race = static_cast<uint8_t>(race & 0xFF);
                data.gender = static_cast<uint8_t>(gender & 0xFF);
                data.classId = static_cast<uint8_t>(cls & 0xFF);
                data.realmName.clear();
                return true;
            }
        }
    }

    packet.setReadPos(start);
    return false;
}

// ============================================================================
// TBC parseItemQueryResponse - SMSG_ITEM_QUERY_SINGLE_RESPONSE (2.4.3 format)
//
// Differences from WotLK (handled by base class ItemQueryResponseParser::parse):
//   - No Flags2 field (WotLK added a second flags uint32 after Flags)
//   - No BuyCount field (WotLK added this between Flags2 and BuyPrice)
//   - Stats: sends exactly statsCount pairs (WotLK always sends 10)
//   - No ScalingStatDistribution / ScalingStatValue (WotLK-only heirloom scaling)
//
// Differences from Classic (ClassicPacketParsers::parseItemQueryResponse):
//   - Has SoundOverrideSubclass (int32) after subClass (Classic lacks it)
//   - Has statsCount prefix (Classic reads 10 pairs with no prefix)
// ============================================================================
bool TbcPacketParsers::parseItemQueryResponse(network::Packet& packet, ItemQueryResponseData& data) {
    data.entry = packet.readUInt32();
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        return true;
    }

    uint32_t itemClass = packet.readUInt32();
    uint32_t subClass  = packet.readUInt32();
    data.itemClass = itemClass;
    data.subClass  = subClass;
    packet.readUInt32(); // SoundOverrideSubclass (int32, -1 = no override)
    data.subclassName = "";

    // Name strings
    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4

    data.displayInfoId = packet.readUInt32();
    data.quality       = packet.readUInt32();

    packet.readUInt32(); // Flags  (TBC: 1 flags field only — no Flags2)
    // TBC: NO Flags2, NO BuyCount
    packet.readUInt32(); // BuyPrice
    data.sellPrice = packet.readUInt32();

    data.inventoryType = packet.readUInt32();

    packet.readUInt32(); // AllowableClass
    packet.readUInt32(); // AllowableRace
    data.itemLevel = packet.readUInt32();
    data.requiredLevel = packet.readUInt32();
    packet.readUInt32(); // RequiredSkill
    packet.readUInt32(); // RequiredSkillRank
    packet.readUInt32(); // RequiredSpell
    packet.readUInt32(); // RequiredHonorRank
    packet.readUInt32(); // RequiredCityRank
    packet.readUInt32(); // RequiredReputationFaction
    packet.readUInt32(); // RequiredReputationRank
    packet.readUInt32(); // MaxCount
    data.maxStack       = static_cast<int32_t>(packet.readUInt32()); // Stackable
    data.containerSlots = packet.readUInt32();

    // TBC: statsCount prefix + exactly statsCount pairs (WotLK always sends 10)
    uint32_t statsCount = packet.readUInt32();
    if (statsCount > 10) statsCount = 10; // sanity cap
    for (uint32_t i = 0; i < statsCount; i++) {
        uint32_t statType  = packet.readUInt32();
        int32_t  statValue = static_cast<int32_t>(packet.readUInt32());
        switch (statType) {
            case 3: data.agility  = statValue; break;
            case 4: data.strength = statValue; break;
            case 5: data.intellect = statValue; break;
            case 6: data.spirit   = statValue; break;
            case 7: data.stamina  = statValue; break;
            default: break;
        }
    }
    // TBC: NO ScalingStatDistribution, NO ScalingStatValue (WotLK-only)

    // 5 damage entries
    bool haveWeaponDamage = false;
    for (int i = 0; i < 5; i++) {
        float    dmgMin     = packet.readFloat();
        float    dmgMax     = packet.readFloat();
        uint32_t damageType = packet.readUInt32();
        if (!haveWeaponDamage && dmgMax > 0.0f) {
            if (damageType == 0 || data.damageMax <= 0.0f) {
                data.damageMin = dmgMin;
                data.damageMax = dmgMax;
                haveWeaponDamage = (damageType == 0);
            }
        }
    }

    data.armor = static_cast<int32_t>(packet.readUInt32());

    if (packet.getSize() - packet.getReadPos() >= 28) {
        packet.readUInt32(); // HolyRes
        packet.readUInt32(); // FireRes
        packet.readUInt32(); // NatureRes
        packet.readUInt32(); // FrostRes
        packet.readUInt32(); // ShadowRes
        packet.readUInt32(); // ArcaneRes
        data.delayMs = packet.readUInt32();
    }

    // AmmoType + RangedModRange
    if (packet.getSize() - packet.getReadPos() >= 8) {
        packet.readUInt32(); // AmmoType
        packet.readFloat();  // RangedModRange
    }

    // 5 item spells
    for (int i = 0; i < 5; i++) {
        if (packet.getReadPos() + 24 > packet.getSize()) break;
        data.spells[i].spellId = packet.readUInt32();
        data.spells[i].spellTrigger = packet.readUInt32();
        packet.readUInt32(); // SpellCharges
        packet.readUInt32(); // SpellCooldown
        packet.readUInt32(); // SpellCategory
        packet.readUInt32(); // SpellCategoryCooldown
    }

    // Bonding type
    if (packet.getReadPos() + 4 <= packet.getSize())
        data.bindType = packet.readUInt32();

    // Flavor/lore text
    if (packet.getReadPos() < packet.getSize())
        data.description = packet.readString();

    data.valid = !data.name.empty();
    LOG_DEBUG("[TBC] Item query: ", data.name, " quality=", data.quality,
              " invType=", data.inventoryType, " armor=", data.armor);
    return true;
}

// ============================================================================
// TbcPacketParsers::parseMailList — TBC 2.4.3 SMSG_MAIL_LIST_RESULT
//
// Differences from WotLK 3.3.5a (base implementation):
//   - Header: uint8 count only (WotLK: uint32 totalCount + uint8 shownCount)
//   - No body field — subject IS the full text (WotLK added body when mailTemplateId==0)
//   - Attachment item GUID: full uint64 (WotLK: uint32 low GUID)
//   - Attachment enchants: 7 × uint32 id only (WotLK: 7 × {id+duration+charges} = 84 bytes)
//   - Header fields: cod + itemTextId + stationery (WotLK has extra unknown uint32 between
//     itemTextId and stationery)
// ============================================================================
bool TbcPacketParsers::parseMailList(network::Packet& packet, std::vector<MailMessage>& inbox) {
    size_t remaining = packet.getSize() - packet.getReadPos();
    if (remaining < 1) return false;

    uint8_t count = packet.readUInt8();
    LOG_INFO("SMSG_MAIL_LIST_RESULT (TBC): count=", (int)count);

    inbox.clear();
    inbox.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        remaining = packet.getSize() - packet.getReadPos();
        if (remaining < 2) break;

        uint16_t msgSize = packet.readUInt16();
        size_t startPos = packet.getReadPos();

        MailMessage msg;
        if (remaining < static_cast<size_t>(msgSize) + 2) {
            LOG_WARNING("[TBC] Mail entry ", i, " truncated");
            break;
        }

        msg.messageId = packet.readUInt32();
        msg.messageType = packet.readUInt8();

        switch (msg.messageType) {
            case 0: msg.senderGuid = packet.readUInt64(); break;
            default: msg.senderEntry = packet.readUInt32(); break;
        }

        msg.cod          = packet.readUInt32();
        packet.readUInt32();         // itemTextId
        // NOTE: TBC has NO extra unknown uint32 here (WotLK added one between itemTextId and stationery)
        msg.stationeryId = packet.readUInt32();
        msg.money        = packet.readUInt32();
        msg.flags        = packet.readUInt32();
        msg.expirationTime = packet.readFloat();
        msg.mailTemplateId = packet.readUInt32();
        msg.subject      = packet.readString();
        // TBC has no separate body field at all

        uint8_t attachCount = packet.readUInt8();
        msg.attachments.reserve(attachCount);
        for (uint8_t j = 0; j < attachCount; ++j) {
            MailAttachment att;
            att.slot         = packet.readUInt8();
            uint64_t itemGuid = packet.readUInt64();   // full 64-bit GUID (TBC)
            att.itemGuidLow  = static_cast<uint32_t>(itemGuid & 0xFFFFFFFF);
            att.itemId       = packet.readUInt32();
            // TBC: 7 × uint32 enchant ID only (no duration/charges per slot)
            for (int e = 0; e < 7; ++e) {
                uint32_t enchId = packet.readUInt32();
                if (e == 0) att.enchantId = enchId;
            }
            att.randomPropertyId     = packet.readUInt32();
            att.randomSuffix         = packet.readUInt32();
            att.stackCount           = packet.readUInt32();
            att.chargesOrDurability  = packet.readUInt32();
            att.maxDurability        = packet.readUInt32();
            packet.readUInt32();  // current durability (separate from chargesOrDurability)
            msg.attachments.push_back(att);
        }

        msg.read = (msg.flags & 0x01) != 0;
        inbox.push_back(std::move(msg));

        // Skip any unread bytes within this mail entry
        size_t consumed = packet.getReadPos() - startPos;
        if (consumed < static_cast<size_t>(msgSize)) {
            packet.setReadPos(startPos + msgSize);
        }
    }

    return !inbox.empty();
}

// ============================================================================
// TbcPacketParsers::parseSpellStart — TBC 2.4.3 SMSG_SPELL_START
//
// TBC uses full uint64 GUIDs for casterGuid and casterUnit.
// WotLK uses packed (variable-length) GUIDs.
// TBC also lacks the castCount byte — format:
//   casterGuid(u64) + casterUnit(u64) + castCount(u8) + spellId(u32) + castFlags(u32) + castTime(u32)
// Wait: TBC DOES have castCount. But WotLK removed spellId in some paths.
// Correct TBC format (cmangos-tbc): objectGuid(u64) + casterGuid(u64) + castCount(u8) + spellId(u32) + castFlags(u32) + castTime(u32)
// ============================================================================
bool TbcPacketParsers::parseSpellStart(network::Packet& packet, SpellStartData& data) {
    if (packet.getSize() - packet.getReadPos() < 22) return false;

    data.casterGuid = packet.readUInt64();   // full GUID (object)
    data.casterUnit = packet.readUInt64();   // full GUID (caster unit)
    data.castCount  = packet.readUInt8();
    data.spellId    = packet.readUInt32();
    data.castFlags  = packet.readUInt32();
    data.castTime   = packet.readUInt32();

    if (packet.getReadPos() + 4 <= packet.getSize()) {
        uint32_t targetFlags = packet.readUInt32();
        if ((targetFlags & 0x02) && packet.getReadPos() + 8 <= packet.getSize()) {
            data.targetGuid = packet.readUInt64();  // full GUID in TBC
        }
    }

    LOG_DEBUG("[TBC] Spell start: spell=", data.spellId, " castTime=", data.castTime, "ms");
    return true;
}

// ============================================================================
// TbcPacketParsers::parseSpellGo — TBC 2.4.3 SMSG_SPELL_GO
//
// TBC uses full uint64 GUIDs, no timestamp field after castFlags.
// WotLK uses packed GUIDs and adds a timestamp (u32) after castFlags.
// ============================================================================
bool TbcPacketParsers::parseSpellGo(network::Packet& packet, SpellGoData& data) {
    if (packet.getSize() - packet.getReadPos() < 19) return false;

    data.casterGuid = packet.readUInt64();   // full GUID in TBC
    data.casterUnit = packet.readUInt64();   // full GUID in TBC
    data.castCount  = packet.readUInt8();
    data.spellId    = packet.readUInt32();
    data.castFlags  = packet.readUInt32();
    // NOTE: NO timestamp field here in TBC (WotLK added packet.readUInt32())

    if (packet.getReadPos() >= packet.getSize()) {
        LOG_DEBUG("[TBC] Spell go: spell=", data.spellId, " (no hit data)");
        return true;
    }

    data.hitCount = packet.readUInt8();
    data.hitTargets.reserve(data.hitCount);
    for (uint8_t i = 0; i < data.hitCount && packet.getReadPos() + 8 <= packet.getSize(); ++i) {
        data.hitTargets.push_back(packet.readUInt64());  // full GUID in TBC
    }

    if (packet.getReadPos() < packet.getSize()) {
        data.missCount = packet.readUInt8();
        data.missTargets.reserve(data.missCount);
        for (uint8_t i = 0; i < data.missCount && packet.getReadPos() + 9 <= packet.getSize(); ++i) {
            SpellGoMissEntry m;
            m.targetGuid = packet.readUInt64();  // full GUID in TBC
            m.missType   = packet.readUInt8();
            data.missTargets.push_back(m);
        }
    }

    LOG_DEBUG("[TBC] Spell go: spell=", data.spellId, " hits=", (int)data.hitCount,
              " misses=", (int)data.missCount);
    return true;
}

// ============================================================================
// TbcPacketParsers::parseCastResult — TBC 2.4.3 SMSG_CAST_RESULT
//
// TBC format: spellId(u32) + result(u8) = 5 bytes
// WotLK adds a castCount(u8) prefix making it 6 bytes.
// Without this override, WotLK parser reads spellId[0] as castCount,
// then the remaining 4 bytes as spellId (off by one), producing wrong result.
// ============================================================================
bool TbcPacketParsers::parseCastResult(network::Packet& packet, uint32_t& spellId, uint8_t& result) {
    if (packet.getSize() - packet.getReadPos() < 5) return false;
    spellId = packet.readUInt32();   // No castCount prefix in TBC
    result  = packet.readUInt8();
    return true;
}

// ============================================================================
// TbcPacketParsers::parseCastFailed — TBC 2.4.3 SMSG_CAST_FAILED
//
// TBC format: spellId(u32) + result(u8)
// WotLK added castCount(u8) before spellId; reading it on TBC would shift
// the spellId by one byte and corrupt all subsequent fields.
// Classic has the same layout, but the result enum starts differently (offset +1);
// TBC uses the same result values as WotLK so no offset is needed.
// ============================================================================
bool TbcPacketParsers::parseCastFailed(network::Packet& packet, CastFailedData& data) {
    if (packet.getSize() - packet.getReadPos() < 5) return false;
    data.castCount = 0;                      // not present in TBC
    data.spellId   = packet.readUInt32();
    data.result    = packet.readUInt8();     // same enum as WotLK
    LOG_DEBUG("[TBC] Cast failed: spell=", data.spellId, " result=", (int)data.result);
    return true;
}

// ============================================================================
// TbcPacketParsers::parseAttackerStateUpdate — TBC 2.4.3 SMSG_ATTACKERSTATEUPDATE
//
// TBC uses full uint64 GUIDs for attacker and target.
// WotLK uses packed (variable-length) GUIDs — using the WotLK reader here
// would mis-parse TBC's GUIDs and corrupt all subsequent damage fields.
// ============================================================================
bool TbcPacketParsers::parseAttackerStateUpdate(network::Packet& packet, AttackerStateUpdateData& data) {
    if (packet.getSize() - packet.getReadPos() < 21) return false;

    data.hitInfo      = packet.readUInt32();
    data.attackerGuid = packet.readUInt64();   // full GUID in TBC
    data.targetGuid   = packet.readUInt64();   // full GUID in TBC
    data.totalDamage  = static_cast<int32_t>(packet.readUInt32());
    data.subDamageCount = packet.readUInt8();

    for (uint8_t i = 0; i < data.subDamageCount; ++i) {
        SubDamage sub;
        sub.schoolMask = packet.readUInt32();
        sub.damage     = packet.readFloat();
        sub.intDamage  = packet.readUInt32();
        sub.absorbed   = packet.readUInt32();
        sub.resisted   = packet.readUInt32();
        data.subDamages.push_back(sub);
    }

    data.victimState = packet.readUInt32();
    data.overkill    = static_cast<int32_t>(packet.readUInt32());

    if (packet.getReadPos() < packet.getSize()) {
        data.blocked = packet.readUInt32();
    }

    LOG_DEBUG("[TBC] Melee hit: ", data.totalDamage, " damage",
              data.isCrit() ? " (CRIT)" : "",
              data.isMiss() ? " (MISS)" : "");
    return true;
}

// ============================================================================
// TbcPacketParsers::parseSpellDamageLog — TBC 2.4.3 SMSG_SPELLNONMELEEDAMAGELOG
//
// TBC uses full uint64 GUIDs; WotLK uses packed GUIDs.
// ============================================================================
bool TbcPacketParsers::parseSpellDamageLog(network::Packet& packet, SpellDamageLogData& data) {
    if (packet.getSize() - packet.getReadPos() < 29) return false;

    data.targetGuid  = packet.readUInt64();   // full GUID in TBC
    data.attackerGuid = packet.readUInt64();  // full GUID in TBC
    data.spellId     = packet.readUInt32();
    data.damage      = packet.readUInt32();
    data.schoolMask  = packet.readUInt8();
    data.absorbed    = packet.readUInt32();
    data.resisted    = packet.readUInt32();

    uint8_t periodicLog = packet.readUInt8();
    (void)periodicLog;
    packet.readUInt8();    // unused
    packet.readUInt32();   // blocked
    uint32_t flags = packet.readUInt32();
    data.isCrit = (flags & 0x02) != 0;

    // TBC does not have an overkill field here
    data.overkill = 0;

    LOG_DEBUG("[TBC] Spell damage: spellId=", data.spellId, " dmg=", data.damage,
              data.isCrit ? " CRIT" : "");
    return true;
}

// ============================================================================
// TbcPacketParsers::parseSpellHealLog — TBC 2.4.3 SMSG_SPELLHEALLOG
//
// TBC uses full uint64 GUIDs; WotLK uses packed GUIDs.
// ============================================================================
bool TbcPacketParsers::parseSpellHealLog(network::Packet& packet, SpellHealLogData& data) {
    if (packet.getSize() - packet.getReadPos() < 25) return false;

    data.targetGuid  = packet.readUInt64();   // full GUID in TBC
    data.casterGuid  = packet.readUInt64();   // full GUID in TBC
    data.spellId     = packet.readUInt32();
    data.heal        = packet.readUInt32();
    data.overheal    = packet.readUInt32();
    // TBC has no absorbed field in SMSG_SPELLHEALLOG; skip crit flag
    if (packet.getReadPos() < packet.getSize()) {
        uint8_t critFlag = packet.readUInt8();
        data.isCrit = (critFlag != 0);
    }

    LOG_DEBUG("[TBC] Spell heal: spellId=", data.spellId, " heal=", data.heal,
              data.isCrit ? " CRIT" : "");
    return true;
}

} // namespace game
} // namespace wowee
