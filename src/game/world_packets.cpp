#include "game/world_packets.hpp"
#include "game/packet_parsers.hpp"
#include "game/opcodes.hpp"
#include "game/character.hpp"
#include "auth/crypto.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <zlib.h>

namespace {
    inline uint32_t bswap32(uint32_t v) {
        return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >> 8)
             | ((v & 0x0000FF00u) << 8)  | ((v & 0x000000FFu) << 24);
    }
    inline uint16_t bswap16(uint16_t v) {
        return static_cast<uint16_t>(((v & 0xFF00u) >> 8) | ((v & 0x00FFu) << 8));
    }

    const char* updateTypeName(wowee::game::UpdateType type) {
        using wowee::game::UpdateType;
        switch (type) {
            case UpdateType::VALUES: return "VALUES";
            case UpdateType::MOVEMENT: return "MOVEMENT";
            case UpdateType::CREATE_OBJECT: return "CREATE_OBJECT";
            case UpdateType::CREATE_OBJECT2: return "CREATE_OBJECT2";
            case UpdateType::OUT_OF_RANGE_OBJECTS: return "OUT_OF_RANGE_OBJECTS";
            case UpdateType::NEAR_OBJECTS: return "NEAR_OBJECTS";
            default: return "UNKNOWN";
        }
    }
}

namespace wowee {
namespace game {

std::string normalizeWowTextTokens(std::string text) {
    if (text.empty()) return text;

    size_t pos = 0;
    while ((pos = text.find('$', pos)) != std::string::npos) {
        if (pos + 1 >= text.size()) break;
        const char code = text[pos + 1];
        if (code == 'b' || code == 'B') {
            text.replace(pos, 2, "\n");
            ++pos;
        } else {
            ++pos;
        }
    }

    pos = 0;
    while ((pos = text.find("|n", pos)) != std::string::npos) {
        text.replace(pos, 2, "\n");
        ++pos;
    }
    pos = 0;
    while ((pos = text.find("|N", pos)) != std::string::npos) {
        text.replace(pos, 2, "\n");
        ++pos;
    }

    return text;
}

network::Packet AuthSessionPacket::build(uint32_t build,
                                          const std::string& accountName,
                                          uint32_t clientSeed,
                                          const std::vector<uint8_t>& sessionKey,
                                          uint32_t serverSeed,
                                          uint32_t realmId) {
    if (sessionKey.size() != 40) {
        LOG_ERROR("Invalid session key size: ", sessionKey.size(), " (expected 40)");
    }

    // Convert account name to uppercase
    std::string upperAccount = accountName;
    std::transform(upperAccount.begin(), upperAccount.end(),
                   upperAccount.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    LOG_INFO("Building CMSG_AUTH_SESSION for account: ", upperAccount);

    // Compute authentication hash
    auto authHash = computeAuthHash(upperAccount, clientSeed, serverSeed, sessionKey);

    LOG_DEBUG("  Build: ", build);
    LOG_DEBUG("  Client seed: 0x", std::hex, clientSeed, std::dec);
    LOG_DEBUG("  Server seed: 0x", std::hex, serverSeed, std::dec);
    LOG_DEBUG("  Auth hash: ", authHash.size(), " bytes");

    // Create packet (opcode will be added by WorldSocket)
    network::Packet packet(wireOpcode(Opcode::CMSG_AUTH_SESSION));

    bool isTbc = (build <= 8606);  // TBC 2.4.3 = 8606, WotLK starts at 11159+

    if (isTbc) {
        // TBC 2.4.3 format (6 fields):
        // Build, ServerID, Account, ClientSeed, Digest, AddonInfo
        packet.writeUInt32(build);
        packet.writeUInt32(realmId);           // server_id
        packet.writeString(upperAccount);
        packet.writeUInt32(clientSeed);
    } else {
        // WotLK 3.3.5a format (11 fields):
        // Build, LoginServerID, Account, LoginServerType, LocalChallenge,
        // RegionID, BattlegroupID, RealmID, DosResponse, Digest, AddonInfo
        packet.writeUInt32(build);
        packet.writeUInt32(0);                 // LoginServerID
        packet.writeString(upperAccount);
        packet.writeUInt32(0);                 // LoginServerType
        packet.writeUInt32(clientSeed);
        // AzerothCore ignores these fields; other cores may validate them.
        // Use 0 for maximum compatibility.
        packet.writeUInt32(0);                 // RegionID
        packet.writeUInt32(0);                 // BattlegroupID
        packet.writeUInt32(realmId);           // RealmID
        LOG_DEBUG("  Realm ID: ", realmId);
        packet.writeUInt32(0);                 // DOS response (uint64)
        packet.writeUInt32(0);
    }

    // Authentication hash/digest (20 bytes)
    packet.writeBytes(authHash.data(), authHash.size());

    // Addon info - compressed block
    // Format differs between expansions:
    //   Vanilla/TBC (CMaNGOS): while-loop of {string name, uint8 flags, uint32 modulusCRC, uint32 urlCRC}
    //   WotLK (AzerothCore): uint32 addonCount + {string name, uint8 enabled, uint32 crc, uint32 unk} + uint32 clientTime
    std::vector<uint8_t> addonData;
    if (isTbc) {
        // Vanilla/TBC: each addon entry = null-terminated name + uint8 flags + uint32 modulusCRC + uint32 urlCRC
        // Send standard Blizzard addons that CMaNGOS anticheat expects for fingerprinting
        static const char* vanillaAddons[] = {
            "Blizzard_AuctionUI", "Blizzard_BattlefieldMinimap", "Blizzard_BindingUI",
            "Blizzard_CombatText", "Blizzard_CraftUI", "Blizzard_GMSurveyUI",
            "Blizzard_InspectUI", "Blizzard_MacroUI", "Blizzard_RaidUI",
            "Blizzard_TalentUI", "Blizzard_TradeSkillUI", "Blizzard_TrainerUI"
        };
        static const uint32_t standardModulusCRC = 0x4C1C776D;
        for (const char* name : vanillaAddons) {
            // string (null-terminated)
            size_t len = strlen(name);
            addonData.insert(addonData.end(), reinterpret_cast<const uint8_t*>(name),
                             reinterpret_cast<const uint8_t*>(name) + len + 1);
            // uint8 flags = 1 (enabled)
            addonData.push_back(0x01);
            // uint32 modulusCRC (little-endian)
            addonData.push_back(static_cast<uint8_t>(standardModulusCRC & 0xFF));
            addonData.push_back(static_cast<uint8_t>((standardModulusCRC >> 8) & 0xFF));
            addonData.push_back(static_cast<uint8_t>((standardModulusCRC >> 16) & 0xFF));
            addonData.push_back(static_cast<uint8_t>((standardModulusCRC >> 24) & 0xFF));
            // uint32 urlCRC = 0
            addonData.push_back(0); addonData.push_back(0);
            addonData.push_back(0); addonData.push_back(0);
        }
    } else {
        // WotLK: uint32 addonCount + entries + uint32 clientTime
        // Send 0 addons
        addonData = { 0, 0, 0, 0,  // addonCount = 0
                      0, 0, 0, 0 }; // clientTime = 0
    }
    uint32_t decompressedSize = static_cast<uint32_t>(addonData.size());

    // Compress with zlib
    uLongf compressedSize = compressBound(decompressedSize);
    std::vector<uint8_t> compressed(compressedSize);
    int ret = compress(compressed.data(), &compressedSize, addonData.data(), decompressedSize);
    if (ret == Z_OK) {
        compressed.resize(compressedSize);
        // Write decompressedSize, then compressed bytes
        packet.writeUInt32(decompressedSize);
        packet.writeBytes(compressed.data(), compressed.size());
        LOG_DEBUG("Addon info: decompressedSize=", decompressedSize,
                  " compressedSize=", compressedSize, " addons=",
                  isTbc ? "12 vanilla" : "0 wotlk");
    } else {
        LOG_ERROR("zlib compress failed with code: ", ret);
        packet.writeUInt32(0);
    }

    LOG_INFO("CMSG_AUTH_SESSION packet built: ", packet.getSize(), " bytes");

    // Dump full packet for protocol debugging
    LOG_DEBUG("CMSG_AUTH_SESSION full dump:\n",
              core::toHexString(packet.getData().data(), packet.getData().size(), true));

    return packet;
}

std::vector<uint8_t> AuthSessionPacket::computeAuthHash(
    const std::string& accountName,
    uint32_t clientSeed,
    uint32_t serverSeed,
    const std::vector<uint8_t>& sessionKey) {

    // Build hash input:
    // account_name + [0,0,0,0] + client_seed + server_seed + session_key

    std::vector<uint8_t> hashInput;
    hashInput.reserve(accountName.size() + 4 + 4 + 4 + sessionKey.size());

    // Account name (as bytes)
    hashInput.insert(hashInput.end(), accountName.begin(), accountName.end());

    // 4 null bytes
    for (int i = 0; i < 4; ++i) {
        hashInput.push_back(0);
    }

    // Client seed (little-endian)
    hashInput.push_back(clientSeed & 0xFF);
    hashInput.push_back((clientSeed >> 8) & 0xFF);
    hashInput.push_back((clientSeed >> 16) & 0xFF);
    hashInput.push_back((clientSeed >> 24) & 0xFF);

    // Server seed (little-endian)
    hashInput.push_back(serverSeed & 0xFF);
    hashInput.push_back((serverSeed >> 8) & 0xFF);
    hashInput.push_back((serverSeed >> 16) & 0xFF);
    hashInput.push_back((serverSeed >> 24) & 0xFF);

    // Session key (40 bytes)
    hashInput.insert(hashInput.end(), sessionKey.begin(), sessionKey.end());

    // Diagnostic: dump auth hash inputs for debugging AUTH_REJECT
    LOG_DEBUG("AUTH HASH: account='", accountName, "' clientSeed=0x", std::hex, clientSeed,
              " serverSeed=0x", serverSeed, std::dec);
    LOG_DEBUG("AUTH HASH: sessionKey=", core::toHexString(sessionKey.data(), sessionKey.size()));
    LOG_DEBUG("AUTH HASH: input(", hashInput.size(), ")=", core::toHexString(hashInput.data(), hashInput.size()));

    // Compute SHA1 hash
    auto result = auth::Crypto::sha1(hashInput);
    LOG_DEBUG("AUTH HASH: digest=", core::toHexString(result.data(), result.size()));

    return result;
}

bool AuthChallengeParser::parse(network::Packet& packet, AuthChallengeData& data) {
    // SMSG_AUTH_CHALLENGE format varies by expansion:
    //   TBC 2.4.3:    uint32 serverSeed                      (4 bytes)
    //   WotLK 3.3.5a: uint32 one + uint32 serverSeed + seeds (40 bytes)

    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_AUTH_CHALLENGE packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    if (packet.getSize() <= 4) {
        // Original vanilla/TBC format: just the server seed (4 bytes)
        data.unknown1 = 0;
        data.serverSeed = packet.readUInt32();
        LOG_INFO("SMSG_AUTH_CHALLENGE: TBC format (", packet.getSize(), " bytes)");
    } else if (packet.getSize() < 40) {
        // Vanilla with encryption seeds (36 bytes): serverSeed + 32 bytes seeds
        // No "unknown1" prefix — first uint32 IS the server seed
        data.unknown1 = 0;
        data.serverSeed = packet.readUInt32();
        LOG_INFO("SMSG_AUTH_CHALLENGE: Classic+seeds format (", packet.getSize(), " bytes)");
    } else {
        // WotLK format (40+ bytes): unknown1 + serverSeed + 32 bytes encryption seeds
        data.unknown1 = packet.readUInt32();
        data.serverSeed = packet.readUInt32();
        LOG_INFO("SMSG_AUTH_CHALLENGE: WotLK format (", packet.getSize(), " bytes)");
        LOG_DEBUG("  Unknown1: 0x", std::hex, data.unknown1, std::dec);
    }

    LOG_DEBUG("  Server seed: 0x", std::hex, data.serverSeed, std::dec);

    return true;
}

bool AuthResponseParser::parse(network::Packet& packet, AuthResponseData& response) {
    // SMSG_AUTH_RESPONSE format:
    // uint8 result

    if (packet.getSize() < 1) {
        LOG_ERROR("SMSG_AUTH_RESPONSE packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    uint8_t resultCode = packet.readUInt8();
    response.result = static_cast<AuthResult>(resultCode);

    LOG_INFO("Parsed SMSG_AUTH_RESPONSE: ", getAuthResultString(response.result));

    return true;
}

const char* getAuthResultString(AuthResult result) {
    switch (result) {
        case AuthResult::OK:
            return "OK - Authentication successful";
        case AuthResult::FAILED:
            return "FAILED - Authentication failed";
        case AuthResult::REJECT:
            return "REJECT - Connection rejected";
        case AuthResult::BAD_SERVER_PROOF:
            return "BAD_SERVER_PROOF - Invalid server proof";
        case AuthResult::UNAVAILABLE:
            return "UNAVAILABLE - Server unavailable";
        case AuthResult::SYSTEM_ERROR:
            return "SYSTEM_ERROR - System error occurred";
        case AuthResult::BILLING_ERROR:
            return "BILLING_ERROR - Billing error";
        case AuthResult::BILLING_EXPIRED:
            return "BILLING_EXPIRED - Subscription expired";
        case AuthResult::VERSION_MISMATCH:
            return "VERSION_MISMATCH - Client version mismatch";
        case AuthResult::UNKNOWN_ACCOUNT:
            return "UNKNOWN_ACCOUNT - Account not found";
        case AuthResult::INCORRECT_PASSWORD:
            return "INCORRECT_PASSWORD - Wrong password";
        case AuthResult::SESSION_EXPIRED:
            return "SESSION_EXPIRED - Session has expired";
        case AuthResult::SERVER_SHUTTING_DOWN:
            return "SERVER_SHUTTING_DOWN - Server is shutting down";
        case AuthResult::ALREADY_LOGGING_IN:
            return "ALREADY_LOGGING_IN - Already logging in";
        case AuthResult::LOGIN_SERVER_NOT_FOUND:
            return "LOGIN_SERVER_NOT_FOUND - Can't contact login server";
        case AuthResult::WAIT_QUEUE:
            return "WAIT_QUEUE - Waiting in queue";
        case AuthResult::BANNED:
            return "BANNED - Account is banned";
        case AuthResult::ALREADY_ONLINE:
            return "ALREADY_ONLINE - Character already logged in";
        case AuthResult::NO_TIME:
            return "NO_TIME - No game time remaining";
        case AuthResult::DB_BUSY:
            return "DB_BUSY - Database is busy";
        case AuthResult::SUSPENDED:
            return "SUSPENDED - Account is suspended";
        case AuthResult::PARENTAL_CONTROL:
            return "PARENTAL_CONTROL - Parental controls active";
        case AuthResult::LOCKED_ENFORCED:
            return "LOCKED_ENFORCED - Account is locked";
        default:
            return "UNKNOWN - Unknown result code";
    }
}

// ============================================================
// Character Creation
// ============================================================

network::Packet CharCreatePacket::build(const CharCreateData& data) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CHAR_CREATE));

    // Convert nonbinary gender to server-compatible value (servers only support male/female)
    Gender serverGender = toServerGender(data.gender);

    packet.writeString(data.name);  // null-terminated name
    packet.writeUInt8(static_cast<uint8_t>(data.race));
    packet.writeUInt8(static_cast<uint8_t>(data.characterClass));
    packet.writeUInt8(static_cast<uint8_t>(serverGender));
    packet.writeUInt8(data.skin);
    packet.writeUInt8(data.face);
    packet.writeUInt8(data.hairStyle);
    packet.writeUInt8(data.hairColor);
    packet.writeUInt8(data.facialHair);
    packet.writeUInt8(0);  // outfitId, always 0
    // Turtle WoW / 1.12.1 clients send 4 extra zero bytes after outfitId.
    // Servers may validate packet length and silently drop undersized packets.
    packet.writeUInt32(0);

    LOG_DEBUG("Built CMSG_CHAR_CREATE: name=", data.name,
              " race=", static_cast<int>(data.race),
              " class=", static_cast<int>(data.characterClass),
              " gender=", static_cast<int>(data.gender),
              " (server gender=", static_cast<int>(serverGender), ")",
              " skin=", static_cast<int>(data.skin),
              " face=", static_cast<int>(data.face),
              " hair=", static_cast<int>(data.hairStyle),
              " hairColor=", static_cast<int>(data.hairColor),
              " facial=", static_cast<int>(data.facialHair));

    // Dump full packet for protocol debugging
    LOG_DEBUG("CMSG_CHAR_CREATE full dump: ",
              core::toHexString(packet.getData().data(), packet.getData().size(), true));

    return packet;
}

bool CharCreateResponseParser::parse(network::Packet& packet, CharCreateResponseData& data) {
    // Validate minimum packet size: result(1)
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("SMSG_CHAR_CREATE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.result = static_cast<CharCreateResult>(packet.readUInt8());
    LOG_INFO("SMSG_CHAR_CREATE result: ", static_cast<int>(data.result));
    return true;
}

network::Packet CharEnumPacket::build() {
    // CMSG_CHAR_ENUM has no body - just the opcode
    network::Packet packet(wireOpcode(Opcode::CMSG_CHAR_ENUM));

    LOG_DEBUG("Built CMSG_CHAR_ENUM packet (no body)");

    return packet;
}

bool CharEnumParser::parse(network::Packet& packet, CharEnumResponse& response) {
    // Upfront validation: count(1) + at least minimal character data
    if (!packet.hasRemaining(1)) return false;

    // Read character count
    uint8_t count = packet.readUInt8();

    LOG_INFO("Parsing SMSG_CHAR_ENUM: ", static_cast<int>(count), " characters");

    response.characters.clear();
    response.characters.reserve(count);

    for (uint8_t i = 0; i < count; ++i) {
        Character character;

        // Validate minimum bytes for this character entry before reading:
        // GUID(8) + name(>=1 for empty string) + race(1) + class(1) + gender(1) +
        // appearanceBytes(4) + facialFeatures(1) + level(1) + zoneId(4) + mapId(4) +
        // x(4) + y(4) + z(4) + guildId(4) + flags(4) + customization(4) + unknown(1) +
        // petDisplayModel(4) + petLevel(4) + petFamily(4) + 23items*(dispModel(4)+invType(1)+enchant(4)) = 207 bytes
        const size_t minCharacterSize = 8 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 4 + 4 + 4 + (23 * 9);
        if (!packet.hasRemaining(minCharacterSize)) {
            LOG_WARNING("CharEnumParser: truncated character at index ", static_cast<int>(i));
            break;
        }

        // Read GUID (8 bytes, little-endian)
        character.guid = packet.readUInt64();

        // Read name (null-terminated string) - validate before reading
        if (!packet.hasData()) {
            LOG_WARNING("CharEnumParser: no bytes for name at index ", static_cast<int>(i));
            break;
        }
        character.name = packet.readString();

        // Validate remaining bytes before reading fixed-size fields
        if (!packet.hasRemaining(1)) {
            LOG_WARNING("CharEnumParser: truncated before race/class/gender at index ", static_cast<int>(i));
            character.race = Race::HUMAN;
            character.characterClass = Class::WARRIOR;
            character.gender = Gender::MALE;
        } else {
            // Read race, class, gender
            character.race = static_cast<Race>(packet.readUInt8());
            if (!packet.hasRemaining(1)) {
                character.characterClass = Class::WARRIOR;
                character.gender = Gender::MALE;
            } else {
                character.characterClass = static_cast<Class>(packet.readUInt8());
                if (!packet.hasRemaining(1)) {
                    character.gender = Gender::MALE;
                } else {
                    character.gender = static_cast<Gender>(packet.readUInt8());
                }
            }
        }

        // Validate before reading appearance data
        if (!packet.hasRemaining(4)) {
            character.appearanceBytes = 0;
            character.facialFeatures = 0;
        } else {
            // Read appearance data
            character.appearanceBytes = packet.readUInt32();
            if (!packet.hasRemaining(1)) {
                character.facialFeatures = 0;
            } else {
                character.facialFeatures = packet.readUInt8();
            }
        }

        // Read level
        if (!packet.hasRemaining(1)) {
            character.level = 1;
        } else {
            character.level = packet.readUInt8();
        }

        // Read location
        if (!packet.hasRemaining(12)) {
            character.zoneId = 0;
            character.mapId = 0;
            character.x = 0.0f;
            character.y = 0.0f;
            character.z = 0.0f;
        } else {
            character.zoneId = packet.readUInt32();
            character.mapId = packet.readUInt32();
            character.x = packet.readFloat();
            character.y = packet.readFloat();
            character.z = packet.readFloat();
        }

        // Read affiliations
        if (!packet.hasRemaining(4)) {
            character.guildId = 0;
        } else {
            character.guildId = packet.readUInt32();
        }

        // Read flags
        if (!packet.hasRemaining(4)) {
            character.flags = 0;
        } else {
            character.flags = packet.readUInt32();
        }

        // Skip customization flag (uint32) and unknown byte
        if (!packet.hasRemaining(4)) {
            // Customization missing, skip unknown
        } else {
            packet.readUInt32();  // Customization
            if (!packet.hasRemaining(1)) {
                // Unknown missing
            } else {
                packet.readUInt8();   // Unknown
            }
        }

        // Read pet data (always present, even if no pet)
        if (!packet.hasRemaining(12)) {
            character.pet.displayModel = 0;
            character.pet.level = 0;
            character.pet.family = 0;
        } else {
            character.pet.displayModel = packet.readUInt32();
            character.pet.level = packet.readUInt32();
            character.pet.family = packet.readUInt32();
        }

        // Read equipment (23 items)
        character.equipment.reserve(23);
        for (int j = 0; j < 23; ++j) {
            if (!packet.hasRemaining(9)) break;
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

    LOG_INFO("Successfully parsed ", response.characters.size(), " characters");

    return true;
}

network::Packet PlayerLoginPacket::build(uint64_t characterGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PLAYER_LOGIN));

    // Write character GUID (8 bytes, little-endian)
    packet.writeUInt64(characterGuid);

    LOG_INFO("Built CMSG_PLAYER_LOGIN packet");
    LOG_INFO("  Character GUID: 0x", std::hex, characterGuid, std::dec);

    return packet;
}

bool LoginVerifyWorldParser::parse(network::Packet& packet, LoginVerifyWorldData& data) {
    // SMSG_LOGIN_VERIFY_WORLD format (WoW 3.3.5a):
    // uint32 mapId
    // float x, y, z (position)
    // float orientation

    if (packet.getSize() < 20) {
        LOG_ERROR("SMSG_LOGIN_VERIFY_WORLD packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    data.mapId = packet.readUInt32();
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();
    data.orientation = packet.readFloat();

    LOG_INFO("Parsed SMSG_LOGIN_VERIFY_WORLD:");
    LOG_INFO("  Map ID: ", data.mapId);
    LOG_INFO("  Position: (", data.x, ", ", data.y, ", ", data.z, ")");
    LOG_INFO("  Orientation: ", data.orientation, " radians");

    return true;
}

bool AccountDataTimesParser::parse(network::Packet& packet, AccountDataTimesData& data) {
    // Common layouts seen in the wild:
    // - WotLK-like: uint32 serverTime, uint8 unk, uint32 mask, uint32[up to 8] slotTimes
    // - Older/variant: uint32 serverTime, uint8 unk, uint32[up to 8] slotTimes
    // Some servers only send a subset of slots.
    if (packet.getSize() < 5) {
        LOG_ERROR("SMSG_ACCOUNT_DATA_TIMES packet too small: ", packet.getSize(),
                  " bytes (need at least 5)");
        return false;
    }

    for (uint32_t& t : data.accountDataTimes) {
        t = 0;
    }
    data.serverTime = packet.readUInt32();
    data.unknown = packet.readUInt8();

    size_t remaining = packet.getRemainingSize();
    uint32_t mask = 0xFF;
    if (remaining >= 4 && ((remaining - 4) % 4) == 0) {
        // Treat first dword as slot mask when payload shape matches.
        mask = packet.readUInt32();
    }
    remaining = packet.getRemainingSize();
    size_t slotWords = std::min<size_t>(8, remaining / 4);

    LOG_DEBUG("Parsed SMSG_ACCOUNT_DATA_TIMES:");
    LOG_DEBUG("  Server time: ", data.serverTime);
    LOG_DEBUG("  Unknown: ", static_cast<int>(data.unknown));
    LOG_DEBUG("  Mask: 0x", std::hex, mask, std::dec, " slotsInPacket=", slotWords);

    for (size_t i = 0; i < slotWords; ++i) {
        data.accountDataTimes[i] = packet.readUInt32();
        if (data.accountDataTimes[i] != 0 || ((mask & (1u << i)) != 0)) {
            LOG_DEBUG("  Data slot ", i, ": ", data.accountDataTimes[i]);
        }
    }
    if (packet.getReadPos() != packet.getSize()) {
        LOG_DEBUG("  AccountDataTimes trailing bytes: ", packet.getRemainingSize());
        packet.skipAll();
    }

    return true;
}

bool MotdParser::parse(network::Packet& packet, MotdData& data) {
    // SMSG_MOTD format (WoW 3.3.5a):
    // uint32 lineCount
    // string[lineCount] lines (null-terminated strings)

    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_MOTD packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    uint32_t lineCount = packet.readUInt32();

    // Cap lineCount to prevent unbounded memory allocation
    const uint32_t MAX_MOTD_LINES = 64;
    if (lineCount > MAX_MOTD_LINES) {
        LOG_WARNING("MotdParser: lineCount capped (requested=", lineCount, ")");
        lineCount = MAX_MOTD_LINES;
    }

    LOG_INFO("Parsed SMSG_MOTD: ", lineCount, " line(s)");

    data.lines.clear();
    data.lines.reserve(lineCount);

    for (uint32_t i = 0; i < lineCount; ++i) {
        // Validate at least 1 byte available for the string
        if (!packet.hasData()) {
            LOG_WARNING("MotdParser: truncated at line ", i + 1);
            break;
        }
        std::string line = packet.readString();
        data.lines.push_back(line);
        LOG_DEBUG("  MOTD[", i + 1, "]: ", line);
    }

    return true;
}

network::Packet PingPacket::build(uint32_t sequence, uint32_t latency) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PING));

    // Write sequence number (uint32, little-endian)
    packet.writeUInt32(sequence);

    // Write latency (uint32, little-endian, in milliseconds)
    packet.writeUInt32(latency);

    LOG_DEBUG("Built CMSG_PING packet");
    LOG_DEBUG("  Sequence: ", sequence);
    LOG_DEBUG("  Latency: ", latency, " ms");

    return packet;
}

bool PongParser::parse(network::Packet& packet, PongData& data) {
    // SMSG_PONG format (WoW 3.3.5a):
    // uint32 sequence (echoed from CMSG_PING)

    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_PONG packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    data.sequence = packet.readUInt32();

    LOG_DEBUG("Parsed SMSG_PONG:");
    LOG_DEBUG("  Sequence: ", data.sequence);

    return true;
}

void MovementPacket::writeMovementPayload(network::Packet& packet, const MovementInfo& info) {
    // Movement packet format (WoW 3.3.5a) payload:
    // uint32 flags
    // uint16 flags2
    // uint32 time
    // float x, y, z
    // float orientation

    // Write movement flags
    packet.writeUInt32(info.flags);
    packet.writeUInt16(info.flags2);

    // Write timestamp
    packet.writeUInt32(info.time);

    // Write position
    packet.writeFloat(info.x);
    packet.writeFloat(info.y);
    packet.writeFloat(info.z);

    // Write orientation
    packet.writeFloat(info.orientation);

    // Write transport data if on transport.
    // 3.3.5a ordering: transport block appears before pitch/fall/jump.
    if (info.hasFlag(MovementFlags::ONTRANSPORT)) {
        // Write packed transport GUID
        packet.writePackedGuid(info.transportGuid);

        // Write transport local position
        packet.writeFloat(info.transportX);
        packet.writeFloat(info.transportY);
        packet.writeFloat(info.transportZ);
        packet.writeFloat(info.transportO);

        // Write transport time
        packet.writeUInt32(info.transportTime);

        // Transport seat is always present in ONTRANSPORT movement info.
        packet.writeUInt8(static_cast<uint8_t>(info.transportSeat));

        // Optional second transport time for interpolated movement.
        if (info.flags2 & 0x0400) { // MOVEMENTFLAG2_INTERPOLATED_MOVEMENT
            packet.writeUInt32(info.transportTime2);
        }
    }

    // Write pitch if swimming/flying
    if (info.hasFlag(MovementFlags::SWIMMING) || info.hasFlag(MovementFlags::FLYING)) {
        packet.writeFloat(info.pitch);
    }

    // Fall time is ALWAYS present in the packet (server reads it unconditionally).
    // Jump velocity/angle data is only present when FALLING flag is set.
    packet.writeUInt32(info.fallTime);

    if (info.hasFlag(MovementFlags::FALLING)) {
        packet.writeFloat(info.jumpVelocity);
        packet.writeFloat(info.jumpSinAngle);
        packet.writeFloat(info.jumpCosAngle);
        packet.writeFloat(info.jumpXYSpeed);
    }
}

network::Packet MovementPacket::build(Opcode opcode, const MovementInfo& info, uint64_t playerGuid) {
    network::Packet packet(wireOpcode(opcode));

    // Movement packet format (WoW 3.3.5a):
    // packed GUID + movement payload
    packet.writePackedGuid(playerGuid);
    writeMovementPayload(packet, info);

    // Detailed hex dump for debugging
    static int mvLog = 5;
    if (mvLog-- > 0) {
        const auto& raw = packet.getData();
        std::string hex;
        for (size_t i = 0; i < raw.size(); i++) {
            char b[4]; snprintf(b, sizeof(b), "%02x ", raw[i]);
            hex += b;
        }
        LOG_DEBUG("MOVEPKT opcode=0x", std::hex, wireOpcode(opcode), std::dec,
                 " guid=0x", std::hex, playerGuid, std::dec,
                 " payload=", raw.size(), " bytes",
                 " flags=0x", std::hex, info.flags, std::dec,
                 " flags2=0x", std::hex, info.flags2, std::dec,
                 " pos=(", info.x, ",", info.y, ",", info.z, ",", info.orientation, ")",
                 " fallTime=", info.fallTime,
                 (info.hasFlag(MovementFlags::ONTRANSPORT) ?
                  " ONTRANSPORT guid=0x" + std::to_string(info.transportGuid) +
                  " localPos=(" + std::to_string(info.transportX) + "," +
                  std::to_string(info.transportY) + "," + std::to_string(info.transportZ) + ")" : ""));
        LOG_DEBUG("MOVEPKT hex: ", hex);
    }

    return packet;
}

bool UpdateObjectParser::parseMovementBlock(network::Packet& packet, UpdateBlock& block) {
    // WoW 3.3.5a UPDATE_OBJECT movement block structure:
    // 1. UpdateFlags (1 byte, sometimes 2)
    // 2. Movement data depends on update flags

    auto rem = [&]() -> size_t { return packet.getRemainingSize(); };
    if (rem() < 2) return false;

    // Update flags (3.3.5a uses 2 bytes for flags)
    uint16_t updateFlags = packet.readUInt16();
    block.updateFlags = updateFlags;

    LOG_DEBUG("  UpdateFlags: 0x", std::hex, updateFlags, std::dec);

    // Log transport-related flag combinations
    if (updateFlags & 0x0002) { // UPDATEFLAG_TRANSPORT
        static int transportFlagLogCount = 0;
        if (transportFlagLogCount < 12) {
            LOG_INFO("  Transport flags detected: 0x", std::hex, updateFlags, std::dec,
                     " (TRANSPORT=", !!(updateFlags & 0x0002),
                     ", POSITION=", !!(updateFlags & 0x0100),
                     ", ROTATION=", !!(updateFlags & 0x0200),
                     ", STATIONARY=", !!(updateFlags & 0x0040), ")");
            transportFlagLogCount++;
        } else {
            LOG_DEBUG("  Transport flags detected: 0x", std::hex, updateFlags, std::dec);
        }
    }

    // UpdateFlags bit meanings:
    // 0x0001 = UPDATEFLAG_SELF
    // 0x0002 = UPDATEFLAG_TRANSPORT
    // 0x0004 = UPDATEFLAG_HAS_TARGET
    // 0x0008 = UPDATEFLAG_LOWGUID
    // 0x0010 = UPDATEFLAG_HIGHGUID
    // 0x0020 = UPDATEFLAG_LIVING
    // 0x0040 = UPDATEFLAG_STATIONARY_POSITION
    // 0x0080 = UPDATEFLAG_VEHICLE
    // 0x0100 = UPDATEFLAG_POSITION (transport)
    // 0x0200 = UPDATEFLAG_ROTATION

    const uint16_t UPDATEFLAG_LIVING = 0x0020;
    const uint16_t UPDATEFLAG_STATIONARY_POSITION = 0x0040;
    const uint16_t UPDATEFLAG_HAS_TARGET = 0x0004;
    const uint16_t UPDATEFLAG_TRANSPORT = 0x0002;
    const uint16_t UPDATEFLAG_POSITION = 0x0100;
    const uint16_t UPDATEFLAG_VEHICLE = 0x0080;
    const uint16_t UPDATEFLAG_ROTATION = 0x0200;
    const uint16_t UPDATEFLAG_LOWGUID = 0x0008;
    const uint16_t UPDATEFLAG_HIGHGUID = 0x0010;

    if (updateFlags & UPDATEFLAG_LIVING) {
        // Minimum: moveFlags(4)+moveFlags2(2)+time(4)+position(16)+fallTime(4)+speeds(36) = 66
        if (rem() < 66) return false;

        // Full movement block for living units
        uint32_t moveFlags = packet.readUInt32();
        uint16_t moveFlags2 = packet.readUInt16();
        /*uint32_t time =*/ packet.readUInt32();

        // Position
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  LIVING movement: (", block.x, ", ", block.y, ", ", block.z,
                  "), o=", block.orientation, " moveFlags=0x", std::hex, moveFlags, std::dec);

        // Transport data (if on transport)
        if (moveFlags & 0x00000200) { // MOVEMENTFLAG_ONTRANSPORT
            if (rem() < 1) return false;
            block.onTransport = true;
            block.transportGuid = packet.readPackedGuid();
            if (rem() < 21) return false; // 4 floats + uint32 + uint8
            block.transportX = packet.readFloat();
            block.transportY = packet.readFloat();
            block.transportZ = packet.readFloat();
            block.transportO = packet.readFloat();
            /*uint32_t tTime =*/ packet.readUInt32();
            /*int8_t tSeat =*/ packet.readUInt8();

            LOG_DEBUG("  OnTransport: guid=0x", std::hex, block.transportGuid, std::dec,
                      " offset=(", block.transportX, ", ", block.transportY, ", ", block.transportZ, ")");

            if (moveFlags2 & 0x0400) { // MOVEMENTFLAG2_INTERPOLATED_MOVEMENT
                if (rem() < 4) return false;
                /*uint32_t tTime2 =*/ packet.readUInt32();
            }
        }

        // Swimming/flying pitch
        // WotLK 3.3.5a movement flags (wire format):
        //   SWIMMING          = 0x00200000
        //   CAN_FLY           = 0x01000000  (ability to fly — no pitch field)
        //   FLYING            = 0x02000000  (actively flying — has pitch field)
        //   SPLINE_ELEVATION  = 0x04000000  (smooth vertical spline offset)
        // MovementFlags2:
        //   MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING = 0x0020
        //
        // Pitch is present when SWIMMING or FLYING are set, or the always-allow flag is set.
        // Note: CAN_FLY (0x01000000) does NOT gate pitch; only FLYING (0x02000000) does.
        // (TBC uses 0x01000000 for FLYING — see TbcMoveFlags in packet_parsers_tbc.cpp.)
        if ((moveFlags & 0x00200000) /* SWIMMING */ ||
            (moveFlags & 0x02000000) /* FLYING */ ||
            (moveFlags2 & 0x0020)    /* MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING */) {
            if (rem() < 4) return false;
            /*float pitch =*/ packet.readFloat();
        }

        // Fall time
        if (rem() < 4) return false;
        /*uint32_t fallTime =*/ packet.readUInt32();

        // Jumping
        if (moveFlags & 0x00001000) { // MOVEMENTFLAG_FALLING
            if (rem() < 16) return false;
            /*float jumpVelocity =*/ packet.readFloat();
            /*float jumpSinAngle =*/ packet.readFloat();
            /*float jumpCosAngle =*/ packet.readFloat();
            /*float jumpXYSpeed =*/ packet.readFloat();
        }

        // Spline elevation
        if (moveFlags & 0x04000000) { // MOVEMENTFLAG_SPLINE_ELEVATION
            if (rem() < 4) return false;
            /*float splineElevation =*/ packet.readFloat();
        }

        // Speeds (9 values in WotLK: walk/run/runBack/swim/swimBack/flight/flightBack/turn/pitch)
        if (rem() < 36) return false;
        /*float walkSpeed =*/ packet.readFloat();
        float runSpeed = packet.readFloat();
        /*float runBackSpeed =*/ packet.readFloat();
        /*float swimSpeed =*/ packet.readFloat();
        /*float swimBackSpeed =*/ packet.readFloat();
        /*float flightSpeed =*/ packet.readFloat();
        /*float flightBackSpeed =*/ packet.readFloat();
        /*float turnRate =*/ packet.readFloat();
        /*float pitchRate =*/ packet.readFloat();

        block.runSpeed = runSpeed;
        block.moveFlags = moveFlags;

        // Spline data
        if (moveFlags & 0x08000000) { // MOVEMENTFLAG_SPLINE_ENABLED
            auto bytesAvailable = [&](size_t n) -> bool { return packet.hasRemaining(n); };
            if (!bytesAvailable(4)) return false;
            uint32_t splineFlags = packet.readUInt32();
            LOG_DEBUG("  Spline: flags=0x", std::hex, splineFlags, std::dec);

            if (splineFlags & 0x00010000) { // SPLINEFLAG_FINAL_POINT
                if (!bytesAvailable(12)) return false;
                /*float finalX =*/ packet.readFloat();
                /*float finalY =*/ packet.readFloat();
                /*float finalZ =*/ packet.readFloat();
            } else if (splineFlags & 0x00020000) { // SPLINEFLAG_FINAL_TARGET
                if (!bytesAvailable(8)) return false;
                /*uint64_t finalTarget =*/ packet.readUInt64();
            } else if (splineFlags & 0x00040000) { // SPLINEFLAG_FINAL_ANGLE
                if (!bytesAvailable(4)) return false;
                /*float finalAngle =*/ packet.readFloat();
            }

            // WotLK spline data layout:
            //   timePassed(4)+duration(4)+splineId(4)+durationMod(4)+durationModNext(4)
            //   +[ANIMATION(5)]+verticalAccel(4)+effectStartTime(4)+pointCount(4)+points+mode(1)+endPoint(12)
            if (!bytesAvailable(12)) return false;
            /*uint32_t timePassed =*/ packet.readUInt32();
            /*uint32_t duration =*/ packet.readUInt32();
            /*uint32_t splineId =*/ packet.readUInt32();

            // Helper: parse spline points + splineMode + endPoint.
            // WotLK uses compressed points by default (first=12 bytes, rest=4 bytes packed).
            auto tryParseSplinePoints = [&](bool compressed, const char* tag) -> bool {
                if (!bytesAvailable(4)) return false;
                size_t prePointCount = packet.getReadPos();
                uint32_t pc = packet.readUInt32();
                if (pc > 256) return false;
                size_t pointsBytes;
                if (compressed && pc > 0) {
                    // First point = 3 floats (12 bytes), rest = packed uint32 (4 bytes each)
                    pointsBytes = 12ull + (pc > 1 ? static_cast<size_t>(pc - 1) * 4ull : 0ull);
                } else {
                    // All uncompressed: 3 floats each
                    pointsBytes = static_cast<size_t>(pc) * 12ull;
                }
                size_t needed = pointsBytes + 13ull; // + splineMode(1) + endPoint(12)
                if (!bytesAvailable(needed)) {
                    packet.setReadPos(prePointCount);
                    return false;
                }
                packet.setReadPos(packet.getReadPos() + pointsBytes);
                uint8_t splineMode = packet.readUInt8();
                if (splineMode > 3) {
                    packet.setReadPos(prePointCount);
                    return false;
                }
                float epX = packet.readFloat();
                float epY = packet.readFloat();
                float epZ = packet.readFloat();
                // Validate endPoint: garbage bytes rarely produce finite world coords
                if (!std::isfinite(epX) || !std::isfinite(epY) || !std::isfinite(epZ) ||
                    std::fabs(epX) > 65000.0f || std::fabs(epY) > 65000.0f ||
                    std::fabs(epZ) > 65000.0f) {
                    packet.setReadPos(prePointCount);
                    return false;
                }
                LOG_DEBUG("  Spline pointCount=", pc, " compressed=", compressed,
                          " endPt=(", epX, ",", epY, ",", epZ, ") (", tag, ")");
                return true;
            };

            // Save position before WotLK spline header for fallback
            size_t beforeSplineHeader = packet.getReadPos();

            // Try 1: WotLK format (durationMod+durationModNext+[ANIMATION]+vertAccel+effectStart+points)
            bool splineParsed = false;
            if (bytesAvailable(8)) {
                /*float durationMod =*/ packet.readFloat();
                /*float durationModNext =*/ packet.readFloat();
                bool wotlkOk = true;
                if (splineFlags & 0x00400000) { // SPLINEFLAG_ANIMATION
                    if (!bytesAvailable(5)) { wotlkOk = false; }
                    else { packet.readUInt8(); packet.readUInt32(); }
                }
                // AzerothCore/ChromieCraft always writes verticalAcceleration(float)
                // + effectStartTime(uint32) unconditionally -- NOT gated by PARABOLIC flag.
                if (wotlkOk) {
                    if (!bytesAvailable(8)) { wotlkOk = false; }
                    else { /*float vertAccel =*/ packet.readFloat(); /*uint32_t effectStart =*/ packet.readUInt32(); }
                }
                if (wotlkOk) {
                    // WotLK: compressed unless CYCLIC(0x80000) or ENTER_CYCLE(0x2000) set
                    bool useCompressed = (splineFlags & (0x00080000 | 0x00002000)) == 0;
                    splineParsed = tryParseSplinePoints(useCompressed, "wotlk-compressed");
                    if (!splineParsed) {
                        splineParsed = tryParseSplinePoints(false, "wotlk-uncompressed");
                    }
                }
            }

            if (!splineParsed) {
                // WotLK compressed+uncompressed both failed. Try without the parabolic
                // fields (some cores don't send vertAccel+effectStart unconditionally).
                packet.setReadPos(beforeSplineHeader);
                if (bytesAvailable(8)) {
                    packet.readFloat(); // durationMod
                    packet.readFloat(); // durationModNext
                    // Skip parabolic fields — try points directly
                    splineParsed = tryParseSplinePoints(false, "wotlk-no-parabolic");
                    if (!splineParsed) {
                        bool useComp = (splineFlags & (0x00080000 | 0x00002000)) == 0;
                        splineParsed = tryParseSplinePoints(useComp, "wotlk-no-parabolic-compressed");
                    }
                }
            }

            // Try 3: bare points (no WotLK header at all — some spline types skip everything)
            if (!splineParsed) {
                packet.setReadPos(beforeSplineHeader);
                splineParsed = tryParseSplinePoints(false, "bare-uncompressed");
                if (!splineParsed) {
                    packet.setReadPos(beforeSplineHeader);
                    bool useComp = (splineFlags & (0x00080000 | 0x00002000)) == 0;
                    splineParsed = tryParseSplinePoints(useComp, "bare-compressed");
                }
            }

            if (!splineParsed) {
                LOG_WARNING("WotLK spline parse failed for guid=0x", std::hex, block.guid, std::dec,
                            " splineFlags=0x", std::hex, splineFlags, std::dec,
                            " remaining=", packet.getRemainingSize());
                return false;
            }
        }
    }
    else if (updateFlags & UPDATEFLAG_POSITION) {
        // Transport position update (UPDATEFLAG_POSITION = 0x0100)
        if (rem() < 1) return false;
        uint64_t transportGuid = packet.readPackedGuid();
        if (rem() < 32) return false; // 8 floats
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.onTransport = (transportGuid != 0);
        block.transportGuid = transportGuid;
        float tx = packet.readFloat();
        float ty = packet.readFloat();
        float tz = packet.readFloat();
        if (block.onTransport) {
            block.transportX = tx;
            block.transportY = ty;
            block.transportZ = tz;
        } else {
            block.transportX = 0.0f;
            block.transportY = 0.0f;
            block.transportZ = 0.0f;
        }
        block.orientation = packet.readFloat();
        /*float corpseOrientation =*/ packet.readFloat();
        block.hasMovement = true;

        if (block.onTransport) {
            LOG_DEBUG("  TRANSPORT POSITION UPDATE: guid=0x", std::hex, transportGuid, std::dec,
                      " pos=(", block.x, ", ", block.y, ", ", block.z, "), o=", block.orientation,
                      " offset=(", block.transportX, ", ", block.transportY, ", ", block.transportZ, ")");
        }
    }
    else if (updateFlags & UPDATEFLAG_STATIONARY_POSITION) {
        if (rem() < 16) return false;
        block.x = packet.readFloat();
        block.y = packet.readFloat();
        block.z = packet.readFloat();
        block.orientation = packet.readFloat();
        block.hasMovement = true;

        LOG_DEBUG("  STATIONARY: (", block.x, ", ", block.y, ", ", block.z, "), o=", block.orientation);
    }

    // Target GUID (for units with target)
    if (updateFlags & UPDATEFLAG_HAS_TARGET) {
        if (rem() < 1) return false;
        /*uint64_t targetGuid =*/ packet.readPackedGuid();
    }

    // Transport time
    if (updateFlags & UPDATEFLAG_TRANSPORT) {
        if (rem() < 4) return false;
        /*uint32_t transportTime =*/ packet.readUInt32();
    }

    // Vehicle
    if (updateFlags & UPDATEFLAG_VEHICLE) {
        if (rem() < 8) return false;
        /*uint32_t vehicleId =*/ packet.readUInt32();
        /*float vehicleOrientation =*/ packet.readFloat();
    }

    // Rotation (GameObjects)
    if (updateFlags & UPDATEFLAG_ROTATION) {
        if (rem() < 8) return false;
        /*int64_t rotation =*/ packet.readUInt64();
    }

    // Low GUID
    if (updateFlags & UPDATEFLAG_LOWGUID) {
        if (rem() < 4) return false;
        /*uint32_t lowGuid =*/ packet.readUInt32();
    }

    // High GUID
    if (updateFlags & UPDATEFLAG_HIGHGUID) {
        if (rem() < 4) return false;
        /*uint32_t highGuid =*/ packet.readUInt32();
    }

    return true;
}

bool UpdateObjectParser::parseUpdateFields(network::Packet& packet, UpdateBlock& block) {
    size_t startPos = packet.getReadPos();

    if (!packet.hasData()) return false;

    // Read number of blocks (each block is 32 fields = 32 bits)
    uint8_t blockCount = packet.readUInt8();

    if (blockCount == 0) {
        return true; // No fields to update
    }

    // Sanity check: UNIT_END=148 needs 5 mask blocks, PLAYER_END=1472 needs 46.
    // VALUES updates don't carry objectType (defaults to 0), so allow up to 55
    // for any VALUES update (could be a PLAYER). Only flag CREATE_OBJECT blocks
    // with genuinely excessive block counts.
    bool isCreateBlock = (block.updateType == UpdateType::CREATE_OBJECT ||
                          block.updateType == UpdateType::CREATE_OBJECT2);
    uint8_t maxExpectedBlocks = isCreateBlock
        ? ((block.objectType == ObjectType::PLAYER) ? 55 : 10)
        : 55;  // VALUES: allow PLAYER-sized masks
    if (blockCount > maxExpectedBlocks) {
        LOG_WARNING("UpdateObjectParser: suspicious maskBlockCount=", static_cast<int>(blockCount),
                    " for objectType=", static_cast<int>(block.objectType),
                    " guid=0x", std::hex, block.guid, std::dec,
                    " updateFlags=0x", std::hex, block.updateFlags, std::dec,
                    " moveFlags=0x", std::hex, block.moveFlags, std::dec,
                    " readPos=", packet.getReadPos(), " size=", packet.getSize());
    }

    uint32_t fieldsCapacity = blockCount * 32;
    LOG_DEBUG("  UPDATE MASK PARSE:");
    LOG_DEBUG("    maskBlockCount = ", static_cast<int>(blockCount));
    LOG_DEBUG("    fieldsCapacity (blocks * 32) = ", fieldsCapacity);

    // Read update mask into a reused scratch buffer to avoid per-block allocations.
    static thread_local std::vector<uint32_t> updateMask;
    updateMask.resize(blockCount);
    for (int i = 0; i < blockCount; ++i) {
        // Validate 4 bytes available before each block read
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("UpdateObjectParser: truncated update mask at block ", i,
                        " type=", updateTypeName(block.updateType),
                        " objectType=", static_cast<int>(block.objectType),
                        " guid=0x", std::hex, block.guid, std::dec,
                        " readPos=", packet.getReadPos(),
                        " size=", packet.getSize(),
                        " maskBlockCount=", static_cast<int>(blockCount));
            return false;
        }
        updateMask[i] = packet.readUInt32();
    }

    // Find highest set bit
    uint16_t highestSetBit = 0;
    uint32_t valuesReadCount = 0;

    // Read only set bits in each mask block (faster than scanning all 32 bits).
    for (int blockIdx = 0; blockIdx < blockCount; ++blockIdx) {
        uint32_t mask = updateMask[blockIdx];
        while (mask != 0) {
            const uint16_t fieldIndex =
#if defined(__GNUC__) || defined(__clang__)
                static_cast<uint16_t>(blockIdx * 32 + __builtin_ctz(mask));
#else
                static_cast<uint16_t>(blockIdx * 32 + [] (uint32_t v) -> uint16_t {
                    uint16_t b = 0;
                    while ((v & 1u) == 0u) { v >>= 1u; ++b; }
                    return b;
                }(mask));
#endif
            if (fieldIndex > highestSetBit) {
                highestSetBit = fieldIndex;
            }
            // Validate 4 bytes available before reading field value
            if (!packet.hasRemaining(4)) {
                LOG_WARNING("UpdateObjectParser: truncated field value at field ", fieldIndex,
                            " type=", updateTypeName(block.updateType),
                            " objectType=", static_cast<int>(block.objectType),
                            " guid=0x", std::hex, block.guid, std::dec,
                            " readPos=", packet.getReadPos(),
                            " size=", packet.getSize(),
                            " maskBlockIndex=", blockIdx,
                            " maskBlock=0x", std::hex, updateMask[blockIdx], std::dec);
                return false;
            }
            uint32_t value = packet.readUInt32();
            // fieldIndex is monotonically increasing here, so end() is a good insertion hint.
            block.fields.emplace_hint(block.fields.end(), fieldIndex, value);
            valuesReadCount++;

            LOG_DEBUG("    Field[", fieldIndex, "] = 0x", std::hex, value, std::dec);
            mask &= (mask - 1u);
        }
    }

    size_t endPos = packet.getReadPos();
    size_t bytesUsed = endPos - startPos;
    size_t bytesRemaining = packet.getSize() - endPos;

    LOG_DEBUG("    highestSetBitIndex = ", highestSetBit);
    LOG_DEBUG("    valuesReadCount = ", valuesReadCount);
    LOG_DEBUG("    bytesUsedForFields = ", bytesUsed);
    LOG_DEBUG("    bytesRemainingInPacket = ", bytesRemaining);
    LOG_DEBUG("  Parsed ", block.fields.size(), " fields");

    return true;
}

bool UpdateObjectParser::parseUpdateBlock(network::Packet& packet, UpdateBlock& block) {
    if (!packet.hasData()) return false;

    // Read update type
    uint8_t updateTypeVal = packet.readUInt8();
    block.updateType = static_cast<UpdateType>(updateTypeVal);

    LOG_DEBUG("Update block: type=", static_cast<int>(updateTypeVal));

    switch (block.updateType) {
        case UpdateType::VALUES: {
            // Partial update - changed fields only
            if (!packet.hasData()) return false;
            block.guid = packet.readPackedGuid();
            LOG_DEBUG("  VALUES update for GUID: 0x", std::hex, block.guid, std::dec);

            return parseUpdateFields(packet, block);
        }

        case UpdateType::MOVEMENT: {
            // Movement update
            if (!packet.hasRemaining(8)) return false;
            block.guid = packet.readUInt64();
            LOG_DEBUG("  MOVEMENT update for GUID: 0x", std::hex, block.guid, std::dec);

            return parseMovementBlock(packet, block);
        }

        case UpdateType::CREATE_OBJECT:
        case UpdateType::CREATE_OBJECT2: {
            // Create new object with full data
            if (!packet.hasData()) return false;
            block.guid = packet.readPackedGuid();
            LOG_DEBUG("  CREATE_OBJECT for GUID: 0x", std::hex, block.guid, std::dec);

            // Read object type
            if (!packet.hasData()) return false;
            uint8_t objectTypeVal = packet.readUInt8();
            block.objectType = static_cast<ObjectType>(objectTypeVal);
            LOG_DEBUG("  Object type: ", static_cast<int>(objectTypeVal));

            // Parse movement if present
            bool hasMovement = parseMovementBlock(packet, block);
            if (!hasMovement) {
                return false;
            }

            // Parse update fields
            return parseUpdateFields(packet, block);
        }

        case UpdateType::OUT_OF_RANGE_OBJECTS: {
            // Objects leaving view range - handled differently
            LOG_DEBUG("  OUT_OF_RANGE_OBJECTS (skipping in block parser)");
            return true;
        }

        case UpdateType::NEAR_OBJECTS: {
            // Objects entering view range - handled differently
            LOG_DEBUG("  NEAR_OBJECTS (skipping in block parser)");
            return true;
        }

        default:
            LOG_WARNING("Unknown update type: ", static_cast<int>(updateTypeVal));
            return false;
    }
}

bool UpdateObjectParser::parse(network::Packet& packet, UpdateObjectData& data) {
    // Keep worst-case packet parsing bounded. Extremely large counts are typically
    // malformed/desynced and can stall a frame long enough to trigger disconnects.
    constexpr uint32_t kMaxReasonableUpdateBlocks = 1024;
    constexpr uint32_t kMaxReasonableOutOfRangeGuids = 4096;

    // Read block count
    data.blockCount = packet.readUInt32();
    if (data.blockCount > kMaxReasonableUpdateBlocks) {
        LOG_ERROR("SMSG_UPDATE_OBJECT rejected: unreasonable blockCount=", data.blockCount,
                  " packetSize=", packet.getSize());
        return false;
    }

    LOG_DEBUG("SMSG_UPDATE_OBJECT:");
    LOG_DEBUG("  objectCount = ", data.blockCount);
    LOG_DEBUG("  packetSize = ", packet.getSize());

    uint32_t remainingBlockCount = data.blockCount;

    // Check for out-of-range objects first
    if (packet.hasRemaining(1)) {
        uint8_t firstByte = packet.readUInt8();

        if (firstByte == static_cast<uint8_t>(UpdateType::OUT_OF_RANGE_OBJECTS)) {
            if (remainingBlockCount == 0) {
                LOG_ERROR("SMSG_UPDATE_OBJECT rejected: OUT_OF_RANGE_OBJECTS with zero blockCount");
                return false;
            }
            --remainingBlockCount;
            // Read out-of-range GUID count
            uint32_t count = packet.readUInt32();
            if (count > kMaxReasonableOutOfRangeGuids) {
                LOG_ERROR("SMSG_UPDATE_OBJECT rejected: unreasonable outOfRange count=", count,
                          " packetSize=", packet.getSize());
                return false;
            }

            for (uint32_t i = 0; i < count; ++i) {
                uint64_t guid = packet.readPackedGuid();
                data.outOfRangeGuids.push_back(guid);
                LOG_DEBUG("    Out of range: 0x", std::hex, guid, std::dec);
            }

            // Done - packet may have more blocks after this
            // Reset read position to after the first byte if needed
        } else {
            // Not out-of-range, rewind
            packet.setReadPos(packet.getReadPos() - 1);
        }
    }

    // Parse update blocks
    data.blockCount = remainingBlockCount;
    data.blocks.reserve(data.blockCount);

    for (uint32_t i = 0; i < data.blockCount; ++i) {
        LOG_DEBUG("Parsing block ", i + 1, " / ", data.blockCount);

        UpdateBlock block;
        if (!parseUpdateBlock(packet, block)) {
            static int parseBlockErrors = 0;
            if (++parseBlockErrors <= 5) {
                LOG_ERROR("Failed to parse update block ", i + 1, " of ", data.blockCount,
                          " (", i, " blocks parsed successfully before failure)");
                if (parseBlockErrors == 5)
                    LOG_ERROR("(suppressing further update block parse errors)");
            }
            // Cannot reliably re-sync to the next block after a parse failure,
            // but still return true so the blocks already parsed are processed.
            break;
        }

        data.blocks.emplace_back(std::move(block));
    }


    return true;
}

bool DestroyObjectParser::parse(network::Packet& packet, DestroyObjectData& data) {
    // SMSG_DESTROY_OBJECT format:
    // uint64 guid
    // uint8 isDeath (0 = despawn, 1 = death) — WotLK only; vanilla/TBC omit this

    if (packet.getSize() < 8) {
        LOG_ERROR("SMSG_DESTROY_OBJECT packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    data.guid = packet.readUInt64();
    // WotLK adds isDeath byte; vanilla/TBC packets are exactly 8 bytes
    if (packet.hasData()) {
        data.isDeath = (packet.readUInt8() != 0);
    } else {
        data.isDeath = false;
    }

    LOG_DEBUG("Parsed SMSG_DESTROY_OBJECT:");
    LOG_DEBUG("  GUID: 0x", std::hex, data.guid, std::dec);
    LOG_DEBUG("  Is death: ", data.isDeath ? "yes" : "no");

    return true;
}

network::Packet MessageChatPacket::build(ChatType type,
                                          ChatLanguage language,
                                          const std::string& message,
                                          const std::string& target) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MESSAGECHAT));

    // Write chat type
    packet.writeUInt32(static_cast<uint32_t>(type));

    // Write language
    packet.writeUInt32(static_cast<uint32_t>(language));

    // Write target (for whispers) or channel name
    if (type == ChatType::WHISPER) {
        packet.writeString(target);
    } else if (type == ChatType::CHANNEL) {
        packet.writeString(target);  // Channel name
    }

    // Write message
    packet.writeString(message);

    LOG_DEBUG("Built CMSG_MESSAGECHAT packet");
    LOG_DEBUG("  Type: ", static_cast<int>(type));
    LOG_DEBUG("  Language: ", static_cast<int>(language));
    LOG_DEBUG("  Message: ", message);

    return packet;
}

bool MessageChatParser::parse(network::Packet& packet, MessageChatData& data) {
    // SMSG_MESSAGECHAT format (WoW 3.3.5a):
    // uint8 type
    // uint32 language
    // uint64 senderGuid
    // uint32 unknown (always 0)
    // [type-specific data]
    // uint32 messageLength
    // string message
    // uint8 chatTag

    if (packet.getSize() < 15) {
        LOG_ERROR("SMSG_MESSAGECHAT packet too small: ", packet.getSize(), " bytes");
        return false;
    }

    // Read chat type
    uint8_t typeVal = packet.readUInt8();
    data.type = static_cast<ChatType>(typeVal);

    // Read language
    uint32_t langVal = packet.readUInt32();
    data.language = static_cast<ChatLanguage>(langVal);

    // Read sender GUID
    data.senderGuid = packet.readUInt64();

    // Read unknown field
    packet.readUInt32();

    auto tryReadSizedCString = [&](std::string& out, uint32_t maxLen, size_t minTrailingBytes) -> bool {
        size_t start = packet.getReadPos();
        size_t remaining = packet.getSize() - start;
        if (remaining < 4 + minTrailingBytes) return false;

        uint32_t len = packet.readUInt32();
        if (len < 2 || len > maxLen) {
            packet.setReadPos(start);
            return false;
        }
        if (!packet.hasRemaining(static_cast<size_t>(len) + minTrailingBytes)) {
            packet.setReadPos(start);
            return false;
        }

        // Stack buffer for typical messages; heap fallback for oversized ones.
        static constexpr uint32_t kStackBufSize = 256;
        std::array<char, kStackBufSize> stackBuf;
        std::string heapBuf;
        char* buf;
        if (len <= kStackBufSize) {
            buf = stackBuf.data();
        } else {
            heapBuf.resize(len);
            buf = heapBuf.data();
        }

        for (uint32_t i = 0; i < len; ++i) {
            buf[i] = static_cast<char>(packet.readUInt8());
        }
        if (buf[len - 1] != '\0') {
            packet.setReadPos(start);
            return false;
        }
        // len >= 2 guaranteed above, so len-1 >= 1 — string body is non-empty.
        for (uint32_t i = 0; i < len - 1; ++i) {
            auto uc = static_cast<unsigned char>(buf[i]);
            if (uc < 32 || uc > 126) {
                packet.setReadPos(start);
                return false;
            }
        }

        out.assign(buf, len - 1);
        return true;
    };

    // Type-specific data
    // WoW 3.3.5 SMSG_MESSAGECHAT format: after senderGuid+unk, most types
    // have a receiverGuid (uint64). Some types have extra fields before it.
    switch (data.type) {
        case ChatType::MONSTER_SAY:
        case ChatType::MONSTER_YELL:
        case ChatType::MONSTER_EMOTE:
        case ChatType::MONSTER_WHISPER:
        case ChatType::MONSTER_PARTY:
        case ChatType::RAID_BOSS_EMOTE:
        case ChatType::RAID_BOSS_WHISPER: {
            // Read sender name (SizedCString: uint32 len including null + chars)
            uint32_t nameLen = packet.readUInt32();
            if (nameLen > packet.getRemainingSize()) return false;
            if (nameLen > 0 && nameLen < 256) {
                data.senderName.resize(nameLen);
                for (uint32_t i = 0; i < nameLen; ++i) {
                    data.senderName[i] = static_cast<char>(packet.readUInt8());
                }
                // Strip trailing null (server includes it in nameLen)
                if (!data.senderName.empty() && data.senderName.back() == '\0') {
                    data.senderName.pop_back();
                }
            }
            // Read receiver GUID (NamedGuid: guid + optional name for non-player targets)
            data.receiverGuid = packet.readUInt64();
            if (data.receiverGuid != 0) {
                // WoW GUID type encoding: bits 48-63 identify entity type.
                // Players have highGuid=0x0000. Pets use 0xF040 (active pet) or
                // 0xF014 (creature treated as pet). Mask 0xF0FF isolates the type
                // nibbles while ignoring the server-specific middle bits.
                constexpr uint16_t kGuidTypeMask   = 0xF0FF;
                constexpr uint16_t kGuidTypePet     = 0xF040;
                constexpr uint16_t kGuidTypeVehicle = 0xF014;
                uint16_t highGuid = static_cast<uint16_t>(data.receiverGuid >> 48);
                bool isPlayer = (highGuid == 0x0000);
                bool isPet = ((highGuid & kGuidTypeMask) == kGuidTypePet) ||
                             ((highGuid & kGuidTypeMask) == kGuidTypeVehicle);
                if (!isPlayer && !isPet) {
                    // Read receiver name (SizedCString)
                    uint32_t recvNameLen = packet.readUInt32();
                    if (recvNameLen > 0 && recvNameLen < 256) {
                        packet.setReadPos(packet.getReadPos() + recvNameLen);
                    }
                }
            }
            break;
        }

        case ChatType::CHANNEL: {
            // Read channel name, then receiver GUID
            data.channelName = packet.readString();
            data.receiverGuid = packet.readUInt64();
            break;
        }

        case ChatType::ACHIEVEMENT:
        case ChatType::GUILD_ACHIEVEMENT: {
            // Read target GUID
            data.receiverGuid = packet.readUInt64();
            break;
        }

        case ChatType::WHISPER:
        case ChatType::WHISPER_INFORM: {
            // Some cores include an explicit sized sender/receiver name for whisper chat.
            // Consume it when present so /r has a reliable last whisper sender.
            if (data.type == ChatType::WHISPER) {
                tryReadSizedCString(data.senderName, 128, 8 + 4 + 1);
            } else {
                tryReadSizedCString(data.receiverName, 128, 8 + 4 + 1);
            }

            data.receiverGuid = packet.readUInt64();

            // Optional trailing whisper target/source name on some formats.
            if (data.type == ChatType::WHISPER && data.receiverName.empty()) {
                tryReadSizedCString(data.receiverName, 128, 4 + 1);
            } else if (data.type == ChatType::WHISPER_INFORM && data.senderName.empty()) {
                tryReadSizedCString(data.senderName, 128, 4 + 1);
            }
            break;
        }

        case ChatType::BG_SYSTEM_NEUTRAL:
        case ChatType::BG_SYSTEM_ALLIANCE:
        case ChatType::BG_SYSTEM_HORDE:
            // BG/Arena system messages — no sender GUID or name field, just message.
            // Reclassify as SYSTEM for consistent display.
            data.type = ChatType::SYSTEM;
            break;

        default:
            // SAY, GUILD, PARTY, YELL, WHISPER, WHISPER_INFORM, RAID, etc.
            // All have receiverGuid (typically senderGuid repeated)
            data.receiverGuid = packet.readUInt64();
            break;
    }

    // Read message length
    uint32_t messageLen = packet.readUInt32();
    if (messageLen > packet.getRemainingSize()) return false;

    // Read message
    if (messageLen > 0 && messageLen < 8192) {
        data.message.resize(messageLen);
        for (uint32_t i = 0; i < messageLen; ++i) {
            data.message[i] = static_cast<char>(packet.readUInt8());
        }
        // Strip trailing null terminator (servers include it in messageLen)
        if (!data.message.empty() && data.message.back() == '\0') {
            data.message.pop_back();
        }
    }

    // Read chat tag
    data.chatTag = packet.readUInt8();

    LOG_DEBUG("Parsed SMSG_MESSAGECHAT:");
    LOG_DEBUG("  Type: ", getChatTypeString(data.type));
    LOG_DEBUG("  Language: ", static_cast<int>(data.language));
    LOG_DEBUG("  Sender GUID: 0x", std::hex, data.senderGuid, std::dec);
    if (!data.senderName.empty()) {
        LOG_DEBUG("  Sender name: ", data.senderName);
    }
    if (!data.channelName.empty()) {
        LOG_DEBUG("  Channel: ", data.channelName);
    }
    LOG_DEBUG("  Message: ", data.message);
    LOG_DEBUG("  Chat tag: 0x", std::hex, static_cast<int>(data.chatTag), std::dec);

    return true;
}

const char* getChatTypeString(ChatType type) {
    switch (type) {
        case ChatType::SAY: return "SAY";
        case ChatType::PARTY: return "PARTY";
        case ChatType::RAID: return "RAID";
        case ChatType::GUILD: return "GUILD";
        case ChatType::OFFICER: return "OFFICER";
        case ChatType::YELL: return "YELL";
        case ChatType::WHISPER: return "WHISPER";
        case ChatType::WHISPER_INFORM: return "WHISPER_INFORM";
        case ChatType::EMOTE: return "EMOTE";
        case ChatType::TEXT_EMOTE: return "TEXT_EMOTE";
        case ChatType::SYSTEM: return "SYSTEM";
        case ChatType::MONSTER_SAY: return "MONSTER_SAY";
        case ChatType::MONSTER_YELL: return "MONSTER_YELL";
        case ChatType::MONSTER_EMOTE: return "MONSTER_EMOTE";
        case ChatType::CHANNEL: return "CHANNEL";
        case ChatType::CHANNEL_JOIN: return "CHANNEL_JOIN";
        case ChatType::CHANNEL_LEAVE: return "CHANNEL_LEAVE";
        case ChatType::CHANNEL_LIST: return "CHANNEL_LIST";
        case ChatType::CHANNEL_NOTICE: return "CHANNEL_NOTICE";
        case ChatType::CHANNEL_NOTICE_USER: return "CHANNEL_NOTICE_USER";
        case ChatType::AFK: return "AFK";
        case ChatType::DND: return "DND";
        case ChatType::IGNORED: return "IGNORED";
        case ChatType::SKILL: return "SKILL";
        case ChatType::LOOT: return "LOOT";
        case ChatType::BATTLEGROUND: return "BATTLEGROUND";
        case ChatType::BATTLEGROUND_LEADER: return "BATTLEGROUND_LEADER";
        case ChatType::RAID_LEADER: return "RAID_LEADER";
        case ChatType::RAID_WARNING: return "RAID_WARNING";
        case ChatType::ACHIEVEMENT: return "ACHIEVEMENT";
        case ChatType::GUILD_ACHIEVEMENT: return "GUILD_ACHIEVEMENT";
        default: return "UNKNOWN";
    }
}

// ============================================================
// Text Emotes
// ============================================================

network::Packet TextEmotePacket::build(uint32_t textEmoteId, uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_TEXT_EMOTE));
    packet.writeUInt32(textEmoteId);
    packet.writeUInt32(0);  // emoteNum (unused)
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built CMSG_TEXT_EMOTE: emoteId=", textEmoteId, " target=0x", std::hex, targetGuid, std::dec);
    return packet;
}

bool TextEmoteParser::parse(network::Packet& packet, TextEmoteData& data, bool legacyFormat) {
    size_t bytesLeft = packet.getRemainingSize();
    if (bytesLeft < 20) {
        LOG_WARNING("SMSG_TEXT_EMOTE too short: ", bytesLeft, " bytes");
        return false;
    }

    if (legacyFormat) {
        // Classic 1.12 / TBC 2.4.3: textEmoteId(u32) + emoteNum(u32) + senderGuid(u64)
        data.textEmoteId = packet.readUInt32();
        data.emoteNum    = packet.readUInt32();
        data.senderGuid  = packet.readUInt64();
    } else {
        // WotLK 3.3.5a: senderGuid(u64) + textEmoteId(u32) + emoteNum(u32)
        data.senderGuid  = packet.readUInt64();
        data.textEmoteId = packet.readUInt32();
        data.emoteNum    = packet.readUInt32();
    }

    uint32_t nameLen = packet.readUInt32();
    if (nameLen > 0 && nameLen <= 256) {
        data.targetName = packet.readString();
    } else if (nameLen > 0) {
        // Implausible name length — misaligned read
        return false;
    }
    return true;
}

// ============================================================
// Channel System
// ============================================================

network::Packet JoinChannelPacket::build(const std::string& channelName, const std::string& password) {
    network::Packet packet(wireOpcode(Opcode::CMSG_JOIN_CHANNEL));
    packet.writeUInt32(0);  // channelId (unused)
    packet.writeUInt8(0);   // hasVoice
    packet.writeUInt8(0);   // joinedByZone
    packet.writeString(channelName);
    packet.writeString(password);
    LOG_DEBUG("Built CMSG_JOIN_CHANNEL: channel=", channelName);
    return packet;
}

network::Packet LeaveChannelPacket::build(const std::string& channelName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LEAVE_CHANNEL));
    packet.writeUInt32(0);  // channelId (unused)
    packet.writeString(channelName);
    LOG_DEBUG("Built CMSG_LEAVE_CHANNEL: channel=", channelName);
    return packet;
}

bool ChannelNotifyParser::parse(network::Packet& packet, ChannelNotifyData& data) {
    size_t bytesLeft = packet.getRemainingSize();
    if (bytesLeft < 2) {
        LOG_WARNING("SMSG_CHANNEL_NOTIFY too short");
        return false;
    }
    data.notifyType = static_cast<ChannelNotifyType>(packet.readUInt8());
    data.channelName = packet.readString();
    // Some notification types have additional fields (guid, etc.)
    bytesLeft = packet.getRemainingSize();
    if (bytesLeft >= 8) {
        data.senderGuid = packet.readUInt64();
    }
    return true;
}

// ============================================================
// Phase 1: Foundation — Targeting, Name Queries
// ============================================================

network::Packet SetSelectionPacket::build(uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_SELECTION));
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built CMSG_SET_SELECTION: target=0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet SetActiveMoverPacket::build(uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_ACTIVE_MOVER));
    packet.writeUInt64(guid);
    LOG_DEBUG("Built CMSG_SET_ACTIVE_MOVER: guid=0x", std::hex, guid, std::dec);
    return packet;
}

network::Packet InspectPacket::build(uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_INSPECT));
    packet.writePackedGuid(targetGuid);
    LOG_DEBUG("Built CMSG_INSPECT: target=0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet QueryInspectAchievementsPacket::build(uint64_t targetGuid) {
    // CMSG_QUERY_INSPECT_ACHIEVEMENTS: PackedGuid targetGuid
    network::Packet packet(wireOpcode(Opcode::CMSG_QUERY_INSPECT_ACHIEVEMENTS));
    packet.writePackedGuid(targetGuid);
    LOG_DEBUG("Built CMSG_QUERY_INSPECT_ACHIEVEMENTS: target=0x", std::hex, targetGuid, std::dec);
    return packet;
}

// ============================================================
// Server Info Commands
// ============================================================

network::Packet QueryTimePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUERY_TIME));
    LOG_DEBUG("Built CMSG_QUERY_TIME");
    return packet;
}

bool QueryTimeResponseParser::parse(network::Packet& packet, QueryTimeResponseData& data) {
    // Validate minimum packet size: serverTime(4) + timeOffset(4)
    if (!packet.hasRemaining(8)) {
        LOG_WARNING("SMSG_QUERY_TIME_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.serverTime = packet.readUInt32();
    data.timeOffset = packet.readUInt32();
    LOG_DEBUG("Parsed SMSG_QUERY_TIME_RESPONSE: time=", data.serverTime, " offset=", data.timeOffset);
    return true;
}

network::Packet RequestPlayedTimePacket::build(bool sendToChat) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PLAYED_TIME));
    packet.writeUInt8(sendToChat ? 1 : 0);
    LOG_DEBUG("Built CMSG_PLAYED_TIME: sendToChat=", sendToChat);
    return packet;
}

bool PlayedTimeParser::parse(network::Packet& packet, PlayedTimeData& data) {
    // Classic/Turtle may omit the trailing trigger-message byte and send only
    // totalTime(4) + levelTime(4). Later expansions append triggerMsg(1).
    if (!packet.hasRemaining(8)) {
        LOG_WARNING("SMSG_PLAYED_TIME: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.totalTimePlayed = packet.readUInt32();
    data.levelTimePlayed = packet.readUInt32();
    data.triggerMessage = (packet.hasRemaining(1)) && (packet.readUInt8() != 0);
    LOG_DEBUG("Parsed SMSG_PLAYED_TIME: total=", data.totalTimePlayed, " level=", data.levelTimePlayed);
    return true;
}

network::Packet WhoPacket::build(uint32_t minLevel, uint32_t maxLevel,
                                 const std::string& playerName,
                                 const std::string& guildName,
                                 uint32_t raceMask, uint32_t classMask,
                                 uint32_t zones) {
    network::Packet packet(wireOpcode(Opcode::CMSG_WHO));
    packet.writeUInt32(minLevel);
    packet.writeUInt32(maxLevel);
    packet.writeString(playerName);
    packet.writeString(guildName);
    packet.writeUInt32(raceMask);
    packet.writeUInt32(classMask);
    packet.writeUInt32(zones);    // Number of zone IDs (0 = no zone filter)
    // Zone ID array would go here if zones > 0
    packet.writeUInt32(0);        // stringCount (number of search strings)
    // String array would go here if stringCount > 0
    LOG_DEBUG("Built CMSG_WHO: player=", playerName);
    return packet;
}

// ============================================================
// Social Commands
// ============================================================

network::Packet AddFriendPacket::build(const std::string& playerName, const std::string& note) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ADD_FRIEND));
    packet.writeString(playerName);
    packet.writeString(note);
    LOG_DEBUG("Built CMSG_ADD_FRIEND: player=", playerName);
    return packet;
}

network::Packet DelFriendPacket::build(uint64_t friendGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_DEL_FRIEND));
    packet.writeUInt64(friendGuid);
    LOG_DEBUG("Built CMSG_DEL_FRIEND: guid=0x", std::hex, friendGuid, std::dec);
    return packet;
}

network::Packet SetContactNotesPacket::build(uint64_t friendGuid, const std::string& note) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_CONTACT_NOTES));
    packet.writeUInt64(friendGuid);
    packet.writeString(note);
    LOG_DEBUG("Built CMSG_SET_CONTACT_NOTES: guid=0x", std::hex, friendGuid, std::dec);
    return packet;
}

bool FriendStatusParser::parse(network::Packet& packet, FriendStatusData& data) {
    // Validate minimum packet size: status(1) + guid(8)
    if (!packet.hasRemaining(9)) {
        LOG_WARNING("SMSG_FRIEND_STATUS: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.status = packet.readUInt8();
    data.guid = packet.readUInt64();
    if (data.status == 1) {  // Online
        // Conditional: note (string) + chatFlag (1)
        if (packet.hasData()) {
            data.note = packet.readString();
            if (packet.hasRemaining(1)) {
                data.chatFlag = packet.readUInt8();
            }
        }
    }
    LOG_DEBUG("Parsed SMSG_FRIEND_STATUS: status=", static_cast<int>(data.status), " guid=0x", std::hex, data.guid, std::dec);
    return true;
}

network::Packet AddIgnorePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ADD_IGNORE));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_ADD_IGNORE: player=", playerName);
    return packet;
}

network::Packet DelIgnorePacket::build(uint64_t ignoreGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_DEL_IGNORE));
    packet.writeUInt64(ignoreGuid);
    LOG_DEBUG("Built CMSG_DEL_IGNORE: guid=0x", std::hex, ignoreGuid, std::dec);
    return packet;
}

network::Packet ComplainPacket::build(uint64_t targetGuid, const std::string& reason) {
    network::Packet packet(wireOpcode(Opcode::CMSG_COMPLAIN));
    packet.writeUInt8(1);               // complaintType: 1 = spam
    packet.writeUInt64(targetGuid);
    packet.writeUInt32(0);              // unk
    packet.writeUInt32(0);              // messageType
    packet.writeUInt32(0);              // channelId
    packet.writeUInt32(static_cast<uint32_t>(time(nullptr))); // timestamp
    packet.writeString(reason);
    LOG_DEBUG("Built CMSG_COMPLAIN: target=0x", std::hex, targetGuid, std::dec, " reason=", reason);
    return packet;
}

// ============================================================
// Logout Commands
// ============================================================

network::Packet LogoutRequestPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOGOUT_REQUEST));
    LOG_DEBUG("Built CMSG_LOGOUT_REQUEST");
    return packet;
}

network::Packet LogoutCancelPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOGOUT_CANCEL));
    LOG_DEBUG("Built CMSG_LOGOUT_CANCEL");
    return packet;
}

bool LogoutResponseParser::parse(network::Packet& packet, LogoutResponseData& data) {
    // Validate minimum packet size: result(4) + instant(1)
    if (!packet.hasRemaining(5)) {
        LOG_WARNING("SMSG_LOGOUT_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.result = packet.readUInt32();
    data.instant = packet.readUInt8();
    LOG_DEBUG("Parsed SMSG_LOGOUT_RESPONSE: result=", data.result, " instant=", static_cast<int>(data.instant));
    return true;
}

// ============================================================
// Stand State
// ============================================================

network::Packet StandStateChangePacket::build(uint8_t state) {
    network::Packet packet(wireOpcode(Opcode::CMSG_STANDSTATECHANGE));
    packet.writeUInt32(state);
    LOG_DEBUG("Built CMSG_STANDSTATECHANGE: state=", static_cast<int>(state));
    return packet;
}

// ============================================================
// Action Bar
// ============================================================

network::Packet SetActionButtonPacket::build(uint8_t button, uint8_t type, uint32_t id, bool isClassic) {
    // Classic/Turtle (1.12): uint8 button + uint16 id + uint8 type + uint8 misc(0)
    //   type encoding: 0=spell, 1=item, 64=macro
    // TBC/WotLK:       uint8 button + uint32 packed (type<<24 | id)
    //   type encoding: 0x00=spell, 0x80=item, 0x40=macro
    //   packed=0 means clear the slot
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_ACTION_BUTTON));
    packet.writeUInt8(button);
    if (isClassic) {
        // Classic: 16-bit id, 8-bit type code, 8-bit misc
        // Map ActionBarSlot::Type (0=EMPTY,1=SPELL,2=ITEM,3=MACRO) → classic type byte
        uint8_t classicType = 0; // 0 = spell
        if (type == 2 /* ITEM */)  classicType = 1;
        if (type == 3 /* MACRO */) classicType = 64;
        packet.writeUInt16(static_cast<uint16_t>(id));
        packet.writeUInt8(classicType);
        packet.writeUInt8(0); // misc
        LOG_DEBUG("Built CMSG_SET_ACTION_BUTTON (Classic): button=", static_cast<int>(button),
                  " id=", id, " type=", static_cast<int>(classicType));
    } else {
        // TBC/WotLK: type in bits 24–31, id in bits 0–23; packed=0 clears slot
        uint8_t packedType = 0x00; // spell
        if (type == 2 /* ITEM */)  packedType = 0x80;
        if (type == 3 /* MACRO */) packedType = 0x40;
        uint32_t packed = (id == 0) ? 0 : (static_cast<uint32_t>(packedType) << 24) | (id & 0x00FFFFFF);
        packet.writeUInt32(packed);
        LOG_DEBUG("Built CMSG_SET_ACTION_BUTTON (TBC/WotLK): button=", static_cast<int>(button),
                  " packed=0x", std::hex, packed, std::dec);
    }
    return packet;
}

// ============================================================
// Display Toggles
// ============================================================

network::Packet ShowingHelmPacket::build(bool show) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SHOWING_HELM));
    packet.writeUInt8(show ? 1 : 0);
    LOG_DEBUG("Built CMSG_SHOWING_HELM: show=", show);
    return packet;
}

network::Packet ShowingCloakPacket::build(bool show) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SHOWING_CLOAK));
    packet.writeUInt8(show ? 1 : 0);
    LOG_DEBUG("Built CMSG_SHOWING_CLOAK: show=", show);
    return packet;
}

// ============================================================
// PvP
// ============================================================

network::Packet TogglePvpPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_TOGGLE_PVP));
    LOG_DEBUG("Built CMSG_TOGGLE_PVP");
    return packet;
}

// ============================================================
// Guild Commands
// ============================================================

network::Packet GuildInfoPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INFO));
    LOG_DEBUG("Built CMSG_GUILD_INFO");
    return packet;
}

network::Packet GuildRosterPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_ROSTER));
    LOG_DEBUG("Built CMSG_GUILD_ROSTER");
    return packet;
}

network::Packet GuildMotdPacket::build(const std::string& motd) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_MOTD));
    packet.writeString(motd);
    LOG_DEBUG("Built CMSG_GUILD_MOTD: ", motd);
    return packet;
}

network::Packet GuildPromotePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_PROMOTE));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GUILD_PROMOTE: ", playerName);
    return packet;
}

network::Packet GuildDemotePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_DEMOTE));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GUILD_DEMOTE: ", playerName);
    return packet;
}

network::Packet GuildLeavePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_LEAVE));
    LOG_DEBUG("Built CMSG_GUILD_LEAVE");
    return packet;
}

network::Packet GuildInvitePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INVITE));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GUILD_INVITE: ", playerName);
    return packet;
}

network::Packet GuildQueryPacket::build(uint32_t guildId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_QUERY));
    packet.writeUInt32(guildId);
    LOG_DEBUG("Built CMSG_GUILD_QUERY: guildId=", guildId);
    return packet;
}

network::Packet GuildRemovePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_REMOVE));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GUILD_REMOVE: ", playerName);
    return packet;
}

network::Packet GuildDisbandPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_DISBAND));
    LOG_DEBUG("Built CMSG_GUILD_DISBAND");
    return packet;
}

network::Packet GuildLeaderPacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_LEADER));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GUILD_LEADER: ", playerName);
    return packet;
}

network::Packet GuildSetPublicNotePacket::build(const std::string& playerName, const std::string& note) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_SET_PUBLIC_NOTE));
    packet.writeString(playerName);
    packet.writeString(note);
    LOG_DEBUG("Built CMSG_GUILD_SET_PUBLIC_NOTE: ", playerName, " -> ", note);
    return packet;
}

network::Packet GuildSetOfficerNotePacket::build(const std::string& playerName, const std::string& note) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_SET_OFFICER_NOTE));
    packet.writeString(playerName);
    packet.writeString(note);
    LOG_DEBUG("Built CMSG_GUILD_SET_OFFICER_NOTE: ", playerName, " -> ", note);
    return packet;
}

network::Packet GuildAcceptPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_ACCEPT));
    LOG_DEBUG("Built CMSG_GUILD_ACCEPT");
    return packet;
}

network::Packet GuildDeclineInvitationPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_DECLINE));
    LOG_DEBUG("Built CMSG_GUILD_DECLINE");
    return packet;
}

network::Packet GuildCreatePacket::build(const std::string& guildName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_CREATE));
    packet.writeString(guildName);
    LOG_DEBUG("Built CMSG_GUILD_CREATE: ", guildName);
    return packet;
}

network::Packet GuildAddRankPacket::build(const std::string& rankName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_ADD_RANK));
    packet.writeString(rankName);
    LOG_DEBUG("Built CMSG_GUILD_ADD_RANK: ", rankName);
    return packet;
}

network::Packet GuildDelRankPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_DEL_RANK));
    LOG_DEBUG("Built CMSG_GUILD_DEL_RANK");
    return packet;
}

network::Packet PetitionShowlistPacket::build(uint64_t npcGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PETITION_SHOWLIST));
    packet.writeUInt64(npcGuid);
    LOG_DEBUG("Built CMSG_PETITION_SHOWLIST: guid=", npcGuid);
    return packet;
}

network::Packet PetitionBuyPacket::build(uint64_t npcGuid, const std::string& guildName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PETITION_BUY));
    packet.writeUInt64(npcGuid);          // NPC GUID
    packet.writeUInt32(0);                // unk
    packet.writeUInt64(0);                // unk
    packet.writeString(guildName);        // guild name
    packet.writeUInt32(0);                // body text (empty)
    packet.writeUInt32(0);                // min sigs
    packet.writeUInt32(0);                // max sigs
    packet.writeUInt32(0);                // unk
    packet.writeUInt32(0);                // unk
    packet.writeUInt32(0);                // unk
    packet.writeUInt32(0);                // unk
    packet.writeUInt16(0);                // unk
    packet.writeUInt32(0);                // unk
    packet.writeUInt32(0);                // unk index
    packet.writeUInt32(0);                // unk
    LOG_DEBUG("Built CMSG_PETITION_BUY: npcGuid=", npcGuid, " name=", guildName);
    return packet;
}

bool PetitionShowlistParser::parse(network::Packet& packet, PetitionShowlistData& data) {
    if (packet.getSize() < 12) {
        LOG_ERROR("SMSG_PETITION_SHOWLIST too small: ", packet.getSize());
        return false;
    }
    data.npcGuid = packet.readUInt64();
    uint32_t count = packet.readUInt32();
    if (count > 0) {
        data.itemId = packet.readUInt32();
        data.displayId = packet.readUInt32();
        data.cost = packet.readUInt32();
        // Skip unused fields if present
        if (packet.hasRemaining(8)) {
            data.charterType = packet.readUInt32();
            data.requiredSigs = packet.readUInt32();
        }
    }
    LOG_INFO("Parsed SMSG_PETITION_SHOWLIST: npcGuid=", data.npcGuid, " cost=", data.cost);
    return true;
}

bool TurnInPetitionResultsParser::parse(network::Packet& packet, uint32_t& result) {
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_TURN_IN_PETITION_RESULTS too small: ", packet.getSize());
        return false;
    }
    result = packet.readUInt32();
    LOG_INFO("Parsed SMSG_TURN_IN_PETITION_RESULTS: result=", result);
    return true;
}

bool GuildQueryResponseParser::parse(network::Packet& packet, GuildQueryResponseData& data) {
    if (packet.getSize() < 8) {
        LOG_ERROR("SMSG_GUILD_QUERY_RESPONSE too small: ", packet.getSize());
        return false;
    }
    data.guildId = packet.readUInt32();

    // Validate before reading guild name
    if (!packet.hasData()) {
        LOG_WARNING("GuildQueryResponseParser: truncated before guild name");
        data.guildName.clear();
        return true;
    }
    data.guildName = packet.readString();

    // Read 10 rank names with validation
    for (int i = 0; i < 10; ++i) {
        if (!packet.hasData()) {
            LOG_WARNING("GuildQueryResponseParser: truncated at rank name ", i);
            data.rankNames[i].clear();
        } else {
            data.rankNames[i] = packet.readString();
        }
    }

    // Validate before reading emblem fields (5 uint32s = 20 bytes)
    if (!packet.hasRemaining(20)) {
        LOG_WARNING("GuildQueryResponseParser: truncated before emblem data");
        data.emblemStyle = 0;
        data.emblemColor = 0;
        data.borderStyle = 0;
        data.borderColor = 0;
        data.backgroundColor = 0;
        return true;
    }

    data.emblemStyle = packet.readUInt32();
    data.emblemColor = packet.readUInt32();
    data.borderStyle = packet.readUInt32();
    data.borderColor = packet.readUInt32();
    data.backgroundColor = packet.readUInt32();

    if (packet.hasRemaining(4)) {
        data.rankCount = packet.readUInt32();
    }
    LOG_INFO("Parsed SMSG_GUILD_QUERY_RESPONSE: guild=", data.guildName, " id=", data.guildId);
    return true;
}

bool GuildInfoParser::parse(network::Packet& packet, GuildInfoData& data) {
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_GUILD_INFO too small: ", packet.getSize());
        return false;
    }
    data.guildName = packet.readString();
    data.creationDay = packet.readUInt32();
    data.creationMonth = packet.readUInt32();
    data.creationYear = packet.readUInt32();
    data.numMembers = packet.readUInt32();
    data.numAccounts = packet.readUInt32();
    LOG_INFO("Parsed SMSG_GUILD_INFO: ", data.guildName, " members=", data.numMembers);
    return true;
}

bool GuildRosterParser::parse(network::Packet& packet, GuildRosterData& data) {
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_GUILD_ROSTER too small: ", packet.getSize());
        return false;
    }
    uint32_t numMembers = packet.readUInt32();

    // Cap members and ranks to prevent unbounded memory allocation
    const uint32_t MAX_GUILD_MEMBERS = 1000;
    if (numMembers > MAX_GUILD_MEMBERS) {
        LOG_WARNING("GuildRosterParser: numMembers capped (requested=", numMembers, ")");
        numMembers = MAX_GUILD_MEMBERS;
    }

    data.motd = packet.readString();
    data.guildInfo = packet.readString();

    if (!packet.hasRemaining(4)) {
        LOG_WARNING("GuildRosterParser: truncated before rankCount");
        data.ranks.clear();
        data.members.clear();
        return true;
    }

    uint32_t rankCount = packet.readUInt32();

    // Cap rank count to prevent unbounded allocation
    const uint32_t MAX_GUILD_RANKS = 20;
    if (rankCount > MAX_GUILD_RANKS) {
        LOG_WARNING("GuildRosterParser: rankCount capped (requested=", rankCount, ")");
        rankCount = MAX_GUILD_RANKS;
    }

    data.ranks.resize(rankCount);
    for (uint32_t i = 0; i < rankCount; ++i) {
        // Validate 4 bytes before each rank rights read
        if (!packet.hasRemaining(4)) {
            LOG_WARNING("GuildRosterParser: truncated rank at index ", i);
            break;
        }
        data.ranks[i].rights = packet.readUInt32();
        if (!packet.hasRemaining(4)) {
            data.ranks[i].goldLimit = 0;
        } else {
            data.ranks[i].goldLimit = packet.readUInt32();
        }
        // 6 bank tab flags + 6 bank tab items per day
        for (int t = 0; t < 6; ++t) {
            if (!packet.hasRemaining(8)) break;
            packet.readUInt32(); // tabFlags
            packet.readUInt32(); // tabItemsPerDay
        }
    }

    data.members.resize(numMembers);
    for (uint32_t i = 0; i < numMembers; ++i) {
        // Validate minimum bytes before reading member (guid+online+name at minimum is 9+ bytes)
        if (!packet.hasRemaining(9)) {
            LOG_WARNING("GuildRosterParser: truncated member at index ", i);
            break;
        }
        auto& m = data.members[i];
        m.guid = packet.readUInt64();
        m.online = (packet.readUInt8() != 0);

        // Validate before reading name string
        if (!packet.hasData()) {
            m.name.clear();
        } else {
            m.name = packet.readString();
        }

        // Validate before reading rank/level/class/gender/zone
        if (!packet.hasRemaining(1)) {
            m.rankIndex = 0;
            m.level = 1;
            m.classId = 0;
            m.gender = 0;
            m.zoneId = 0;
        } else {
            m.rankIndex = packet.readUInt32();
            if (!packet.hasRemaining(3)) {
                m.level = 1;
                m.classId = 0;
                m.gender = 0;
            } else {
                m.level = packet.readUInt8();
                m.classId = packet.readUInt8();
                m.gender = packet.readUInt8();
            }
            if (!packet.hasRemaining(4)) {
                m.zoneId = 0;
            } else {
                m.zoneId = packet.readUInt32();
            }
        }

        // Online status affects next fields
        if (!m.online) {
            if (!packet.hasRemaining(4)) {
                m.lastOnline = 0.0f;
            } else {
                m.lastOnline = packet.readFloat();
            }
        }

        // Read notes
        if (!packet.hasData()) {
            m.publicNote.clear();
            m.officerNote.clear();
        } else {
            m.publicNote = packet.readString();
            if (!packet.hasData()) {
                m.officerNote.clear();
            } else {
                m.officerNote = packet.readString();
            }
        }
    }
    LOG_INFO("Parsed SMSG_GUILD_ROSTER: ", numMembers, " members, motd=", data.motd);
    return true;
}

bool GuildEventParser::parse(network::Packet& packet, GuildEventData& data) {
    if (packet.getSize() < 2) {
        LOG_ERROR("SMSG_GUILD_EVENT too small: ", packet.getSize());
        return false;
    }
    data.eventType = packet.readUInt8();
    data.numStrings = packet.readUInt8();
    for (uint8_t i = 0; i < data.numStrings && i < 3; ++i) {
        data.strings[i] = packet.readString();
    }
    if (packet.hasRemaining(8)) {
        data.guid = packet.readUInt64();
    }
    LOG_INFO("Parsed SMSG_GUILD_EVENT: type=", static_cast<int>(data.eventType), " strings=", static_cast<int>(data.numStrings));
    return true;
}

bool GuildInviteResponseParser::parse(network::Packet& packet, GuildInviteResponseData& data) {
    if (packet.getSize() < 2) {
        LOG_ERROR("SMSG_GUILD_INVITE too small: ", packet.getSize());
        return false;
    }
    data.inviterName = packet.readString();
    data.guildName = packet.readString();
    LOG_INFO("Parsed SMSG_GUILD_INVITE: from=", data.inviterName, " guild=", data.guildName);
    return true;
}

bool GuildCommandResultParser::parse(network::Packet& packet, GuildCommandResultData& data) {
    if (packet.getSize() < 8) {
        LOG_ERROR("SMSG_GUILD_COMMAND_RESULT too small: ", packet.getSize());
        return false;
    }
    data.command = packet.readUInt32();
    data.name = packet.readString();
    data.errorCode = packet.readUInt32();
    LOG_INFO("Parsed SMSG_GUILD_COMMAND_RESULT: cmd=", data.command, " error=", data.errorCode);
    return true;
}

// ============================================================
// Ready Check
// ============================================================

network::Packet ReadyCheckPacket::build() {
    network::Packet packet(wireOpcode(Opcode::MSG_RAID_READY_CHECK));
    LOG_DEBUG("Built MSG_RAID_READY_CHECK");
    return packet;
}

network::Packet ReadyCheckConfirmPacket::build(bool ready) {
    network::Packet packet(wireOpcode(Opcode::MSG_RAID_READY_CHECK_CONFIRM));
    packet.writeUInt8(ready ? 1 : 0);
    LOG_DEBUG("Built MSG_RAID_READY_CHECK_CONFIRM: ready=", ready);
    return packet;
}

// ============================================================
// Duel
// ============================================================

network::Packet DuelAcceptPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_DUEL_ACCEPTED));
    LOG_DEBUG("Built CMSG_DUEL_ACCEPTED");
    return packet;
}

network::Packet DuelCancelPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_DUEL_CANCELLED));
    LOG_DEBUG("Built CMSG_DUEL_CANCELLED");
    return packet;
}

// ============================================================
// Party/Raid Management
// ============================================================

network::Packet GroupUninvitePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_UNINVITE_GUID));
    packet.writeString(playerName);
    LOG_DEBUG("Built CMSG_GROUP_UNINVITE_GUID for player: ", playerName);
    return packet;
}

network::Packet GroupDisbandPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_DISBAND));
    LOG_DEBUG("Built CMSG_GROUP_DISBAND");
    return packet;
}

network::Packet GroupRaidConvertPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_RAID_CONVERT));
    LOG_DEBUG("Built CMSG_GROUP_RAID_CONVERT");
    return packet;
}

network::Packet SetLootMethodPacket::build(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT_METHOD));
    packet.writeUInt32(method);
    packet.writeUInt32(threshold);
    packet.writeUInt64(masterLooterGuid);
    LOG_DEBUG("Built CMSG_LOOT_METHOD: method=", method, " threshold=", threshold,
              " masterLooter=0x", std::hex, masterLooterGuid, std::dec);
    return packet;
}

network::Packet RaidTargetUpdatePacket::build(uint8_t targetIndex, uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::MSG_RAID_TARGET_UPDATE));
    packet.writeUInt8(targetIndex);
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built MSG_RAID_TARGET_UPDATE, index: ", static_cast<uint32_t>(targetIndex), ", guid: 0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet RequestRaidInfoPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_REQUEST_RAID_INFO));
    LOG_DEBUG("Built CMSG_REQUEST_RAID_INFO");
    return packet;
}

// ============================================================
// Combat and Trade
// ============================================================

network::Packet DuelProposedPacket::build(uint64_t targetGuid) {
    // Duels are initiated via CMSG_CAST_SPELL with spell 7266 (Duel) targeted at the opponent.
    // There is no separate CMSG_DUEL_PROPOSED opcode in WoW.
    auto packet = CastSpellPacket::build(7266, targetGuid, 0);
    LOG_DEBUG("Built duel request (spell 7266) for target: 0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet BeginTradePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_BEGIN_TRADE));
    LOG_DEBUG("Built CMSG_BEGIN_TRADE");
    return packet;
}

network::Packet CancelTradePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_TRADE));
    LOG_DEBUG("Built CMSG_CANCEL_TRADE");
    return packet;
}

network::Packet AcceptTradePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_ACCEPT_TRADE));
    LOG_DEBUG("Built CMSG_ACCEPT_TRADE");
    return packet;
}

network::Packet SetTradeItemPacket::build(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_TRADE_ITEM));
    packet.writeUInt8(tradeSlot);
    packet.writeUInt8(bag);
    packet.writeUInt8(bagSlot);
    LOG_DEBUG("Built CMSG_SET_TRADE_ITEM slot=", static_cast<int>(tradeSlot), " bag=", static_cast<int>(bag), " bagSlot=", static_cast<int>(bagSlot));
    return packet;
}

network::Packet ClearTradeItemPacket::build(uint8_t tradeSlot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CLEAR_TRADE_ITEM));
    packet.writeUInt8(tradeSlot);
    LOG_DEBUG("Built CMSG_CLEAR_TRADE_ITEM slot=", static_cast<int>(tradeSlot));
    return packet;
}

network::Packet SetTradeGoldPacket::build(uint64_t copper) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_TRADE_GOLD));
    packet.writeUInt64(copper);
    LOG_DEBUG("Built CMSG_SET_TRADE_GOLD copper=", copper);
    return packet;
}

network::Packet UnacceptTradePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_UNACCEPT_TRADE));
    LOG_DEBUG("Built CMSG_UNACCEPT_TRADE");
    return packet;
}

network::Packet InitiateTradePacket::build(uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_INITIATE_TRADE));
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built CMSG_INITIATE_TRADE for target: 0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet AttackSwingPacket::build(uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ATTACKSWING));
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built CMSG_ATTACKSWING for target: 0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet AttackStopPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_ATTACKSTOP));
    LOG_DEBUG("Built CMSG_ATTACKSTOP");
    return packet;
}

network::Packet CancelCastPacket::build(uint32_t spellId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_CAST));
    packet.writeUInt32(0); // cast count/sequence
    packet.writeUInt32(spellId);
    LOG_DEBUG("Built CMSG_CANCEL_CAST for spell: ", spellId);
    return packet;
}

// ============================================================
// Random Roll
// ============================================================

network::Packet RandomRollPacket::build(uint32_t minRoll, uint32_t maxRoll) {
    network::Packet packet(wireOpcode(Opcode::MSG_RANDOM_ROLL));
    packet.writeUInt32(minRoll);
    packet.writeUInt32(maxRoll);
    LOG_DEBUG("Built MSG_RANDOM_ROLL: ", minRoll, "-", maxRoll);
    return packet;
}

bool RandomRollParser::parse(network::Packet& packet, RandomRollData& data) {
    // WotLK 3.3.5a format: min(4) + max(4) + result(4) + rollerGuid(8) = 20 bytes.
    // Previously read guid first (treating min|max as a uint64 GUID), producing
    // garbled roller identity and random numbers in /roll chat messages.
    if (!packet.hasRemaining(20)) {
        LOG_WARNING("SMSG_RANDOM_ROLL: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.minRoll = packet.readUInt32();
    data.maxRoll = packet.readUInt32();
    data.result = packet.readUInt32();
    data.rollerGuid = packet.readUInt64();
    data.targetGuid = 0;  // not present in protocol; kept for struct compatibility
    LOG_DEBUG("Parsed SMSG_RANDOM_ROLL: roller=0x", std::hex, data.rollerGuid, std::dec,
              " result=", data.result, " (", data.minRoll, "-", data.maxRoll, ")");
    return true;
}

network::Packet NameQueryPacket::build(uint64_t playerGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_NAME_QUERY));
    packet.writeUInt64(playerGuid);
    LOG_DEBUG("Built CMSG_NAME_QUERY: guid=0x", std::hex, playerGuid, std::dec);
    return packet;
}

bool NameQueryResponseParser::parse(network::Packet& packet, NameQueryResponseData& data) {
    // 3.3.5a: packedGuid, uint8 found
    // If found==0: CString name, CString realmName, uint8 race, uint8 gender, uint8 classId
    // Validation: packed GUID (1-8 bytes) + found flag (1 byte minimum)
    if (!packet.hasRemaining(2)) return false; // At least 1 for packed GUID + 1 for found

    size_t startPos = packet.getReadPos();
    data.guid = packet.readPackedGuid();

    // Validate found flag read
    if (!packet.hasRemaining(1)) {
        packet.setReadPos(startPos);
        return false;
    }
    data.found = packet.readUInt8();

    if (data.found != 0) {
        LOG_DEBUG("Name query: player not found for GUID 0x", std::hex, data.guid, std::dec);
        return true; // Valid response, just not found
    }

    // Validate strings: need at least 2 null terminators for empty strings
    if (!packet.hasRemaining(2)) {
        data.name.clear();
        data.realmName.clear();
        return !data.name.empty(); // Fail if name was required
    }

    data.name = packet.readString();
    data.realmName = packet.readString();

    // Validate final 3 uint8 fields (race, gender, classId)
    if (!packet.hasRemaining(3)) {
        LOG_WARNING("Name query: truncated fields after realmName, expected 3 uint8s");
        data.race = 0;
        data.gender = 0;
        data.classId = 0;
        return !data.name.empty();
    }

    data.race = packet.readUInt8();
    data.gender = packet.readUInt8();
    data.classId = packet.readUInt8();

    LOG_DEBUG("Name query response: ", data.name, " (race=", static_cast<int>(data.race),
             " class=", static_cast<int>(data.classId), ")");
    return true;
}

network::Packet CreatureQueryPacket::build(uint32_t entry, uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CREATURE_QUERY));
    packet.writeUInt32(entry);
    packet.writeUInt64(guid);
    LOG_DEBUG("Built CMSG_CREATURE_QUERY: entry=", entry, " guid=0x", std::hex, guid, std::dec);
    return packet;
}

bool CreatureQueryResponseParser::parse(network::Packet& packet, CreatureQueryResponseData& data) {
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_CREATURE_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    // High bit set means creature not found
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        LOG_DEBUG("Creature query: entry ", data.entry, " not found");
        data.name = "";
        return true;
    }

    // 4 name strings (only first is usually populated)
    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4
    data.subName = packet.readString();
    data.iconName = packet.readString();

    // WotLK: 4 fixed fields after iconName (typeFlags, creatureType, family, rank)
    // Validate minimum size for these fields: 4×4 = 16 bytes
    if (!packet.hasRemaining(16)) {
        LOG_WARNING("SMSG_CREATURE_QUERY_RESPONSE: truncated before typeFlags (entry=", data.entry, ")");
        data.typeFlags = 0;
        data.creatureType = 0;
        data.family = 0;
        data.rank = 0;
        return true;  // Have name/sub/icon; base fields are important but optional
    }

    data.typeFlags = packet.readUInt32();
    data.creatureType = packet.readUInt32();
    data.family = packet.readUInt32();
    data.rank = packet.readUInt32();

    // killCredit[2] + displayId[4] = 6 × 4 = 24 bytes
    if (!packet.hasRemaining(24)) {
        LOG_WARNING("SMSG_CREATURE_QUERY_RESPONSE: truncated before displayIds (entry=", data.entry, ")");
        LOG_DEBUG("Creature query response: ", data.name, " (type=", data.creatureType,
                 " rank=", data.rank, ")");
        return true;
    }

    packet.readUInt32();  // killCredit[0]
    packet.readUInt32();  // killCredit[1]
    data.displayId[0] = packet.readUInt32();
    data.displayId[1] = packet.readUInt32();
    data.displayId[2] = packet.readUInt32();
    data.displayId[3] = packet.readUInt32();

    // Skip remaining fields (healthMultiplier, powerMultiplier, racialLeader, questItems, movementId)

    LOG_DEBUG("Creature query response: ", data.name, " (type=", data.creatureType,
             " rank=", data.rank, " displayIds=[", data.displayId[0], ",",
             data.displayId[1], ",", data.displayId[2], ",", data.displayId[3], "])");
    return true;
}

// ---- GameObject Query ----

network::Packet GameObjectQueryPacket::build(uint32_t entry, uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GAMEOBJECT_QUERY));
    packet.writeUInt32(entry);
    packet.writeUInt64(guid);
    LOG_DEBUG("Built CMSG_GAMEOBJECT_QUERY: entry=", entry, " guid=0x", std::hex, guid, std::dec);
    return packet;
}

bool GameObjectQueryResponseParser::parse(network::Packet& packet, GameObjectQueryResponseData& data) {
    // Validate minimum packet size: entry(4)
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_GAMEOBJECT_QUERY_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    // High bit set means gameobject not found
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        LOG_DEBUG("GameObject query: entry ", data.entry, " not found");
        data.name = "";
        return true;
    }

    // Validate minimum size for fixed fields: type(4) + displayId(4)
    if (!packet.hasRemaining(8)) {
        LOG_ERROR("SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated before names (entry=", data.entry, ")");
        return false;
    }

    data.type = packet.readUInt32();       // GameObjectType
    data.displayId = packet.readUInt32();
    // 4 name strings (only first is usually populated)
    data.name = packet.readString();
    // name2, name3, name4
    packet.readString();
    packet.readString();
    packet.readString();

    // WotLK: 3 extra strings before data[] (iconName, castBarCaption, unk1)
    packet.readString();  // iconName
    packet.readString();  // castBarCaption
    packet.readString();  // unk1

    // Read 24 type-specific data fields
    size_t remaining = packet.getRemainingSize();
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
            LOG_WARNING("SMSG_GAMEOBJECT_QUERY_RESPONSE: truncated in data fields (", fieldsToRead,
                        " of 24 read, entry=", data.entry, ")");
        }
    }

    LOG_DEBUG("GameObject query response: ", data.name, " (type=", data.type, " entry=", data.entry, ")");
    return true;
}

network::Packet PageTextQueryPacket::build(uint32_t pageId, uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_PAGE_TEXT_QUERY));
    packet.writeUInt32(pageId);
    packet.writeUInt64(guid);
    return packet;
}

bool PageTextQueryResponseParser::parse(network::Packet& packet, PageTextQueryResponseData& data) {
    if (!packet.hasRemaining(4)) return false;
    data.pageId = packet.readUInt32();
    data.text = normalizeWowTextTokens(packet.readString());
    if (packet.hasRemaining(4)) {
        data.nextPageId = packet.readUInt32();
    } else {
        data.nextPageId = 0;
    }
    return data.isValid();
}

// ---- Item Query ----

network::Packet ItemQueryPacket::build(uint32_t entry, uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ITEM_QUERY_SINGLE));
    packet.writeUInt32(entry);
    packet.writeUInt64(guid);
    LOG_DEBUG("Built CMSG_ITEM_QUERY_SINGLE: entry=", entry, " guid=0x", std::hex, guid, std::dec);
    return packet;
}

const char* getItemSubclassName(uint32_t itemClass, uint32_t subClass) {
    if (itemClass == 2) { // Weapon
        switch (subClass) {
            case 0: return "Axe"; case 1: return "Axe";
            case 2: return "Bow"; case 3: return "Gun";
            case 4: return "Mace"; case 5: return "Mace";
            case 6: return "Polearm"; case 7: return "Sword";
            case 8: return "Sword"; case 9: return "Obsolete";
            case 10: return "Staff"; case 13: return "Fist Weapon";
            case 15: return "Dagger"; case 16: return "Thrown";
            case 18: return "Crossbow"; case 19: return "Wand";
            case 20: return "Fishing Pole";
            default: return "Weapon";
        }
    }
    if (itemClass == 4) { // Armor
        switch (subClass) {
            case 0: return "Miscellaneous"; case 1: return "Cloth";
            case 2: return "Leather"; case 3: return "Mail";
            case 4: return "Plate"; case 6: return "Shield";
            default: return "Armor";
        }
    }
    return "";
}

bool ItemQueryResponseParser::parse(network::Packet& packet, ItemQueryResponseData& data) {
    // Validate minimum packet size: entry(4) + item not found check
    if (packet.getSize() < 4) {
        LOG_ERROR("SMSG_ITEM_QUERY_SINGLE_RESPONSE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.entry = packet.readUInt32();

    // High bit set means item not found
    if (data.entry & 0x80000000) {
        data.entry &= ~0x80000000;
        LOG_DEBUG("Item query: entry ", data.entry, " not found");
        return true;
    }

    // Validate minimum size for fixed fields before reading: itemClass(4) + subClass(4) + soundOverride(4)
    // + 4 name strings + displayInfoId(4) + quality(4) = at least 24 bytes more
    if (!packet.hasRemaining(24)) {
        LOG_ERROR("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before displayInfoId (entry=", data.entry, ")");
        return false;
    }

    uint32_t itemClass = packet.readUInt32();
    uint32_t subClass = packet.readUInt32();
    data.itemClass = itemClass;
    data.subClass = subClass;
    packet.readUInt32(); // SoundOverrideSubclass

    data.subclassName = getItemSubclassName(itemClass, subClass);

    // 4 name strings
    data.name = packet.readString();
    packet.readString(); // name2
    packet.readString(); // name3
    packet.readString(); // name4

    data.displayInfoId = packet.readUInt32();
    data.quality = packet.readUInt32();

    // WotLK 3.3.5a (TrinityCore/AzerothCore): Flags, Flags2, BuyCount, BuyPrice, SellPrice
    // Some server variants omit BuyCount (4 fields instead of 5).
    // Read 5 fields and validate InventoryType; if it looks implausible, rewind and try 4.
    const size_t postQualityPos = packet.getReadPos();
    if (!packet.hasRemaining(24)) {
        LOG_ERROR("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before flags (entry=", data.entry, ")");
        return false;
    }
    data.itemFlags = packet.readUInt32(); // Flags
    packet.readUInt32(); // Flags2
    packet.readUInt32(); // BuyCount
    packet.readUInt32(); // BuyPrice
    data.sellPrice = packet.readUInt32(); // SellPrice
    data.inventoryType = packet.readUInt32();

    if (data.inventoryType > 28) {
        // inventoryType out of range — BuyCount probably not present; rewind and try 4 fields
        packet.setReadPos(postQualityPos);
        data.itemFlags = packet.readUInt32(); // Flags
        packet.readUInt32(); // Flags2
        packet.readUInt32(); // BuyPrice
        data.sellPrice = packet.readUInt32(); // SellPrice
        data.inventoryType = packet.readUInt32();
    }

    // Validate minimum size for remaining fixed fields before inventoryType through containerSlots: 13×4 = 52 bytes
    if (!packet.hasRemaining(52)) {
        LOG_ERROR("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before statsCount (entry=", data.entry, ")");
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

    // Read statsCount with bounds validation
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated at statsCount (entry=", data.entry, ")");
        return true;  // Have enough for core fields; stats are optional
    }
    uint32_t statsCount = packet.readUInt32();

    // Cap statsCount to prevent excessive iteration
    constexpr uint32_t kMaxItemStats = 10;
    if (statsCount > kMaxItemStats) {
        LOG_WARNING("SMSG_ITEM_QUERY_SINGLE_RESPONSE: statsCount=", statsCount, " exceeds max ",
                    kMaxItemStats, " (entry=", data.entry, "), capping");
        statsCount = kMaxItemStats;
    }

    // Server sends exactly statsCount stat pairs (not always 10).
    uint32_t statsToRead = std::min(statsCount, 10u);
    for (uint32_t i = 0; i < statsToRead; i++) {
        // Each stat is 2 uint32s (type + value) = 8 bytes
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("SMSG_ITEM_QUERY_SINGLE_RESPONSE: stat ", i, " truncated (entry=", data.entry, ")");
            break;
        }
        uint32_t statType = packet.readUInt32();
        int32_t statValue = static_cast<int32_t>(packet.readUInt32());
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

    // ScalingStatDistribution and ScalingStatValue
    if (!packet.hasRemaining(8)) {
        LOG_WARNING("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before scaling stats (entry=", data.entry, ")");
        return true;  // Have core fields; scaling is optional
    }
    packet.readUInt32(); // ScalingStatDistribution
    packet.readUInt32(); // ScalingStatValue

    // WotLK 3.3.5a: 2 damage entries (12 bytes each) + armor + 6 resists + delay + ammoType + rangedModRange
    // = 24 + 36 + 4 = 64 bytes minimum. Guard here because the section above
    // returns early on truncation, and every other section has its own guard.
    if (!packet.hasRemaining(64)) {
        LOG_WARNING("SMSG_ITEM_QUERY_SINGLE_RESPONSE: truncated before damage/armor (entry=", data.entry, ")");
        return true;
    }

    // WotLK 3.3.5a: MAX_ITEM_PROTO_DAMAGES = 2
    bool haveWeaponDamage = false;
    for (int i = 0; i < 2; i++) {
        float dmgMin = packet.readFloat();
        float dmgMax = packet.readFloat();
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
    data.holyRes   = static_cast<int32_t>(packet.readUInt32()); // HolyRes
    data.fireRes   = static_cast<int32_t>(packet.readUInt32()); // FireRes
    data.natureRes = static_cast<int32_t>(packet.readUInt32()); // NatureRes
    data.frostRes  = static_cast<int32_t>(packet.readUInt32()); // FrostRes
    data.shadowRes = static_cast<int32_t>(packet.readUInt32()); // ShadowRes
    data.arcaneRes = static_cast<int32_t>(packet.readUInt32()); // ArcaneRes
    data.delayMs = packet.readUInt32();
    packet.readUInt32(); // AmmoType
    packet.readFloat();  // RangedModRange

    // 5 item spells: SpellId, SpellTrigger, SpellCharges, SpellCooldown, SpellCategory, SpellCategoryCooldown
    for (int i = 0; i < 5; i++) {
        if (!packet.hasRemaining(24)) break;
        data.spells[i].spellId = packet.readUInt32();
        data.spells[i].spellTrigger = packet.readUInt32();
        packet.readUInt32(); // SpellCharges
        packet.readUInt32(); // SpellCooldown
        packet.readUInt32(); // SpellCategory
        packet.readUInt32(); // SpellCategoryCooldown
    }

    // Bonding type (0=none, 1=BoP, 2=BoE, 3=BoU, 4=BoQ)
    if (packet.hasRemaining(4))
        data.bindType = packet.readUInt32();

    // Flavor/lore text (Description cstring)
    if (packet.hasData())
        data.description = packet.readString();

    // Post-description fields: PageText, LanguageID, PageMaterial, StartQuest
    if (packet.hasRemaining(16)) {
        packet.readUInt32(); // PageText
        packet.readUInt32(); // LanguageID
        packet.readUInt32(); // PageMaterial
        data.startQuestId = packet.readUInt32(); // StartQuest
    }

    // WotLK 3.3.5a: additional fields after StartQuest (read up to socket data)
    // LockID(4), Material(4), Sheath(4), RandomProperty(4), RandomSuffix(4),
    // Block(4), ItemSet(4), MaxDurability(4), Area(4), Map(4), BagFamily(4),
    // TotemCategory(4) = 48 bytes before sockets
    constexpr size_t kPreSocketSkip = 48;
    if (packet.getReadPos() + kPreSocketSkip + 28 <= packet.getSize()) {
        // LockID(0), Material(1), Sheath(2), RandomProperty(3), RandomSuffix(4), Block(5)
        for (size_t i = 0; i < 6; ++i) packet.readUInt32();
        data.itemSetId = packet.readUInt32(); // ItemSet(6)
        // MaxDurability(7), Area(8), Map(9), BagFamily(10), TotemCategory(11)
        for (size_t i = 0; i < 5; ++i) packet.readUInt32();
        // 3 socket slots: socketColor (4 bytes each)
        data.socketColor[0] = packet.readUInt32();
        data.socketColor[1] = packet.readUInt32();
        data.socketColor[2] = packet.readUInt32();
        // 3 socket content (gem enchantment IDs — skip, not currently displayed)
        packet.readUInt32();
        packet.readUInt32();
        packet.readUInt32();
        // socketBonus (enchantmentId)
        data.socketBonus = packet.readUInt32();
    }

    data.valid = !data.name.empty();
    return true;
}

// ============================================================
// Creature Movement
// ============================================================

bool MonsterMoveParser::parse(network::Packet& packet, MonsterMoveData& data) {
    // PackedGuid
    data.guid = packet.readPackedGuid();
    if (data.guid == 0) return false;

    // uint8 unk (toggle for MOVEMENTFLAG2_UNK7)
    if (!packet.hasData()) return false;
    packet.readUInt8();

    // Current position (server coords: float x, y, z)
    if (!packet.hasRemaining(12)) return false;
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();

    // uint32 splineId
    if (!packet.hasRemaining(4)) return false;
    packet.readUInt32();

    // uint8 moveType
    if (!packet.hasData()) return false;
    data.moveType = packet.readUInt8();

    if (data.moveType == 1) {
        // Stop - no more required data
        data.destX = data.x;
        data.destY = data.y;
        data.destZ = data.z;
        data.hasDest = false;
        return true;
    }

    // Read facing data based on move type
    if (data.moveType == 2) {
        // FacingSpot: float x, y, z
        if (!packet.hasRemaining(12)) return false;
        packet.readFloat(); packet.readFloat(); packet.readFloat();
    } else if (data.moveType == 3) {
        // FacingTarget: uint64 guid
        if (!packet.hasRemaining(8)) return false;
        data.facingTarget = packet.readUInt64();
    } else if (data.moveType == 4) {
        // FacingAngle: float angle
        if (!packet.hasRemaining(4)) return false;
        data.facingAngle = packet.readFloat();
    }

    // uint32 splineFlags
    if (!packet.hasRemaining(4)) return false;
    data.splineFlags = packet.readUInt32();

    // WotLK 3.3.5a SplineFlags (from TrinityCore/MaNGOS MoveSplineFlag.h):
    //   Animation    = 0x00400000
    //   Parabolic    = 0x00000800
    //   Catmullrom   = 0x00080000  \  either means uncompressed (absolute) waypoints
    //   Flying       = 0x00002000  /

    // [if Animation] uint8 animationType + int32 effectStartTime (5 bytes)
    if (data.splineFlags & 0x00400000) {
        if (!packet.hasRemaining(5)) return false;
        packet.readUInt8();  // animationType
        packet.readUInt32(); // effectStartTime (int32, read as uint32 same size)
    }

    // uint32 duration
    if (!packet.hasRemaining(4)) return false;
    data.duration = packet.readUInt32();

    // [if Parabolic] float verticalAcceleration + int32 effectStartTime (8 bytes)
    if (data.splineFlags & 0x00000800) {
        if (!packet.hasRemaining(8)) return false;
        packet.readFloat(); // verticalAcceleration
        packet.readUInt32(); // effectStartTime
    }

    // uint32 pointCount
    if (!packet.hasRemaining(4)) return false;
    uint32_t pointCount = packet.readUInt32();

    if (pointCount == 0) return true;

    constexpr uint32_t kMaxSplinePoints = 1000;
    if (pointCount > kMaxSplinePoints) {
        LOG_WARNING("SMSG_MONSTER_MOVE: pointCount=", pointCount, " exceeds max ", kMaxSplinePoints,
                    " (guid=0x", std::hex, data.guid, std::dec, ")");
        return false;
    }

    // Catmullrom or Flying → all waypoints stored as absolute float3 (uncompressed).
    // Otherwise: first float3 is final destination, remaining are packed deltas.
    bool uncompressed = (data.splineFlags & (0x00080000 | 0x00002000)) != 0;

    if (uncompressed) {
        // Read last point as destination
        // Skip to last point: each point is 12 bytes
        if (pointCount > 1) {
            for (uint32_t i = 0; i < pointCount - 1; i++) {
                if (!packet.hasRemaining(12)) return true;
                packet.readFloat(); packet.readFloat(); packet.readFloat();
            }
        }
        if (!packet.hasRemaining(12)) return true;
        data.destX = packet.readFloat();
        data.destY = packet.readFloat();
        data.destZ = packet.readFloat();
        data.hasDest = true;
    } else {
        // Compressed: first 3 floats are the destination (final point)
        if (!packet.hasRemaining(12)) return true;
        data.destX = packet.readFloat();
        data.destY = packet.readFloat();
        data.destZ = packet.readFloat();
        data.hasDest = true;
    }

    LOG_DEBUG("MonsterMove: guid=0x", std::hex, data.guid, std::dec,
              " type=", static_cast<int>(data.moveType), " dur=", data.duration, "ms",
              " dest=(", data.destX, ",", data.destY, ",", data.destZ, ")");

    return true;
}

bool MonsterMoveParser::parseVanilla(network::Packet& packet, MonsterMoveData& data) {
    data.guid = packet.readPackedGuid();
    if (data.guid == 0) return false;

    if (!packet.hasRemaining(12)) return false;
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();

    // Turtle WoW movement payload uses a spline-style layout after XYZ:
    //   uint32 splineIdOrTick
    //   uint8  moveType
    //   [if moveType 2/3/4] facing payload
    //   uint32 splineFlags
    //   [if Animation] uint8 + uint32
    //   uint32 duration
    //   [if Parabolic] float + uint32
    //   uint32 pointCount
    //   float[3] dest
    //   uint32 packedPoints[pointCount-1]
    if (!packet.hasRemaining(4)) return false;
    /*uint32_t splineIdOrTick =*/ packet.readUInt32();

    if (!packet.hasData()) return false;
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

    // Animation flag (same bit as WotLK MoveSplineFlag::Animation)
    if (data.splineFlags & 0x00400000) {
        if (!packet.hasRemaining(5)) return false;
        packet.readUInt8();
        packet.readUInt32();
    }

    if (!packet.hasRemaining(4)) return false;
    data.duration = packet.readUInt32();

    // Parabolic flag (same bit as WotLK MoveSplineFlag::Parabolic)
    if (data.splineFlags & 0x00000800) {
        if (!packet.hasRemaining(8)) return false;
        packet.readFloat();
        packet.readUInt32();
    }

    if (!packet.hasRemaining(4)) return false;
    uint32_t pointCount = packet.readUInt32();

    if (pointCount == 0) return true;

    // Reject extreme point counts from malformed packets.
    constexpr uint32_t kMaxSplinePoints = 1000;
    if (pointCount > kMaxSplinePoints) {
        return false;
    }

    size_t requiredBytes = 12;
    if (pointCount > 1) {
        requiredBytes += static_cast<size_t>(pointCount - 1) * 4ull;
    }
    if (!packet.hasRemaining(requiredBytes)) return false;

    // First float[3] is destination.
    data.destX = packet.readFloat();
    data.destY = packet.readFloat();
    data.destZ = packet.readFloat();
    data.hasDest = true;

    // Remaining waypoints are packed as uint32 deltas.
    if (pointCount > 1) {
        size_t skipBytes = static_cast<size_t>(pointCount - 1) * 4;
        size_t newPos = packet.getReadPos() + skipBytes;
        if (newPos > packet.getSize()) return false;
        packet.setReadPos(newPos);
    }

    LOG_DEBUG("MonsterMove(turtle): guid=0x", std::hex, data.guid, std::dec,
              " type=", static_cast<int>(data.moveType), " dur=", data.duration, "ms",
              " dest=(", data.destX, ",", data.destY, ",", data.destZ, ")");

    return true;
}


// ============================================================
// Phase 2: Combat Core
// ============================================================

bool AttackStartParser::parse(network::Packet& packet, AttackStartData& data) {
    if (packet.getSize() < 16) return false;
    data.attackerGuid = packet.readUInt64();
    data.victimGuid = packet.readUInt64();
    LOG_DEBUG("Attack started: 0x", std::hex, data.attackerGuid,
             " -> 0x", data.victimGuid, std::dec);
    return true;
}

bool AttackStopParser::parse(network::Packet& packet, AttackStopData& data) {
    data.attackerGuid = packet.readPackedGuid();
    data.victimGuid = packet.readPackedGuid();
    if (packet.hasData()) {
        data.unknown = packet.readUInt32();
    }
    LOG_DEBUG("Attack stopped: 0x", std::hex, data.attackerGuid, std::dec);
    return true;
}

bool AttackerStateUpdateParser::parse(network::Packet& packet, AttackerStateUpdateData& data) {
    // Upfront validation: hitInfo(4) + packed GUIDs(1-8 each) + totalDamage(4) + overkill(4) + subDamageCount(1) = 17 bytes minimum
    if (!packet.hasRemaining(17)) return false;

    size_t startPos = packet.getReadPos();
    data.hitInfo = packet.readUInt32();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.attackerGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.targetGuid = packet.readPackedGuid();

    // Validate totalDamage + overkill + subDamageCount can be read (9 bytes)
    // WotLK (AzerothCore) sends: damage(4) + overkill(4) + subDamageCount(1)
    if (!packet.hasRemaining(9)) {
        packet.setReadPos(startPos);
        return false;
    }

    data.totalDamage = static_cast<int32_t>(packet.readUInt32());
    data.overkill = static_cast<int32_t>(packet.readUInt32());
    data.subDamageCount = packet.readUInt8();

    // Cap subDamageCount: each entry is 20 bytes. If the claimed count
    // exceeds what the remaining bytes can hold, a GUID was mis-parsed
    // (off by one byte), causing the school-mask byte to be read as count.
    // In that case clamp to the number of full entries that fit.
    {
        size_t remaining = packet.getRemainingSize();
        size_t maxFit    = remaining / 20;
        if (data.subDamageCount > maxFit) {
            data.subDamageCount = static_cast<uint8_t>(std::min<size_t>(maxFit, 64));
        } else if (data.subDamageCount > 64) {
            data.subDamageCount = 64;
        }
    }
    if (data.subDamageCount == 0) return false;

    data.subDamages.reserve(data.subDamageCount);
    for (uint8_t i = 0; i < data.subDamageCount; ++i) {
        // Each sub-damage entry needs 20 bytes: schoolMask(4) + damage(4) + intDamage(4) + absorbed(4) + resisted(4)
        if (!packet.hasRemaining(20)) {
            data.subDamageCount = i;
            break;
        }
        SubDamage sub;
        sub.schoolMask = packet.readUInt32();
        sub.damage = packet.readFloat();
        sub.intDamage = packet.readUInt32();
        sub.absorbed = packet.readUInt32();
        sub.resisted = packet.readUInt32();
        data.subDamages.push_back(sub);
    }

    // Validate victimState + overkill fields (8 bytes)
    if (!packet.hasRemaining(8)) {
        data.victimState = 0;
        return !data.subDamages.empty();
    }

    data.victimState = packet.readUInt32();
    // WotLK: attackerState(4) + meleeSpellId(4) follow victimState
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() >= 4) packet.readUInt32(); // attackerState (always 0)
    if (rem() >= 4) packet.readUInt32(); // meleeSpellId (0 for auto-attack)

    // hitInfo-conditional fields: HITINFO_BLOCK(0x2000), RAGE_GAIN(0x20000), FAKE_DAMAGE(0x40)
    if ((data.hitInfo & 0x2000) && rem() >= 4)  data.blocked  = packet.readUInt32();
    else data.blocked = 0;
    // RAGE_GAIN and FAKE_DAMAGE both add a uint32 we can skip
    if ((data.hitInfo & 0x20000) && rem() >= 4) packet.readUInt32(); // rage gain
    if ((data.hitInfo & 0x40)    && rem() >= 4) packet.readUInt32(); // fake damage total

    LOG_DEBUG("Melee hit: ", data.totalDamage, " damage",
              data.isCrit() ? " (CRIT)" : "",
              data.isMiss() ? " (MISS)" : "");
    return true;
}

bool SpellDamageLogParser::parse(network::Packet& packet, SpellDamageLogData& data) {
    // Upfront validation:
    // packed GUIDs(1-8 each) + spellId(4) + damage(4) + overkill(4) + schoolMask(1)
    // + absorbed(4) + resisted(4) + periodicLog(1) + unused(1) + blocked(4) + flags(4)
    // = 33 bytes minimum.
    if (!packet.hasRemaining(33)) return false;

    size_t startPos = packet.getReadPos();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.targetGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.attackerGuid = packet.readPackedGuid();

    // Validate core fields (spellId + damage + overkill + schoolMask + absorbed + resisted = 21 bytes)
    if (!packet.hasRemaining(21)) {
        packet.setReadPos(startPos);
        return false;
    }

    data.spellId = packet.readUInt32();
    data.damage = packet.readUInt32();
    data.overkill = packet.readUInt32();
    data.schoolMask = packet.readUInt8();
    data.absorbed = packet.readUInt32();
    data.resisted = packet.readUInt32();

    // Remaining fields are required for a complete event.
    // Reject truncated packets so we do not emit partial/incorrect combat entries.
    if (!packet.hasRemaining(10)) {
        packet.setReadPos(startPos);
        return false;
    }

    (void)packet.readUInt8(); // periodicLog (not displayed)
    packet.readUInt8(); // unused
    packet.readUInt32(); // blocked
    uint32_t flags = packet.readUInt32();  // flags IS used — bit 0x02 = crit
    data.isCrit = (flags & 0x02) != 0;

    LOG_DEBUG("Spell damage: spellId=", data.spellId, " dmg=", data.damage,
              data.isCrit ? " CRIT" : "");
    return true;
}

bool SpellHealLogParser::parse(network::Packet& packet, SpellHealLogData& data) {
    // Upfront validation: packed GUIDs(1-8 each) + spellId(4) + heal(4) + overheal(4) + absorbed(4) + critFlag(1) = 21 bytes minimum
    if (!packet.hasRemaining(21)) return false;

    size_t startPos = packet.getReadPos();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.targetGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.casterGuid = packet.readPackedGuid();

    // Validate remaining fields (spellId + heal + overheal + absorbed + critFlag = 17 bytes)
    if (!packet.hasRemaining(17)) {
        packet.setReadPos(startPos);
        return false;
    }

    data.spellId = packet.readUInt32();
    data.heal = packet.readUInt32();
    data.overheal = packet.readUInt32();
    data.absorbed = packet.readUInt32();
    uint8_t critFlag = packet.readUInt8();
    data.isCrit = (critFlag != 0);

    LOG_DEBUG("Spell heal: spellId=", data.spellId, " heal=", data.heal,
              data.isCrit ? " CRIT" : "");
    return true;
}

// ============================================================
// XP Gain
// ============================================================

bool XpGainParser::parse(network::Packet& packet, XpGainData& data) {
    // Validate minimum packet size: victimGuid(8) + totalXp(4) + type(1)
    if (!packet.hasRemaining(13)) {
        LOG_WARNING("SMSG_LOG_XPGAIN: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.victimGuid = packet.readUInt64();
    data.totalXp = packet.readUInt32();
    data.type = packet.readUInt8();
    if (data.type == 0) {
        // Kill XP: float groupRate (1.0 = solo) + uint8 RAF flag
        // Validate before reading conditional fields
        if (packet.hasRemaining(5)) {
            float groupRate = packet.readFloat();
            packet.readUInt8(); // RAF bonus flag
            // Group bonus = total - (total / rate); only if grouped (rate > 1)
            if (groupRate > 1.0f) {
                data.groupBonus = data.totalXp - static_cast<uint32_t>(data.totalXp / groupRate);
            }
        }
    }
    LOG_DEBUG("XP gain: ", data.totalXp, " xp (type=", static_cast<int>(data.type), ")");
    return data.totalXp > 0;
}

// ============================================================
// Phase 3: Spells, Action Bar, Auras
// ============================================================

bool InitialSpellsParser::parse(network::Packet& packet, InitialSpellsData& data,
                                bool vanillaFormat) {
    // Validate minimum packet size for header: talentSpec(1) + spellCount(2)
    if (!packet.hasRemaining(3)) {
        LOG_ERROR("SMSG_INITIAL_SPELLS: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.talentSpec = packet.readUInt8();
    uint16_t spellCount = packet.readUInt16();

    // Cap spell count to prevent excessive iteration.
    // WotLK characters with all ranks, mounts, professions, and racials can
    // know 400-600 spells; 1024 covers all practical cases with headroom.
    constexpr uint16_t kMaxSpells = 1024;
    if (spellCount > kMaxSpells) {
        LOG_WARNING("SMSG_INITIAL_SPELLS: spellCount=", spellCount, " exceeds max ", kMaxSpells,
                    ", capping");
        spellCount = kMaxSpells;
    }

    LOG_DEBUG("SMSG_INITIAL_SPELLS: spellCount=", spellCount,
              vanillaFormat ? " (vanilla uint16 format)" : " (TBC/WotLK uint32 format)");

    data.spellIds.reserve(spellCount);
    for (uint16_t i = 0; i < spellCount; ++i) {
        // Vanilla spell: spellId(2) + slot(2) = 4 bytes
        // TBC/WotLK spell: spellId(4) + unknown(2) = 6 bytes
        size_t spellEntrySize = vanillaFormat ? 4 : 6;
        if (!packet.hasRemaining(spellEntrySize)) {
            LOG_WARNING("SMSG_INITIAL_SPELLS: spell ", i, " truncated (", spellCount, " expected)");
            break;
        }

        uint32_t spellId;
        if (vanillaFormat) {
            spellId = packet.readUInt16();
            packet.readUInt16(); // slot
        } else {
            spellId = packet.readUInt32();
            packet.readUInt16(); // unknown (always 0)
        }
        if (spellId != 0) {
            data.spellIds.push_back(spellId);
        }
    }

    // Validate minimum packet size for cooldownCount (2 bytes)
    if (!packet.hasRemaining(2)) {
        LOG_WARNING("SMSG_INITIAL_SPELLS: truncated before cooldownCount (parsed ", data.spellIds.size(),
                    " spells)");
        return true;  // Have spells; cooldowns are optional
    }

    uint16_t cooldownCount = packet.readUInt16();

    // Cap cooldown count to prevent excessive iteration.
    // Some servers include entries for all spells (even with zero remaining time)
    // to communicate category cooldown data, so the count can be high.
    constexpr uint16_t kMaxCooldowns = 1024;
    if (cooldownCount > kMaxCooldowns) {
        LOG_WARNING("SMSG_INITIAL_SPELLS: cooldownCount=", cooldownCount, " exceeds max ", kMaxCooldowns,
                    ", capping");
        cooldownCount = kMaxCooldowns;
    }

    data.cooldowns.reserve(cooldownCount);
    for (uint16_t i = 0; i < cooldownCount; ++i) {
        // Vanilla cooldown: spellId(2) + itemId(2) + categoryId(2) + cooldownMs(4) + categoryCooldownMs(4) = 14 bytes
        // TBC/WotLK cooldown: spellId(4) + itemId(2) + categoryId(2) + cooldownMs(4) + categoryCooldownMs(4) = 16 bytes
        size_t cooldownEntrySize = vanillaFormat ? 14 : 16;
        if (!packet.hasRemaining(cooldownEntrySize)) {
            LOG_WARNING("SMSG_INITIAL_SPELLS: cooldown ", i, " truncated (", cooldownCount, " expected)");
            break;
        }

        SpellCooldownEntry entry;
        if (vanillaFormat) {
            entry.spellId = packet.readUInt16();
        } else {
            entry.spellId = packet.readUInt32();
        }
        entry.itemId = packet.readUInt16();
        entry.categoryId = packet.readUInt16();
        entry.cooldownMs = packet.readUInt32();
        entry.categoryCooldownMs = packet.readUInt32();
        data.cooldowns.push_back(entry);
    }

    LOG_INFO("Initial spells parsed: ", data.spellIds.size(), " spells, ",
             data.cooldowns.size(), " cooldowns");

    if (!data.spellIds.empty()) {
        std::string first10;
        for (size_t i = 0; i < std::min(size_t(10), data.spellIds.size()); ++i) {
            if (!first10.empty()) first10 += ", ";
            first10 += std::to_string(data.spellIds[i]);
        }
        LOG_DEBUG("Initial spell IDs (first 10): ", first10);
    }

    return true;
}

network::Packet CastSpellPacket::build(uint32_t spellId, uint64_t targetGuid, uint8_t castCount) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CAST_SPELL));
    packet.writeUInt8(castCount);
    packet.writeUInt32(spellId);
    packet.writeUInt8(0x00); // castFlags = 0 for normal cast

    // SpellCastTargets
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
        for (int i = 0; i < byteCount; ++i) {
            packet.writeUInt8(bytes[i]);
        }
    } else {
        packet.writeUInt32(0x00); // TARGET_FLAG_SELF
    }

    LOG_DEBUG("Built CMSG_CAST_SPELL: spell=", spellId, " target=0x",
              std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet CancelAuraPacket::build(uint32_t spellId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_AURA));
    packet.writeUInt32(spellId);
    return packet;
}

network::Packet PetActionPacket::build(uint64_t petGuid, uint32_t action, uint64_t targetGuid) {
    // CMSG_PET_ACTION: petGuid(8) + action(4) + targetGuid(8)
    network::Packet packet(wireOpcode(Opcode::CMSG_PET_ACTION));
    packet.writeUInt64(petGuid);
    packet.writeUInt32(action);
    packet.writeUInt64(targetGuid);
    return packet;
}

bool CastFailedParser::parse(network::Packet& packet, CastFailedData& data) {
    // WotLK format: castCount(1) + spellId(4) + result(1) = 6 bytes minimum
    if (!packet.hasRemaining(6)) return false;

    data.castCount = packet.readUInt8();
    data.spellId = packet.readUInt32();
    data.result = packet.readUInt8();
    LOG_INFO("Cast failed: spell=", data.spellId, " result=", static_cast<int>(data.result));
    return true;
}

bool SpellStartParser::parse(network::Packet& packet, SpellStartData& data) {
    data = SpellStartData{};

    // Packed GUIDs are variable-length; only require minimal packet shape up front:
    // two GUID masks + castCount(1) + spellId(4) + castFlags(4) + castTime(4).
    if (!packet.hasRemaining(15)) return false;

    size_t startPos = packet.getReadPos();
    if (!packet.hasFullPackedGuid()) {
        return false;
    }
    data.casterGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.casterUnit = packet.readPackedGuid();

    // Validate remaining fixed fields (castCount + spellId + castFlags + castTime = 13 bytes)
    if (!packet.hasRemaining(13)) {
        packet.setReadPos(startPos);
        return false;
    }

    data.castCount = packet.readUInt8();
    data.spellId = packet.readUInt32();
    data.castFlags = packet.readUInt32();
    data.castTime = packet.readUInt32();

    // SpellCastTargets starts with target flags and is mandatory.
    if (!packet.hasRemaining(4)) {
        LOG_WARNING("Spell start: missing targetFlags");
        packet.setReadPos(startPos);
        return false;
    }

    // WotLK 3.3.5a SpellCastTargets — consume ALL target payload bytes so that
    // subsequent fields (e.g. school mask, cast flags 0x20 extra data) are not
    // misaligned for ground-targeted or AoE spells.
    uint32_t targetFlags = packet.readUInt32();

    auto readPackedTarget = [&](uint64_t* out) -> bool {
        if (!packet.hasFullPackedGuid()) return false;
        uint64_t g = packet.readPackedGuid();
        if (out) *out = g;
        return true;
    };
    auto skipPackedAndFloats3 = [&]() -> bool {
        if (!packet.hasFullPackedGuid()) return false;
        packet.readPackedGuid(); // transport GUID (may be zero)
        if (!packet.hasRemaining(12)) return false;
        packet.readFloat(); packet.readFloat(); packet.readFloat();
        return true;
    };

    // UNIT/UNIT_MINIPET/CORPSE_ALLY/GAMEOBJECT share a single object target GUID
    if (targetFlags & (0x0002u | 0x0004u | 0x0400u | 0x0800u)) {
        readPackedTarget(&data.targetGuid); // best-effort; ignore failure
    }
    // ITEM/TRADE_ITEM share a single item target GUID
    if (targetFlags & (0x0010u | 0x0100u)) {
        readPackedTarget(nullptr);
    }
    // SOURCE_LOCATION: PackedGuid (transport) + float x,y,z
    if (targetFlags & 0x0020u) {
        skipPackedAndFloats3();
    }
    // DEST_LOCATION: PackedGuid (transport) + float x,y,z
    if (targetFlags & 0x0040u) {
        skipPackedAndFloats3();
    }
    // STRING: null-terminated
    if (targetFlags & 0x0200u) {
        while (packet.hasData() && packet.readUInt8() != 0) {}
    }

    LOG_DEBUG("Spell start: spell=", data.spellId, " castTime=", data.castTime, "ms");
    return true;
}

bool SpellGoParser::parse(network::Packet& packet, SpellGoData& data) {
    // Always reset output to avoid stale targets when callers reuse buffers.
    data = SpellGoData{};

    // Packed GUIDs are variable-length, so only require the smallest possible
    // shape up front: 2 GUID masks + fixed fields through hitCount.
    if (!packet.hasRemaining(16)) return false;

    size_t startPos = packet.getReadPos();
    if (!packet.hasFullPackedGuid()) {
        return false;
    }
    data.casterGuid = packet.readPackedGuid();
    if (!packet.hasFullPackedGuid()) {
        packet.setReadPos(startPos);
        return false;
    }
    data.casterUnit = packet.readPackedGuid();

    // Validate remaining fixed fields up to hitCount/missCount
    if (!packet.hasRemaining(14)) { // castCount(1) + spellId(4) + castFlags(4) + timestamp(4) + hitCount(1)
        packet.setReadPos(startPos);
        return false;
    }

    data.castCount = packet.readUInt8();
    data.spellId = packet.readUInt32();
    data.castFlags = packet.readUInt32();
    // Timestamp in 3.3.5a
    packet.readUInt32();

    const uint8_t rawHitCount = packet.readUInt8();
    if (rawHitCount > 128) {
        LOG_WARNING("Spell go: hitCount capped (requested=", static_cast<int>(rawHitCount), ")");
    }
    const uint8_t storedHitLimit = std::min<uint8_t>(rawHitCount, 128);

    bool truncatedTargets = false;

    data.hitTargets.reserve(storedHitLimit);
    for (uint16_t i = 0; i < rawHitCount; ++i) {
        // WotLK 3.3.5a hit targets are full uint64 GUIDs (not PackedGuid).
        if (!packet.hasRemaining(8)) {
            LOG_WARNING("Spell go: truncated hit targets at index ", i, "/", static_cast<int>(rawHitCount));
            truncatedTargets = true;
            break;
        }
        const uint64_t targetGuid = packet.readUInt64();
        if (i < storedHitLimit) {
            data.hitTargets.push_back(targetGuid);
        }
    }
    if (truncatedTargets) {
        packet.setReadPos(startPos);
        return false;
    }
    data.hitCount = static_cast<uint8_t>(data.hitTargets.size());

    // missCount is mandatory in SMSG_SPELL_GO. Missing byte means truncation.
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("Spell go: missing missCount after hit target list");
        packet.setReadPos(startPos);
        return false;
    }

    const size_t missCountPos = packet.getReadPos();
    const uint8_t rawMissCount = packet.readUInt8();
    if (rawMissCount > 20) {
        // Likely offset error — dump context bytes for diagnostics.
        const auto& raw = packet.getData();
        std::string hexCtx;
        size_t dumpStart = (missCountPos >= 8) ? missCountPos - 8 : startPos;
        size_t dumpEnd = std::min(missCountPos + 16, raw.size());
        for (size_t i = dumpStart; i < dumpEnd; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x ", raw[i]);
            hexCtx += buf;
            if (i == missCountPos - 1) hexCtx += "[";
            if (i == missCountPos) hexCtx += "] ";
        }
        LOG_WARNING("Spell go: suspect missCount=", static_cast<int>(rawMissCount),
                    " spell=", data.spellId, " hits=", static_cast<int>(data.hitCount),
                    " castFlags=0x", std::hex, data.castFlags, std::dec,
                    " missCountPos=", missCountPos, " pktSize=", packet.getSize(),
                    " ctx=", hexCtx);
    }
    if (rawMissCount > 128) {
        LOG_WARNING("Spell go: missCount capped (requested=", static_cast<int>(rawMissCount),
                    ") spell=", data.spellId, " hits=", static_cast<int>(data.hitCount),
                    " remaining=", packet.getRemainingSize());
    }
    const uint8_t storedMissLimit = std::min<uint8_t>(rawMissCount, 128);

    data.missTargets.reserve(storedMissLimit);
    for (uint16_t i = 0; i < rawMissCount; ++i) {
        // WotLK 3.3.5a miss targets are full uint64 GUIDs + uint8 missType.
        // REFLECT additionally appends uint8 reflectResult.
        if (!packet.hasRemaining(9)) { // 8 GUID + 1 missType
            LOG_WARNING("Spell go: truncated miss targets at index ", i, "/", static_cast<int>(rawMissCount),
                        " spell=", data.spellId, " hits=", static_cast<int>(data.hitCount));
            truncatedTargets = true;
            break;
        }
        SpellGoMissEntry m;
        m.targetGuid = packet.readUInt64();
        m.missType = packet.readUInt8();
        if (m.missType == 11) { // SPELL_MISS_REFLECT
            if (!packet.hasRemaining(1)) {
                LOG_WARNING("Spell go: truncated reflect payload at miss index ", i, "/", static_cast<int>(rawMissCount));
                truncatedTargets = true;
                break;
            }
            (void)packet.readUInt8(); // reflectResult
        }
        if (i < storedMissLimit) {
            data.missTargets.push_back(m);
        }
    }
    data.missCount = static_cast<uint8_t>(data.missTargets.size());

    // If miss targets were truncated, salvage the successfully-parsed hit data
    // rather than discarding the entire spell. The server already applied effects;
    // we just need the hit list for UI feedback (combat text, health bars).
    if (truncatedTargets) {
        LOG_DEBUG("Spell go: salvaging ", static_cast<int>(data.hitCount), " hits despite miss truncation");
        packet.skipAll(); // consume remaining bytes
        return true;
    }

    // WotLK 3.3.5a SpellCastTargets — consume ALL target payload bytes so that
    // any trailing fields after the target section are not misaligned for
    // ground-targeted or AoE spells.  Same layout as SpellStartParser.
    if (packet.hasData()) {
        if (packet.hasRemaining(4)) {
            uint32_t targetFlags = packet.readUInt32();

            auto readPackedTarget = [&](uint64_t* out) -> bool {
                if (!packet.hasFullPackedGuid()) return false;
                uint64_t g = packet.readPackedGuid();
                if (out) *out = g;
                return true;
            };
            auto skipPackedAndFloats3 = [&]() -> bool {
                if (!packet.hasFullPackedGuid()) return false;
                packet.readPackedGuid(); // transport GUID
                if (!packet.hasRemaining(12)) return false;
                packet.readFloat(); packet.readFloat(); packet.readFloat();
                return true;
            };

            // UNIT/UNIT_MINIPET/CORPSE_ALLY/GAMEOBJECT share one object target GUID
            if (targetFlags & (0x0002u | 0x0004u | 0x0400u | 0x0800u)) {
                readPackedTarget(&data.targetGuid);
            }
            // ITEM/TRADE_ITEM share one item target GUID
            if (targetFlags & (0x0010u | 0x0100u)) {
                readPackedTarget(nullptr);
            }
            // SOURCE_LOCATION: PackedGuid (transport) + float x,y,z
            if (targetFlags & 0x0020u) {
                skipPackedAndFloats3();
            }
            // DEST_LOCATION: PackedGuid (transport) + float x,y,z
            if (targetFlags & 0x0040u) {
                skipPackedAndFloats3();
            }
            // STRING: null-terminated
            if (targetFlags & 0x0200u) {
                while (packet.hasData() && packet.readUInt8() != 0) {}
            }
        }
    }

    LOG_DEBUG("Spell go: spell=", data.spellId, " hits=", static_cast<int>(data.hitCount),
             " misses=", static_cast<int>(data.missCount));
    return true;
}

bool AuraUpdateParser::parse(network::Packet& packet, AuraUpdateData& data, bool isAll) {
    // Validation: packed GUID (1-8 bytes minimum for reading)
    if (!packet.hasRemaining(1)) return false;

    data.guid = packet.readPackedGuid();

    // Cap number of aura entries to prevent unbounded loop DoS
    uint32_t maxAuras = isAll ? 512 : 1;
    uint32_t auraCount = 0;

    while (packet.hasData() && auraCount < maxAuras) {
        // Validate we can read slot (1) + spellId (4) = 5 bytes minimum
        if (!packet.hasRemaining(5)) {
            LOG_DEBUG("Aura update: truncated entry at position ", auraCount);
            break;
        }

        uint8_t slot = packet.readUInt8();
        uint32_t spellId = packet.readUInt32();
        auraCount++;

        AuraSlot aura;
        if (spellId != 0) {
            aura.spellId = spellId;

            // Validate flags + level + charges (3 bytes)
            if (!packet.hasRemaining(3)) {
                LOG_WARNING("Aura update: truncated flags/level/charges at entry ", auraCount);
                aura.flags = 0;
                aura.level = 0;
                aura.charges = 0;
            } else {
                aura.flags = packet.readUInt8();
                aura.level = packet.readUInt8();
                aura.charges = packet.readUInt8();
            }

            if (!(aura.flags & 0x08)) { // NOT_CASTER flag
                // Validate space for packed GUID read (minimum 1 byte)
                if (!packet.hasRemaining(1)) {
                    aura.casterGuid = 0;
                } else {
                    aura.casterGuid = packet.readPackedGuid();
                }
            }

            if (aura.flags & 0x20) { // DURATION - need 8 bytes (two uint32s)
                if (!packet.hasRemaining(8)) {
                    LOG_WARNING("Aura update: truncated duration fields at entry ", auraCount);
                    aura.maxDurationMs = 0;
                    aura.durationMs = 0;
                } else {
                    aura.maxDurationMs = static_cast<int32_t>(packet.readUInt32());
                    aura.durationMs = static_cast<int32_t>(packet.readUInt32());
                }
            }

            if (aura.flags & 0x40) { // EFFECT_AMOUNTS
                // Only read amounts for active effect indices (flags 0x01, 0x02, 0x04)
                for (int i = 0; i < 3; ++i) {
                    if (aura.flags & (1 << i)) {
                        if (packet.hasRemaining(4)) {
                            packet.readUInt32();
                        } else {
                            LOG_WARNING("Aura update: truncated effect amount ", i, " at entry ", auraCount);
                            break;
                        }
                    }
                }
            }
        }

        data.updates.push_back({slot, aura});

        // For single update, only one entry
        if (!isAll) break;
    }

    if (auraCount >= maxAuras && packet.hasData()) {
        LOG_WARNING("Aura update: capped at ", maxAuras, " entries, remaining data ignored");
    }

    LOG_DEBUG("Aura update for 0x", std::hex, data.guid, std::dec,
              ": ", data.updates.size(), " slots");
    return true;
}

bool SpellCooldownParser::parse(network::Packet& packet, SpellCooldownData& data) {
    // Upfront validation: guid(8) + flags(1) = 9 bytes minimum
    if (!packet.hasRemaining(9)) return false;

    data.guid = packet.readUInt64();
    data.flags = packet.readUInt8();

    // Cap cooldown entries to prevent unbounded memory allocation (each entry is 8 bytes)
    uint32_t maxCooldowns = 512;
    uint32_t cooldownCount = 0;

    while (packet.hasRemaining(8) && cooldownCount < maxCooldowns) {
        uint32_t spellId = packet.readUInt32();
        uint32_t cooldownMs = packet.readUInt32();
        data.cooldowns.push_back({spellId, cooldownMs});
        cooldownCount++;
    }

    if (cooldownCount >= maxCooldowns && packet.hasRemaining(8)) {
        LOG_WARNING("Spell cooldowns: capped at ", maxCooldowns, " entries, remaining data ignored");
    }

    LOG_DEBUG("Spell cooldowns: ", data.cooldowns.size(), " entries");
    return true;
}

// ============================================================
// Phase 4: Group/Party System
// ============================================================

network::Packet GroupInvitePacket::build(const std::string& playerName) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_INVITE));
    packet.writeString(playerName);
    packet.writeUInt32(0); // unused
    LOG_DEBUG("Built CMSG_GROUP_INVITE: ", playerName);
    return packet;
}

bool GroupInviteResponseParser::parse(network::Packet& packet, GroupInviteResponseData& data) {
    // Validate minimum packet size: canAccept(1)
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("SMSG_GROUP_INVITE: packet too small (", packet.getSize(), " bytes)");
        return false;
    }

    data.canAccept = packet.readUInt8();
    // Note: inviterName is a string, which is always safe to read even if empty
    data.inviterName = packet.readString();
    LOG_INFO("Group invite from: ", data.inviterName, " (canAccept=", static_cast<int>(data.canAccept), ")");
    return true;
}

network::Packet GroupAcceptPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_ACCEPT));
    packet.writeUInt32(0); // unused in 3.3.5a
    return packet;
}

network::Packet GroupDeclinePacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_DECLINE));
    return packet;
}

bool GroupListParser::parse(network::Packet& packet, GroupListData& data, bool hasRoles) {
    auto rem = [&]() { return packet.getRemainingSize(); };

    if (rem() < 3) return false;
    data.groupType = packet.readUInt8();
    data.subGroup  = packet.readUInt8();
    data.flags     = packet.readUInt8();

    // WotLK 3.3.5a added a roles byte (tank/healer/dps) for the dungeon finder.
    // Classic 1.12 and TBC 2.4.3 do not have this byte.
    if (hasRoles) {
        if (rem() < 1) return false;
        data.roles = packet.readUInt8();
    } else {
        data.roles = 0;
    }

    // WotLK: LFG data gated by groupType bit 0x04 (LFD group type)
    if (hasRoles && (data.groupType & 0x04)) {
        if (rem() < 5) return false;
        packet.readUInt8();  // lfg state
        packet.readUInt32(); // lfg entry
        // WotLK 3.3.5a may or may not send the lfg flags byte — read it only if present
        if (rem() >= 13) { // enough for lfgFlags(1)+groupGuid(8)+counter(4)
            packet.readUInt8(); // lfg flags
        }
    }

    if (rem() < 12) return false;
    packet.readUInt64(); // group GUID
    packet.readUInt32(); // update counter

    if (rem() < 4) return false;
    data.memberCount = packet.readUInt32();
    if (data.memberCount > 40) {
        LOG_WARNING("GroupListParser: implausible memberCount=", data.memberCount, ", clamping");
        data.memberCount = 40;
    }
    data.members.reserve(data.memberCount);

    for (uint32_t i = 0; i < data.memberCount; ++i) {
        if (rem() == 0) break;
        GroupMember member;
        member.name     = packet.readString();
        if (rem() < 8) break;
        member.guid     = packet.readUInt64();
        if (rem() < 3) break;
        member.isOnline = packet.readUInt8();
        member.subGroup = packet.readUInt8();
        member.flags    = packet.readUInt8();
        // WotLK added per-member roles byte; Classic/TBC do not have it.
        if (hasRoles) {
            if (rem() < 1) break;
            member.roles = packet.readUInt8();
        } else {
            member.roles = 0;
        }
        data.members.push_back(member);
    }

    if (rem() < 8) {
        LOG_INFO("Group list: ", data.memberCount, " members (no leader GUID in packet)");
        return true;
    }
    data.leaderGuid = packet.readUInt64();

    if (data.memberCount > 0 && rem() >= 10) {
        data.lootMethod   = packet.readUInt8();
        data.looterGuid   = packet.readUInt64();
        data.lootThreshold = packet.readUInt8();
        // Dungeon difficulty (heroic/normal) — Classic doesn't send this; TBC/WotLK do
        if (rem() >= 1) data.difficultyId     = packet.readUInt8();
        // Raid difficulty — WotLK only
        if (rem() >= 1) data.raidDifficultyId = packet.readUInt8();
        // Extra byte in some 3.3.5a builds
        if (hasRoles && rem() >= 1) packet.readUInt8();
    }

    LOG_INFO("Group list: ", data.memberCount, " members, leader=0x",
             std::hex, data.leaderGuid, std::dec);
    return true;
}

bool PartyCommandResultParser::parse(network::Packet& packet, PartyCommandResultData& data) {
    // Upfront validation: command(4) + name(var) + result(4) = 8 bytes minimum (plus name string)
    if (!packet.hasRemaining(8)) return false;

    data.command = static_cast<PartyCommand>(packet.readUInt32());
    data.name = packet.readString();

    // Validate result field exists (4 bytes)
    if (!packet.hasRemaining(4)) {
        data.result = static_cast<PartyResult>(0);
        return true; // Partial read is acceptable
    }

    data.result = static_cast<PartyResult>(packet.readUInt32());
    LOG_DEBUG("Party command result: ", static_cast<int>(data.result));
    return true;
}

bool GroupDeclineResponseParser::parse(network::Packet& packet, GroupDeclineData& data) {
    // Upfront validation: playerName is a CString (minimum 1 null terminator)
    if (!packet.hasRemaining(1)) return false;

    data.playerName = packet.readString();
    LOG_INFO("Group decline from: ", data.playerName);
    return true;
}

// ============================================================
// Phase 5: Loot System
// ============================================================

network::Packet LootPacket::build(uint64_t targetGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT));
    packet.writeUInt64(targetGuid);
    LOG_DEBUG("Built CMSG_LOOT: target=0x", std::hex, targetGuid, std::dec);
    return packet;
}

network::Packet AutostoreLootItemPacket::build(uint8_t slotIndex) {
    network::Packet packet(wireOpcode(Opcode::CMSG_AUTOSTORE_LOOT_ITEM));
    packet.writeUInt8(slotIndex);
    return packet;
}

network::Packet UseItemPacket::build(uint8_t bagIndex, uint8_t slotIndex, uint64_t itemGuid, uint32_t spellId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_USE_ITEM));
    packet.writeUInt8(bagIndex);
    packet.writeUInt8(slotIndex);
    packet.writeUInt8(0);  // cast count
    packet.writeUInt32(spellId); // spell id from item data
    packet.writeUInt64(itemGuid); // full 8-byte GUID
    packet.writeUInt32(0); // glyph index
    packet.writeUInt8(0);  // cast flags
    // SpellCastTargets: self
    packet.writeUInt32(0x00);
    return packet;
}

network::Packet OpenItemPacket::build(uint8_t bagIndex, uint8_t slotIndex) {
    network::Packet packet(wireOpcode(Opcode::CMSG_OPEN_ITEM));
    packet.writeUInt8(bagIndex);
    packet.writeUInt8(slotIndex);
    return packet;
}

network::Packet AutoEquipItemPacket::build(uint8_t srcBag, uint8_t srcSlot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_AUTOEQUIP_ITEM));
    packet.writeUInt8(srcBag);
    packet.writeUInt8(srcSlot);
    return packet;
}

network::Packet SwapItemPacket::build(uint8_t dstBag, uint8_t dstSlot, uint8_t srcBag, uint8_t srcSlot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SWAP_ITEM));
    packet.writeUInt8(dstBag);
    packet.writeUInt8(dstSlot);
    packet.writeUInt8(srcBag);
    packet.writeUInt8(srcSlot);
    return packet;
}

network::Packet SplitItemPacket::build(uint8_t srcBag, uint8_t srcSlot,
                                       uint8_t dstBag, uint8_t dstSlot, uint8_t count) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SPLIT_ITEM));
    packet.writeUInt8(srcBag);
    packet.writeUInt8(srcSlot);
    packet.writeUInt8(dstBag);
    packet.writeUInt8(dstSlot);
    packet.writeUInt8(count);
    return packet;
}

network::Packet SwapInvItemPacket::build(uint8_t srcSlot, uint8_t dstSlot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SWAP_INV_ITEM));
    packet.writeUInt8(srcSlot);
    packet.writeUInt8(dstSlot);
    return packet;
}

network::Packet LootMoneyPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT_MONEY));
    return packet;
}

network::Packet LootReleasePacket::build(uint64_t lootGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT_RELEASE));
    packet.writeUInt64(lootGuid);
    return packet;
}

bool LootResponseParser::parse(network::Packet& packet, LootResponseData& data, bool isWotlkFormat) {
    data = LootResponseData{};
    size_t avail = packet.getRemainingSize();

    // Minimum is guid(8)+lootType(1) = 9 bytes.  Servers send a short packet with
    // lootType=0 (LOOT_NONE) when loot is unavailable (e.g. chest not yet opened,
    // needs a key, or another player is looting).  We treat this as an empty-loot
    // signal and return false so the caller knows not to open the loot window.
    if (avail < 9) {
        LOG_WARNING("LootResponseParser: packet too short (", avail, " bytes)");
        return false;
    }

    data.lootGuid = packet.readUInt64();
    data.lootType = packet.readUInt8();

    // Short failure packet — no gold/item data follows.
    avail = packet.getRemainingSize();
    if (avail < 5) {
        LOG_DEBUG("LootResponseParser: lootType=", static_cast<int>(data.lootType), " (empty/failure response)");
        return false;
    }

    data.gold = packet.readUInt32();
    uint8_t itemCount = packet.readUInt8();

    // Per-item wire size is 22 bytes across all expansions:
    //   slot(1)+itemId(4)+count(4)+displayInfo(4)+randSuffix(4)+randProp(4)+slotType(1) = 22
    constexpr size_t kItemSize = 22u;

    auto parseLootItemList = [&](uint8_t listCount, bool markQuestItems) -> bool {
        for (uint8_t i = 0; i < listCount; ++i) {
            size_t remaining = packet.getRemainingSize();
            if (remaining < kItemSize) {
                return false;
            }

            LootItem item;
            item.slotIndex        = packet.readUInt8();
            item.itemId           = packet.readUInt32();
            item.count            = packet.readUInt32();
            item.displayInfoId    = packet.readUInt32();
            item.randomSuffix     = packet.readUInt32();
            item.randomPropertyId = packet.readUInt32();
            item.lootSlotType     = packet.readUInt8();
            item.isQuestItem      = markQuestItems;
            data.items.push_back(item);
        }
        return true;
    };

    data.items.reserve(itemCount);
    if (!parseLootItemList(itemCount, false)) {
        LOG_WARNING("LootResponseParser: truncated regular item list");
        return false;
    }

    // Quest item section only present in WotLK 3.3.5a
    uint8_t questItemCount = 0;
    if (isWotlkFormat && packet.hasRemaining(1)) {
        questItemCount = packet.readUInt8();
        data.items.reserve(data.items.size() + questItemCount);
        if (!parseLootItemList(questItemCount, true)) {
            LOG_WARNING("LootResponseParser: truncated quest item list");
            return false;
        }
    }

    LOG_DEBUG("Loot response: ", static_cast<int>(itemCount), " regular + ", static_cast<int>(questItemCount),
             " quest items, ", data.gold, " copper");
    return true;
}

// ============================================================
// Phase 5: NPC Gossip
// ============================================================

network::Packet GossipHelloPacket::build(uint64_t npcGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GOSSIP_HELLO));
    packet.writeUInt64(npcGuid);
    return packet;
}

network::Packet QuestgiverHelloPacket::build(uint64_t npcGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_HELLO));
    packet.writeUInt64(npcGuid);
    return packet;
}

network::Packet GossipSelectOptionPacket::build(uint64_t npcGuid, uint32_t menuId, uint32_t optionId, const std::string& code) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GOSSIP_SELECT_OPTION));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(menuId);
    packet.writeUInt32(optionId);
    if (!code.empty()) {
        packet.writeString(code);
    }
    return packet;
}

network::Packet QuestgiverQueryQuestPacket::build(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_QUERY_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    packet.writeUInt8(1);  // isDialogContinued = 1 (from gossip)
    return packet;
}

network::Packet QuestgiverAcceptQuestPacket::build(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    packet.writeUInt32(0);  // AzerothCore/WotLK expects trailing unk1
    return packet;
}

bool QuestDetailsParser::parse(network::Packet& packet, QuestDetailsData& data) {
    if (packet.getSize() < 20) return false;
    data.npcGuid = packet.readUInt64();

    // WotLK has informUnit(u64) before questId; Vanilla/TBC do not.
    // Detect: try WotLK first (read informUnit + questId), then check if title
    // string looks valid. If not, rewind and try vanilla (questId directly).
    size_t preInform = packet.getReadPos();
    /*informUnit*/ packet.readUInt64();
    data.questId = packet.readUInt32();
    data.title = normalizeWowTextTokens(packet.readString());
    if (data.title.empty() || data.questId > 100000) {
        // Likely vanilla format — rewind past informUnit
        packet.setReadPos(preInform);
        data.questId = packet.readUInt32();
        data.title = normalizeWowTextTokens(packet.readString());
    }
    data.details = normalizeWowTextTokens(packet.readString());
    data.objectives = normalizeWowTextTokens(packet.readString());

    if (!packet.hasRemaining(10)) {
        LOG_DEBUG("Quest details (short): id=", data.questId, " title='", data.title, "'");
        return true;
    }

    /*activateAccept*/ packet.readUInt8();
    /*flags*/ packet.readUInt32();
    data.suggestedPlayers = packet.readUInt32();
    /*isFinished*/ packet.readUInt8();

    // Reward choice items: server always writes 6 entries (QUEST_REWARD_CHOICES_COUNT)
    if (packet.hasRemaining(4)) {
        /*choiceCount*/ packet.readUInt32();
        for (int i = 0; i < 6; i++) {
            if (!packet.hasRemaining(12)) break;
            uint32_t itemId = packet.readUInt32();
            uint32_t count  = packet.readUInt32();
            uint32_t dispId = packet.readUInt32();
            if (itemId != 0) {
                QuestRewardItem ri;
                ri.itemId = itemId; ri.count = count; ri.displayInfoId = dispId;
                ri.choiceSlot = static_cast<uint32_t>(i);
                data.rewardChoiceItems.push_back(ri);
            }
        }
    }

    // Reward items: server always writes 4 entries (QUEST_REWARDS_COUNT)
    if (packet.hasRemaining(4)) {
        /*rewardCount*/ packet.readUInt32();
        for (int i = 0; i < 4; i++) {
            if (!packet.hasRemaining(12)) break;
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

    // Money and XP rewards
    if (packet.hasRemaining(4))
        data.rewardMoney = packet.readUInt32();
    if (packet.hasRemaining(4))
        data.rewardXp = packet.readUInt32();

    LOG_DEBUG("Quest details: id=", data.questId, " title='", data.title, "'");
    return true;
}

bool GossipMessageParser::parse(network::Packet& packet, GossipMessageData& data) {
    // Upfront validation: npcGuid(8) + menuId(4) + titleTextId(4) + optionCount(4) = 20 bytes minimum
    if (!packet.hasRemaining(20)) return false;

    data.npcGuid = packet.readUInt64();
    data.menuId = packet.readUInt32();
    data.titleTextId = packet.readUInt32();
    uint32_t optionCount = packet.readUInt32();

    // Cap option count to prevent unbounded memory allocation
    const uint32_t MAX_GOSSIP_OPTIONS = 64;
    if (optionCount > MAX_GOSSIP_OPTIONS) {
        LOG_WARNING("GossipMessageParser: optionCount capped (requested=", optionCount, ")");
        optionCount = MAX_GOSSIP_OPTIONS;
    }

    data.options.clear();
    data.options.reserve(optionCount);
    for (uint32_t i = 0; i < optionCount; ++i) {
        // Each option: id(4) + icon(1) + isCoded(1) + boxMoney(4) + text(var) + boxText(var)
        // Minimum: 10 bytes + 2 empty strings (2 null terminators) = 12 bytes
        if (!packet.hasRemaining(12)) {
            LOG_WARNING("GossipMessageParser: truncated options at index ", i, "/", optionCount);
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

    // Validate questCount field exists (4 bytes)
    if (!packet.hasRemaining(4)) {
        LOG_DEBUG("Gossip: ", data.options.size(), " options (no quest data)");
        return true;
    }

    uint32_t questCount = packet.readUInt32();
    // Cap quest count to prevent unbounded memory allocation
    const uint32_t MAX_GOSSIP_QUESTS = 64;
    if (questCount > MAX_GOSSIP_QUESTS) {
        LOG_WARNING("GossipMessageParser: questCount capped (requested=", questCount, ")");
        questCount = MAX_GOSSIP_QUESTS;
    }

    data.quests.clear();
    data.quests.reserve(questCount);
    for (uint32_t i = 0; i < questCount; ++i) {
        // Each quest: questId(4) + questIcon(4) + questLevel(4) + questFlags(4) + isRepeatable(1) + title(var)
        // Minimum: 17 bytes + empty string (1 null terminator) = 18 bytes
        if (!packet.hasRemaining(18)) {
            LOG_WARNING("GossipMessageParser: truncated quests at index ", i, "/", questCount);
            break;
        }
        GossipQuestItem quest;
        quest.questId = packet.readUInt32();
        quest.questIcon = packet.readUInt32();
        quest.questLevel = static_cast<int32_t>(packet.readUInt32());
        quest.questFlags = packet.readUInt32();
        quest.isRepeatable = packet.readUInt8();
        quest.title = normalizeWowTextTokens(packet.readString());
        data.quests.push_back(quest);
    }

    LOG_DEBUG("Gossip: ", data.options.size(), " options, ", data.quests.size(), " quests");
    return true;
}

// ============================================================
// Bind Point (Hearthstone)
// ============================================================

network::Packet BinderActivatePacket::build(uint64_t npcGuid) {
    network::Packet pkt(wireOpcode(Opcode::CMSG_BINDER_ACTIVATE));
    pkt.writeUInt64(npcGuid);
    return pkt;
}

bool BindPointUpdateParser::parse(network::Packet& packet, BindPointUpdateData& data) {
    if (packet.getSize() < 20) return false;
    data.x = packet.readFloat();
    data.y = packet.readFloat();
    data.z = packet.readFloat();
    data.mapId = packet.readUInt32();
    data.zoneId = packet.readUInt32();
    return true;
}

bool QuestRequestItemsParser::parse(network::Packet& packet, QuestRequestItemsData& data) {
    if (!packet.hasRemaining(20)) return false;
    data.npcGuid = packet.readUInt64();
    data.questId = packet.readUInt32();
    data.title = normalizeWowTextTokens(packet.readString());
    data.completionText = normalizeWowTextTokens(packet.readString());

    if (!packet.hasRemaining(9)) {
        LOG_DEBUG("Quest request items (short): id=", data.questId, " title='", data.title, "'");
        return true;
    }

    struct ParsedTail {
        uint32_t requiredMoney = 0;
        uint32_t completableFlags = 0;
        std::vector<QuestRewardItem> requiredItems;
        bool ok = false;
        int score = -1;
    };

    auto parseTail = [&](size_t startPos, size_t prefixSkip) -> ParsedTail {
        ParsedTail out;
        packet.setReadPos(startPos);

        if (!packet.hasRemaining(prefixSkip)) return out;
        packet.setReadPos(packet.getReadPos() + prefixSkip);

        if (!packet.hasRemaining(8)) return out;
        out.requiredMoney = packet.readUInt32();
        uint32_t requiredItemCount = packet.readUInt32();
        if (requiredItemCount > 64) return out;  // sanity guard against misalignment

        out.requiredItems.reserve(requiredItemCount);
        for (uint32_t i = 0; i < requiredItemCount; ++i) {
            if (!packet.hasRemaining(12)) return out;
            QuestRewardItem item;
            item.itemId = packet.readUInt32();
            item.count = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            if (item.itemId != 0) out.requiredItems.push_back(item);
        }

        if (!packet.hasRemaining(4)) return out;
        out.completableFlags = packet.readUInt32();
        out.ok = true;

        // Prefer layouts that produce plausible quest-requirement shapes.
        out.score = 0;
        if (requiredItemCount <= 6) out.score += 4;
        if (out.requiredItems.size() == requiredItemCount) out.score += 3;
        if ((out.completableFlags & ~0x3u) == 0) out.score += 5;
        if (out.requiredMoney == 0) out.score += 4;
        else if (out.requiredMoney <= 100000) out.score += 2;       // <=10g is common
        else if (out.requiredMoney >= 1000000) out.score -= 3;      // implausible for most quests
        if (!out.requiredItems.empty()) out.score += 1;
        size_t remaining = packet.getRemainingSize();
        if (remaining <= 16) out.score += 3;
        else if (remaining <= 32) out.score += 2;
        else if (remaining <= 64) out.score += 1;
        if (prefixSkip == 0) out.score += 1;
        else if (prefixSkip <= 12) out.score += 1;
        return out;
    };

    size_t tailStart = packet.getReadPos();
    std::vector<ParsedTail> candidates;
    candidates.reserve(25);
    for (size_t skip = 0; skip <= 24; ++skip) {
        candidates.push_back(parseTail(tailStart, skip));
    }

    const ParsedTail* chosen = nullptr;
    for (const auto& cand : candidates) {
        if (!cand.ok) continue;
        if (!chosen || cand.score > chosen->score) chosen = &cand;
    }
    if (!chosen) {
        return true;
    }

    data.requiredMoney = chosen->requiredMoney;
    data.completableFlags = chosen->completableFlags;
    data.requiredItems = chosen->requiredItems;

    LOG_DEBUG("Quest request items: id=", data.questId, " title='", data.title,
             "' items=", data.requiredItems.size(), " completable=", data.isCompletable());
    return true;
}

bool QuestOfferRewardParser::parse(network::Packet& packet, QuestOfferRewardData& data) {
    if (!packet.hasRemaining(20)) return false;
    data.npcGuid = packet.readUInt64();
    data.questId = packet.readUInt32();
    data.title = normalizeWowTextTokens(packet.readString());
    data.rewardText = normalizeWowTextTokens(packet.readString());

    if (!packet.hasRemaining(8)) {
        LOG_DEBUG("Quest offer reward (short): id=", data.questId, " title='", data.title, "'");
        return true;
    }

    // After the two strings the packet contains a variable prefix (autoFinish + optional fields)
    // before the emoteCount.  Different expansions and server emulator versions differ:
    //   Classic 1.12   : uint8 autoFinish + uint32 suggestedPlayers  = 5 bytes
    //   TBC 2.4.3      : uint32 autoFinish + uint32 suggestedPlayers = 8 bytes (variable arrays)
    //   WotLK 3.3.5a   : uint32 autoFinish + uint32 suggestedPlayers = 8 bytes (fixed 6/4 arrays)
    // Some vanilla-family servers omit autoFinish entirely (0 bytes of prefix).
    // We scan prefix sizes 0..16 bytes with both fixed and variable array layouts, scoring each.

    struct ParsedTail {
        uint32_t rewardMoney = 0;
        uint32_t rewardXp = 0;
        std::vector<QuestRewardItem> choiceRewards;
        std::vector<QuestRewardItem> fixedRewards;
        bool ok = false;
        int score = -1000;
        size_t prefixSkip = 0;
        bool fixedArrays = false;
    };

    auto parseTail = [&](size_t startPos, size_t prefixSkip, bool fixedArrays) -> ParsedTail {
        ParsedTail out;
        out.prefixSkip  = prefixSkip;
        out.fixedArrays = fixedArrays;
        packet.setReadPos(startPos);

        // Skip the prefix bytes (autoFinish + optional suggestedPlayers before emoteCount)
        if (!packet.hasRemaining(prefixSkip)) return out;
        packet.setReadPos(packet.getReadPos() + prefixSkip);

        if (!packet.hasRemaining(4)) return out;
        uint32_t emoteCount = packet.readUInt32();
        if (emoteCount > 32) return out;  // guard against misalignment
        for (uint32_t i = 0; i < emoteCount; ++i) {
            if (!packet.hasRemaining(8)) return out;
            packet.readUInt32(); // delay
            packet.readUInt32(); // emote type
        }

        if (!packet.hasRemaining(4)) return out;
        uint32_t choiceCount = packet.readUInt32();
        if (choiceCount > 6) return out;
        uint32_t choiceSlots = fixedArrays ? 6u : choiceCount;
        out.choiceRewards.reserve(choiceCount);
        uint32_t nonZeroChoice = 0;
        for (uint32_t i = 0; i < choiceSlots; ++i) {
            if (!packet.hasRemaining(12)) return out;
            QuestRewardItem item;
            item.itemId = packet.readUInt32();
            item.count = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            item.choiceSlot = i;
            if (item.itemId > 0) {
                out.choiceRewards.push_back(item);
                ++nonZeroChoice;
            }
        }

        if (!packet.hasRemaining(4)) return out;
        uint32_t rewardCount = packet.readUInt32();
        if (rewardCount > 4) return out;
        uint32_t rewardSlots = fixedArrays ? 4u : rewardCount;
        out.fixedRewards.reserve(rewardCount);
        uint32_t nonZeroFixed = 0;
        for (uint32_t i = 0; i < rewardSlots; ++i) {
            if (!packet.hasRemaining(12)) return out;
            QuestRewardItem item;
            item.itemId = packet.readUInt32();
            item.count = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            if (item.itemId > 0) {
                out.fixedRewards.push_back(item);
                ++nonZeroFixed;
            }
        }

        if (packet.hasRemaining(4))
            out.rewardMoney = packet.readUInt32();
        if (packet.hasRemaining(4))
            out.rewardXp = packet.readUInt32();

        out.ok = true;
        out.score = 0;
        // Prefer the standard WotLK/TBC 8-byte prefix (uint32 autoFinish + uint32 suggestedPlayers)
        if (prefixSkip == 8) out.score += 3;
        else if (prefixSkip == 5) out.score += 1;  // Classic uint8 autoFinish + uint32 suggestedPlayers
        // Prefer fixed arrays (WotLK/TBC servers always send 6+4 slots)
        if (fixedArrays) out.score += 2;
        // Valid counts
        if (choiceCount <= 6) out.score += 3;
        if (rewardCount <= 4) out.score += 3;
        // All non-zero items are within declared counts
        if (nonZeroChoice <= choiceCount) out.score += 2;
        if (nonZeroFixed <= rewardCount) out.score += 2;
        // No bytes left over (or only a few)
        size_t remaining = packet.getRemainingSize();
        if (remaining == 0) out.score += 5;
        else if (remaining <= 4) out.score += 3;
        else if (remaining <= 8) out.score += 2;
        else if (remaining <= 16) out.score += 1;
        else out.score -= static_cast<int>(remaining / 4);
        // Plausible money/XP values
        if (out.rewardMoney < 5000000u) out.score += 1;   // < 500g
        if (out.rewardXp < 200000u) out.score += 1;       // < 200k XP
        return out;
    };

    size_t tailStart = packet.getReadPos();
    // Try prefix sizes 0..16 bytes with both fixed and variable array layouts
    std::vector<ParsedTail> candidates;
    candidates.reserve(34);
    for (size_t skip = 0; skip <= 16; ++skip) {
        candidates.push_back(parseTail(tailStart, skip, true));   // fixed arrays
        candidates.push_back(parseTail(tailStart, skip, false));  // variable arrays
    }

    const ParsedTail* best = nullptr;
    for (const auto& cand : candidates) {
        if (!cand.ok) continue;
        if (!best || cand.score > best->score) best = &cand;
    }

    if (best) {
        data.choiceRewards = best->choiceRewards;
        data.fixedRewards  = best->fixedRewards;
        data.rewardMoney   = best->rewardMoney;
        data.rewardXp      = best->rewardXp;
    }

    LOG_DEBUG("Quest offer reward: id=", data.questId, " title='", data.title,
             "' choices=", data.choiceRewards.size(), " fixed=", data.fixedRewards.size(),
             " prefix=", (best ? best->prefixSkip : size_t(0)),
             (best && best->fixedArrays ? " fixed" : " var"));
    return true;
}

network::Packet QuestgiverCompleteQuestPacket::build(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_COMPLETE_QUEST));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    return packet;
}

network::Packet QuestgiverRequestRewardPacket::build(uint64_t npcGuid, uint32_t questId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_REQUEST_REWARD));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    return packet;
}

network::Packet QuestgiverChooseRewardPacket::build(uint64_t npcGuid, uint32_t questId, uint32_t rewardIndex) {
    network::Packet packet(wireOpcode(Opcode::CMSG_QUESTGIVER_CHOOSE_REWARD));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(questId);
    packet.writeUInt32(rewardIndex);
    return packet;
}

// ============================================================
// Phase 5: Vendor
// ============================================================

network::Packet ListInventoryPacket::build(uint64_t npcGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LIST_INVENTORY));
    packet.writeUInt64(npcGuid);
    return packet;
}

network::Packet BuyItemPacket::build(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    network::Packet packet(wireOpcode(Opcode::CMSG_BUY_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt32(itemId);  // item entry
    packet.writeUInt32(slot);    // vendor slot index from SMSG_LIST_INVENTORY
    packet.writeUInt32(count);
    // Note: WotLK/AzerothCore expects a trailing byte; Classic/TBC do not.
    // This static helper always adds it (appropriate for CMaNGOS/AzerothCore).
    // For Classic/TBC, use the GameHandler::buyItem() path which checks expansion.
    packet.writeUInt8(0);
    return packet;
}

network::Packet SellItemPacket::build(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SELL_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt64(itemGuid);
    packet.writeUInt32(count);
    return packet;
}

network::Packet BuybackItemPacket::build(uint64_t vendorGuid, uint32_t slot) {
    network::Packet packet(wireOpcode(Opcode::CMSG_BUYBACK_ITEM));
    packet.writeUInt64(vendorGuid);
    packet.writeUInt32(slot);
    return packet;
}

bool ListInventoryParser::parse(network::Packet& packet, ListInventoryData& data) {
    // Preserve canRepair — it was set by the gossip handler before this packet
    // arrived and is not part of the wire format.
    const bool savedCanRepair = data.canRepair;
    data = ListInventoryData{};
    data.canRepair = savedCanRepair;

    if (!packet.hasRemaining(9)) {
        LOG_WARNING("ListInventoryParser: packet too short");
        return false;
    }

    data.vendorGuid = packet.readUInt64();
    uint8_t itemCount = packet.readUInt8();

    if (itemCount == 0) {
        LOG_INFO("Vendor has nothing for sale");
        return true;
    }

    // Auto-detect whether server sends 7 fields (28 bytes/item) or 8 fields (32 bytes/item).
    // Some servers omit the extendedCost field entirely; reading 8 fields on a 7-field packet
    // misaligns every item after the first and produces garbage prices.
    size_t remaining = packet.getRemainingSize();
    const size_t bytesPerItemNoExt = 28;
    const size_t bytesPerItemWithExt = 32;
    bool hasExtendedCost = false;
    if (remaining < static_cast<size_t>(itemCount) * bytesPerItemNoExt) {
        LOG_WARNING("ListInventoryParser: truncated packet (items=", static_cast<int>(itemCount),
                    ", remaining=", remaining, ")");
        return false;
    }
    if (remaining >= static_cast<size_t>(itemCount) * bytesPerItemWithExt) {
        hasExtendedCost = true;
    }

    data.items.reserve(itemCount);
    for (uint8_t i = 0; i < itemCount; ++i) {
        const size_t perItemBytes = hasExtendedCost ? bytesPerItemWithExt : bytesPerItemNoExt;
        if (!packet.hasRemaining(perItemBytes)) {
            LOG_WARNING("ListInventoryParser: item ", static_cast<int>(i), " truncated");
            return false;
        }
        VendorItem item;
        item.slot = packet.readUInt32();
        item.itemId = packet.readUInt32();
        item.displayInfoId = packet.readUInt32();
        item.maxCount = static_cast<int32_t>(packet.readUInt32());
        item.buyPrice = packet.readUInt32();
        item.durability = packet.readUInt32();
        item.stackCount = packet.readUInt32();
        item.extendedCost = hasExtendedCost ? packet.readUInt32() : 0;
        data.items.push_back(item);
    }

    LOG_DEBUG("Vendor inventory: ", static_cast<int>(itemCount), " items (extendedCost: ", hasExtendedCost ? "yes" : "no", ")");
    return true;
}

// ============================================================
// Trainer
// ============================================================

bool TrainerListParser::parse(network::Packet& packet, TrainerListData& data, bool isClassic) {
    // WotLK per-entry: spellId(4) + state(1) + cost(4) + profDialog(4) + profButton(4) +
    //                  reqLevel(1) + reqSkill(4) + reqSkillValue(4) + chain×3(12) = 38 bytes
    // Classic per-entry: spellId(4) + state(1) + cost(4) + reqLevel(1) +
    //                    reqSkill(4) + reqSkillValue(4) + chain×3(12) + unk(4) = 34 bytes
    data = TrainerListData{};
    if (!packet.hasRemaining(16)) return false; // guid(8) + type(4) + count(4)

    data.trainerGuid = packet.readUInt64();
    data.trainerType = packet.readUInt32();
    uint32_t spellCount = packet.readUInt32();

    if (spellCount > 1000) {
        LOG_ERROR("TrainerListParser: unreasonable spell count ", spellCount);
        return false;
    }

    data.spells.reserve(spellCount);
    for (uint32_t i = 0; i < spellCount; ++i) {
        // Validate minimum entry size before reading
        const size_t minEntrySize = isClassic ? 34 : 38;
        if (!packet.hasRemaining(minEntrySize)) {
            LOG_WARNING("TrainerListParser: truncated at spell ", i);
            break;
        }

        TrainerSpell spell;
        spell.spellId   = packet.readUInt32();
        spell.state     = packet.readUInt8();
        spell.spellCost = packet.readUInt32();
        if (isClassic) {
            // Classic 1.12: reqLevel immediately after cost; no profDialog/profButton
            spell.profDialog = 0;
            spell.profButton = 0;
            spell.reqLevel   = packet.readUInt8();
        } else {
            // TBC / WotLK: profDialog + profButton before reqLevel
            spell.profDialog = packet.readUInt32();
            spell.profButton = packet.readUInt32();
            spell.reqLevel   = packet.readUInt8();
        }
        spell.reqSkill      = packet.readUInt32();
        spell.reqSkillValue = packet.readUInt32();
        spell.chainNode1    = packet.readUInt32();
        spell.chainNode2    = packet.readUInt32();
        spell.chainNode3    = packet.readUInt32();
        if (isClassic) {
            packet.readUInt32(); // trailing unk / sort index
        }
        data.spells.push_back(spell);
    }

    if (!packet.hasData()) {
        LOG_WARNING("TrainerListParser: truncated before greeting");
        data.greeting.clear();
    } else {
        data.greeting = packet.readString();
    }

    LOG_INFO("Trainer list (", isClassic ? "Classic" : "TBC/WotLK", "): ",
             spellCount, " spells, type=", data.trainerType,
             ", greeting=\"", data.greeting, "\"");
    return true;
}

network::Packet TrainerBuySpellPacket::build(uint64_t trainerGuid, uint32_t spellId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_TRAINER_BUY_SPELL));
    packet.writeUInt64(trainerGuid);
    packet.writeUInt32(spellId);
    return packet;
}

// ============================================================
// Talents
// ============================================================

bool TalentsInfoParser::parse(network::Packet& packet, TalentsInfoData& data) {
    // SMSG_TALENTS_INFO format (AzerothCore variant):
    // uint8  activeSpec
    // uint8  unspentPoints
    // be32   talentCount (metadata, may not match entry count)
    // be16   entryCount (actual number of id+rank entries)
    // Entry[entryCount]: { le32 id, uint8 rank }
    // le32   glyphSlots
    // le16   glyphIds[glyphSlots]

    const size_t startPos = packet.getReadPos();
    const size_t remaining = packet.getSize() - startPos;

    if (remaining < 2 + 4 + 2) {
        LOG_ERROR("SMSG_TALENTS_INFO: packet too short (remaining=", remaining, ")");
        return false;
    }

    data = TalentsInfoData{};

    // Read header
    data.talentSpec = packet.readUInt8();
    data.unspentPoints = packet.readUInt8();

    // These two counts are big-endian (network byte order)
    uint32_t talentCountBE = packet.readUInt32();
    uint32_t talentCount = bswap32(talentCountBE);

    uint16_t entryCountBE = packet.readUInt16();
    uint16_t entryCount = bswap16(entryCountBE);

    // Sanity check: prevent corrupt packets from allocating excessive memory
    if (entryCount > 64) {
        LOG_ERROR("SMSG_TALENTS_INFO: entryCount too large (", entryCount, "), rejecting packet");
        return false;
    }

    LOG_INFO("SMSG_TALENTS_INFO: spec=", static_cast<int>(data.talentSpec),
             " unspent=", static_cast<int>(data.unspentPoints),
             " talentCount=", talentCount,
             " entryCount=", entryCount);

    // Parse learned entries (id + rank pairs)
    // These may be talents, glyphs, or other learned abilities
    data.talents.clear();
    data.talents.reserve(entryCount);

    for (uint16_t i = 0; i < entryCount; ++i) {
        if (!packet.hasRemaining(5)) {
            LOG_ERROR("SMSG_TALENTS_INFO: truncated entry list at i=", i);
            return false;
        }
        uint32_t id = packet.readUInt32();  // LE
        uint8_t rank = packet.readUInt8();
        data.talents.push_back({id, rank});

        LOG_INFO("  Entry: id=", id, " rank=", static_cast<int>(rank));
    }

    // Parse glyph tail: glyphSlots + glyphIds[]
    if (!packet.hasRemaining(1)) {
        LOG_WARNING("SMSG_TALENTS_INFO: no glyph tail data");
        return true;  // Not fatal, older formats may not have glyphs
    }

    uint8_t glyphSlots = packet.readUInt8();

    // Sanity check: Wrath has 6 glyph slots, cap at 12 for safety
    if (glyphSlots > 12) {
        LOG_WARNING("SMSG_TALENTS_INFO: glyphSlots too large (", static_cast<int>(glyphSlots), "), clamping to 12");
        glyphSlots = 12;
    }

    LOG_INFO("  GlyphSlots: ", static_cast<int>(glyphSlots));

    data.glyphs.clear();
    data.glyphs.reserve(glyphSlots);

    for (uint8_t i = 0; i < glyphSlots; ++i) {
        if (!packet.hasRemaining(2)) {
            LOG_ERROR("SMSG_TALENTS_INFO: truncated glyph list at i=", i);
            return false;
        }
        uint16_t glyphId = packet.readUInt16();  // LE
        data.glyphs.push_back(glyphId);
        if (glyphId != 0) {
            LOG_INFO("    Glyph slot ", i, ": ", glyphId);
        }
    }

    LOG_INFO("SMSG_TALENTS_INFO: bytesConsumed=", (packet.getReadPos() - startPos),
             " bytesRemaining=", (packet.getRemainingSize()));

    return true;
}

network::Packet LearnTalentPacket::build(uint32_t talentId, uint32_t requestedRank) {
    network::Packet packet(wireOpcode(Opcode::CMSG_LEARN_TALENT));
    packet.writeUInt32(talentId);
    packet.writeUInt32(requestedRank);
    return packet;
}

network::Packet TalentWipeConfirmPacket::build(bool accept) {
    network::Packet packet(wireOpcode(Opcode::MSG_TALENT_WIPE_CONFIRM));
    packet.writeUInt32(accept ? 1 : 0);
    return packet;
}

network::Packet ActivateTalentGroupPacket::build(uint32_t group) {
    // CMSG_SET_ACTIVE_TALENT_GROUP_OBSOLETE (0x4C3 in WotLK 3.3.5a)
    // Payload: uint32 group (0 = primary, 1 = secondary)
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_ACTIVE_TALENT_GROUP_OBSOLETE));
    packet.writeUInt32(group);
    return packet;
}

// ============================================================
// Death/Respawn
// ============================================================

network::Packet RepopRequestPacket::build() {
    network::Packet packet(wireOpcode(Opcode::CMSG_REPOP_REQUEST));
    packet.writeUInt8(1);  // request release (1 = manual)
    return packet;
}

network::Packet ReclaimCorpsePacket::build(uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_RECLAIM_CORPSE));
    packet.writeUInt64(guid);
    return packet;
}

network::Packet SpiritHealerActivatePacket::build(uint64_t npcGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_SPIRIT_HEALER_ACTIVATE));
    packet.writeUInt64(npcGuid);
    return packet;
}

network::Packet ResurrectResponsePacket::build(uint64_t casterGuid, bool accept) {
    network::Packet packet(wireOpcode(Opcode::CMSG_RESURRECT_RESPONSE));
    packet.writeUInt64(casterGuid);
    packet.writeUInt8(accept ? 1 : 0);
    return packet;
}

// ============================================================
// Taxi / Flight Paths
// ============================================================

bool ShowTaxiNodesParser::parse(network::Packet& packet, ShowTaxiNodesData& data) {
    // Minimum: windowInfo(4) + npcGuid(8) + nearestNode(4) + at least 1 mask uint32(4)
    size_t remaining = packet.getRemainingSize();
    if (remaining < 4 + 8 + 4 + 4) {
        LOG_ERROR("ShowTaxiNodesParser: packet too short (", remaining, " bytes)");
        return false;
    }
    data.windowInfo = packet.readUInt32();
    data.npcGuid = packet.readUInt64();
    data.nearestNode = packet.readUInt32();
    // Read as many mask uint32s as available (Classic/Vanilla=4, WotLK=12)
    size_t maskBytes = packet.getRemainingSize();
    uint32_t maskCount = static_cast<uint32_t>(maskBytes / 4);
    if (maskCount > TLK_TAXI_MASK_SIZE) maskCount = TLK_TAXI_MASK_SIZE;
    for (uint32_t i = 0; i < maskCount; ++i) {
        data.nodeMask[i] = packet.readUInt32();
    }
    LOG_INFO("ShowTaxiNodes: window=", data.windowInfo, " npc=0x", std::hex, data.npcGuid, std::dec,
             " nearest=", data.nearestNode, " maskSlots=", maskCount);
    return true;
}

bool ActivateTaxiReplyParser::parse(network::Packet& packet, ActivateTaxiReplyData& data) {
    size_t remaining = packet.getRemainingSize();
    if (remaining >= 4) {
        data.result = packet.readUInt32();
    } else if (remaining >= 1) {
        data.result = packet.readUInt8();
    } else {
        LOG_ERROR("ActivateTaxiReplyParser: packet too short");
        return false;
    }
    LOG_INFO("ActivateTaxiReply: result=", data.result);
    return true;
}

network::Packet ActivateTaxiExpressPacket::build(uint64_t npcGuid, uint32_t totalCost, const std::vector<uint32_t>& pathNodes) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ACTIVATETAXIEXPRESS));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(totalCost);
    packet.writeUInt32(static_cast<uint32_t>(pathNodes.size()));
    for (uint32_t nodeId : pathNodes) {
        packet.writeUInt32(nodeId);
    }
    LOG_INFO("ActivateTaxiExpress: npc=0x", std::hex, npcGuid, std::dec,
             " cost=", totalCost, " nodes=", pathNodes.size());
    return packet;
}

network::Packet ActivateTaxiPacket::build(uint64_t npcGuid, uint32_t srcNode, uint32_t destNode) {
    network::Packet packet(wireOpcode(Opcode::CMSG_ACTIVATETAXI));
    packet.writeUInt64(npcGuid);
    packet.writeUInt32(srcNode);
    packet.writeUInt32(destNode);
    return packet;
}

network::Packet GameObjectUsePacket::build(uint64_t guid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GAMEOBJ_USE));
    packet.writeUInt64(guid);
    return packet;
}

// ============================================================
// Mail System
// ============================================================

network::Packet GetMailListPacket::build(uint64_t mailboxGuid) {
    network::Packet packet(wireOpcode(Opcode::CMSG_GET_MAIL_LIST));
    packet.writeUInt64(mailboxGuid);
    return packet;
}

network::Packet SendMailPacket::build(uint64_t mailboxGuid, const std::string& recipient,
                                      const std::string& subject, const std::string& body,
                                      uint64_t money, uint64_t cod,
                                      const std::vector<uint64_t>& itemGuids) {
    // WotLK 3.3.5a format
    network::Packet packet(wireOpcode(Opcode::CMSG_SEND_MAIL));
    packet.writeUInt64(mailboxGuid);
    packet.writeString(recipient);
    packet.writeString(subject);
    packet.writeString(body);
    packet.writeUInt32(0);       // stationery
    packet.writeUInt32(0);       // unknown
    uint8_t attachCount = static_cast<uint8_t>(itemGuids.size());
    packet.writeUInt8(attachCount);
    for (uint8_t i = 0; i < attachCount; ++i) {
        packet.writeUInt8(i);            // attachment slot index
        packet.writeUInt64(itemGuids[i]);
    }
    packet.writeUInt64(money);
    packet.writeUInt64(cod);
    return packet;
}

network::Packet MailTakeMoneyPacket::build(uint64_t mailboxGuid, uint32_t mailId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_TAKE_MONEY));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    return packet;
}

network::Packet MailTakeItemPacket::build(uint64_t mailboxGuid, uint32_t mailId, uint32_t itemGuidLow) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_TAKE_ITEM));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    // WotLK expects attachment item GUID low, not attachment slot index.
    packet.writeUInt32(itemGuidLow);
    return packet;
}

network::Packet MailDeletePacket::build(uint64_t mailboxGuid, uint32_t mailId, uint32_t mailTemplateId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_DELETE));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    packet.writeUInt32(mailTemplateId);
    return packet;
}

network::Packet MailMarkAsReadPacket::build(uint64_t mailboxGuid, uint32_t mailId) {
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_MARK_AS_READ));
    packet.writeUInt64(mailboxGuid);
    packet.writeUInt32(mailId);
    return packet;
}

// ============================================================================
// PacketParsers::parseMailList — WotLK 3.3.5a format (base/default)
// ============================================================================
bool PacketParsers::parseMailList(network::Packet& packet, std::vector<MailMessage>& inbox) {
    size_t remaining = packet.getRemainingSize();
    if (remaining < 5) return false;

    uint32_t totalCount = packet.readUInt32();
    uint8_t shownCount = packet.readUInt8();
    (void)totalCount;

    LOG_INFO("SMSG_MAIL_LIST_RESULT (WotLK): total=", totalCount, " shown=", static_cast<int>(shownCount));

    inbox.clear();
    inbox.reserve(shownCount);

    for (uint8_t i = 0; i < shownCount; ++i) {
        remaining = packet.getRemainingSize();
        if (remaining < 2) break;

        uint16_t msgSize = packet.readUInt16();
        size_t startPos = packet.getReadPos();

        MailMessage msg;
        if (remaining < static_cast<size_t>(msgSize) + 2) {
            LOG_WARNING("Mail entry ", i, " truncated");
            break;
        }

        msg.messageId = packet.readUInt32();
        msg.messageType = packet.readUInt8();

        switch (msg.messageType) {
            case 0: msg.senderGuid = packet.readUInt64(); break;
            case 2: case 3: case 4: case 5:
                msg.senderEntry = packet.readUInt32(); break;
            default: msg.senderEntry = packet.readUInt32(); break;
        }

        msg.cod = packet.readUInt64();
        packet.readUInt32(); // item text id
        packet.readUInt32(); // unknown
        msg.stationeryId = packet.readUInt32();
        msg.money = packet.readUInt64();
        msg.flags = packet.readUInt32();
        msg.expirationTime = packet.readFloat();
        msg.mailTemplateId = packet.readUInt32();
        msg.subject = packet.readString();
        // WotLK 3.3.5a always includes body text in SMSG_MAIL_LIST_RESULT.
        // mailTemplateId != 0 still carries a (possibly empty) body string.
        msg.body = packet.readString();

        uint8_t attachCount = packet.readUInt8();
        msg.attachments.reserve(attachCount);
        for (uint8_t j = 0; j < attachCount; ++j) {
            MailAttachment att;
            att.slot = packet.readUInt8();
            att.itemGuidLow = packet.readUInt32();
            att.itemId = packet.readUInt32();
            for (int e = 0; e < 7; ++e) {
                uint32_t enchId = packet.readUInt32();
                packet.readUInt32(); // duration
                packet.readUInt32(); // charges
                if (e == 0) att.enchantId = enchId;
            }
            att.randomPropertyId = packet.readUInt32();
            att.randomSuffix = packet.readUInt32();
            att.stackCount = packet.readUInt32();
            att.chargesOrDurability = packet.readUInt32();
            att.maxDurability = packet.readUInt32();
            packet.readUInt32(); // durability/current durability
            packet.readUInt8();  // unknown WotLK trailing byte per attachment
            msg.attachments.push_back(att);
        }

        msg.read = (msg.flags & 0x01) != 0;
        inbox.push_back(std::move(msg));

        // Skip unread bytes
        size_t consumed = packet.getReadPos() - startPos;
        if (consumed < msgSize) {
            size_t skip = msgSize - consumed;
            for (size_t s = 0; s < skip && packet.hasData(); ++s)
                packet.readUInt8();
        }
    }

    LOG_INFO("Parsed ", inbox.size(), " mail messages");
    return true;
}

// ============================================================
// Bank System
// ============================================================

network::Packet BankerActivatePacket::build(uint64_t guid) {
    network::Packet p(wireOpcode(Opcode::CMSG_BANKER_ACTIVATE));
    p.writeUInt64(guid);
    return p;
}

network::Packet BuyBankSlotPacket::build(uint64_t guid) {
    network::Packet p(wireOpcode(Opcode::CMSG_BUY_BANK_SLOT));
    p.writeUInt64(guid);
    return p;
}

network::Packet AutoBankItemPacket::build(uint8_t srcBag, uint8_t srcSlot) {
    network::Packet p(wireOpcode(Opcode::CMSG_AUTOBANK_ITEM));
    p.writeUInt8(srcBag);
    p.writeUInt8(srcSlot);
    return p;
}

network::Packet AutoStoreBankItemPacket::build(uint8_t srcBag, uint8_t srcSlot) {
    network::Packet p(wireOpcode(Opcode::CMSG_AUTOSTORE_BANK_ITEM));
    p.writeUInt8(srcBag);
    p.writeUInt8(srcSlot);
    return p;
}

// ============================================================
// Guild Bank System
// ============================================================

network::Packet GuildBankerActivatePacket::build(uint64_t guid) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANKER_ACTIVATE));
    p.writeUInt64(guid);
    p.writeUInt8(0);  // full slots update
    return p;
}

network::Packet GuildBankQueryTabPacket::build(uint64_t guid, uint8_t tabId, bool fullUpdate) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_QUERY_TAB));
    p.writeUInt64(guid);
    p.writeUInt8(tabId);
    p.writeUInt8(fullUpdate ? 1 : 0);
    return p;
}

network::Packet GuildBankBuyTabPacket::build(uint64_t guid, uint8_t tabId) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_BUY_TAB));
    p.writeUInt64(guid);
    p.writeUInt8(tabId);
    return p;
}

network::Packet GuildBankDepositMoneyPacket::build(uint64_t guid, uint32_t amount) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_DEPOSIT_MONEY));
    p.writeUInt64(guid);
    p.writeUInt32(amount);
    return p;
}

network::Packet GuildBankWithdrawMoneyPacket::build(uint64_t guid, uint32_t amount) {
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_WITHDRAW_MONEY));
    p.writeUInt64(guid);
    p.writeUInt32(amount);
    return p;
}

network::Packet GuildBankSwapItemsPacket::buildBankToInventory(
    uint64_t guid, uint8_t tabId, uint8_t bankSlot,
    uint8_t destBag, uint8_t destSlot, uint32_t splitCount)
{
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS));
    p.writeUInt64(guid);
    p.writeUInt8(0);  // bankToCharacter = false -> bank source
    p.writeUInt8(tabId);
    p.writeUInt8(bankSlot);
    p.writeUInt32(0);  // itemEntry (unused client side)
    p.writeUInt8(0);  // autoStore = false
    if (splitCount > 0) {
        p.writeUInt8(splitCount);
    }
    p.writeUInt8(destBag);
    p.writeUInt8(destSlot);
    return p;
}

network::Packet GuildBankSwapItemsPacket::buildInventoryToBank(
    uint64_t guid, uint8_t tabId, uint8_t bankSlot,
    uint8_t srcBag, uint8_t srcSlot, uint32_t splitCount)
{
    network::Packet p(wireOpcode(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS));
    p.writeUInt64(guid);
    p.writeUInt8(1);  // bankToCharacter = true -> char to bank
    p.writeUInt8(tabId);
    p.writeUInt8(bankSlot);
    p.writeUInt32(0);  // itemEntry
    p.writeUInt8(0);  // autoStore
    if (splitCount > 0) {
        p.writeUInt8(splitCount);
    }
    p.writeUInt8(srcBag);
    p.writeUInt8(srcSlot);
    return p;
}

bool GuildBankListParser::parse(network::Packet& packet, GuildBankData& data) {
    if (!packet.hasRemaining(14)) return false;

    data.money = packet.readUInt64();
    data.tabId = packet.readUInt8();
    data.withdrawAmount = static_cast<int32_t>(packet.readUInt32());
    uint8_t fullUpdate = packet.readUInt8();

    if (fullUpdate) {
        if (!packet.hasRemaining(1)) {
            LOG_WARNING("GuildBankListParser: truncated before tabCount");
            data.tabs.clear();
        } else {
            uint8_t tabCount = packet.readUInt8();
            // Cap at 8 (normal guild bank tab limit in WoW)
            if (tabCount > 8) {
                LOG_WARNING("GuildBankListParser: tabCount capped (requested=", static_cast<int>(tabCount), ")");
                tabCount = 8;
            }
            data.tabs.resize(tabCount);
            for (uint8_t i = 0; i < tabCount; ++i) {
                // Validate before reading strings
                if (!packet.hasData()) {
                    LOG_WARNING("GuildBankListParser: truncated tab at index ", static_cast<int>(i));
                    break;
                }
                data.tabs[i].tabName = packet.readString();
                if (!packet.hasData()) {
                    data.tabs[i].tabIcon.clear();
                } else {
                    data.tabs[i].tabIcon = packet.readString();
                }
            }
        }
    }

    if (!packet.hasRemaining(1)) {
        LOG_WARNING("GuildBankListParser: truncated before numSlots");
        data.tabItems.clear();
        return true;
    }

    uint8_t numSlots = packet.readUInt8();
    data.tabItems.clear();
    for (uint8_t i = 0; i < numSlots; ++i) {
        // Validate minimum bytes before reading slot (slotId(1) + itemEntry(4) = 5)
        if (!packet.hasRemaining(5)) {
            LOG_WARNING("GuildBankListParser: truncated slot at index ", static_cast<int>(i));
            break;
        }
        GuildBankItemSlot slot;
        slot.slotId = packet.readUInt8();
        slot.itemEntry = packet.readUInt32();
        if (slot.itemEntry != 0) {
            // Validate before reading enchant mask
            if (!packet.hasRemaining(4)) break;
            // Enchant info
            uint32_t enchantMask = packet.readUInt32();
            for (int bit = 0; bit < 10; ++bit) {
                if (enchantMask & (1u << bit)) {
                    if (!packet.hasRemaining(12)) {
                        LOG_WARNING("GuildBankListParser: truncated enchant data");
                        break;
                    }
                    uint32_t enchId = packet.readUInt32();
                    uint32_t enchDur = packet.readUInt32();
                    uint32_t enchCharges = packet.readUInt32();
                    if (bit == 0) slot.enchantId = enchId;
                    (void)enchDur; (void)enchCharges;
                }
            }
            // Validate before reading remaining item fields
            if (!packet.hasRemaining(12)) {
                LOG_WARNING("GuildBankListParser: truncated item fields");
                break;
            }
            slot.stackCount = packet.readUInt32();
            /*spare=*/ packet.readUInt32();
            slot.randomPropertyId = packet.readUInt32();
            if (slot.randomPropertyId) {
                if (!packet.hasRemaining(4)) {
                    LOG_WARNING("GuildBankListParser: truncated suffix factor");
                    break;
                }
                /*suffixFactor=*/ packet.readUInt32();
            }
        }
        data.tabItems.push_back(slot);
    }
    return true;
}

// ============================================================
// Auction House System
// ============================================================

network::Packet AuctionHelloPacket::build(uint64_t guid) {
    network::Packet p(wireOpcode(Opcode::MSG_AUCTION_HELLO));
    p.writeUInt64(guid);
    return p;
}

bool AuctionHelloParser::parse(network::Packet& packet, AuctionHelloData& data) {
    size_t remaining = packet.getRemainingSize();
    if (remaining < 12) {
        LOG_WARNING("AuctionHelloParser: too small, remaining=", remaining);
        return false;
    }
    data.auctioneerGuid = packet.readUInt64();
    data.auctionHouseId = packet.readUInt32();
    // WotLK has an extra uint8 enabled field; Vanilla does not
    if (packet.hasData()) {
        data.enabled = packet.readUInt8();
    } else {
        data.enabled = 1;
    }
    return true;
}

network::Packet AuctionListItemsPacket::build(
    uint64_t guid, uint32_t offset,
    const std::string& searchName,
    uint8_t levelMin, uint8_t levelMax,
    uint32_t invTypeMask, uint32_t itemClass,
    uint32_t itemSubClass, uint32_t quality,
    uint8_t usableOnly, uint8_t exactMatch)
{
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_LIST_ITEMS));
    p.writeUInt64(guid);
    p.writeUInt32(offset);
    p.writeString(searchName);
    p.writeUInt8(levelMin);
    p.writeUInt8(levelMax);
    p.writeUInt32(invTypeMask);
    p.writeUInt32(itemClass);
    p.writeUInt32(itemSubClass);
    p.writeUInt32(quality);
    p.writeUInt8(usableOnly);
    p.writeUInt8(0);  // getAll (0 = normal search)
    p.writeUInt8(exactMatch);
    // Sort columns (0 = none)
    p.writeUInt8(0);
    return p;
}

network::Packet AuctionSellItemPacket::build(
    uint64_t auctioneerGuid, uint64_t itemGuid,
    uint32_t stackCount, uint32_t bid,
    uint32_t buyout, uint32_t duration,
    bool preWotlk)
{
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_SELL_ITEM));
    p.writeUInt64(auctioneerGuid);
    if (!preWotlk) {
        // WotLK: itemCount(4) + per-item [guid(8) + stackCount(4)]
        p.writeUInt32(1);
        p.writeUInt64(itemGuid);
        p.writeUInt32(stackCount);
    } else {
        // Classic/TBC: just itemGuid, no count fields
        p.writeUInt64(itemGuid);
    }
    p.writeUInt32(bid);
    p.writeUInt32(buyout);
    p.writeUInt32(duration);
    return p;
}

network::Packet AuctionPlaceBidPacket::build(uint64_t auctioneerGuid, uint32_t auctionId, uint32_t amount) {
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_PLACE_BID));
    p.writeUInt64(auctioneerGuid);
    p.writeUInt32(auctionId);
    p.writeUInt32(amount);
    return p;
}

network::Packet AuctionRemoveItemPacket::build(uint64_t auctioneerGuid, uint32_t auctionId) {
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_REMOVE_ITEM));
    p.writeUInt64(auctioneerGuid);
    p.writeUInt32(auctionId);
    return p;
}

network::Packet AuctionListOwnerItemsPacket::build(uint64_t auctioneerGuid, uint32_t offset) {
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_LIST_OWNER_ITEMS));
    p.writeUInt64(auctioneerGuid);
    p.writeUInt32(offset);
    return p;
}

network::Packet AuctionListBidderItemsPacket::build(
    uint64_t auctioneerGuid, uint32_t offset,
    const std::vector<uint32_t>& outbiddedIds)
{
    network::Packet p(wireOpcode(Opcode::CMSG_AUCTION_LIST_BIDDER_ITEMS));
    p.writeUInt64(auctioneerGuid);
    p.writeUInt32(offset);
    p.writeUInt32(static_cast<uint32_t>(outbiddedIds.size()));
    for (uint32_t id : outbiddedIds)
        p.writeUInt32(id);
    return p;
}

bool AuctionListResultParser::parse(network::Packet& packet, AuctionListResult& data, int numEnchantSlots) {
    // Per-entry fixed size: auctionId(4) + itemEntry(4) + enchantSlots×3×4 +
    //   randProp(4) + suffix(4) + stack(4) + charges(4) + flags(4) +
    //   ownerGuid(8) + startBid(4) + outbid(4) + buyout(4) + expire(4) +
    //   bidderGuid(8) + curBid(4)
    // Classic: numEnchantSlots=1 → 80 bytes/entry
    // TBC/WotLK: numEnchantSlots=3 → 104 bytes/entry
    if (!packet.hasRemaining(4)) return false;

    uint32_t count = packet.readUInt32();
    // Cap auction count to prevent unbounded memory allocation
    const uint32_t MAX_AUCTION_RESULTS = 256;
    if (count > MAX_AUCTION_RESULTS) {
        LOG_WARNING("AuctionListResultParser: count capped (requested=", count, ")");
        count = MAX_AUCTION_RESULTS;
    }

    data.auctions.clear();
    data.auctions.reserve(count);

    const size_t minPerEntry = static_cast<size_t>(8 + numEnchantSlots * 12 + 28 + 8 + 8);
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(minPerEntry)) break;
        AuctionEntry e;
        e.auctionId = packet.readUInt32();
        e.itemEntry = packet.readUInt32();
        // First enchant slot always present
        e.enchantId = packet.readUInt32();
        packet.readUInt32(); // enchant1 duration
        packet.readUInt32(); // enchant1 charges
        // Extra enchant slots for TBC/WotLK
        for (int s = 1; s < numEnchantSlots; ++s) {
            packet.readUInt32(); // enchant N id
            packet.readUInt32(); // enchant N duration
            packet.readUInt32(); // enchant N charges
        }
        e.randomPropertyId = packet.readUInt32();
        e.suffixFactor     = packet.readUInt32();
        e.stackCount       = packet.readUInt32();
        packet.readUInt32(); // item charges
        packet.readUInt32(); // item flags (unused)
        e.ownerGuid        = packet.readUInt64();
        e.startBid         = packet.readUInt32();
        e.minBidIncrement  = packet.readUInt32();
        e.buyoutPrice      = packet.readUInt32();
        e.timeLeftMs       = packet.readUInt32();
        e.bidderGuid       = packet.readUInt64();
        e.currentBid       = packet.readUInt32();
        data.auctions.push_back(e);
    }

    if (packet.hasRemaining(8)) {
        data.totalCount = packet.readUInt32();
        data.searchDelay = packet.readUInt32();
    }
    return true;
}

bool AuctionCommandResultParser::parse(network::Packet& packet, AuctionCommandResult& data) {
    if (!packet.hasRemaining(12)) return false;
    data.auctionId = packet.readUInt32();
    data.action = packet.readUInt32();
    data.errorCode = packet.readUInt32();
    if (data.errorCode != 0 && data.action == 2 && packet.hasRemaining(4)) {
        data.bidError = packet.readUInt32();
    }
    return true;
}

// ============================================================
// Pet Stable System
// ============================================================

network::Packet ListStabledPetsPacket::build(uint64_t stableMasterGuid) {
    network::Packet p(wireOpcode(Opcode::MSG_LIST_STABLED_PETS));
    p.writeUInt64(stableMasterGuid);
    return p;
}

network::Packet StablePetPacket::build(uint64_t stableMasterGuid, uint8_t slot) {
    network::Packet p(wireOpcode(Opcode::CMSG_STABLE_PET));
    p.writeUInt64(stableMasterGuid);
    p.writeUInt8(slot);
    return p;
}

network::Packet UnstablePetPacket::build(uint64_t stableMasterGuid, uint32_t petNumber) {
    network::Packet p(wireOpcode(Opcode::CMSG_UNSTABLE_PET));
    p.writeUInt64(stableMasterGuid);
    p.writeUInt32(petNumber);
    return p;
}

network::Packet PetRenamePacket::build(uint64_t petGuid, const std::string& name, uint8_t isDeclined) {
    network::Packet p(wireOpcode(Opcode::CMSG_PET_RENAME));
    p.writeUInt64(petGuid);
    p.writeString(name);    // null-terminated
    p.writeUInt8(isDeclined);
    return p;
}

network::Packet SetTitlePacket::build(int32_t titleBit) {
    // CMSG_SET_TITLE: int32 titleBit (-1 = remove active title)
    network::Packet p(wireOpcode(Opcode::CMSG_SET_TITLE));
    p.writeUInt32(static_cast<uint32_t>(titleBit));
    return p;
}

network::Packet AlterAppearancePacket::build(uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair) {
    // CMSG_ALTER_APPEARANCE: uint32 hairStyle + uint32 hairColor + uint32 facialHair
    network::Packet p(wireOpcode(Opcode::CMSG_ALTER_APPEARANCE));
    p.writeUInt32(hairStyle);
    p.writeUInt32(hairColor);
    p.writeUInt32(facialHair);
    return p;
}

} // namespace game
} // namespace wowee
