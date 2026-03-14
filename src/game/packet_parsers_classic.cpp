#include "game/packet_parsers.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace game {

namespace {

bool hasFullPackedGuid(const network::Packet& packet) {
    if (packet.getReadPos() >= packet.getSize()) {
        return false;
    }

    const auto& rawData = packet.getData();
    const uint8_t mask = rawData[packet.getReadPos()];
    size_t guidBytes = 1;
    for (int bit = 0; bit < 8; ++bit) {
        if ((mask & (1u << bit)) != 0) {
            ++guidBytes;
        }
    }
    return packet.getSize() - packet.getReadPos() >= guidBytes;
}

} // namespace

// ============================================================================
// Classic 1.12.1 movement flag constants
// Key differences from TBC:
// - SPLINE_ENABLED at 0x00400000 (TBC/WotLK: 0x08000000)
// - No FLYING flag (flight was added in TBC)
// - ONTRANSPORT at 0x02000000 (not used for pitch in Classic)
// Same as TBC: ON_TRANSPORT=0x200, JUMPING=0x2000, SWIMMING=0x200000,
//              SPLINE_ELEVATION=0x04000000
// ============================================================================
namespace ClassicMoveFlags {
    constexpr uint32_t ONTRANSPORT      = 0x02000000;  // Gates transport data (vmangos authoritative)
    constexpr uint32_t JUMPING          = 0x00002000;  // Gates jump data
    constexpr uint32_t SWIMMING         = 0x00200000;  // Gates pitch
    constexpr uint32_t SPLINE_ENABLED   = 0x00400000;  // TBC/WotLK: 0x08000000
    constexpr uint32_t SPLINE_ELEVATION = 0x04000000;  // Same as TBC
}

// ============================================================================
// Classic parseMovementBlock
// Key differences from TBC:
// - NO moveFlags2 (TBC reads u8, WotLK reads u16)
// - SPLINE_ENABLED at 0x00400000 (not 0x08000000)
// - Transport data: NO timestamp (TBC adds u32 timestamp)
// - Pitch: only SWIMMING (no ONTRANSPORT secondary pitch, no FLYING)
// Same as TBC: u8 UpdateFlags, JUMPING=0x2000, 8 speeds, no pitchRate
// ============================================================================
bool ClassicPacketParsers::parseMovementBlock(network::Packet& packet, UpdateBlock& block) {
    // Validate minimum packet size for updateFlags byte
    if (packet.getReadPos() >= packet.getSize()) {
        LOG_WARNING("[Classic] Movement block packet too small (need at least 1 byte for updateFlags)");
        return false;
    }

    // Classic: UpdateFlags is uint8 (same as TBC)
    uint8_t updateFlags = packet.readUInt8();
    block.updateFlags = static_cast<uint16_t>(updateFlags);

    LOG_DEBUG("  [Classic] UpdateFlags: 0x", std::hex, (int)updateFlags, std::dec);

    const uint8_t UPDATEFLAG_LIVING       = 0x20;
    const uint8_t UPDATEFLAG_HAS_POSITION = 0x40;
    const uint8_t UPDATEFLAG_HAS_TARGET   = 0x04;
    const uint8_t UPDATEFLAG_TRANSPORT    = 0x02;
    const uint8_t UPDATEFLAG_LOWGUID      = 0x08;
    const uint8_t UPDATEFLAG_HIGHGUID     = 0x10;

    if (updateFlags & UPDATEFLAG_LIVING) {
        // Movement flags (u32 only — NO extra flags byte in Classic)
        uint32_t moveFlags = packet.readUInt32();
        /*uint32_t time =*/ packet.readUInt32();

        // Position
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [Classic] LIVING: (", block.x, ", ", block.y, ", ", block.z,
                  "), o=", block.orientation, " moveFlags=0x", std::hex, moveFlags, std::dec);

        // Transport data (Classic: ONTRANSPORT=0x02000000, no timestamp)
        if (moveFlags & ClassicMoveFlags::ONTRANSPORT) {
            block.onTransport = true;
            block.transportGuid = UpdateObjectParser::readPackedGuid(packet);
            block.transportX = packet.readFloat();
            block.transportY = packet.readFloat();
            block.transportZ = packet.readFloat();
            block.transportO = packet.readFloat();
            // Classic: NO transport timestamp (TBC adds u32 timestamp)
            // Classic: NO transport seat byte
        }

        // Pitch (Classic: only SWIMMING, no FLYING or ONTRANSPORT pitch)
        if (moveFlags & ClassicMoveFlags::SWIMMING) {
            /*float pitch =*/ packet.readFloat();
        }

        // Fall time (always present)
        /*uint32_t fallTime =*/ packet.readUInt32();

        // Jumping (Classic: JUMPING=0x2000, same as TBC)
        if (moveFlags & ClassicMoveFlags::JUMPING) {
            /*float jumpVelocity =*/ packet.readFloat();
            /*float jumpSinAngle =*/ packet.readFloat();
            /*float jumpCosAngle =*/ packet.readFloat();
            /*float jumpXYSpeed =*/ packet.readFloat();
        }

        // Spline elevation
        if (moveFlags & ClassicMoveFlags::SPLINE_ELEVATION) {
            /*float splineElevation =*/ packet.readFloat();
        }

        // Speeds (Classic: 6 values — no flight speeds, no pitchRate)
        // TBC added flying_speed + backwards_flying_speed (8 total)
        // WotLK added pitchRate (9 total)
        /*float walkSpeed =*/ packet.readFloat();
        float runSpeed = packet.readFloat();
        /*float runBackSpeed =*/ packet.readFloat();
        /*float swimSpeed =*/ packet.readFloat();
        /*float swimBackSpeed =*/ packet.readFloat();
        /*float turnRate =*/ packet.readFloat();

        block.runSpeed = runSpeed;
        block.moveFlags = moveFlags;

        // Spline data (Classic: SPLINE_ENABLED=0x00400000)
        if (moveFlags & ClassicMoveFlags::SPLINE_ENABLED) {
            uint32_t splineFlags = packet.readUInt32();
            LOG_DEBUG("  [Classic] Spline: flags=0x", std::hex, splineFlags, std::dec);

            if (splineFlags & 0x00010000) { // FINAL_POINT
                /*float finalX =*/ packet.readFloat();
                /*float finalY =*/ packet.readFloat();
                /*float finalZ =*/ packet.readFloat();
            } else if (splineFlags & 0x00020000) { // FINAL_TARGET
                /*uint64_t finalTarget =*/ packet.readUInt64();
            } else if (splineFlags & 0x00040000) { // FINAL_ANGLE
                /*float finalAngle =*/ packet.readFloat();
            }

            // Classic spline: timePassed, duration, id, nodes, finalNode (same as TBC)
            /*uint32_t timePassed =*/ packet.readUInt32();
            /*uint32_t duration =*/ packet.readUInt32();
            /*uint32_t splineId =*/ packet.readUInt32();

            uint32_t pointCount = packet.readUInt32();
            if (pointCount > 256) {
                static uint32_t badClassicSplineCount = 0;
                ++badClassicSplineCount;
                if (badClassicSplineCount <= 5 || (badClassicSplineCount % 100) == 0) {
                    LOG_WARNING("  [Classic] Spline pointCount=", pointCount,
                                " exceeds max, capping (occurrence=", badClassicSplineCount, ")");
                }
                pointCount = 0;
            }
            for (uint32_t i = 0; i < pointCount; i++) {
                /*float px =*/ packet.readFloat();
                /*float py =*/ packet.readFloat();
                /*float pz =*/ packet.readFloat();
            }

            // Classic: NO splineMode byte
            /*float endPointX =*/ packet.readFloat();
            /*float endPointY =*/ packet.readFloat();
            /*float endPointZ =*/ packet.readFloat();
        }
    }
    else if (updateFlags & UPDATEFLAG_HAS_POSITION) {
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [Classic] STATIONARY: (", block.x, ", ", block.y, ", ", block.z, ")");
    }

    // Target GUID
    if (updateFlags & UPDATEFLAG_HAS_TARGET) {
        /*uint64_t targetGuid =*/ UpdateObjectParser::readPackedGuid(packet);
    }

    // Transport time
    if (updateFlags & UPDATEFLAG_TRANSPORT) {
        /*uint32_t transportTime =*/ packet.readUInt32();
    }

    // Low GUID
    if (updateFlags & UPDATEFLAG_LOWGUID) {
        /*uint32_t lowGuid =*/ packet.readUInt32();
    }

    // High GUID
    if (updateFlags & UPDATEFLAG_HIGHGUID) {
        /*uint32_t highGuid =*/ packet.readUInt32();
    }

    return true;
}

// ============================================================================
// Classic writeMovementPayload
// Key differences from TBC:
// - NO flags2 byte (TBC writes u8)
// - Transport data: NO timestamp
// - Pitch: only SWIMMING (no ONTRANSPORT pitch)
// ============================================================================
void ClassicPacketParsers::writeMovementPayload(network::Packet& packet, const MovementInfo& info) {
    // Movement flags (uint32)
    packet.writeUInt32(info.flags);

    // Classic: NO flags2 byte (TBC has u8, WotLK has u16)

    // Timestamp
    packet.writeUInt32(info.time);

    // Position
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.x), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.y), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.z), sizeof(float));
    packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.orientation), sizeof(float));

    // Transport data (Classic ONTRANSPORT = 0x02000000, no timestamp)
    if (info.flags & ClassicMoveFlags::ONTRANSPORT) {
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

        // Classic: NO transport timestamp
        // Classic: NO transport seat byte
    }

    // Pitch (Classic: only SWIMMING)
    if (info.flags & ClassicMoveFlags::SWIMMING) {
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.pitch), sizeof(float));
    }

    // Fall time (always present)
    packet.writeUInt32(info.fallTime);

    // Jump data (Classic JUMPING = 0x2000)
    if (info.flags & ClassicMoveFlags::JUMPING) {
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpVelocity), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpSinAngle), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpCosAngle), sizeof(float));
        packet.writeBytes(reinterpret_cast<const uint8_t*>(&info.jumpXYSpeed), sizeof(float));
    }
}

// ============================================================================
// Classic buildMovementPacket
// Classic/TBC: client movement packets do NOT include PackedGuid prefix
// (WotLK added PackedGuid to client packets)
// ============================================================================
network::Packet ClassicPacketParsers::buildMovementPacket(LogicalOpcode opcode,
                                                           const MovementInfo& info,
                                                           uint64_t /*playerGuid*/) {
    network::Packet packet(wireOpcode(opcode));

    // Classic: NO PackedGuid prefix for client packets
    writeMovementPayload(packet, info);

    return packet;
}

// ============================================================================
// Classic buildCastSpell
// Vanilla 1.12.x: NO castCount prefix, NO castFlags byte
// Format: uint32 spellId + uint16 targetFlags + [PackedGuid if unit target]
// ============================================================================
network::Packet ClassicPacketParsers::buildCastSpell(uint32_t spellId, uint64_t targetGuid, uint8_t /*castCount*/) {
    network::Packet packet(wireOpcode(LogicalOpcode::CMSG_CAST_SPELL));

    packet.writeUInt32(spellId);

    // SpellCastTargets — vanilla/CMaNGOS uses uint16 target mask (WotLK uses uint32)
    if (targetGuid != 0) {
        packet.writeUInt16(0x02); // TARGET_FLAG_UNIT

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
        for (int i = 0; i < byteCount; ++i) {
            packet.writeUInt8(bytes[i]);
        }
    } else {
        packet.writeUInt16(0x00); // TARGET_FLAG_SELF
    }

    return packet;
}

// ============================================================================
// Classic CMSG_USE_ITEM
// Vanilla 1.12.x: bag(u8) + slot(u8) + spellIndex(u8) + SpellCastTargets(u16)
// NO spellId, itemGuid, glyphIndex, or castFlags fields (those are WotLK)
// ============================================================================
network::Packet ClassicPacketParsers::buildUseItem(uint8_t bagIndex, uint8_t slotIndex, uint64_t /*itemGuid*/, uint32_t /*spellId*/) {
    network::Packet packet(wireOpcode(LogicalOpcode::CMSG_USE_ITEM));
    packet.writeUInt8(bagIndex);
    packet.writeUInt8(slotIndex);
    packet.writeUInt8(0);       // spell_index (which item spell to trigger, usually 0)
    packet.writeUInt16(0x0000); // SpellCastTargets: TARGET_FLAG_SELF
    return packet;
}

// ============================================================================
// Classic parseSpellStart — Vanilla 1.12 SMSG_SPELL_START
//
// Key differences from TBC:
//   - GUIDs are PackedGuid (variable-length byte mask + non-zero bytes),
//     NOT full uint64 as in TBC/WotLK.
//   - castFlags is uint16 (NOT uint32 as in TBC/WotLK).
//   - SpellCastTargets uses uint16 targetFlags (NOT uint32 as in TBC).
//
// Format: PackedGuid(casterObj) + PackedGuid(casterUnit) + uint8(castCount)
//       + uint32(spellId) + uint16(castFlags) + uint32(castTime)
//       + uint16(targetFlags) [+ PackedGuid(unitTarget) if TARGET_FLAG_UNIT]
// ============================================================================
bool ClassicPacketParsers::parseSpellStart(network::Packet& packet, SpellStartData& data) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 2) return false;

    data.casterGuid = UpdateObjectParser::readPackedGuid(packet);
    if (rem() < 1) return false;
    data.casterUnit = UpdateObjectParser::readPackedGuid(packet);

    // uint8 castCount + uint32 spellId + uint16 castFlags + uint32 castTime = 11 bytes
    if (rem() < 11) return false;
    data.castCount = packet.readUInt8();
    data.spellId   = packet.readUInt32();
    data.castFlags = packet.readUInt16();   // uint16 in Vanilla (uint32 in TBC/WotLK)
    data.castTime  = packet.readUInt32();

    // SpellCastTargets: uint16 targetFlags in Vanilla (uint32 in TBC/WotLK)
    if (rem() < 2) return true;
    uint16_t targetFlags = packet.readUInt16();
    // TARGET_FLAG_UNIT (0x02) or TARGET_FLAG_OBJECT (0x800) carry a packed GUID
    if (((targetFlags & 0x02) || (targetFlags & 0x800)) && rem() >= 1) {
        data.targetGuid = UpdateObjectParser::readPackedGuid(packet);
    }

    LOG_DEBUG("[Classic] Spell start: spell=", data.spellId, " castTime=", data.castTime, "ms");
    return true;
}

// ============================================================================
// Classic parseSpellGo — Vanilla 1.12 SMSG_SPELL_GO
//
// Same GUID and castFlags format differences as parseSpellStart:
//   - GUIDs are PackedGuid (not full uint64)
//   - castFlags is uint16 (not uint32)
//   - Hit/miss target GUIDs are also PackedGuid in Vanilla
//
// Format: PackedGuid(casterObj) + PackedGuid(casterUnit) + uint8(castCount)
//       + uint32(spellId) + uint16(castFlags)
//       + uint8(hitCount) + [PackedGuid(hitTarget) × hitCount]
//       + uint8(missCount) + [PackedGuid(missTarget) + uint8(missType)] × missCount
// ============================================================================
bool ClassicPacketParsers::parseSpellGo(network::Packet& packet, SpellGoData& data) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 2) return false;

    data.casterGuid = UpdateObjectParser::readPackedGuid(packet);
    if (rem() < 1) return false;
    data.casterUnit = UpdateObjectParser::readPackedGuid(packet);

    // uint8 castCount + uint32 spellId + uint16 castFlags = 7 bytes
    if (rem() < 7) return false;
    data.castCount = packet.readUInt8();
    data.spellId   = packet.readUInt32();
    data.castFlags = packet.readUInt16();   // uint16 in Vanilla (uint32 in TBC/WotLK)

    // Hit targets
    if (rem() < 1) return true;
    data.hitCount = packet.readUInt8();
    // Cap hit count to prevent OOM from huge target lists
    if (data.hitCount > 128) {
        LOG_WARNING("[Classic] Spell go: hitCount capped (requested=", (int)data.hitCount, ")");
        data.hitCount = 128;
    }
    data.hitTargets.reserve(data.hitCount);
    for (uint8_t i = 0; i < data.hitCount && rem() >= 1; ++i) {
        data.hitTargets.push_back(UpdateObjectParser::readPackedGuid(packet));
    }
    // Check if we read all expected hits
    if (data.hitTargets.size() < data.hitCount) {
        LOG_WARNING("[Classic] Spell go: truncated hit targets at index ", (int)data.hitTargets.size(),
                    "/", (int)data.hitCount);
        data.hitCount = data.hitTargets.size();
    }

    // Miss targets
    if (rem() < 1) return true;
    data.missCount = packet.readUInt8();
    // Cap miss count to prevent OOM
    if (data.missCount > 128) {
        LOG_WARNING("[Classic] Spell go: missCount capped (requested=", (int)data.missCount, ")");
        data.missCount = 128;
    }
    data.missTargets.reserve(data.missCount);
    for (uint8_t i = 0; i < data.missCount && rem() >= 2; ++i) {
        SpellGoMissEntry m;
        m.targetGuid = UpdateObjectParser::readPackedGuid(packet);
        if (rem() < 1) break;
        m.missType = packet.readUInt8();
        if (m.missType == 11) {
            if (rem() < 5) break;
            (void)packet.readUInt32();
            (void)packet.readUInt8();
        }
        data.missTargets.push_back(m);
    }
    // Check if we read all expected misses
    if (data.missTargets.size() < data.missCount) {
        LOG_WARNING("[Classic] Spell go: truncated miss targets at index ", (int)data.missTargets.size(),
                    "/", (int)data.missCount);
        data.missCount = data.missTargets.size();
    }

    LOG_DEBUG("[Classic] Spell go: spell=", data.spellId, " hits=", (int)data.hitCount,
              " misses=", (int)data.missCount);
    return true;
}

// ============================================================================
// Classic parseAttackerStateUpdate — Vanilla 1.12 SMSG_ATTACKERSTATEUPDATE
//
// Identical to TBC format except GUIDs are PackedGuid (not full uint64).
// Format: uint32(hitInfo) + PackedGuid(attacker) + PackedGuid(target)
//       + int32(totalDamage) + uint8(subDamageCount)
//       + [per sub: uint32(schoolMask) + float(damage) + uint32(intDamage)
//                 + uint32(absorbed) + uint32(resisted)]
//       + uint32(victimState) + int32(overkill) [+ uint32(blocked)]
// ============================================================================
bool ClassicPacketParsers::parseAttackerStateUpdate(network::Packet& packet, AttackerStateUpdateData& data) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 5) return false;  // hitInfo(4) + at least GUID mask byte(1)

    data.hitInfo      = packet.readUInt32();
    data.attackerGuid = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla
    if (rem() < 1) return false;
    data.targetGuid   = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla

    if (rem() < 5) return false;  // int32 totalDamage + uint8 subDamageCount
    data.totalDamage    = static_cast<int32_t>(packet.readUInt32());
    data.subDamageCount = packet.readUInt8();

    for (uint8_t i = 0; i < data.subDamageCount && rem() >= 20; ++i) {
        SubDamage sub;
        sub.schoolMask = packet.readUInt32();
        sub.damage     = packet.readFloat();
        sub.intDamage  = packet.readUInt32();
        sub.absorbed   = packet.readUInt32();
        sub.resisted   = packet.readUInt32();
        data.subDamages.push_back(sub);
    }

    if (rem() < 8) return true;
    data.victimState = packet.readUInt32();
    data.overkill    = static_cast<int32_t>(packet.readUInt32());

    if (rem() >= 4) {
        data.blocked = packet.readUInt32();
    }

    LOG_DEBUG("[Classic] Melee hit: ", data.totalDamage, " damage",
              data.isCrit() ? " (CRIT)" : "",
              data.isMiss() ? " (MISS)" : "");
    return true;
}

// ============================================================================
// Classic parseSpellDamageLog — Vanilla 1.12 SMSG_SPELLNONMELEEDAMAGELOG
//
// Identical to TBC except GUIDs are PackedGuid (not full uint64).
// Format: PackedGuid(target) + PackedGuid(caster) + uint32(spellId)
//       + uint32(damage) + uint8(schoolMask) + uint32(absorbed) + uint32(resisted)
//       + uint8(periodicLog) + uint8(unused) + uint32(blocked) + uint32(flags)
// ============================================================================
bool ClassicPacketParsers::parseSpellDamageLog(network::Packet& packet, SpellDamageLogData& data) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 2 || !hasFullPackedGuid(packet)) return false;

    data.targetGuid   = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla
    if (rem() < 1 || !hasFullPackedGuid(packet)) return false;
    data.attackerGuid = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla

    // uint32(spellId) + uint32(damage) + uint8(schoolMask) + uint32(absorbed)
    // + uint32(resisted) + uint8 + uint8 + uint32(blocked) + uint32(flags) = 21 bytes
    if (rem() < 21) return false;
    data.spellId    = packet.readUInt32();
    data.damage     = packet.readUInt32();
    data.schoolMask = packet.readUInt8();
    data.absorbed   = packet.readUInt32();
    data.resisted   = packet.readUInt32();
    packet.readUInt8();    // periodicLog
    packet.readUInt8();    // unused
    packet.readUInt32();   // blocked
    uint32_t flags  = packet.readUInt32();
    data.isCrit     = (flags & 0x02) != 0;
    data.overkill   = 0;  // no overkill field in Vanilla (same as TBC)

    LOG_DEBUG("[Classic] Spell damage: spellId=", data.spellId, " dmg=", data.damage,
              data.isCrit ? " CRIT" : "");
    return true;
}

// ============================================================================
// Classic parseSpellHealLog — Vanilla 1.12 SMSG_SPELLHEALLOG
//
// Identical to TBC except GUIDs are PackedGuid (not full uint64).
// Format: PackedGuid(target) + PackedGuid(caster) + uint32(spellId)
//       + uint32(heal) + uint32(overheal) + uint8(crit)
// ============================================================================
bool ClassicPacketParsers::parseSpellHealLog(network::Packet& packet, SpellHealLogData& data) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 2 || !hasFullPackedGuid(packet)) return false;

    data.targetGuid = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla
    if (rem() < 1 || !hasFullPackedGuid(packet)) return false;
    data.casterGuid = UpdateObjectParser::readPackedGuid(packet); // PackedGuid in Vanilla

    if (rem() < 13) return false;  // uint32 + uint32 + uint32 + uint8 = 13 bytes
    data.spellId  = packet.readUInt32();
    data.heal     = packet.readUInt32();
    data.overheal = packet.readUInt32();
    data.isCrit   = (packet.readUInt8() != 0);

    LOG_DEBUG("[Classic] Spell heal: spellId=", data.spellId, " heal=", data.heal,
              data.isCrit ? " CRIT" : "");
    return true;
}

// ============================================================================
// Classic parseAuraUpdate — Vanilla 1.12 SMSG_AURA_UPDATE / SMSG_AURA_UPDATE_ALL
//
// Classic has SMSG_AURA_UPDATE (TBC does not — TBC uses a different aura system
// and the TBC override returns false with a warning).  Classic inherits TBC's
// override by default, so this override is needed to restore aura tracking.
//
// Classic aura flags differ from WotLK:
//   0x01/0x02/0x04 = effect indices active   (same as WotLK)
//   0x08 = CANCELABLE / NOT-NEGATIVE         (WotLK: 0x08 = NOT_CASTER)
//   0x10 = DURATION                          (WotLK: 0x20 = DURATION)
//   0x20 = NOT_CASTER                        (WotLK: no caster GUID at all if 0x08)
//   0x40 = POSITIVE                          (WotLK: 0x40 = EFFECT_AMOUNTS)
//
// Key differences from WotLK parser:
//   - No caster GUID field in Classic SMSG_AURA_UPDATE packets
//   - DURATION bit is 0x10, not 0x20
//   - No effect amounts field (WotLK 0x40 = EFFECT_AMOUNTS does not exist here)
//
// Format: PackedGuid(entity) + [uint8(slot) + uint32(spellId)
//         [+ uint8(flags) + uint8(level) + uint8(charges)
//          + [uint32(maxDuration) + uint32(duration) if flags & 0x10]]*
// ============================================================================
bool ClassicPacketParsers::parseAuraUpdate(network::Packet& packet, AuraUpdateData& data, bool isAll) {
    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 1) return false;

    data.guid = UpdateObjectParser::readPackedGuid(packet);

    while (rem() > 0) {
        if (rem() < 1) break;
        uint8_t slot = packet.readUInt8();
        if (rem() < 4) break;
        uint32_t spellId = packet.readUInt32();

        AuraSlot aura;
        if (spellId != 0) {
            aura.spellId = spellId;
            if (rem() < 3) { data.updates.push_back({slot, aura}); break; }
            aura.flags   = packet.readUInt8();
            aura.level   = packet.readUInt8();
            aura.charges = packet.readUInt8();

            // Classic DURATION flag is 0x10 (WotLK uses 0x20)
            if ((aura.flags & 0x10) && rem() >= 8) {
                aura.maxDurationMs = static_cast<int32_t>(packet.readUInt32());
                aura.durationMs    = static_cast<int32_t>(packet.readUInt32());
            }
            // No caster GUID field in Classic (WotLK added it gated by 0x08 NOT_CASTER)
            // No effect amounts field in Classic (WotLK added it gated by 0x40)
        }

        data.updates.push_back({slot, aura});
        if (!isAll) break;
    }

    LOG_DEBUG("[Classic] Aura update for 0x", std::hex, data.guid, std::dec,
              ": ", data.updates.size(), " slots");
    return true;
}

// ============================================================================
// Classic SMSG_NAME_QUERY_RESPONSE format (1.12 / vmangos):
//   uint64 guid (full, GetObjectGuid)
//   CString name
//   CString realmName (usually empty = single \0 byte)
//   uint32 race
//   uint32 gender
//   uint32 class
//
// TBC Variant A (inherited from TbcPacketParsers) skips the realmName CString,
// causing it to misread the uint32 race field (absorbs the realmName \0 byte
// as the low byte), producing race=0 and shifted gender/class values.
// ============================================================================
bool ClassicPacketParsers::parseNameQueryResponse(network::Packet& packet, NameQueryResponseData& data) {
    data = NameQueryResponseData{};

    auto rem = [&]() { return packet.getSize() - packet.getReadPos(); };
    if (rem() < 8) return false;

    data.guid = packet.readUInt64();     // full uint64, not PackedGuid
    data.name = packet.readString();     // null-terminated name
    if (rem() == 0) return !data.name.empty();

    data.realmName = packet.readString(); // null-terminated realm name (usually "")
    if (rem() < 12) return !data.name.empty();

    uint32_t race   = packet.readUInt32();
    uint32_t gender = packet.readUInt32();
    uint32_t cls    = packet.readUInt32();
    data.race    = static_cast<uint8_t>(race   & 0xFF);
    data.gender  = static_cast<uint8_t>(gender & 0xFF);
    data.classId = static_cast<uint8_t>(cls    & 0xFF);
    data.found   = 0;

    LOG_DEBUG("[Classic] Name query response: ", data.name,
              " (race=", (int)data.race, " gender=", (int)data.gender,
              " class=", (int)data.classId, ")");
    return !data.name.empty();
}

// ============================================================================
// Classic SMSG_CAST_FAILED: no castCount byte (added in TBC/WotLK)
// Format: spellId(u32) + result(u8)
// ============================================================================
bool ClassicPacketParsers::parseCastFailed(network::Packet& packet, CastFailedData& data) {
    data.castCount = 0;
    data.spellId = packet.readUInt32();
    uint8_t vanillaResult = packet.readUInt8();
    // Vanilla enum starts at 0=AFFECTING_COMBAT (no SUCCESS entry).
    // WotLK enum starts at 0=SUCCESS, 1=AFFECTING_COMBAT.
    // Shift +1 to align with WotLK result strings.
    data.result = vanillaResult + 1;
    LOG_DEBUG("[Classic] Cast failed: spell=", data.spellId, " vanillaResult=", (int)vanillaResult);
    return true;
}

// ============================================================================
// Classic SMSG_CAST_RESULT: same layout as parseCastFailed (spellId + result),
// but the result enum starts at 0=AFFECTING_COMBAT (no SUCCESS entry).
// Apply the same +1 shift used in parseCastFailed so the result codes
// align with WotLK's getSpellCastResultString table.
// ============================================================================
bool ClassicPacketParsers::parseCastResult(network::Packet& packet, uint32_t& spellId, uint8_t& result) {
    if (packet.getSize() - packet.getReadPos() < 5) return false;
    spellId = packet.readUInt32();
    uint8_t vanillaResult = packet.readUInt8();
    // Shift +1: Vanilla result 0=AFFECTING_COMBAT maps to WotLK result 1=AFFECTING_COMBAT
    result = vanillaResult + 1;
    LOG_DEBUG("[Classic] Cast result: spell=", spellId, " vanillaResult=", (int)vanillaResult);
    return true;
}

// ============================================================================
// Classic 1.12.1 parseCharEnum
// Differences from TBC:
// - Equipment: 20 items, but NO enchantment field per slot
//   Classic: displayId(u32) + inventoryType(u8) = 5 bytes/slot
//   TBC/WotLK: displayId(u32) + inventoryType(u8) + enchant(u32) = 9 bytes/slot
// - After flags: uint8 firstLogin (same as TBC)
// ============================================================================
bool ClassicPacketParsers::parseCharEnum(network::Packet& packet, CharEnumResponse& response) {
    // Validate minimum packet size for count byte
    if (packet.getSize() < 1) {
        LOG_ERROR("[Classic] SMSG_CHAR_ENUM packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    uint8_t count = packet.readUInt8();

    // Cap count to prevent excessive memory allocation
    constexpr uint8_t kMaxCharacters = 32;
    if (count > kMaxCharacters) {
        LOG_WARNING("[Classic] Character count ", (int)count, " exceeds max ", (int)kMaxCharacters,
                    ", capping");
        count = kMaxCharacters;
    }

    LOG_INFO("[Classic] Parsing SMSG_CHAR_ENUM: ", (int)count, " characters");

    response.characters.clear();
    response.characters.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        // Sanity check: ensure we have at least minimal data before reading next character
        // Minimum: guid(8) + name(1) + race(1) + class(1) + gender(1) + appearance(4)
        //          + facialFeatures(1) + level(1) + zone(4) + map(4) + pos(12) + guild(4)
        //          + flags(4) + firstLogin(1) + pet(12) + equipment(20*5)
        constexpr size_t kMinCharacterSize = 8 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 4 + 4 + 12 + 4 + 4 + 1 + 12 + 100;
        if (packet.getReadPos() + kMinCharacterSize > packet.getSize()) {
            LOG_WARNING("[Classic] Character enum packet truncated at character ", (int)(i + 1),
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

        // Classic: uint8 firstLogin (same as TBC)
        /*uint8_t firstLogin =*/ packet.readUInt8();

        // Pet data (always present)
        character.pet.displayModel = packet.readUInt32();
        character.pet.level = packet.readUInt32();
        character.pet.family = packet.readUInt32();

        // Equipment (Classic: 20 items, NO enchantment field)
        character.equipment.reserve(20);
        for (int j = 0; j < 20; ++j) {
            EquipmentItem item;
            item.displayModel = packet.readUInt32();
            item.inventoryType = packet.readUInt8();
            item.enchantment = 0;  // Classic has no enchant field in char enum
            character.equipment.push_back(item);
        }

        LOG_DEBUG("  Character ", (int)(i + 1), ": ", character.name,
                  " (", getRaceName(character.race), " ", getClassName(character.characterClass),
                  " level ", (int)character.level, " zone ", character.zoneId, ")");

        response.characters.push_back(character);
    }

    LOG_INFO("[Classic] Parsed ", response.characters.size(), " characters");
    return true;
}

// ============================================================================
// Classic 1.12.1 parseMessageChat
// Differences from WotLK:
// - NO uint32 unknown field after senderGuid
// - CHANNEL type: channelName + rank(u32) + senderGuid (not just channelName)
// - No ACHIEVEMENT/GUILD_ACHIEVEMENT types
// ============================================================================
bool ClassicPacketParsers::parseMessageChat(network::Packet& packet, MessageChatData& data) {
    if (packet.getSize() < 10) {
        LOG_ERROR("[Classic] SMSG_MESSAGECHAT packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    // Read chat type
    uint8_t typeVal = packet.readUInt8();
    data.type = static_cast<ChatType>(typeVal);

    // Read language
    uint32_t langVal = packet.readUInt32();
    data.language = static_cast<ChatLanguage>(langVal);

    // Classic: NO uint32 unknown field here (WotLK has one)

    // Type-specific data (matches CMaNGOS-Classic BuildChatPacket)
    switch (data.type) {
        case ChatType::MONSTER_EMOTE: {
            // nameLen(u32) + name + targetGuid(u64)
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

        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_YELL: {
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

    // Read message length
    uint32_t messageLen = packet.readUInt32();

    // Read message
    if (messageLen > 0 && messageLen < 8192) {
        data.message.resize(messageLen);
        for (uint32_t i = 0; i < messageLen; ++i) {
            data.message[i] = static_cast<char>(packet.readUInt8());
        }
        // Remove null terminator if present
        if (!data.message.empty() && data.message.back() == '\0') {
            data.message.pop_back();
        }
    }

    // Read chat tag
    if (packet.getReadPos() < packet.getSize()) {
        data.chatTag = packet.readUInt8();
    }

    LOG_DEBUG("[Classic] SMSG_MESSAGECHAT: type=", getChatTypeString(data.type),
             " sender=", data.senderName.empty() ? std::to_string(data.senderGuid) : data.senderName);

    return true;
}

// ============================================================================
// Classic CMSG_JOIN_CHANNEL / CMSG_LEAVE_CHANNEL
// Classic format: just string channelName + string password (no channelId/hasVoice/joinedByZone)
// ============================================================================

network::Packet ClassicPacketParsers::buildJoinChannel(const std::string& channelName, const std::string& password) {
    network::Packet packet(wireOpcode(Opcode::CMSG_JOIN_CHANNEL));
    packet.writeString(channelName);
    packet.writeString(password);
    LOG_DEBUG("[Classic] Built CMSG_JOIN_CHANNEL: channel=", channelName);
    return packet;
}

network::Packet ClassicPacketParsers::buildLeaveChannel(const std::string& channelName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LEAVE_CHANNEL));
    packet.writeString(channelName);
    LOG_DEBUG("[Classic] Built CMSG_LEAVE_CHANNEL: channel=", channelName);
    return packet;
}

// ============================================================================
// Classic guild roster parser
// Differences from WotLK:
// - No rankCount field (fixed 10 ranks, read rights only)
// - No per-rank bank tab data
// - No gender byte per member
// ============================================================================

bool ClassicPacketParsers::parseGuildRoster(network::Packet& packet, GuildRosterData& data) {
    if (packet.getSize() < 4) {
        LOG_ERROR("Classic SMSG_GUILD_ROSTER too small: ", packet.getSize());
        return false;
    }
    uint32_t numMembers = packet.readUInt32();
    data.motd = packet.readString();
    data.guildInfo = packet.readString();

    // Classic: fixed 10 ranks, just uint32 rights each (no goldLimit, no bank tabs)
    data.ranks.resize(10);
    for (int i = 0; i < 10; ++i) {
        data.ranks[i].rights = packet.readUInt32();
        data.ranks[i].goldLimit = 0;
    }

    data.members.resize(numMembers);
    for (uint32_t i = 0; i < numMembers; ++i) {
        auto& m = data.members[i];
        m.guid = packet.readUInt64();
        m.online = (packet.readUInt8() != 0);
        m.name = packet.readString();
        m.rankIndex = packet.readUInt32();
        m.level = packet.readUInt8();
        m.classId = packet.readUInt8();
        // Classic: NO gender byte
        m.gender = 0;
        m.zoneId = packet.readUInt32();
        if (!m.online) {
            m.lastOnline = packet.readFloat();
        }
        m.publicNote = packet.readString();
        m.officerNote = packet.readString();
    }
    LOG_INFO("Parsed Classic SMSG_GUILD_ROSTER: ", numMembers, " members");
    return true;
}

// ============================================================================
// Classic guild query response parser
// Differences from WotLK:
// - No trailing rankCount uint32
// ============================================================================

bool ClassicPacketParsers::parseGuildQueryResponse(network::Packet& packet, GuildQueryResponseData& data) {
    if (packet.getSize() < 8) {
        LOG_ERROR("Classic SMSG_GUILD_QUERY_RESPONSE too small: ", packet.getSize());
        return false;
    }
    data.guildId = packet.readUInt32();
    data.guildName = packet.readString();
    for (int i = 0; i < 10; ++i) {
        data.rankNames[i] = packet.readString();
    }
    data.emblemStyle = packet.readUInt32();
    data.emblemColor = packet.readUInt32();
    data.borderStyle = packet.readUInt32();
    data.borderColor = packet.readUInt32();
    data.backgroundColor = packet.readUInt32();
    // Classic: NO trailing rankCount
    data.rankCount = 10;
    LOG_INFO("Parsed Classic SMSG_GUILD_QUERY_RESPONSE: guild=", data.guildName);
    return true;
}

// ============================================================================
// GameObject Query — Classic has no extra strings before data[]
// WotLK has iconName + castBarCaption + unk1 between names and data[].
// Vanilla: entry, type, displayId, name[4], data[24]
// ============================================================================

bool ClassicPacketParsers::parseGameObjectQueryResponse(network::Packet& packet, GameObjectQueryResponseData& data) {
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("Classic SMSG_GAMEOBJECT_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    // High bit set means gameobject not found
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        data.name = "";
        return true;
    }

    // Validate minimum size for fixed fields: type(4) + displayId(4)
    if (packet.getSize() - packet.getReadPos() < 8) {
        LOG_ERROR("Classic SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated before names (entry=", data.entry, ")");
        return false;
    }

    data.type = packet.readUInt32();
    data.displayId = packet.readUInt32();
    // 4 name strings
    data.name = packet.readString();
    packet.readString();
    packet.readString();
    packet.readString();

    // Classic: data[24] comes immediately after names (no extra strings)
    size_t remaining = packet.getSize() - packet.getReadPos();
    if (remaining >= 24 * 4) {
        for (int i = 0; i < 24; i++) {
            data.data[i] = packet.readUInt32();
        }
        data.hasData = true;
    } else if (remaining > 0) {
        // Partial data field; read what we can
        uint32_t fieldsToRead = remaining / 4;
        for (uint32_t i = 0; i < fieldsToRead && i < 24; i++) {
            data.data[i] = packet.readUInt32();
        }
        if (fieldsToRead < 24) {
            LOG_WARNING("Classic SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated in data fields (", fieldsToRead,
                        " of 24 read, entry=", data.entry, ")");
        }
    }

    if (data.type == 15) { // MO_TRANSPORT
        LOG_DEBUG("Classic GO query: MO_TRANSPORT entry=", data.entry,
                  " name=\"", data.name, "\" displayId=", data.displayId,
                  " taxiPathId=", data.data[0], " moveSpeed=", data.data[1]);
    } else {
        LOG_DEBUG("Classic GO query: ", data.name, " type=", data.type, " entry=", data.entry);
    }
    return true;
}

// ============================================================================
// Gossip — Classic has no menuId, and quest items lack questFlags + isRepeatable
// ============================================================================

bool ClassicPacketParsers::parseGossipMessage(network::Packet& packet, GossipMessageData& data) {
    size_t remaining = packet.getSize() - packet.getReadPos();
    if (remaining < 8 + 4 + 4) {
        LOG_ERROR("Classic SMSG_GOSSIP_MESSAGE too small: ", remaining, " bytes");
        return false;
    }

    data.npcGuid = packet.readUInt64();
    // Classic: NO menuId field (WotLK adds uint32 menuId here)
    data.menuId = 0;
    data.titleTextId = packet.readUInt32();
    uint32_t optionCount = packet.readUInt32();

    // Cap option count to reasonable maximum
    constexpr uint32_t kMaxGossipOptions = 256;
    if (optionCount > kMaxGossipOptions) {
        LOG_WARNING("Classic SMSG_GOSSIP_MESSAGE optionCount=", optionCount, " exceeds max ",
                    kMaxGossipOptions, ", capping");
        optionCount = kMaxGossipOptions;
    }

    data.options.clear();
    data.options.reserve(optionCount);
    for (uint32_t i = 0; i < optionCount; ++i) {
        // Sanity check: ensure minimum bytes available for option (id(4)+icon(1)+isCoded(1)+text(1))
        remaining = packet.getSize() - packet.getReadPos();
        if (remaining < 7) {
            LOG_WARNING("Classic gossip option ", i, " truncated (", remaining, " bytes left)");
            break;
        }

        GossipOption opt;
        opt.id = packet.readUInt32();
        opt.icon = packet.readUInt8();
        opt.isCoded = (packet.readUInt8() != 0);
        // Classic/Vanilla: NO boxMoney or boxText fields (commented out in mangoszero)
        opt.boxMoney = 0;
        opt.text = packet.readString();
        opt.boxText = "";
        data.options.push_back(opt);
    }

    // Ensure we have at least 4 bytes for questCount
    remaining = packet.getSize() - packet.getReadPos();
    if (remaining < 4) {
        LOG_WARNING("Classic SMSG_GOSSIP_MESSAGE truncated before questCount");
        return data.options.size() > 0;  // Return true if we got at least some options
    }

    uint32_t questCount = packet.readUInt32();

    // Cap quest count to reasonable maximum
    constexpr uint32_t kMaxGossipQuests = 256;
    if (questCount > kMaxGossipQuests) {
        LOG_WARNING("Classic SMSG_GOSSIP_MESSAGE questCount=", questCount, " exceeds max ",
                    kMaxGossipQuests, ", capping");
        questCount = kMaxGossipQuests;
    }

    data.quests.clear();
    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        // Sanity check: ensure minimum bytes available for quest (id(4)+icon(4)+level(4)+title(1))
        remaining = packet.getSize() - packet.getReadPos();
        if (remaining < 13) {
            LOG_WARNING("Classic gossip quest ", i, " truncated (", remaining, " bytes left)");
            break;
        }

        GossipQuestItem quest;
        quest.questId = packet.readUInt32();
        quest.questIcon = packet.readUInt32();
        quest.questLevel = static_cast<int32_t>(packet.readUInt32());
        // Classic: NO questFlags, NO isRepeatable
        quest.questFlags = 0;
        quest.isRepeatable = 0;
        quest.title = normalizeWowTextTokens(packet.readString());
        data.quests.push_back(quest);
    }

    LOG_DEBUG("Classic Gossip: ", optionCount, " options, ", questCount, " quests");
    return true;
}

// ============================================================================
// Classic CMSG_SEND_MAIL — Vanilla 1.12 format
// Differences from WotLK:
// - Single uint64 itemGuid instead of uint8 attachmentCount + item array
// - Trailing uint64 unk3 + uint8 unk4 (clients > 1.9.4)
// ============================================================================
network::Packet ClassicPacketParsers::buildSendMail(uint64_t mailboxGuid,
                                                     const std::string& recipient,
                                                     const std::string& subject,
                                                     const std::string& body,
                                                     uint32_t money, uint32_t cod,
                                                     const std::vector<uint64_t>& itemGuids) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SEND_MAIL));
    packet.writeUInt64(mailboxGuid);
    packet.writeString(recipient);
    packet.writeString(subject);
    packet.writeString(body);
    packet.writeUInt32(0);       // stationery
    packet.writeUInt32(0);       // unknown
    // Vanilla supports only one item attachment (single uint64 GUID)
    uint64_t singleItemGuid = itemGuids.empty() ? 0 : itemGuids[0];
    packet.writeUInt64(singleItemGuid);
    packet.writeUInt32(money);
    packet.writeUInt32(cod);
    packet.writeUInt64(0);       // unk3 (clients > 1.9.4)
    packet.writeUInt8(0);        // unk4 (clients > 1.9.4)
    return packet;
}

// ============================================================================
// Classic SMSG_MAIL_LIST_RESULT — Vanilla 1.12 format (per vmangos)
// Key differences from WotLK:
// - uint8 count (not uint32 totalCount + uint8 shownCount)
// - No msgSize prefix per entry
// - Subject comes before item data
// - Single inline item (not attachment count + array)
// - uint8 stackCount (not uint32)
// - No enchantment array (single permanentEnchant uint32)
// ============================================================================
bool ClassicPacketParsers::parseMailList(network::Packet& packet,
                                         std::vector<MailMessage>& inbox) {
    size_t remaining = packet.getSize() - packet.getReadPos();
    if (remaining < 1) return false;

    uint8_t count = packet.readUInt8();
    LOG_INFO("SMSG_MAIL_LIST_RESULT (Classic): count=", (int)count);

    inbox.clear();
    inbox.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        remaining = packet.getSize() - packet.getReadPos();
        if (remaining < 5) {
            LOG_WARNING("Classic mail entry ", i, " truncated (", remaining, " bytes left)");
            break;
        }

        MailMessage msg;

        // vmangos HandleGetMailList format:
        // u32 messageId, u8 messageType, sender (guid or u32),
        // string subject, u32 itemTextId, u32 package, u32 stationery,
        // item fields (entry, enchant, randomProp, suffixFactor,
        //              u8 stackCount, u32 charges, u32 maxDur, u32 dur),
        // u32 money, u32 cod, u32 flags, float expirationTime,
        // u32 mailTemplateId (build-dependent)
        msg.messageId = packet.readUInt32();
        msg.messageType = packet.readUInt8();

        switch (msg.messageType) {
            case 0: msg.senderGuid = packet.readUInt64(); break;
            default: msg.senderEntry = packet.readUInt32(); break;
        }

        msg.subject = packet.readString();

        uint32_t itemTextId = packet.readUInt32();
        (void)itemTextId;
        packet.readUInt32(); // package (unused)
        msg.stationeryId = packet.readUInt32();

        // Single inline item (Vanilla: one item per mail)
        uint32_t itemEntry = packet.readUInt32();
        uint32_t permanentEnchant = packet.readUInt32();
        uint32_t randomPropertyId = packet.readUInt32();
        uint32_t suffixFactor = packet.readUInt32();
        uint8_t stackCount = packet.readUInt8();
        packet.readUInt32(); // charges
        uint32_t maxDurability = packet.readUInt32();
        uint32_t durability = packet.readUInt32();

        if (itemEntry != 0) {
            MailAttachment att;
            att.slot = 0;
            att.itemGuidLow = 0; // Not provided in Vanilla list
            att.itemId = itemEntry;
            att.enchantId = permanentEnchant;
            att.randomPropertyId = randomPropertyId;
            att.randomSuffix = suffixFactor;
            att.stackCount = stackCount;
            att.chargesOrDurability = durability;
            att.maxDurability = maxDurability;
            msg.attachments.push_back(att);
        }

        msg.money = packet.readUInt32();
        msg.cod = packet.readUInt32();
        msg.flags = packet.readUInt32();
        msg.expirationTime = packet.readFloat();
        msg.mailTemplateId = packet.readUInt32();

        msg.read = (msg.flags & 0x01) != 0;
        inbox.push_back(std::move(msg));
    }

    LOG_INFO("Parsed ", inbox.size(), " mail messages");
    return true;
}

// ============================================================================
// Classic CMSG_MAIL_TAKE_ITEM — Vanilla only sends mailboxGuid + mailId
// (no itemSlot — Vanilla only supports 1 item per mail)
// ============================================================================
network::Packet ClassicPacketParsers::buildMailTakeItem(uint64_t mailboxGuid,
                                                         uint32_t mailId,
                                                         uint32_t /*itemSlot*/) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_TAKE_ITEM));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    return packet;
}

// ============================================================================
// Classic CMSG_MAIL_DELETE — Vanilla only sends mailboxGuid + mailId
// (no mailTemplateId field)
// ============================================================================
network::Packet ClassicPacketParsers::buildMailDelete(uint64_t mailboxGuid,
                                                       uint32_t mailId,
                                                       uint32_t /*mailTemplateId*/) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_DELETE));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    return packet;
}

// ============================================================================
// Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE
// Vanilla has NO SoundOverrideSubclass, NO Flags2, NO ScalingStatDistribution,
// NO ScalingStatValue, and only 2 damage types (not 5).
// ============================================================================
network::Packet ClassicPacketParsers::buildItemQuery(uint32_t entry, uint64_t guid) {
    // Vanilla CMSG_ITEM_QUERY_SINGLE: uint32 entry + uint64 guid (same as WotLK)
    network::Packet packet(wireOpcode(Opcode::CMSG_ITEM_QUERY_SINGLE));
    packet.writeUInt32(entry);
    packet.writeUInt64(guid);
    LOG_DEBUG("[Classic] Built CMSG_ITEM_QUERY_SINGLE: entry=", entry, " guid=0x", std::hex, guid, std::dec);
    return packet;
}

bool ClassicPacketParsers::parseItemQueryResponse(network::Packet& packet, ItemQueryResponseData& data) {
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    // High bit set means item not found
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        return true;
    }

    // Validate minimum size for fixed fields: itemClass(4) + subClass(4) + 4 name strings + displayInfoId(4) + quality(4)
    if (packet.getSize() - packet.getReadPos() < 8) {
        LOG_ERROR("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before names (entry=", data.entry, ")");
        return false;
    }

    uint32_t itemClass = packet.readUInt32();
    uint32_t subClass = packet.readUInt32();
    // Vanilla: NO SoundOverrideSubclass

    data.itemClass = itemClass;
    data.subClass = subClass;
    data.subclassName = "";
    if (itemClass == 2) { // Weapon
        switch (subClass) {
            case 0: data.subclassName = "Axe"; break;
            case 1: data.subclassName = "Axe"; break;
            case 2: data.subclassName = "Bow"; break;
            case 3: data.subclassName = "Gun"; break;
            case 4: data.subclassName = "Mace"; break;
            case 5: data.subclassName = "Mace"; break;
            case 6: data.subclassName = "Polearm"; break;
            case 7: data.subclassName = "Sword"; break;
            case 8: data.subclassName = "Sword"; break;
            case 10: data.subclassName = "Staff"; break;
            case 13: data.subclassName = "Fist Weapon"; break;
            case 15: data.subclassName = "Dagger"; break;
            case 16: data.subclassName = "Thrown"; break;
            case 18: data.subclassName = "Crossbow"; break;
            case 19: data.subclassName = "Wand"; break;
            case 20: data.subclassName = "Fishing Pole"; break;
            default: data.subclassName = "Weapon"; break;
        }
    } else if (itemClass == 4) { // Armor
        switch (subClass) {
            case 0: data.subclassName = "Miscellaneous"; break;
            case 1: data.subclassName = "Cloth"; break;
            case 2: data.subclassName = "Leather"; break;
            case 3: data.subclassName = "Mail"; break;
            case 4: data.subclassName = "Plate"; break;
            case 6: data.subclassName = "Shield"; break;
            default: data.subclassName = "Armor"; break;
        }
    }

    // 4 name strings
    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4

    data.displayInfoId = packet.readUInt32();
    data.quality = packet.readUInt32();

    // Validate minimum size for fixed fields: Flags(4) + BuyPrice(4) + SellPrice(4) + inventoryType(4)
    if (packet.getSize() - packet.getReadPos() < 16) {
        LOG_ERROR("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before inventoryType (entry=", data.entry, ")");
        return false;
    }

    data.itemFlags = packet.readUInt32(); // Flags
    // Vanilla: NO Flags2
    packet.readUInt32(); // BuyPrice
    data.sellPrice = packet.readUInt32(); // SellPrice

    data.inventoryType = packet.readUInt32();

    // Validate minimum size for remaining fixed fields: 13×4 = 52 bytes
    if (packet.getSize() - packet.getReadPos() < 52) {
        LOG_ERROR("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before stats (entry=", data.entry, ")");
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

    // Vanilla: 10 stat pairs, NO statsCount prefix (10×8 = 80 bytes)
    if (packet.getSize() - packet.getReadPos() < 80) {
        LOG_WARNING("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated in stats section (entry=", data.entry, ")");
        // Read what we can
    }
    for (uint32_t i = 0; i < 10; i++) {
        if (packet.getSize() - packet.getReadPos() < 8) {
            LOG_WARNING("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: stat ", i, " truncated (entry=", data.entry, ")");
            break;
        }
        uint32_t statType = packet.readUInt32();
        int32_t statValue = static_cast<int32_t>(packet.readUInt32());
        if (statType != 0) {
            switch (statType) {
                case 3: data.agility = statValue; break;
                case 4: data.strength = statValue; break;
                case 5: data.intellect = statValue; break;
                case 6: data.spirit = statValue; break;
                case 7: data.stamina = statValue; break;
                default:
                    if (statValue != 0)
                        data.extraStats.push_back({statType, statValue});
                    break;
            }
        }
    }

    // Vanilla: NO ScalingStatDistribution, NO ScalingStatValue

    // Vanilla: 5 damage types (same count as WotLK)
    bool haveWeaponDamage = false;
    for (int i = 0; i < 5; i++) {
        // Each damage entry is dmgMin(4) + dmgMax(4) + damageType(4) = 12 bytes
        if (packet.getSize() - packet.getReadPos() < 12) {
            LOG_WARNING("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: damage ", i, " truncated (entry=", data.entry, ")");
            break;
        }
        float dmgMin = packet.readFloat();
        float dmgMax = packet.readFloat();
        uint32_t damageType = packet.readUInt32();
        if (!haveWeaponDamage && dmgMax > 0.0f) {
            // Prefer physical damage (type 0) when present.
            if (damageType == 0 || data.damageMax <= 0.0f) {
                data.damageMin = dmgMin;
                data.damageMax = dmgMax;
                haveWeaponDamage = (damageType == 0);
            }
        }
    }

    // Validate minimum size for armor field (4 bytes)
    if (packet.getSize() - packet.getReadPos() < 4) {
        LOG_WARNING("Classic SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before armor (entry=", data.entry, ")");
        return true;  // Have core fields; armor is important but optional
    }
    data.armor = static_cast<int32_t>(packet.readUInt32());

    // Remaining tail can vary by core. Read resistances + delay when present.
    if (packet.getSize() - packet.getReadPos() >= 28) {
        data.holyRes   = static_cast<int32_t>(packet.readUInt32()); // HolyRes
        data.fireRes   = static_cast<int32_t>(packet.readUInt32()); // FireRes
        data.natureRes = static_cast<int32_t>(packet.readUInt32()); // NatureRes
        data.frostRes  = static_cast<int32_t>(packet.readUInt32()); // FrostRes
        data.shadowRes = static_cast<int32_t>(packet.readUInt32()); // ShadowRes
        data.arcaneRes = static_cast<int32_t>(packet.readUInt32()); // ArcaneRes
        data.delayMs = packet.readUInt32();
    }

    // AmmoType + RangedModRange (2 fields, 8 bytes)
    if (packet.getSize() - packet.getReadPos() >= 8) {
        packet.readUInt32(); // AmmoType
        packet.readFloat();  // RangedModRange
    }

    // 2 item spells in Vanilla (3 fields each: SpellId, Trigger, Charges)
    // Actually vanilla has 5 spells: SpellId, Trigger, Charges, Cooldown, Category, CatCooldown = 24 bytes each
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

    // Description (flavor/lore text)
    if (packet.getReadPos() < packet.getSize())
        data.description = packet.readString();

    // Post-description: PageText, LanguageID, PageMaterial, StartQuest
    if (packet.getReadPos() + 16 <= packet.getSize()) {
        packet.readUInt32(); // PageText
        packet.readUInt32(); // LanguageID
        packet.readUInt32(); // PageMaterial
        data.startQuestId = packet.readUInt32(); // StartQuest
    }

    data.valid = !data.name.empty();
    LOG_DEBUG("[Classic] Item query response: ", data.name, " (quality=", data.quality,
             " invType=", data.inventoryType, " stack=", data.maxStack, ")");
    return true;
}

// ============================================================================
// Turtle WoW (build 7234) parseMovementBlock
//
// Turtle WoW is a heavily modified vanilla (1.12.1) server.  Through hex dump
// analysis the wire format is nearly identical to Classic with one key addition:
//
//   LIVING section:
//     moveFlags       u32     (NO moveFlags2 — confirmed by position alignment)
//     time            u32
//     position        4×float
//     transport       guarded by moveFlags & 0x02000000 (Classic flag)
//                     packed GUID + 4 floats + u32 timestamp (TBC-style addition)
//     pitch           guarded by SWIMMING (0x200000)
//     fallTime        u32
//     jump data       guarded by JUMPING  (0x2000)
//     splineElev      guarded by 0x04000000
//     speeds          6 floats (walk/run/runBack/swim/swimBack/turnRate)
//     spline          guarded by 0x00400000 (Classic flag) OR 0x08000000 (TBC flag)
//
//   Tail (same as Classic):
//     LOWGUID  → 1×u32
//     HIGHGUID → 1×u32
//
// The ONLY confirmed difference from pure Classic is:
//   Transport data includes a u32 timestamp after the 4 transport floats
//   (Classic omits this; TBC/WotLK include it).  Without this, entities on
//   transports cause a 4-byte desync that cascades to later blocks.
// ============================================================================
namespace TurtleMoveFlags {
    constexpr uint32_t ONTRANSPORT      = 0x02000000;  // Classic transport flag
    constexpr uint32_t JUMPING          = 0x00002000;
    constexpr uint32_t SWIMMING         = 0x00200000;
    constexpr uint32_t SPLINE_ELEVATION = 0x04000000;
    constexpr uint32_t SPLINE_CLASSIC   = 0x00400000;  // Classic spline enabled
    constexpr uint32_t SPLINE_TBC       = 0x08000000;  // TBC spline enabled
}

bool TurtlePacketParsers::parseMovementBlock(network::Packet& packet, UpdateBlock& block) {
    uint8_t updateFlags = packet.readUInt8();
    block.updateFlags = static_cast<uint16_t>(updateFlags);

    LOG_DEBUG("  [Turtle] UpdateFlags: 0x", std::hex, (int)updateFlags, std::dec);

    const uint8_t UPDATEFLAG_LIVING       = 0x20;
    const uint8_t UPDATEFLAG_HAS_POSITION = 0x40;
    const uint8_t UPDATEFLAG_HAS_TARGET   = 0x04;
    const uint8_t UPDATEFLAG_TRANSPORT    = 0x02;
    const uint8_t UPDATEFLAG_LOWGUID      = 0x08;
    const uint8_t UPDATEFLAG_HIGHGUID     = 0x10;

    if (updateFlags & UPDATEFLAG_LIVING) {
        size_t livingStart = packet.getReadPos();

        uint32_t moveFlags = packet.readUInt32();
        // Turtle: NO moveFlags2 (confirmed by hex dump — positions are only correct without it)
        /*uint32_t time =*/ packet.readUInt32();

        // Position
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [Turtle] LIVING: (", block.x, ", ", block.y, ", ", block.z,
                  "), o=", block.orientation, " moveFlags=0x", std::hex, moveFlags, std::dec);

        // Transport — Classic flag position 0x02000000
        if (moveFlags & TurtleMoveFlags::ONTRANSPORT) {
            block.onTransport = true;
            block.transportGuid = UpdateObjectParser::readPackedGuid(packet);
            block.transportX = packet.readFloat();
            block.transportY = packet.readFloat();
            block.transportZ = packet.readFloat();
            block.transportO = packet.readFloat();
            /*uint32_t transportTime =*/ packet.readUInt32();  // Turtle adds TBC-style timestamp
        }

        // Pitch (swimming only, Classic-style)
        if (moveFlags & TurtleMoveFlags::SWIMMING) {
            /*float pitch =*/ packet.readFloat();
        }

        // Fall time (always present)
        /*uint32_t fallTime =*/ packet.readUInt32();

        // Jump data
        if (moveFlags & TurtleMoveFlags::JUMPING) {
            /*float jumpVelocity =*/ packet.readFloat();
            /*float jumpSinAngle =*/ packet.readFloat();
            /*float jumpCosAngle =*/ packet.readFloat();
            /*float jumpXYSpeed =*/ packet.readFloat();
        }

        // Spline elevation
        if (moveFlags & TurtleMoveFlags::SPLINE_ELEVATION) {
            /*float splineElevation =*/ packet.readFloat();
        }

        // Turtle: 6 speeds (same as Classic — no flight speeds)
        float walkSpeed = packet.readFloat();
        float runSpeed = packet.readFloat();
        float runBackSpeed = packet.readFloat();
        float swimSpeed = packet.readFloat();
        float swimBackSpeed = packet.readFloat();
        float turnRate = packet.readFloat();

        block.runSpeed = runSpeed;

        LOG_DEBUG("  [Turtle] Speeds: walk=", walkSpeed, " run=", runSpeed,
                  " runBack=", runBackSpeed, " swim=", swimSpeed,
                  " swimBack=", swimBackSpeed, " turn=", turnRate);

        // Spline data — check both Classic (0x00400000) and TBC (0x08000000) flag positions
        bool hasSpline = (moveFlags & TurtleMoveFlags::SPLINE_CLASSIC) ||
                         (moveFlags & TurtleMoveFlags::SPLINE_TBC);
        if (hasSpline) {
            uint32_t splineFlags = packet.readUInt32();
            LOG_DEBUG("  [Turtle] Spline: flags=0x", std::hex, splineFlags, std::dec);

            if (splineFlags & 0x00010000) {
                packet.readFloat(); packet.readFloat(); packet.readFloat();
            } else if (splineFlags & 0x00020000) {
                packet.readUInt64();
            } else if (splineFlags & 0x00040000) {
                packet.readFloat();
            }

            /*uint32_t timePassed =*/ packet.readUInt32();
            /*uint32_t duration =*/ packet.readUInt32();
            /*uint32_t splineId =*/ packet.readUInt32();

            uint32_t pointCount = packet.readUInt32();
            if (pointCount > 256) {
                static uint32_t badTurtleSplineCount = 0;
                ++badTurtleSplineCount;
                if (badTurtleSplineCount <= 5 || (badTurtleSplineCount % 100) == 0) {
                    LOG_WARNING("  [Turtle] Spline pointCount=", pointCount,
                                " exceeds max, capping (occurrence=", badTurtleSplineCount, ")");
                }
                pointCount = 0;
            }
            for (uint32_t i = 0; i < pointCount; i++) {
                packet.readFloat(); packet.readFloat(); packet.readFloat();
            }

            // End point
            packet.readFloat(); packet.readFloat(); packet.readFloat();
        }

        LOG_DEBUG("  [Turtle] LIVING block consumed ", packet.getReadPos() - livingStart,
                  " bytes, readPos now=", packet.getReadPos());
    }
    else if (updateFlags & UPDATEFLAG_HAS_POSITION) {
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  [Turtle] STATIONARY: (", block.x, ", ", block.y, ", ", block.z, ")");
    }

    // Target GUID
    if (updateFlags & UPDATEFLAG_HAS_TARGET) {
        /*uint64_t targetGuid =*/ UpdateObjectParser::readPackedGuid(packet);
    }

    // Transport time
    if (updateFlags & UPDATEFLAG_TRANSPORT) {
        /*uint32_t transportTime =*/ packet.readUInt32();
    }

    // Low GUID — Classic-style: 1×u32 (NOT TBC's 2×u32)
    if (updateFlags & UPDATEFLAG_LOWGUID) {
        /*uint32_t lowGuid =*/ packet.readUInt32();
    }

    // High GUID — 1×u32
    if (updateFlags & UPDATEFLAG_HIGHGUID) {
        /*uint32_t highGuid =*/ packet.readUInt32();
    }

    return true;
}

bool TurtlePacketParsers::parseMonsterMove(network::Packet& packet, MonsterMoveData& data) {
    // Turtle realms can emit both vanilla-like and WotLK-like monster move bodies.
    // Try the canonical Turtle/vanilla parser first, then fall back to WotLK layout.
    size_t start = packet.getReadPos();
    if (MonsterMoveParser::parseVanilla(packet, data)) {
        return true;
    }

    packet.setReadPos(start);
    if (MonsterMoveParser::parse(packet, data)) {
        LOG_DEBUG("[Turtle] SMSG_MONSTER_MOVE parsed via WotLK fallback layout");
        return true;
    }

    packet.setReadPos(start);
    return false;
}

// ============================================================================
// Classic/Vanilla quest giver status
//
// Vanilla sends status as uint32 with different enum values:
//   0=NONE, 1=UNAVAILABLE, 2=CHAT, 3=INCOMPLETE, 4=REWARD_REP, 5=AVAILABLE
// WotLK uses uint8 with:
//   0=NONE, 1=UNAVAILABLE, 5=INCOMPLETE, 6=REWARD_REP, 7=AVAILABLE_LOW, 8=AVAILABLE, 10=REWARD
//
// Read uint32, translate to WotLK enum values.
// ============================================================================
uint8_t ClassicPacketParsers::readQuestGiverStatus(network::Packet& packet) {
    uint32_t vanillaStatus = packet.readUInt32();
    switch (vanillaStatus) {
        case 0: return 0;  // NONE
        case 1: return 1;  // UNAVAILABLE
        case 2: return 0;  // CHAT → NONE (no marker)
        case 3: return 5;  // INCOMPLETE → WotLK INCOMPLETE
        case 4: return 6;  // REWARD_REP → WotLK REWARD_REP
        case 5: return 8;  // AVAILABLE → WotLK AVAILABLE
        case 6: return 10; // REWARD → WotLK REWARD
        default: return 0;
    }
}

// ============================================================================
// Classic CMSG_QUESTGIVER_QUERY_QUEST — Vanilla 1.12 format
// WotLK appends a trailing unk1 byte; Vanilla servers don't expect it and
// some reject or misparse the 13-byte packet, preventing quest details from
// being sent back.  Classic format: guid(8) + questId(4) = 12 bytes.
// ============================================================================
network::Packet ClassicPacketParsers::buildQueryQuestPacket(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_QUERY_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    // No trailing unk byte (WotLK-only field)
    return packet;
}

network::Packet ClassicPacketParsers::buildAcceptQuestPacket(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    // Classic/Turtle: no trailing unk1 uint32
    return packet;
}

// ============================================================================
// ClassicPacketParsers::parseCreatureQueryResponse
//
// Classic 1.12 SMSG_CREATURE_QUERY_RESPONSE lacks the iconName CString field
// that TBC 2.4.3 and WotLK 3.3.5a include between subName and typeFlags.
// Without this override, the TBC/WotLK parser reads typeFlags bytes as the
// iconName string, shifting typeFlags/creatureType/family/rank by 1-4 bytes.
// ============================================================================
bool ClassicPacketParsers::parseCreatureQueryResponse(network::Packet& packet,
                                                       CreatureQueryResponseData& data) {
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("Classic SMSG_CREATURE_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        data.name = "";
        return true;
    }

    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4
    data.subName = packet.readString();
    // NOTE: NO iconName field in Classic 1.12 — goes straight to typeFlags
    if (packet.getReadPos() + 16 > packet.getSize()) {
        LOG_WARNING("Classic SMSG_CREATURE_QUERY_RESPONSE: truncated at typeFlags (entry=", data.entry, ")");
        data.typeFlags = 0;
        data.creatureType = 0;
        data.family = 0;
        data.rank = 0;
        return true;  // Have name/sub fields; base fields are important but optional
    }
    data.typeFlags    = packet.readUInt32();
    data.creatureType = packet.readUInt32();
    data.family       = packet.readUInt32();
    data.rank         = packet.readUInt32();

    LOG_DEBUG("Classic SMSG_CREATURE_QUERY_RESPONSE: ", data.name, " type=", data.creatureType,
              " rank=", data.rank);
    return true;
}

} // namespace game
} // namespace wowee
