#include "game/packet_parsers.hpp"
#include "game/spline_packet.hpp"
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
    auto rem = [&]() -> size_t { return packet.getRemainingSize(); };
    if (rem() < 1) return false;

    // TBC 2.4.3: UpdateFlags is uint8 (1 byte)
    uint8_t updateFlags = packet.readUInt8();
    block.updateFlags = static_cast<uint16_t>(updateFlags);

    LOG_DEBUG("  [TBC] UpdateFlags: 0x", std::hex, static_cast<int>(updateFlags), std::dec);

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
        // Minimum: moveFlags(4)+moveFlags2(1)+time(4)+position(16)+fallTime(4)+speeds(32) = 61
        if (rem() < 61) return false;

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
            if (rem() < 1) return false;
            block.onTransport = true;
            block.transportGuid = packet.readPackedGuid();
            if (rem() < 20) return false; // 4 floats + 1 uint32
            block.transportX = packet.readFloat();
            block.transportY = packet.readFloat();
            block.transportZ = packet.readFloat();
            block.transportO = packet.readFloat();
            /*uint32_t tTime =*/ packet.readUInt32();
        }

        // Pitch: SWIMMING, or else ONTRANSPORT (TBC-specific secondary pitch)
        if (moveFlags & TbcMoveFlags::SWIMMING) {
            if (rem() < 4) return false;
            /*float pitch =*/ packet.readFloat();
        } else if (moveFlags & TbcMoveFlags::ONTRANSPORT) {
            if (rem() < 4) return false;
            /*float pitch =*/ packet.readFloat();
        }

        // Fall time (always present)
        if (rem() < 4) return false;
        /*uint32_t fallTime =*/ packet.readUInt32();

        // Jumping (TBC: JUMPING=0x2000, WotLK: FALLING=0x1000)
        if (moveFlags & TbcMoveFlags::JUMPING) {
            if (rem() < 16) return false;
            /*float jumpVelocity =*/ packet.readFloat();
            /*float jumpSinAngle =*/ packet.readFloat();
            /*float jumpCosAngle =*/ packet.readFloat();
            /*float jumpXYSpeed =*/ packet.readFloat();
        }

        // Spline elevation (TBC: 0x02000000, WotLK: 0x04000000)
        if (moveFlags & TbcMoveFlags::SPLINE_ELEVATION) {
            if (rem() < 4) return false;
            /*float splineElevation =*/ packet.readFloat();
        }

        // Speeds (TBC: 8 values — walk, run, runBack, swim, fly, flyBack, swimBack, turn)
        if (rem() < 32) return false;
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
            if (rem() < 4) return false;
            uint32_t splineFlags = packet.readUInt32();
            LOG_DEBUG("  [TBC] Spline: flags=0x", std::hex, splineFlags, std::dec);

            if (splineFlags & 0x00010000) { // FINAL_POINT
                if (rem() < 12) return false;
                /*float finalX =*/ packet.readFloat();
                /*float finalY =*/ packet.readFloat();
                /*float finalZ =*/ packet.readFloat();
            } else if (splineFlags & 0x00020000) { // FINAL_TARGET
                if (rem() < 8) return false;
                /*uint64_t finalTarget =*/ packet.readUInt64();
            } else if (splineFlags & 0x00040000) { // FINAL_ANGLE
                if (rem() < 4) return false;
                /*float finalAngle =*/ packet.readFloat();
            }

            // TBC spline: timePassed, duration, id, pointCount
            if (rem() < 16) return false;
            /*uint32_t timePassed =*/ packet.readUInt32();
            /*uint32_t duration =*/ packet.readUInt32();
            /*uint32_t splineId =*/ packet.readUInt32();

            uint32_t pointCount = packet.readUInt32();
            // Cap waypoints to prevent DoS from malformed packets allocating huge arrays
            if (pointCount > 256) return false;

            // points + endPoint (no splineMode in TBC)
            if (rem() < static_cast<size_t>(pointCount) * 12 + 12) return false;
            for (uint32_t i = 0; i < pointCount; i++) {
                /*float px =*/ packet.readFloat();
                /*float py =*/ packet.readFloat();
                /*float pz =*/ packet.readFloat();
            }

            /*float endPointX =*/ packet.readFloat();
            /*float endPointY =*/ packet.readFloat();
            /*float endPointZ =*/ packet.readFloat();
        }
    }
    else if (updateFlags & UPDATEFLAG_HAS_POSITION) {
        if (rem() < 16) return false;
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [TBC] STATIONARY: (", block.x, ", ", block.y, ", ", block.z, ")");
    }

    // Target GUID
    if (updateFlags & UPDATEFLAG_HAS_TARGET) {
        if (rem() < 1) return false;
        /*uint64_t targetGuid =*/ packet.readPackedGuid();
    }

    // Transport time
    if (updateFlags & UPDATEFLAG_TRANSPORT) {
        if (rem() < 4) return false;
        /*uint32_t transportTime =*/ packet.readUInt32();
    }

    // LOWGUID (0x08) — TBC has 2 u32s, Classic has 1 u32
    if (updateFlags & UPDATEFLAG_LOWGUID) {
        if (rem() < 8) return false;
        /*uint32_t unknown0 =*/ packet.readUInt32();
        /*uint32_t unknown1 =*/ packet.readUInt32();
    }

    // HIGHGUID (0x10)
    if (updateFlags & UPDATEFLAG_HIGHGUID) {
        if (rem() < 4) return false;
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
    packet.writeFloat(info.x);
    packet.writeFloat(info.y);
    packet.writeFloat(info.z);
    packet.writeFloat(info.orientation);

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
        packet.writeFloat(info.transportX);
        packet.writeFloat(info.transportY);
        packet.writeFloat(info.transportZ);
        packet.writeFloat(info.transportO);

        // Transport time
        packet.writeUInt32(info.transportTime);

        // TBC: NO transport seat byte
        // TBC: NO interpolated movement time
    }

    // Pitch: SWIMMING or else ONTRANSPORT (TBC flag positions)
    if (info.flags & TbcMoveFlags::SWIMMING) {
        packet.writeFloat(info.pitch);
    } else if (info.flags & TbcMoveFlags::ONTRANSPORT) {
        packet.writeFloat(info.pitch);
    }

    // Fall time (always present)
    packet.writeUInt32(info.fallTime);

    // Jump data (TBC JUMPING = 0x2000, WotLK FALLING = 0x1000)
    if (info.flags & TbcMoveFlags::JUMPING) {
        packet.writeFloat(info.jumpVelocity);
        packet.writeFloat(info.jumpSinAngle);
        packet.writeFloat(info.jumpCosAngle);
        packet.writeFloat(info.jumpXYSpeed);
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
    // Validate minimum packet size for count byte
    if (packet.getSize() < 1) {
        LOG_ERROR("[TBC] SMSG_CHAR_ENUM packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    uint8_t count = packet.readUInt8();

    // Cap count to prevent excessive memory allocation
    constexpr uint8_t kMaxCharacters = 32;
    if (count > kMaxCharacters) {
        LOG_WARNING("[TBC] Character count ", static_cast<int>(count), " exceeds max ", static_cast<int>(kMaxCharacters),
                    ", capping");
        count = kMaxCharacters;
    }

    LOG_INFO("[TBC] Parsing SMSG_CHAR_ENUM: ", static_cast<int>(count), " characters");

    response.characters.clear();
    response.characters.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        // Sanity check: ensure we have at least minimal data before reading next character
        // Minimum: guid(8) + name(1) + race(1) + class(1) + gender(1) + appearance(4)
        //          + facialFeatures(1) + level(1) + zone(4) + map(4) + pos(12) + guild(4)
        //          + flags(4) + firstLogin(1) + pet(12) + equipment(20*9)
        constexpr size_t kMinCharacterSize = 8 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 4 + 4 + 12 + 4 + 4 + 1 + 12 + 180;
        if (!packet.hasRemaining(kMinCharacterSize)) {
            LOG_WARNING("[TBC] Character enum packet truncated at character ", static_cast<int>(i + 1),
                        ", pos=", packet.getReadPos(), " needed=", kMinCharacterSize,
                        " size=", packet.getSize());
            break;
        }

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

        LOG_DEBUG("  Character ", static_cast<int>(i + 1), ": ", character.name,
                  " (", getRaceName(character.race), " ", getClassName(character.characterClass),
                  " level ", static_cast<int>(character.level), " zone ", character.zoneId, ")");

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

        uint32_t remainingBlockCount = out.blockCount;

        if (packet.hasRemaining(1)) {
            uint8_t firstByte = packet.readUInt8();
            if (firstByte == static_cast<uint8_t>(UpdateType::OUT_OF_RANGE_OBJECTS)) {
                if (remainingBlockCount == 0) {
                    packet.setReadPos(start);
                    return false;
                }
                --remainingBlockCount;
                if (!packet.hasRemaining(4)) {
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
                    uint64_t guid = packet.readPackedGuid();
                    out.outOfRangeGuids.push_back(guid);
                }
            } else {
                packet.setReadPos(packet.getReadPos() - 1);
            }
        }

        out.blockCount = remainingBlockCount;
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
                    block.guid = packet.readPackedGuid();
                    ok = UpdateObjectParser::parseUpdateFields(packet, block);
                    break;
                }
                case UpdateType::MOVEMENT: {
                    block.guid = packet.readUInt64();
                    ok = this->parseMovementBlock(packet, block);
                    break;
                }
                case UpdateType::CREATE_OBJECT:
                case UpdateType::CREATE_OBJECT2: {
                    block.guid = packet.readPackedGuid();
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
    if (!packet.hasRemaining(16)) return false;

    data.npcGuid = packet.readUInt64();
    data.menuId = packet.readUInt32();      // TBC added menuId (Classic doesn't have it)
    data.titleTextId = packet.readUInt32();
    uint32_t optionCount = packet.readUInt32();

    // Cap option count to reasonable maximum
    constexpr uint32_t kMaxGossipOptions = 256;
    if (optionCount > kMaxGossipOptions) {
        LOG_WARNING("[TBC] SMSG_GOSSIP_MESSAGE optionCount=", optionCount, " exceeds max ",
                    kMaxGossipOptions, ", capping");
        optionCount = kMaxGossipOptions;
    }

    data.options.clear();
    data.options.reserve(optionCount);
    for (uint32_t i = 0; i < optionCount; ++i) {
        // Sanity check: ensure minimum bytes available for option
        // (id(4)+icon(1)+isCoded(1)+boxMoney(4)+text(1)+boxText(1))
        size_t remaining = packet.getRemainingSize();
        if (remaining < 12) {
            LOG_WARNING("[TBC] gossip option ", i, " truncated (", remaining, " bytes left)");
            break;
        }

        GossipOption opt;
        opt.id = packet.readUInt32();
        opt.icon = packet.readUInt8();
        opt.isCoded = (packet.readUInt8() != 0);
        opt.boxMoney = packet.readUInt32();
        opt.text = packet.readString();
        opt.boxText = packet.readString();
        data.options.push_back(opt);
    }

    // Ensure we have at least 4 bytes for questCount
    size_t remaining = packet.getRemainingSize();
    if (remaining < 4) {
        LOG_WARNING("[TBC] SMSG_GOSSIP_MESSAGE truncated before questCount");
        return data.options.size() > 0;  // Return true if we got at least some options
    }

    uint32_t questCount = packet.readUInt32();

    // Cap quest count to reasonable maximum
    constexpr uint32_t kMaxGossipQuests = 256;
    if (questCount > kMaxGossipQuests) {
        LOG_WARNING("[TBC] SMSG_GOSSIP_MESSAGE questCount=", questCount, " exceeds max ",
                    kMaxGossipQuests, ", capping");
        questCount = kMaxGossipQuests;
    }

    data.quests.clear();
    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        // Sanity check: ensure minimum bytes available for quest
        // (id(4)+icon(4)+level(4)+title(1))
        remaining = packet.getRemainingSize();
        if (remaining < 13) {
            LOG_WARNING("[TBC] gossip quest ", i, " truncated (", remaining, " bytes left)");
            break;
        }

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
    data.guid = packet.readPackedGuid();
    if (data.guid == 0) return false;
    // No unk byte here in TBC 2.4.3

    if (!packet.hasRemaining(12)) return false;
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();

    if (!packet.hasRemaining(4)) return false;
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
        if (!packet.hasRemaining(12)) return false;
        packet.readFloat(); packet.readFloat(); packet.readFloat();
    } else if (data.moveType == 3) {
        if (!packet.hasRemaining(8)) return false;
        data.facingTarget = packet.readUInt64();
    } else if (data.moveType == 4) {
        if (!packet.hasRemaining(4)) return false;
        data.facingAngle = packet.readFloat();
    }

    if (!packet.hasRemaining(4)) return false;
    data.splineFlags = packet.readUInt32();

    // Consolidated spline body parser (TBC uses different uncompressed mask)
    {
        SplineBlockData spline;
        if (!parseMonsterMoveSplineBody(packet, spline, data.splineFlags,
                                        glm::vec3(data.x, data.y, data.z), true)) {
            return false;
        }
        data.duration = spline.duration;
        if (spline.hasDest) {
            data.destX = spline.destination.x;
            data.destY = spline.destination.y;
            data.destZ = spline.destination.z;
            data.hasDest = true;
        }
    }

    LOG_DEBUG("[TBC] MonsterMove: guid=0x", std::hex, data.guid, std::dec,
              " type=", static_cast<int>(data.moveType), " dur=", data.duration, "ms",
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

    if (!packet.hasRemaining(5)) {
        LOG_DEBUG("Quest details tbc/classic (short): id=", data.questId, " title='", data.title, "'");
        return !data.title.empty() || data.questId != 0;
    }

    /*activateAccept*/ packet.readUInt8();
    data.suggestedPlayers = packet.readUInt32();

    // TBC/Classic: emote section before reward items
    if (packet.hasRemaining(4)) {
        uint32_t emoteCount = packet.readUInt32();
        for (uint32_t i = 0; i < emoteCount && packet.hasRemaining(8); ++i) {
            packet.readUInt32(); // delay
            packet.readUInt32(); // type
        }
    }

    // Choice reward items (variable count, up to QUEST_REWARD_CHOICES_COUNT)
    if (packet.hasRemaining(4)) {
        uint32_t choiceCount = packet.readUInt32();
        for (uint32_t i = 0; i < choiceCount && packet.hasRemaining(12); ++i) {
            uint32_t itemId = packet.readUInt32();
            uint32_t count  = packet.readUInt32();
            uint32_t dispId = packet.readUInt32();
            if (itemId != 0) {
                QuestRewardItem ri;
                ri.itemId = itemId; ri.count = count; ri.displayInfoId = dispId;
                ri.choiceSlot = i;
                data.rewardChoiceItems.push_back(ri);
            }
        }
    }

    // Fixed reward items (variable count, up to QUEST_REWARDS_COUNT)
    if (packet.hasRemaining(4)) {
        uint32_t rewardCount = packet.readUInt32();
        for (uint32_t i = 0; i < rewardCount && packet.hasRemaining(12); ++i) {
            uint32_t itemId = packet.readUInt32();
            uint32_t count  = packet.readUInt32();
            uint32_t dispId = packet.readUInt32();
            if (itemId != 0) {
                QuestRewardItem ri;
                ri.itemId = itemId; ri.count = count; ri.displayInfoId = dispId;
                data.rewardItems.push_back(ri);
            }
        }
    }

    if (packet.hasRemaining(4))
        data.rewardMoney = packet.readUInt32();
    if (packet.hasRemaining(4))
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
        if (!data.name.empty() && packet.hasRemaining(12)) {
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
        if (!packet.hasRemaining(1)) {
            packet.setReadPos(start);
            return false;
        }
        uint8_t found = packet.readUInt8();
        // Guard: only treat it as a found flag if a CString likely follows.
        if ((found == 0 || found == 1) && hasNullWithin(packet, packet.getReadPos(), 64)) {
            data.found = found;
            if (data.found != 0) return true;
            data.name = packet.readString();
            if (!data.name.empty() && packet.hasRemaining(12)) {
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
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        return true;
    }

    // Validate minimum size for fixed fields: itemClass(4) + subClass(4) + soundOverride(4) + 4 name strings + displayInfoId(4) + quality(4)
    if (!packet.hasRemaining(12)) {
        LOG_ERROR("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before names (entry=", data.entry, ")");
        return false;
    }

    uint32_t itemClass = packet.readUInt32();
    uint32_t subClass  = packet.readUInt32();
    data.itemClass = itemClass;
    data.subClass  = subClass;
    packet.readUInt32(); // SoundOverrideSubclass (int32, -1 = no override)
    data.subclassName = getItemSubclassName(itemClass, subClass);

    // Name strings
    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4

    data.displayInfoId = packet.readUInt32();
    data.quality       = packet.readUInt32();

    // Validate minimum size for fixed fields: Flags(4) + BuyPrice(4) + SellPrice(4) + inventoryType(4)
    if (!packet.hasRemaining(16)) {
        LOG_ERROR("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before inventoryType (entry=", data.entry, ")");
        return false;
    }

    data.itemFlags = packet.readUInt32(); // Flags  (TBC: 1 flags field only — no Flags2)
    // TBC: NO Flags2, NO BuyCount
    packet.readUInt32(); // BuyPrice
    data.sellPrice = packet.readUInt32();

    data.inventoryType = packet.readUInt32();

    // Validate minimum size for remaining fixed fields: 13×4 = 52 bytes
    if (!packet.hasRemaining(52)) {
        LOG_ERROR("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before statsCount (entry=", data.entry, ")");
        return false;
    }

    data.allowableClass = packet.readUInt32(); // AllowableClass
    data.allowableRace  = packet.readUInt32(); // AllowableRace
    data.itemLevel = packet.readUInt32();
    data.requiredLevel = packet.readUInt32();
    data.requiredSkill     = packet.readUInt32(); // RequiredSkill
    data.requiredSkillRank = packet.readUInt32(); // RequiredSkillRank
    packet.readUInt32(); // RequiredSpell
    packet.readUInt32(); // RequiredHonorRank
    packet.readUInt32(); // RequiredCityRank
    data.requiredReputationFaction = packet.readUInt32(); // RequiredReputationFaction
    data.requiredReputationRank    = packet.readUInt32(); // RequiredReputationRank
    data.maxCount = static_cast<int32_t>(packet.readUInt32()); // MaxCount (1 = Unique)
    data.maxStack = static_cast<int32_t>(packet.readUInt32()); // Stackable
    data.containerSlots = packet.readUInt32();

    // TBC: statsCount prefix + exactly statsCount pairs (WotLK always sends 10)
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated at statsCount (entry=", data.entry, ")");
        return true;  // Have core fields; stats are optional
    }
    uint32_t statsCount = packet.readUInt32();
    if (statsCount > 10) {
        LOG_WARNING("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: statsCount=", statsCount, " exceeds max 10 (entry=",
                    data.entry, "), capping");
        statsCount = 10;
    }
    for (uint32_t i = 0; i < statsCount; i++) {
        // Each stat is 2 uint32s = 8 bytes
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: stat ", i, " truncated (entry=", data.entry, ")");
            break;
        }
        uint32_t statType  = packet.readUInt32();
        int32_t  statValue = static_cast<int32_t>(packet.readUInt32());
        switch (statType) {
            case 3: data.agility  = statValue; break;
            case 4: data.strength = statValue; break;
            case 5: data.intellect = statValue; break;
            case 6: data.spirit   = statValue; break;
            case 7: data.stamina  = statValue; break;
            default:
                if (statValue != 0)
                    data.extraStats.push_back({statType, statValue});
                break;
        }
    }
    // TBC: NO ScalingStatDistribution, NO ScalingStatValue (WotLK-only)

    // 5 damage entries (5×12 = 60 bytes)
    bool haveWeaponDamage = false;
    for (int i = 0; i < 5; i++) {
        // Each damage entry is dmgMin(4) + dmgMax(4) + damageType(4) = 12 bytes
        if (!packet.hasRemaining(12)) {
            LOG_WARNING("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: damage ", i, " truncated (entry=", data.entry, ")");
            break;
        }
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

    // Validate minimum size for armor (4 bytes)
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before armor (entry=", data.entry, ")");
        return true;  // Have core fields; armor is important but optional
    }
    data.armor = static_cast<int32_t>(packet.readUInt32());

    if (packet.hasRemaining(28)) {
        data.holyRes   = static_cast<int32_t>(packet.readUInt32()); // HolyRes
        data.fireRes   = static_cast<int32_t>(packet.readUInt32()); // FireRes
        data.natureRes = static_cast<int32_t>(packet.readUInt32()); // NatureRes
        data.frostRes  = static_cast<int32_t>(packet.readUInt32()); // FrostRes
        data.shadowRes = static_cast<int32_t>(packet.readUInt32()); // ShadowRes
        data.arcaneRes = static_cast<int32_t>(packet.readUInt32()); // ArcaneRes
        data.delayMs = packet.readUInt32();
    }

    // AmmoType + RangedModRange
    if (packet.hasRemaining(8)) {
        packet.readUInt32(); // AmmoType
        packet.readFloat();  // RangedModRange
    }

    // 5 item spells
    for (int i = 0; i < 5; i++) {
        if (!packet.hasRemaining(24)) break;
        data.spells[i].spellId = packet.readUInt32();
        data.spells[i].spellTrigger = packet.readUInt32();
        packet.readUInt32(); // SpellCharges
        packet.readUInt32(); // SpellCooldown
        packet.readUInt32(); // SpellCategory
        packet.readUInt32(); // SpellCategoryCooldown
    }

    // Bonding type
    if (packet.hasRemaining(4))
        data.bindType = packet.readUInt32();

    // Flavor/lore text
    if (packet.getReadPos() < packet.getSize())
        data.description = packet.readString();

    // Post-description: PageText, LanguageID, PageMaterial, StartQuest
    if (packet.hasRemaining(16)) {
        packet.readUInt32(); // PageText
        packet.readUInt32(); // LanguageID
        packet.readUInt32(); // PageMaterial
        data.startQuestId = packet.readUInt32(); // StartQuest
    }

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
    size_t remaining = packet.getRemainingSize();
    if (remaining < 1) return false;

    uint8_t count = packet.readUInt8();
    LOG_INFO("SMSG_MAIL_LIST_RESULT (TBC): count=", static_cast<int>(count));

    inbox.clear();
    inbox.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        remaining = packet.getRemainingSize();
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

    return true;
}

// ============================================================================
// ---------------------------------------------------------------------------
// skipTbcSpellCastTargets — consume all SpellCastTargets payload bytes for TBC.
//
// TBC uses uint32 targetFlags (Classic: uint16). Unit/item/object/corpse targets
// are PackedGuid (same as Classic). Source/dest location is 3 floats (12 bytes)
// with no transport guid (Classic: same; WotLK adds a transport PackedGuid).
//
// This helper is used by parseSpellStart to ensure the read position advances
// past ALL target payload fields so subsequent fields (e.g. those parsed by the
// caller after spell targets) are not corrupted.
// ---------------------------------------------------------------------------
static bool skipTbcSpellCastTargets(network::Packet& packet, uint64_t* primaryTargetGuid = nullptr) {
    if (!packet.hasRemaining(4)) return false;

    const uint32_t targetFlags = packet.readUInt32();

    // Returns false if the packed guid can't be read, otherwise reads and optionally captures it.
    auto readPackedGuidCond = [&](uint32_t flag, bool capture) -> bool {
        if (!(targetFlags & flag)) return true;
        // Packed GUID: 1-byte mask + up to 8 data bytes
        if (packet.getReadPos() >= packet.getSize()) return false;
        uint8_t mask = packet.getData()[packet.getReadPos()];
        size_t needed = 1;
        for (int b = 0; b < 8; ++b) if (mask & (1u << b)) ++needed;
        if (!packet.hasRemaining(needed)) return false;
        uint64_t g = packet.readPackedGuid();
        if (capture && primaryTargetGuid && *primaryTargetGuid == 0) *primaryTargetGuid = g;
        return true;
    };
    auto skipFloats3 = [&](uint32_t flag) -> bool {
        if (!(targetFlags & flag)) return true;
        if (!packet.hasRemaining(12)) return false;
        (void)packet.readFloat(); (void)packet.readFloat(); (void)packet.readFloat();
        return true;
    };

    // Process in wire order matching cmangos-tbc SpellCastTargets::write()
    if (!readPackedGuidCond(0x0002, true))  return false;  // UNIT
    if (!readPackedGuidCond(0x0004, false)) return false;  // UNIT_MINIPET
    if (!readPackedGuidCond(0x0010, false)) return false;  // ITEM
    if (!skipFloats3(0x0020))               return false;  // SOURCE_LOCATION
    if (!skipFloats3(0x0040))               return false;  // DEST_LOCATION

    if (targetFlags & 0x1000) {  // TRADE_ITEM: uint8
        if (packet.getReadPos() >= packet.getSize()) return false;
        (void)packet.readUInt8();
    }
    if (targetFlags & 0x2000) {  // STRING: null-terminated
        const auto& raw = packet.getData();
        size_t pos = packet.getReadPos();
        while (pos < raw.size() && raw[pos] != 0) ++pos;
        if (pos >= raw.size()) return false;
        packet.setReadPos(pos + 1);
    }
    if (!readPackedGuidCond(0x8200, false)) return false;  // CORPSE / PVP_CORPSE
    if (!readPackedGuidCond(0x0800, true))  return false;  // OBJECT

    return true;
}

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
    data = SpellStartData{};
    if (!packet.hasRemaining(22)) return false;

    data.casterGuid = packet.readUInt64();   // full GUID (object)
    data.casterUnit = packet.readUInt64();   // full GUID (caster unit)
    data.castCount  = packet.readUInt8();
    data.spellId    = packet.readUInt32();
    data.castFlags  = packet.readUInt32();
    data.castTime   = packet.readUInt32();

    // SpellCastTargets: consume ALL target payload types to keep the read position
    // aligned for any bytes the caller may parse after this (ammo, etc.).
    // The previous code only read UNIT(0x02)/OBJECT(0x800) target GUIDs and left
    // DEST_LOCATION(0x40)/SOURCE_LOCATION(0x20)/ITEM(0x10) bytes unconsumed,
    // corrupting subsequent reads for every AOE/ground-targeted spell cast.
    {
        uint64_t targetGuid = 0;
        skipTbcSpellCastTargets(packet, &targetGuid);  // non-fatal on truncation
        data.targetGuid = targetGuid;
    }

    LOG_DEBUG("[TBC] Spell start: spell=", data.spellId, " castTime=", data.castTime, "ms",
              " targetGuid=0x", std::hex, data.targetGuid, std::dec);
    return true;
}

// ============================================================================
// TbcPacketParsers::parseSpellGo — TBC 2.4.3 SMSG_SPELL_GO
//
// TBC uses full uint64 GUIDs, no timestamp field after castFlags.
// WotLK uses packed GUIDs and adds a timestamp (u32) after castFlags.
// ============================================================================
bool TbcPacketParsers::parseSpellGo(network::Packet& packet, SpellGoData& data) {
    // Always reset output to avoid stale targets when callers reuse buffers.
    data = SpellGoData{};

    const size_t startPos = packet.getReadPos();
    // Fixed header before hit/miss lists:
    // casterGuid(u64) + casterUnit(u64) + castCount(u8) + spellId(u32) + castFlags(u32)
    if (!packet.hasRemaining(25)) return false;

    data.casterGuid = packet.readUInt64();   // full GUID in TBC
    data.casterUnit = packet.readUInt64();   // full GUID in TBC
    data.castCount  = packet.readUInt8();
    data.spellId    = packet.readUInt32();
    data.castFlags  = packet.readUInt32();
    // NOTE: NO timestamp field here in TBC (WotLK added packet.readUInt32())

    if (packet.getReadPos() >= packet.getSize()) {
        LOG_WARNING("[TBC] Spell go: missing hitCount after fixed fields");
        packet.setReadPos(startPos);
        return false;
    }

    // Cap hit targets to prevent oversized allocations from malformed spell packets.
    // 128 is well above any real WoW AOE spell target count (max ~20 in practice).
    const uint8_t rawHitCount = packet.readUInt8();
    if (rawHitCount > 128) {
        LOG_WARNING("[TBC] Spell go: hitCount capped (requested=", static_cast<int>(rawHitCount), ")");
    }
    const uint8_t storedHitLimit = std::min<uint8_t>(rawHitCount, 128);
    data.hitTargets.reserve(storedHitLimit);
    bool truncatedTargets = false;
    for (uint16_t i = 0; i < rawHitCount; ++i) {
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("[TBC] Spell go: truncated hit targets at index ", i,
                        "/", static_cast<int>(rawHitCount));
            truncatedTargets = true;
            break;
        }
        const uint64_t targetGuid = packet.readUInt64();  // full GUID in TBC
        if (i < storedHitLimit) {
            data.hitTargets.push_back(targetGuid);
        }
    }
    if (truncatedTargets) {
        packet.setReadPos(startPos);
        return false;
    }
    data.hitCount = static_cast<uint8_t>(data.hitTargets.size());

    if (packet.getReadPos() >= packet.getSize()) {
        LOG_WARNING("[TBC] Spell go: missing missCount after hit target list");
        packet.setReadPos(startPos);
        return false;
    }

    const uint8_t rawMissCount = packet.readUInt8();
    if (rawMissCount > 128) {
        LOG_WARNING("[TBC] Spell go: missCount capped (requested=", static_cast<int>(rawMissCount), ")");
    }
    const uint8_t storedMissLimit = std::min<uint8_t>(rawMissCount, 128);
    data.missTargets.reserve(storedMissLimit);
    for (uint16_t i = 0; i < rawMissCount; ++i) {
        if (!packet.hasRemaining(9)) {
            LOG_WARNING("[TBC] Spell go: truncated miss targets at index ", i,
                        "/", static_cast<int>(rawMissCount));
            truncatedTargets = true;
            break;
        }
        SpellGoMissEntry m;
        m.targetGuid = packet.readUInt64();  // full GUID in TBC
        m.missType   = packet.readUInt8();
        if (m.missType == 11) { // SPELL_MISS_REFLECT
            if (!packet.hasRemaining(1)) {
                LOG_WARNING("[TBC] Spell go: truncated reflect payload at miss index ", i,
                            "/", static_cast<int>(rawMissCount));
                truncatedTargets = true;
                break;
            }
            (void)packet.readUInt8(); // reflectResult
        }
        if (i < storedMissLimit) {
            data.missTargets.push_back(m);
        }
    }
    if (truncatedTargets) {
        packet.setReadPos(startPos);
        return false;
    }
    data.missCount = static_cast<uint8_t>(data.missTargets.size());

    // SpellCastTargets follows the miss list — consume all target bytes so that
    // any subsequent fields are not misaligned for ground-targeted AoE spells.
    skipTbcSpellCastTargets(packet, &data.targetGuid);

    LOG_DEBUG("[TBC] Spell go: spell=", data.spellId, " hits=", static_cast<int>(data.hitCount),
              " misses=", static_cast<int>(data.missCount));
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
    if (!packet.hasRemaining(5)) return false;
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
    if (!packet.hasRemaining(5)) return false;
    data.castCount = 0;                      // not present in TBC
    data.spellId   = packet.readUInt32();
    data.result    = packet.readUInt8();     // same enum as WotLK
    LOG_DEBUG("[TBC] Cast failed: spell=", data.spellId, " result=", static_cast<int>(data.result));
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
    data = AttackerStateUpdateData{};

    const size_t startPos = packet.getReadPos();
    auto rem = [&]() { return packet.getRemainingSize(); };

    // Fixed fields before sub-damage list:
    // hitInfo(4) + attackerGuid(8) + targetGuid(8) + totalDamage(4) + subDamageCount(1) = 25 bytes
    if (rem() < 25) return false;

    data.hitInfo        = packet.readUInt32();
    data.attackerGuid   = packet.readUInt64();   // full GUID in TBC
    data.targetGuid     = packet.readUInt64();   // full GUID in TBC
    data.totalDamage    = static_cast<int32_t>(packet.readUInt32());
    data.subDamageCount = packet.readUInt8();

    // Clamp to what can fit in the remaining payload (20 bytes per sub-damage entry).
    const uint8_t maxSubDamageCount = static_cast<uint8_t>(std::min<size_t>(rem() / 20, 64));
    if (data.subDamageCount > maxSubDamageCount) {
        data.subDamageCount = maxSubDamageCount;
    }

    data.subDamages.reserve(data.subDamageCount);
    for (uint8_t i = 0; i < data.subDamageCount; ++i) {
        if (rem() < 20) {
            packet.setReadPos(startPos);
            return false;
        }
        SubDamage sub;
        sub.schoolMask = packet.readUInt32();
        sub.damage     = packet.readFloat();
        sub.intDamage  = packet.readUInt32();
        sub.absorbed   = packet.readUInt32();
        sub.resisted   = packet.readUInt32();
        data.subDamages.push_back(sub);
    }

    data.subDamageCount = static_cast<uint8_t>(data.subDamages.size());

    // victimState + overkill are part of the expected payload.
    if (rem() < 8) {
        packet.setReadPos(startPos);
        return false;
    }
    data.victimState = packet.readUInt32();
    data.overkill    = static_cast<int32_t>(packet.readUInt32());

    if (rem() >= 4) {
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
    // Fixed TBC payload size:
    // targetGuid(8) + attackerGuid(8) + spellId(4) + damage(4) + schoolMask(1)
    // + absorbed(4) + resisted(4) + periodicLog(1) + unused(1) + blocked(4) + flags(4)
    // = 43 bytes
    // Some servers append additional trailing fields; consume the canonical minimum
    // and leave any extension bytes unread.
    if (!packet.hasRemaining(43)) return false;

    data = SpellDamageLogData{};

    data.targetGuid   = packet.readUInt64();  // full GUID in TBC
    data.attackerGuid = packet.readUInt64();  // full GUID in TBC
    data.spellId      = packet.readUInt32();
    data.damage       = packet.readUInt32();
    data.schoolMask   = packet.readUInt8();
    data.absorbed     = packet.readUInt32();
    data.resisted     = packet.readUInt32();

    uint8_t periodicLog = packet.readUInt8();
    (void)periodicLog;
    packet.readUInt8();   // unused
    packet.readUInt32();  // blocked
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
    // Fixed payload is 28 bytes; many cores append crit flag (1 byte).
    // targetGuid(8) + casterGuid(8) + spellId(4) + heal(4) + overheal(4)
    if (!packet.hasRemaining(28)) return false;

    data = SpellHealLogData{};

    data.targetGuid = packet.readUInt64();  // full GUID in TBC
    data.casterGuid = packet.readUInt64();  // full GUID in TBC
    data.spellId    = packet.readUInt32();
    data.heal       = packet.readUInt32();
    data.overheal   = packet.readUInt32();
    // TBC has no absorbed field in SMSG_SPELLHEALLOG; skip crit flag
    if (packet.getReadPos() < packet.getSize()) {
        uint8_t critFlag = packet.readUInt8();
        data.isCrit = (critFlag != 0);
    }

    LOG_DEBUG("[TBC] Spell heal: spellId=", data.spellId, " heal=", data.heal,
              data.isCrit ? " CRIT" : "");
    return true;
}

// ============================================================================
// TBC 2.4.3 SMSG_MESSAGECHAT
// TBC format: type(u8) + language(u32) + [type-specific data] + msgLen(u32) + msg + tag(u8)
// WotLK adds senderGuid(u64) + unknown(u32) before type-specific data.
// ============================================================================

bool TbcPacketParsers::parseMessageChat(network::Packet& packet, MessageChatData& data) {
    if (packet.getSize() < 10) {
        LOG_ERROR("[TBC] SMSG_MESSAGECHAT packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    uint8_t typeVal = packet.readUInt8();
    data.type = static_cast<ChatType>(typeVal);

    uint32_t langVal = packet.readUInt32();
    data.language = static_cast<ChatLanguage>(langVal);

    // TBC: NO senderGuid or unknown field here (WotLK has senderGuid(u64) + unk(u32))

    switch (data.type) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_EMOTE:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_PARTY:
        case ChatType::RAID_BOSS_EMOTE: {
            // senderGuid(u64) + nameLen(u32) + name + targetGuid(u64)
            data.senderGuid = packet.readUInt64();
            uint32_t nameLen = packet.readUInt32();
            if (nameLen > 0 && nameLen < 256) {
                data.senderName.resize(nameLen);
                for (uint32_t i = 0; i < nameLen; ++i) {
                    data.senderName[i] = static_cast<char>(packet.readUInt8());
                }
                if (!data.senderName.empty() && data.senderName.back() == '\0') {
                    data.senderName.pop_back();
                }
            }
            data.receiverGuid = packet.readUInt64();
            break;
        }

        case ChatType::SAY:
        case ChatType::PARTY:
        case ChatType::YELL:
        case ChatType::WHISPER:
        case ChatType::WHISPER_INFORM:
        case ChatType::GUILD:
        case ChatType::OFFICER:
        case ChatType::RAID:
        case ChatType::RAID_LEADER:
        case ChatType::RAID_WARNING:
        case ChatType::EMOTE:
        case ChatType::TEXT_EMOTE: {
            // senderGuid(u64) + senderGuid(u64) — written twice by server
            data.senderGuid = packet.readUInt64();
            /*duplicateGuid*/ packet.readUInt64();
            break;
        }

        case ChatType::CHANNEL: {
            // channelName(string) + rank(u32) + senderGuid(u64)
            data.channelName = packet.readString();
            /*uint32_t rank =*/ packet.readUInt32();
            data.senderGuid = packet.readUInt64();
            break;
        }

        default: {
            // All other types: senderGuid(u64) + senderGuid(u64) — written twice
            data.senderGuid = packet.readUInt64();
            /*duplicateGuid*/ packet.readUInt64();
            break;
        }
    }

    // Read message length + message
    uint32_t messageLen = packet.readUInt32();
    if (messageLen > 0 && messageLen < 8192) {
        data.message.resize(messageLen);
        for (uint32_t i = 0; i < messageLen; ++i) {
            data.message[i] = static_cast<char>(packet.readUInt8());
        }
        if (!data.message.empty() && data.message.back() == '\0') {
            data.message.pop_back();
        }
    }

    // Read chat tag
    if (packet.getReadPos() < packet.getSize()) {
        data.chatTag = packet.readUInt8();
    }

    LOG_DEBUG("[TBC] SMSG_MESSAGECHAT: type=", getChatTypeString(data.type),
             " sender=", data.senderName.empty() ? std::to_string(data.senderGuid) : data.senderName);

    return true;
}

// ============================================================================
// TBC 2.4.3 quest giver status
// TBC sends uint32 (like Classic), WotLK changed to uint8.
// TBC 2.4.3 enum: 0=NONE,1=UNAVAILABLE,2=CHAT,3=INCOMPLETE,4=REWARD_REP,
//   5=AVAILABLE_REP,6=AVAILABLE,7=REWARD2,8=REWARD
// ============================================================================

uint8_t TbcPacketParsers::readQuestGiverStatus(network::Packet& packet) {
    uint32_t tbcStatus = packet.readUInt32();
    switch (tbcStatus) {
        case 0: return 0;   // NONE
        case 1: return 1;   // UNAVAILABLE
        case 2: return 0;   // CHAT → NONE (no marker)
        case 3: return 5;   // INCOMPLETE → WotLK INCOMPLETE
        case 4: return 6;   // REWARD_REP → WotLK REWARD_REP
        case 5: return 7;   // AVAILABLE_REP → WotLK AVAILABLE_LOW_LEVEL
        case 6: return 8;   // AVAILABLE → WotLK AVAILABLE
        case 7: return 10;  // REWARD2 → WotLK REWARD
        case 8: return 10;  // REWARD → WotLK REWARD
        default: return 0;
    }
}

// ============================================================================
// TBC 2.4.3 channel join/leave
// Classic/TBC: just name+password (no channelId/hasVoice/joinedByZone prefix)
// ============================================================================

network::Packet TbcPacketParsers::buildJoinChannel(const std::string& channelName, const std::string& password) {
    network::Packet packet(wireOpcode(Opcode::CMSG_JOIN_CHANNEL));
    packet.writeString(channelName);
    packet.writeString(password);
    LOG_DEBUG("[TBC] Built CMSG_JOIN_CHANNEL: channel=", channelName);
    return packet;
}

network::Packet TbcPacketParsers::buildLeaveChannel(const std::string& channelName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LEAVE_CHANNEL));
    packet.writeString(channelName);
    LOG_DEBUG("[TBC] Built CMSG_LEAVE_CHANNEL: channel=", channelName);
    return packet;
}

// ============================================================================
// TBC 2.4.3 SMSG_GAMEOBJECT_QUERY_RESPONSE
// TBC has 2 extra strings after name[4] (iconName + castBarCaption).
// WotLK has 3 (adds unk1). Classic has 0.
// ============================================================================

bool TbcPacketParsers::parseGameObjectQueryResponse(network::Packet& packet, GameObjectQueryResponseData& data) {
    if (packet.getSize() < 4) {
        LOG_ERROR("TBC SMSG_GAMEOBJECT_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        data.name = "";
        return true;
    }

    if (!packet.hasRemaining(8)) {
        LOG_ERROR("TBC SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated before names (entry=", data.entry, ")");
        return false;
    }

    data.type = packet.readUInt32();
    data.displayId = packet.readUInt32();
    // 4 name strings
    data.name = packet.readString();
    packet.readString();
    packet.readString();
    packet.readString();

    // TBC: 2 extra strings (iconName + castBarCaption) — WotLK has 3, Classic has 0
    packet.readString();  // iconName
    packet.readString();  // castBarCaption

    // Read 24 type-specific data fields
    size_t remaining = packet.getRemainingSize();
    if (remaining >= 24 * 4) {
        for (int i = 0; i < 24; i++) {
            data.data[i] = packet.readUInt32();
        }
        data.hasData = true;
    } else if (remaining > 0) {
        uint32_t fieldsToRead = remaining / 4;
        for (uint32_t i = 0; i < fieldsToRead && i < 24; i++) {
            data.data[i] = packet.readUInt32();
        }
        if (fieldsToRead < 24) {
            LOG_WARNING("TBC SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated in data fields (", fieldsToRead,
                        " of 24, entry=", data.entry, ")");
        }
    }

    if (data.type == 15) { // MO_TRANSPORT
        LOG_DEBUG("TBC GO query: MO_TRANSPORT entry=", data.entry,
                  " name=\"", data.name, "\" displayId=", data.displayId,
                  " taxiPathId=", data.data[0], " moveSpeed=", data.data[1]);
    } else {
        LOG_DEBUG("TBC GO query: ", data.name, " type=", data.type, " entry=", data.entry);
    }
    return true;
}

// ============================================================================
// TBC 2.4.3 guild roster parser
// Same rank structure as WotLK (variable rankCount + goldLimit + bank tabs),
// but NO gender byte per member (WotLK added it).
// ============================================================================

bool TbcPacketParsers::parseGuildRoster(network::Packet& packet, GuildRosterData& data) {
    if (packet.getSize() < 4) {
        LOG_ERROR("TBC SMSG_GUILD_ROSTER too small: ", packet.getSize());
        return false;
    }
    uint32_t numMembers = packet.readUInt32();

    // Safety cap — guilds rarely exceed 500 members; 1000 prevents excessive
    // memory allocation from malformed packets while covering all real cases
    const uint32_t MAX_GUILD_MEMBERS = 1000;
    if (numMembers > MAX_GUILD_MEMBERS) {
        LOG_WARNING("TBC GuildRoster: numMembers capped (requested=", numMembers, ")");
        numMembers = MAX_GUILD_MEMBERS;
    }

    data.motd = packet.readString();
    data.guildInfo = packet.readString();

    if (!packet.hasRemaining(4)) {
        LOG_WARNING("TBC GuildRoster: truncated before rankCount");
        data.ranks.clear();
        data.members.clear();
        return true;
    }

    uint32_t rankCount = packet.readUInt32();
    const uint32_t MAX_GUILD_RANKS = 20;
    if (rankCount > MAX_GUILD_RANKS) {
        LOG_WARNING("TBC GuildRoster: rankCount capped (requested=", rankCount, ")");
        rankCount = MAX_GUILD_RANKS;
    }

    data.ranks.resize(rankCount);
    for (uint32_t i = 0; i < rankCount; ++i) {
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("TBC GuildRoster: truncated rank at index ", i);
            break;
        }
        data.ranks[i].rights = packet.readUInt32();
        if (!packet.hasRemaining(4)) {
            data.ranks[i].goldLimit = 0;
        } else {
            data.ranks[i].goldLimit = packet.readUInt32();
        }
        // 6 bank tab flags + 6 bank tab items per day (guild banks added in TBC 2.3)
        for (int t = 0; t < 6; ++t) {
            if (!packet.hasRemaining(8)) break;
            packet.readUInt32(); // tabFlags
            packet.readUInt32(); // tabItemsPerDay
        }
    }

    data.members.resize(numMembers);
    for (uint32_t i = 0; i < numMembers; ++i) {
        if (!packet.hasRemaining(9)) {
            LOG_WARNING("TBC GuildRoster: truncated member at index ", i);
            break;
        }
        auto& m = data.members[i];
        m.guid = packet.readUInt64();
        m.online = (packet.readUInt8() != 0);

        if (packet.getReadPos() >= packet.getSize()) {
            m.name.clear();
        } else {
            m.name = packet.readString();
        }

        if (!packet.hasRemaining(1)) {
            m.rankIndex = 0;
            m.level = 1;
            m.classId = 0;
            m.gender = 0;
            m.zoneId = 0;
        } else {
            m.rankIndex = packet.readUInt32();
            if (!packet.hasRemaining(2)) {
                m.level = 1;
                m.classId = 0;
            } else {
                m.level = packet.readUInt8();
                m.classId = packet.readUInt8();
            }
            // TBC: NO gender byte (WotLK added it)
            m.gender = 0;
            if (!packet.hasRemaining(4)) {
                m.zoneId = 0;
            } else {
                m.zoneId = packet.readUInt32();
            }
        }

        if (!m.online) {
            if (!packet.hasRemaining(4)) {
                m.lastOnline = 0.0f;
            } else {
                m.lastOnline = packet.readFloat();
            }
        }

        if (packet.getReadPos() >= packet.getSize()) {
            m.publicNote.clear();
            m.officerNote.clear();
        } else {
            m.publicNote = packet.readString();
            if (packet.getReadPos() >= packet.getSize()) {
                m.officerNote.clear();
            } else {
                m.officerNote = packet.readString();
            }
        }
    }
    LOG_INFO("Parsed TBC SMSG_GUILD_ROSTER: ", numMembers, " members, motd=", data.motd);
    return true;
}

} // namespace game
} // namespace wowee
