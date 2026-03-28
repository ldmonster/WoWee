#include "game/game_handler.hpp"
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

bool isAuthCharPipelineOpcode(LogicalOpcode op) {
    switch (op) {
        case Opcode::SMSG_AUTH_CHALLENGE:
        case Opcode::SMSG_AUTH_RESPONSE:
        case Opcode::SMSG_CLIENTCACHE_VERSION:
        case Opcode::SMSG_TUTORIAL_FLAGS:
        case Opcode::SMSG_WARDEN_DATA:
        case Opcode::SMSG_CHAR_ENUM:
        case Opcode::SMSG_CHAR_CREATE:
        case Opcode::SMSG_CHAR_DELETE:
            return true;
        default:
            return false;
    }
}

} // end anonymous namespace

// Build a WoW-format item link for use in system chat messages.
// The chat renderer in game_screen.cpp parses this format and draws the
// item name in its quality colour with a small icon and tooltip.
// Format: |cff<rrggbb>|Hitem:<id>:0:0:0:0:0:0:0:0|h[<name>]|h|r
std::string buildItemLink(uint32_t itemId, uint32_t quality, const std::string& name) {
    static const char* kQualHex[] = {
        "9d9d9d",  // 0 Poor
        "ffffff",  // 1 Common
        "1eff00",  // 2 Uncommon
        "0070dd",  // 3 Rare
        "a335ee",  // 4 Epic
        "ff8000",  // 5 Legendary
        "e6cc80",  // 6 Artifact
        "e6cc80",  // 7 Heirloom
    };
    uint32_t qi = quality < 8 ? quality : 1u;
    char buf[512];
    snprintf(buf, sizeof(buf), "|cff%s|Hitem:%u:0:0:0:0:0:0:0:0|h[%s]|h|r",
             kQualHex[qi], itemId, name.c_str());
    return buf;
}

namespace {

bool isActiveExpansion(const char* expansionId) {
    auto& app = core::Application::getInstance();
    auto* registry = app.getExpansionRegistry();
    if (!registry) return false;
    auto* profile = registry->getActive();
    if (!profile) return false;
    return profile->id == expansionId;
}

bool isClassicLikeExpansion() {
    return isActiveExpansion("classic") || isActiveExpansion("turtle");
}

bool isPreWotlk() {
    return isClassicLikeExpansion() || isActiveExpansion("tbc");
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

int incomingPacketsBudgetPerUpdate(WorldState state) {
    static const int inWorldBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKETS", 24, 1, 512);
    static const int loginBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKETS_LOGIN", 96, 1, 512);
    return state == WorldState::IN_WORLD ? inWorldBudget : loginBudget;
}

float incomingPacketBudgetMs(WorldState state) {
    static const int inWorldBudgetMs =
        parseEnvIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKET_MS", 2, 1, 50);
    static const int loginBudgetMs =
        parseEnvIntClamped("WOWEE_NET_MAX_GAMEHANDLER_PACKET_MS_LOGIN", 8, 1, 50);
    return static_cast<float>(state == WorldState::IN_WORLD ? inWorldBudgetMs : loginBudgetMs);
}

int updateObjectBlocksBudgetPerUpdate(WorldState state) {
    static const int inWorldBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_UPDATE_OBJECT_BLOCKS", 24, 1, 2048);
    static const int loginBudget =
        parseEnvIntClamped("WOWEE_NET_MAX_UPDATE_OBJECT_BLOCKS_LOGIN", 128, 1, 4096);
    return state == WorldState::IN_WORLD ? inWorldBudget : loginBudget;
}

float slowPacketLogThresholdMs() {
    static const int thresholdMs =
        parseEnvIntClamped("WOWEE_NET_SLOW_PACKET_LOG_MS", 10, 1, 60000);
    return static_cast<float>(thresholdMs);
}

float slowUpdateObjectBlockLogThresholdMs() {
    static const int thresholdMs =
        parseEnvIntClamped("WOWEE_NET_SLOW_UPDATE_BLOCK_LOG_MS", 10, 1, 60000);
    return static_cast<float>(thresholdMs);
}

constexpr size_t kMaxQueuedInboundPackets = 4096;

CombatTextEntry::Type combatTextTypeFromSpellMissInfo(uint8_t missInfo) {
    switch (missInfo) {
        case 0: return CombatTextEntry::MISS;
        case 1: return CombatTextEntry::DODGE;
        case 2: return CombatTextEntry::PARRY;
        case 3: return CombatTextEntry::BLOCK;
        case 4: return CombatTextEntry::EVADE;
        case 5: return CombatTextEntry::IMMUNE;
        case 6: return CombatTextEntry::DEFLECT;
        case 7: return CombatTextEntry::ABSORB;
        case 8: return CombatTextEntry::RESIST;
        case 9:  // Some cores encode SPELL_MISS_IMMUNE2 as 9.
        case 10: // Others encode SPELL_MISS_IMMUNE2 as 10.
            return CombatTextEntry::IMMUNE;
        case 11: return CombatTextEntry::REFLECT;
        default: return CombatTextEntry::MISS;
    }
}

} // end anonymous namespace

std::string formatCopperAmount(uint32_t amount) {
    uint32_t gold = amount / 10000;
    uint32_t silver = (amount / 100) % 100;
    uint32_t copper = amount % 100;

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

namespace {

std::string displaySpellName(GameHandler& handler, uint32_t spellId) {
    if (spellId == 0) return {};
    const std::string& name = handler.getSpellName(spellId);
    if (!name.empty()) return name;
    return "spell " + std::to_string(spellId);
}

std::string formatSpellNameList(GameHandler& handler,
                                const std::vector<uint32_t>& spellIds,
                                size_t maxShown = 3) {
    if (spellIds.empty()) return {};

    const size_t shownCount = std::min(spellIds.size(), maxShown);
    std::ostringstream oss;
    for (size_t i = 0; i < shownCount; ++i) {
        if (i > 0) {
            if (shownCount == 2) {
                oss << " and ";
            } else if (i == shownCount - 1) {
                oss << ", and ";
            } else {
                oss << ", ";
            }
        }
        oss << displaySpellName(handler, spellIds[i]);
    }

    if (spellIds.size() > shownCount) {
        oss << ", and " << (spellIds.size() - shownCount) << " more";
    }

    return oss.str();
}

bool readCStringAt(const std::vector<uint8_t>& data, size_t start, std::string& out, size_t& nextPos) {
    out.clear();
    if (start >= data.size()) return false;
    size_t i = start;
    while (i < data.size()) {
        uint8_t b = data[i++];
        if (b == 0) {
            nextPos = i;
            return true;
        }
        out.push_back(static_cast<char>(b));
    }
    return false;
}

std::string asciiLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> splitWowPath(const std::string& wowPath) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : wowPath) {
        if (c == '\\' || c == '/') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

int pathCaseScore(const std::string& name) {
    int score = 0;
    for (unsigned char c : name) {
        if (std::islower(c)) score += 2;
        else if (std::isupper(c)) score -= 1;
    }
    return score;
}

std::string resolveCaseInsensitiveDataPath(const std::string& dataRoot, const std::string& wowPath) {
    if (dataRoot.empty() || wowPath.empty()) return std::string();
    std::filesystem::path cur(dataRoot);
    std::error_code ec;
    if (!std::filesystem::exists(cur, ec) || !std::filesystem::is_directory(cur, ec)) {
        return std::string();
    }

    for (const std::string& segment : splitWowPath(wowPath)) {
        std::string wanted = asciiLower(segment);
        std::filesystem::path bestPath;
        int bestScore = std::numeric_limits<int>::min();
        bool found = false;

        for (const auto& entry : std::filesystem::directory_iterator(cur, ec)) {
            if (ec) break;
            std::string name = entry.path().filename().string();
            if (asciiLower(name) != wanted) continue;
            int score = pathCaseScore(name);
            if (!found || score > bestScore) {
                found = true;
                bestScore = score;
                bestPath = entry.path();
            }
        }
        if (!found) return std::string();
        cur = bestPath;
    }

    if (!std::filesystem::exists(cur, ec) || std::filesystem::is_directory(cur, ec)) {
        return std::string();
    }
    return cur.string();
}

std::vector<uint8_t> readFileBinary(const std::string& fsPath) {
    std::ifstream in(fsPath, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) return {};
    return data;
}

bool hmacSha1Matches(const uint8_t seedBytes[4], const std::string& text, const uint8_t expected[20]) {
    uint8_t out[SHA_DIGEST_LENGTH];
    unsigned int outLen = 0;
    HMAC(EVP_sha1(),
         seedBytes, 4,
         reinterpret_cast<const uint8_t*>(text.data()),
         static_cast<int>(text.size()),
         out, &outLen);
    return outLen == SHA_DIGEST_LENGTH && std::memcmp(out, expected, SHA_DIGEST_LENGTH) == 0;
}

const std::unordered_map<std::string, std::array<uint8_t, 20>>& knownDoorHashes() {
    static const std::unordered_map<std::string, std::array<uint8_t, 20>> k = {
        {"world\\lordaeron\\stratholme\\activedoodads\\doors\\nox_door_plague.m2",
         {0xB4,0x45,0x2B,0x6D,0x95,0xC9,0x8B,0x18,0x6A,0x70,0xB0,0x08,0xFA,0x07,0xBB,0xAE,0xF3,0x0D,0xF7,0xA2}},
        {"world\\kalimdor\\onyxiaslair\\doors\\onyxiasgate01.m2",
         {0x75,0x19,0x5E,0x4A,0xED,0xA0,0xBC,0xAF,0x04,0x8C,0xA0,0xE3,0x4D,0x95,0xA7,0x0D,0x4F,0x53,0xC7,0x46}},
        {"world\\generic\\human\\activedoodads\\doors\\deadminedoor02.m2",
         {0x3D,0xFF,0x01,0x1B,0x9A,0xB1,0x34,0xF3,0x7F,0x88,0x50,0x97,0xE6,0x95,0x35,0x1B,0x91,0x95,0x35,0x64}},
        {"world\\kalimdor\\silithus\\activedoodads\\ahnqirajdoor\\ahnqirajdoor02.m2",
         {0xDB,0xD4,0xF4,0x07,0xC4,0x68,0xCC,0x36,0x13,0x4E,0x62,0x1D,0x16,0x01,0x78,0xFD,0xA4,0xD0,0xD2,0x49}},
        {"world\\kalimdor\\diremaul\\activedoodads\\doors\\diremaulsmallinstancedoor.m2",
         {0x0D,0xC8,0xDB,0x46,0xC8,0x55,0x49,0xC0,0xFF,0x1A,0x60,0x0F,0x6C,0x23,0x63,0x57,0xC3,0x05,0x78,0x1A}},
    };
    return k;
}

bool isReadableQuestText(const std::string& s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    bool hasAlpha = false;
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
        if (std::isalpha(c)) hasAlpha = true;
    }
    return hasAlpha;
}

bool isPlaceholderQuestTitle(const std::string& s) {
    return s.rfind("Quest #", 0) == 0;
}

float mergeCooldownSeconds(float current, float incoming) {
    constexpr float kEpsilon = 0.05f;
    if (incoming <= 0.0f) return 0.0f;
    if (current <= 0.0f) return incoming;
    // Cooldowns should normally tick down. If a duplicate/late packet reports a
    // larger value, keep the local remaining time to avoid visible timer resets.
    if (incoming > current + kEpsilon) return current;
    return incoming;
}

bool looksLikeQuestDescriptionText(const std::string& s) {
    int spaces = 0;
    int commas = 0;
    for (unsigned char c : s) {
        if (c == ' ') spaces++;
        if (c == ',') commas++;
    }
    const int words = spaces + 1;
    if (words > 8) return true;
    if (commas > 0 && words > 5) return true;
    if (s.find(". ") != std::string::npos) return true;
    if (s.find(':') != std::string::npos && words > 5) return true;
    return false;
}

bool isStrongQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 6, 72)) return false;
    if (looksLikeQuestDescriptionText(s)) return false;
    unsigned char first = static_cast<unsigned char>(s.front());
    return std::isupper(first) != 0;
}

int scoreQuestTitle(const std::string& s) {
    if (!isReadableQuestText(s, 4, 72)) return -1000;
    if (looksLikeQuestDescriptionText(s)) return -1000;
    int score = 0;
    score += static_cast<int>(std::min<size_t>(s.size(), 32));
    unsigned char first = static_cast<unsigned char>(s.front());
    if (std::isupper(first)) score += 20;
    if (std::islower(first)) score -= 20;
    if (s.find(' ') != std::string::npos) score += 8;
    if (s.find('.') != std::string::npos) score -= 18;
    if (s.find('!') != std::string::npos || s.find('?') != std::string::npos) score -= 6;
    return score;
}

struct QuestQueryTextCandidate {
    std::string title;
    std::string objectives;
    int score = -1000;
};

QuestQueryTextCandidate pickBestQuestQueryTexts(const std::vector<uint8_t>& data, bool classicHint) {
    QuestQueryTextCandidate best;
    if (data.size() <= 9) return best;

    std::vector<size_t> seedOffsets;
    const size_t base = 8;
    const size_t classicOffset = base + 40u * 4u;
    const size_t wotlkOffset = base + 55u * 4u;
    if (classicHint) {
        seedOffsets.push_back(classicOffset);
        seedOffsets.push_back(wotlkOffset);
    } else {
        seedOffsets.push_back(wotlkOffset);
        seedOffsets.push_back(classicOffset);
    }
    for (size_t off : seedOffsets) {
        if (off < data.size()) {
            std::string title;
            size_t next = off;
            if (readCStringAt(data, off, title, next)) {
                QuestQueryTextCandidate c;
                c.title = title;
                c.score = scoreQuestTitle(title) + 20; // Prefer expected struct offsets

                std::string s2;
                size_t n2 = next;
                if (readCStringAt(data, next, s2, n2) && isReadableQuestText(s2, 8, 600)) {
                    c.objectives = s2;
                }
                if (c.score > best.score) best = c;
            }
        }
    }

    // Fallback: scan packet for best printable C-string title candidate.
    for (size_t start = 8; start < data.size(); ++start) {
        std::string title;
        size_t next = start;
        if (!readCStringAt(data, start, title, next)) continue;

        QuestQueryTextCandidate c;
        c.title = title;
        c.score = scoreQuestTitle(title);
        if (c.score < 0) continue;

        std::string s2, s3;
        size_t n2 = next, n3 = next;
        if (readCStringAt(data, next, s2, n2)) {
            if (isReadableQuestText(s2, 8, 600)) c.objectives = s2;
            else if (readCStringAt(data, n2, s3, n3) && isReadableQuestText(s3, 8, 600)) c.objectives = s3;
        }
        if (c.score > best.score) best = c;
    }

    return best;
}

// Parse kill/item objectives from SMSG_QUEST_QUERY_RESPONSE raw data.
// Returns true if the objective block was found and at least one entry read.
//
// Format after the fixed integer header (40*4 Classic or 55*4 WotLK bytes post questId+questMethod):
//   N strings (title, objectives, details, endText; + completedText for WotLK)
//   4x { int32 npcOrGoId, uint32 count }  -- entity (kill/interact) objectives
//   6x { uint32 itemId, uint32 count }    -- item collect objectives
//   4x cstring                            -- per-objective display text
//
// We use the same fixed-offset heuristic as pickBestQuestQueryTexts and then scan past
// the string section to reach the objective data.
struct QuestQueryObjectives {
    struct Kill { int32_t npcOrGoId; uint32_t required; };
    struct Item { uint32_t itemId; uint32_t required; };
    std::array<Kill, 4> kills{};
    std::array<Item, 6> items{};
    bool valid = false;
};

static uint32_t readU32At(const std::vector<uint8_t>& d, size_t pos) {
    return static_cast<uint32_t>(d[pos])
         | (static_cast<uint32_t>(d[pos + 1]) << 8)
         | (static_cast<uint32_t>(d[pos + 2]) << 16)
         | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

// Try to parse objective block starting at `startPos` with `nStrings` strings before it.
// Returns a valid QuestQueryObjectives if the data looks plausible, otherwise invalid.
static QuestQueryObjectives tryParseQuestObjectivesAt(const std::vector<uint8_t>& data,
                                                       size_t startPos, int nStrings) {
    QuestQueryObjectives out;
    size_t pos = startPos;

    // Scan past each string (null-terminated).
    for (int si = 0; si < nStrings; ++si) {
        while (pos < data.size() && data[pos] != 0) ++pos;
        if (pos >= data.size()) return out;  // truncated
        ++pos;  // consume null terminator
    }

    // Read 4 entity objectives: int32 npcOrGoId + uint32 count each.
    for (int i = 0; i < 4; ++i) {
        if (pos + 8 > data.size()) return out;
        out.kills[i].npcOrGoId = static_cast<int32_t>(readU32At(data, pos));  pos += 4;
        out.kills[i].required  = readU32At(data, pos);                         pos += 4;
    }

    // Read 6 item objectives: uint32 itemId + uint32 count each.
    for (int i = 0; i < 6; ++i) {
        if (pos + 8 > data.size()) break;
        out.items[i].itemId   = readU32At(data, pos);  pos += 4;
        out.items[i].required = readU32At(data, pos);  pos += 4;
    }

    out.valid = true;
    return out;
}

QuestQueryObjectives extractQuestQueryObjectives(const std::vector<uint8_t>& data, bool classicHint) {
    if (data.size() < 16) return {};

    // questId(4) + questMethod(4) prefix before the fixed integer header.
    const size_t base = 8;
    // Classic/TBC: 40 fixed uint32 fields + 4 strings before objectives.
    // WotLK:       55 fixed uint32 fields + 5 strings before objectives.
    const size_t classicStart = base + 40u * 4u;
    const size_t wotlkStart   = base + 55u * 4u;

    // Try the expected layout first, then fall back to the other.
    if (classicHint) {
        auto r = tryParseQuestObjectivesAt(data, classicStart, 4);
        if (r.valid) return r;
        return tryParseQuestObjectivesAt(data, wotlkStart, 5);
    } else {
        auto r = tryParseQuestObjectivesAt(data, wotlkStart, 5);
        if (r.valid) return r;
        return tryParseQuestObjectivesAt(data, classicStart, 4);
    }
}

// Parse quest reward fields from SMSG_QUEST_QUERY_RESPONSE fixed header.
// Classic/TBC: 40 fixed fields; WotLK: 55 fixed fields.
struct QuestQueryRewards {
    int32_t  rewardMoney = 0;
    std::array<uint32_t, 4> itemId{};
    std::array<uint32_t, 4> itemCount{};
    std::array<uint32_t, 6> choiceItemId{};
    std::array<uint32_t, 6> choiceItemCount{};
    bool valid = false;
};

static QuestQueryRewards tryParseQuestRewards(const std::vector<uint8_t>& data,
                                               bool classicLayout) {
    const size_t base = 8; // after questId(4) + questMethod(4)
    const size_t fieldCount = classicLayout ? 40u : 55u;
    const size_t headerEnd = base + fieldCount * 4u;
    if (data.size() < headerEnd) return {};

    // Field indices (0-based) for each expansion:
    //   Classic/TBC: rewardMoney=[14], rewardItemId[4]=[20..23], rewardItemCount[4]=[24..27],
    //                rewardChoiceItemId[6]=[28..33], rewardChoiceItemCount[6]=[34..39]
    //   WotLK:       rewardMoney=[17], rewardItemId[4]=[30..33], rewardItemCount[4]=[34..37],
    //                rewardChoiceItemId[6]=[38..43], rewardChoiceItemCount[6]=[44..49]
    const size_t moneyField     = classicLayout ? 14u : 17u;
    const size_t itemIdField    = classicLayout ? 20u : 30u;
    const size_t itemCountField = classicLayout ? 24u : 34u;
    const size_t choiceIdField  = classicLayout ? 28u : 38u;
    const size_t choiceCntField = classicLayout ? 34u : 44u;

    QuestQueryRewards out;
    out.rewardMoney = static_cast<int32_t>(readU32At(data, base + moneyField * 4u));
    for (size_t i = 0; i < 4; ++i) {
        out.itemId[i]    = readU32At(data, base + (itemIdField    + i) * 4u);
        out.itemCount[i] = readU32At(data, base + (itemCountField + i) * 4u);
    }
    for (size_t i = 0; i < 6; ++i) {
        out.choiceItemId[i]    = readU32At(data, base + (choiceIdField  + i) * 4u);
        out.choiceItemCount[i] = readU32At(data, base + (choiceCntField + i) * 4u);
    }
    out.valid = true;
    return out;
}

} // namespace

template<typename ManagerGetter, typename Callback>
void GameHandler::withSoundManager(ManagerGetter getter, Callback cb) {
    if (auto* renderer = core::Application::getInstance().getRenderer()) {
        if (auto* mgr = (renderer->*getter)()) cb(mgr);
    }
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

GameHandler::GameHandler() {
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
    chatHandler_      = std::make_unique<ChatHandler>(*this);
    movementHandler_  = std::make_unique<MovementHandler>(*this);
    combatHandler_    = std::make_unique<CombatHandler>(*this);
    spellHandler_     = std::make_unique<SpellHandler>(*this);
    inventoryHandler_ = std::make_unique<InventoryHandler>(*this);
    socialHandler_    = std::make_unique<SocialHandler>(*this);
    questHandler_     = std::make_unique<QuestHandler>(*this);
    wardenHandler_    = std::make_unique<WardenHandler>(*this);
    wardenHandler_->initModuleManager();

    // Default spells always available
    knownSpells.insert(6603);  // Attack
    knownSpells.insert(8690);  // Hearthstone

    // Default action bar layout
    actionBar[0].type = ActionBarSlot::SPELL;
    actionBar[0].id = 6603;   // Attack in slot 1
    actionBar[11].type = ActionBarSlot::SPELL;
    actionBar[11].id = 8690;  // Hearthstone in slot 12

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
    playerNameCache.clear();
    pendingNameQueries.clear();
    guildNameCache_.clear();
    pendingGuildNameQueries_.clear();
    friendGuids_.clear();
    contacts_.clear();
    transportAttachments_.clear();
    serverUpdatedTransportGuids_.clear();
    // Clear in-flight query sets so reconnect can re-issue queries for any
    // entries whose responses were lost during the disconnect.
    pendingCreatureQueries.clear();
    pendingGameObjectQueries_.clear();
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
    pendingIncomingPackets_.clear();
    pendingUpdateObjectWork_.clear();
    // Fire despawn callbacks so the renderer releases M2/character model resources.
    for (const auto& [guid, entity] : entityManager.getEntities()) {
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
    if (spellHandler_) spellHandler_->unitCastStates_.clear();
    if (spellHandler_) spellHandler_->unitAurasCache_.clear();
    if (combatHandler_) combatHandler_->clearCombatText();
    entityManager.clear();
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
    auto* am = core::Application::getInstance().getAssetManager();
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
        movementHandler_->monsterMovePacketsThisTick_ = 0;
        movementHandler_->monsterMovePacketsDroppedThisTick_ = 0;
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
        if (silenceMs > 10000 && !rxSilenceLogged_) {
            rxSilenceLogged_ = true;
            LOG_WARNING("RX SILENCE: No packets from server for ", silenceMs, "ms — possible soft disconnect");
        }
        if (silenceMs > 15000 && silenceMs < 15500) {
            LOG_WARNING("RX SILENCE: 15s — server appears to have stopped sending");
        }
    }

    // Detect server-side disconnect (socket closed during update)
    if (socket && !socket->isConnected() && state != WorldState::DISCONNECTED) {
        if (pendingIncomingPackets_.empty() && pendingUpdateObjectWork_.empty()) {
            LOG_WARNING("Server closed connection in state: ", worldStateName(state));
            disconnect();
            return;
        }
        LOG_DEBUG("World socket closed with ", pendingIncomingPackets_.size(),
                  " queued packet(s) and ", pendingUpdateObjectWork_.size(),
                  " update-object batch(es) pending dispatch");
    }

    // Post-gate visibility: determine whether server goes silent or closes after Warden requirement.
    if (wardenGateSeen_ && socket && socket->isConnected()) {
        wardenGateElapsed_ += deltaTime;
        if (wardenGateElapsed_ >= wardenGateNextStatusLog_) {
            LOG_DEBUG("Warden gate status: elapsed=", wardenGateElapsed_,
                     "s connected=", socket->isConnected() ? "yes" : "no",
                     " packetsAfterGate=", wardenPacketsAfterGate_);
            wardenGateNextStatusLog_ += 30.0f;
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
    auto playerEntity = entityManager.getEntity(playerGuid);
    auto unit = std::dynamic_pointer_cast<Unit>(playerEntity);
    if (unit &&
        (unit->getUnitFlags() & 0x00000100) == 0 &&
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
    auto playerEntity = entityManager.getEntity(playerGuid);
    auto playerUnit = std::dynamic_pointer_cast<Unit>(playerEntity);
    if (playerUnit) {
        serverStillTaxi = (playerUnit->getUnitFlags() & 0x00000100) != 0;
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
    auto playerEntity = entityManager.getEntity(playerGuid);
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
    auto playerEntity = entityManager.getEntity(playerGuid);
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
const float updateRadiusSq = 150.0f * 150.0f;  // 150 unit radius
auto playerEntity = entityManager.getEntity(playerGuid);
glm::vec3 playerPos = playerEntity ? glm::vec3(playerEntity->getX(), playerEntity->getY(), playerEntity->getZ()) : glm::vec3(0.0f);

for (auto& [guid, entity] : entityManager.getEntities()) {
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
                if (spellHandler_ && spellHandler_->casting_ && spellHandler_->currentCastSpellId_ != 0) {
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
            for (const auto& [guid, entity] : entityManager.getEntities()) {
                if (!entity || entity->getType() != ObjectType::PLAYER) continue;
                if (guid == playerGuid) continue;
                auto player = std::static_pointer_cast<Player>(entity);
                if (!player->getName().empty()) continue;
                if (playerNameCache.count(guid)) continue;
                if (pendingNameQueries.count(guid)) continue;
                // Player entity exists with empty name and no pending query — resend.
                LOG_DEBUG("Name resync: re-querying guid=0x", std::hex, guid, std::dec);
                pendingNameQueries.insert(guid);
                auto pkt = NameQueryPacket::build(guid);
                socket->send(pkt);
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
                auto* renderer = core::Application::getInstance().getRenderer();
                if (renderer) {
                    if (auto* sfx = renderer->getUiSoundManager()) {
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
        if (guid != 0 && guid != playerGuid && entityManager.hasEntity(guid)) {
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
    if (targetGuid != 0 && !entityManager.hasEntity(targetGuid)) {
        clearTarget();
    }

    // Update auto-follow: refresh render position or cancel if entity disappeared
    if (followTargetGuid_ != 0) {
        auto followEnt = entityManager.getEntity(followTargetGuid_);
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
        if (movementHandler_) movementHandler_->timeSinceLastMoveHeartbeat_ += deltaTime;

        const float currentPingInterval =
            (isPreWotlk()) ? 10.0f : pingInterval;
        if (timeSinceLastPing >= currentPingInterval) {
            if (socket) {
                sendPing();
            }
            timeSinceLastPing = 0.0f;
        }

        const bool classicLikeCombatSync =
            (combatHandler_ && combatHandler_->hasAutoAttackIntent()) && (isPreWotlk());
        const uint32_t locomotionFlags =
            static_cast<uint32_t>(MovementFlags::FORWARD) |
            static_cast<uint32_t>(MovementFlags::BACKWARD) |
            static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
            static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
            static_cast<uint32_t>(MovementFlags::TURN_LEFT) |
            static_cast<uint32_t>(MovementFlags::TURN_RIGHT) |
            static_cast<uint32_t>(MovementFlags::ASCENDING) |
            static_cast<uint32_t>(MovementFlags::FALLING) |
            static_cast<uint32_t>(MovementFlags::FALLINGFAR);
        const bool classicLikeStationaryCombatSync =
            classicLikeCombatSync &&
            !onTaxiFlight_ &&
            !taxiActivatePending_ &&
            !taxiClientActive_ &&
            (movementInfo.flags & locomotionFlags) == 0;
        float heartbeatInterval = (onTaxiFlight_ || taxiActivatePending_ || taxiClientActive_)
                                      ? 0.25f
                                      : (classicLikeStationaryCombatSync ? 0.75f
                                                                         : (classicLikeCombatSync ? 0.20f
                                                                                                  : moveHeartbeatInterval_));
        if (movementHandler_ && movementHandler_->timeSinceLastMoveHeartbeat_ >= heartbeatInterval) {
            sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
            movementHandler_->timeSinceLastMoveHeartbeat_ = 0.0f;
        }

        // Check area triggers (instance portals, tavern rests, etc.)
        areaTriggerCheckTimer_ += deltaTime;
        if (areaTriggerCheckTimer_ >= 0.25f) {
            areaTriggerCheckTimer_ = 0.0f;
            checkAreaTriggers();
        }

        // Update cast timer (Phase 3)
        if (pendingGameObjectInteractGuid_ != 0 &&
            combatHandler_ && (combatHandler_->isAutoAttacking() || combatHandler_->hasAutoAttackIntent())) {
            pendingGameObjectInteractGuid_ = 0;
            if (spellHandler_) { spellHandler_->casting_ = false; spellHandler_->castIsChannel_ = false; spellHandler_->currentCastSpellId_ = 0; spellHandler_->castTimeRemaining_ = 0.0f; }
            addUIError("Interrupted.");
            addSystemChatMessage("Interrupted.");
        }
        if (spellHandler_ && spellHandler_->casting_ && spellHandler_->castTimeRemaining_ > 0.0f) {
            spellHandler_->castTimeRemaining_ -= deltaTime;
            if (spellHandler_->castTimeRemaining_ <= 0.0f) {
                if (pendingGameObjectInteractGuid_ != 0) {
                    uint64_t interactGuid = pendingGameObjectInteractGuid_;
                    pendingGameObjectInteractGuid_ = 0;
                    performGameObjectInteractionNow(interactGuid);
                }
                spellHandler_->casting_ = false;
                spellHandler_->castIsChannel_ = false;
                spellHandler_->currentCastSpellId_ = 0;
                spellHandler_->castTimeRemaining_ = 0.0f;
            }
        }

        // Tick down all tracked unit cast bars (in SpellHandler)
        if (spellHandler_) {
            for (auto it = spellHandler_->unitCastStates_.begin(); it != spellHandler_->unitCastStates_.end(); ) {
                auto& s = it->second;
                if (s.casting && s.timeRemaining > 0.0f) {
                    s.timeRemaining -= deltaTime;
                    if (s.timeRemaining <= 0.0f) {
                        it = spellHandler_->unitCastStates_.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        // Update spell cooldowns (in SpellHandler)
        if (spellHandler_) {
            for (auto it = spellHandler_->spellCooldowns_.begin(); it != spellHandler_->spellCooldowns_.end(); ) {
                it->second -= deltaTime;
                if (it->second <= 0.0f) {
                    it = spellHandler_->spellCooldowns_.erase(it);
                } else {
                    ++it;
                }
            }
        }

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
            auto npc = entityManager.getEntity(npcGuid);
            if (!npc) return;
            float dx = movementInfo.x - npc->getX();
            float dy = movementInfo.y - npc->getY();
            if (std::sqrt(dx * dx + dy * dy) > 15.0f) {
                closeFn();
                LOG_INFO(label, " closed: walked too far from NPC");
            }
        };
        closeIfTooFar(vendorWindowOpen, currentVendorItems.vendorGuid, [this]{ closeVendor(); }, "Vendor");
        closeIfTooFar(gossipWindowOpen, currentGossip.npcGuid, [this]{ closeGossip(); }, "Gossip");
        closeIfTooFar(taxiWindowOpen_, taxiNpcGuid_, [this]{ closeTaxi(); }, "Taxi window");
        closeIfTooFar(trainerWindowOpen_, currentTrainerList_.trainerGuid, [this]{ closeTrainer(); }, "Trainer");

        updateEntityInterpolation(deltaTime);

    }
}

void GameHandler::registerOpcodeHandlers() {
    // -----------------------------------------------------------------------
    // Auth / session / pre-world handshake
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_AUTH_CHALLENGE] = [this](network::Packet& packet) {
        if (state == WorldState::CONNECTED)
            handleAuthChallenge(packet);
        else
            LOG_WARNING("Unexpected SMSG_AUTH_CHALLENGE in state: ", worldStateName(state));
    };
    dispatchTable_[Opcode::SMSG_AUTH_RESPONSE] = [this](network::Packet& packet) {
        if (state == WorldState::AUTH_SENT)
            handleAuthResponse(packet);
        else
            LOG_WARNING("Unexpected SMSG_AUTH_RESPONSE in state: ", worldStateName(state));
    };
    dispatchTable_[Opcode::SMSG_CHAR_CREATE] = [this](network::Packet& packet) {
        handleCharCreateResponse(packet);
    };
    dispatchTable_[Opcode::SMSG_CHAR_DELETE] = [this](network::Packet& packet) {
        uint8_t result = packet.readUInt8();
        lastCharDeleteResult_ = result;
        bool success = (result == 0x00 || result == 0x47);
        LOG_INFO("SMSG_CHAR_DELETE result: ", static_cast<int>(result), success ? " (success)" : " (failed)");
        requestCharacterList();
        if (charDeleteCallback_) charDeleteCallback_(success);
    };
    dispatchTable_[Opcode::SMSG_CHAR_ENUM] = [this](network::Packet& packet) {
        if (state == WorldState::CHAR_LIST_REQUESTED)
            handleCharEnum(packet);
        else
            LOG_WARNING("Unexpected SMSG_CHAR_ENUM in state: ", worldStateName(state));
    };
    registerHandler(Opcode::SMSG_CHARACTER_LOGIN_FAILED, &GameHandler::handleCharLoginFailed);
    dispatchTable_[Opcode::SMSG_LOGIN_VERIFY_WORLD] = [this](network::Packet& packet) {
        if (state == WorldState::ENTERING_WORLD || state == WorldState::IN_WORLD)
            handleLoginVerifyWorld(packet);
        else
            LOG_WARNING("Unexpected SMSG_LOGIN_VERIFY_WORLD in state: ", worldStateName(state));
    };
    registerHandler(Opcode::SMSG_LOGIN_SETTIMESPEED, &GameHandler::handleLoginSetTimeSpeed);
    registerHandler(Opcode::SMSG_CLIENTCACHE_VERSION, &GameHandler::handleClientCacheVersion);
    registerHandler(Opcode::SMSG_TUTORIAL_FLAGS, &GameHandler::handleTutorialFlags);
    registerHandler(Opcode::SMSG_ACCOUNT_DATA_TIMES, &GameHandler::handleAccountDataTimes);
    registerHandler(Opcode::SMSG_MOTD, &GameHandler::handleMotd);
    registerHandler(Opcode::SMSG_NOTIFICATION, &GameHandler::handleNotification);
    registerHandler(Opcode::SMSG_PONG, &GameHandler::handlePong);

    // -----------------------------------------------------------------------
    // World object updates
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_UPDATE_OBJECT] = [this](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_UPDATE_OBJECT, state=", static_cast<int>(state), " size=", packet.getSize());
        if (state == WorldState::IN_WORLD) handleUpdateObject(packet);
    };
    dispatchTable_[Opcode::SMSG_COMPRESSED_UPDATE_OBJECT] = [this](network::Packet& packet) {
        LOG_DEBUG("Received SMSG_COMPRESSED_UPDATE_OBJECT, state=", static_cast<int>(state), " size=", packet.getSize());
        if (state == WorldState::IN_WORLD) handleCompressedUpdateObject(packet);
    };
    dispatchTable_[Opcode::SMSG_DESTROY_OBJECT] = [this](network::Packet& packet) {
        if (state == WorldState::IN_WORLD) handleDestroyObject(packet);
    };

    // -----------------------------------------------------------------------
    // Item push / logout / entity queries
    // -----------------------------------------------------------------------
    registerHandler(Opcode::SMSG_NAME_QUERY_RESPONSE, &GameHandler::handleNameQueryResponse);
    registerHandler(Opcode::SMSG_CREATURE_QUERY_RESPONSE, &GameHandler::handleCreatureQueryResponse);
    registerSkipHandler(Opcode::SMSG_ADDON_INFO);
    registerSkipHandler(Opcode::SMSG_EXPECTED_SPAM_RECORDS);

    // -----------------------------------------------------------------------
    // XP / exploration
    // -----------------------------------------------------------------------
    registerHandler(Opcode::SMSG_LOG_XPGAIN, &GameHandler::handleXpGain);
    dispatchTable_[Opcode::SMSG_EXPLORATION_EXPERIENCE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t areaId   = packet.readUInt32();
            uint32_t xpGained = packet.readUInt32();
            if (xpGained > 0) {
                std::string areaName = getAreaName(areaId);
                std::string msg;
                if (!areaName.empty()) {
                    msg = "Discovered " + areaName + "! Gained " + std::to_string(xpGained) + " experience.";
                } else {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "Discovered new area! Gained %u experience.", xpGained);
                    msg = buf;
                }
                addSystemChatMessage(msg);
                addCombatText(CombatTextEntry::XP_GAIN, static_cast<int32_t>(xpGained), 0, true);
                if (areaDiscoveryCallback_) areaDiscoveryCallback_(areaName, xpGained);
                                    fireAddonEvent("CHAT_MSG_COMBAT_XP_GAIN", {msg, std::to_string(xpGained)});
            }
        }
    };

    registerSkipHandler(Opcode::SMSG_PET_NAME_QUERY_RESPONSE);

    // -----------------------------------------------------------------------
    // Entity delta updates: health / power / world state / combo / timers / PvP
    // (SMSG_HEALTH_UPDATE, SMSG_POWER_UPDATE, SMSG_UPDATE_COMBO_POINTS,
    //  SMSG_PVP_CREDIT, SMSG_PROCRESIST → moved to CombatHandler)
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_UPDATE_WORLD_STATE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint32_t field = packet.readUInt32();
        uint32_t value = packet.readUInt32();
        worldStates_[field] = value;
        LOG_DEBUG("SMSG_UPDATE_WORLD_STATE: field=", field, " value=", value);
        fireAddonEvent("UPDATE_WORLD_STATES", {});
    };
    dispatchTable_[Opcode::SMSG_WORLD_STATE_UI_TIMER_UPDATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t serverTime = packet.readUInt32();
            LOG_DEBUG("SMSG_WORLD_STATE_UI_TIMER_UPDATE: serverTime=", serverTime);
        }
    };
    dispatchTable_[Opcode::SMSG_START_MIRROR_TIMER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(21)) return;
        uint32_t type  = packet.readUInt32();
        int32_t  value = static_cast<int32_t>(packet.readUInt32());
        int32_t  maxV  = static_cast<int32_t>(packet.readUInt32());
        int32_t  scale = static_cast<int32_t>(packet.readUInt32());
        /*uint32_t tracker =*/ packet.readUInt32();
        uint8_t  paused = packet.readUInt8();
        if (type < 3) {
            mirrorTimers_[type].value    = value;
            mirrorTimers_[type].maxValue = maxV;
            mirrorTimers_[type].scale    = scale;
            mirrorTimers_[type].paused   = (paused != 0);
            mirrorTimers_[type].active   = true;
                            fireAddonEvent("MIRROR_TIMER_START", {
                    std::to_string(type), std::to_string(value),
                    std::to_string(maxV), std::to_string(scale),
                    paused ? "1" : "0"});
        }
    };
    dispatchTable_[Opcode::SMSG_STOP_MIRROR_TIMER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t type = packet.readUInt32();
        if (type < 3) {
            mirrorTimers_[type].active = false;
            mirrorTimers_[type].value  = 0;
            fireAddonEvent("MIRROR_TIMER_STOP", {std::to_string(type)});
        }
    };
    dispatchTable_[Opcode::SMSG_PAUSE_MIRROR_TIMER] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) return;
        uint32_t type   = packet.readUInt32();
        uint8_t  paused = packet.readUInt8();
        if (type < 3) {
            mirrorTimers_[type].paused = (paused != 0);
            fireAddonEvent("MIRROR_TIMER_PAUSE", {paused ? "1" : "0"});
        }
    };

    // -----------------------------------------------------------------------
    // Cast result / spell proc
    // (SMSG_CAST_RESULT, SMSG_SPELL_FAILED_OTHER → moved to SpellHandler)
    // (SMSG_PROCRESIST → moved to CombatHandler)
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Pet stable
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::MSG_LIST_STABLED_PETS] = [this](network::Packet& packet) {
        if (state == WorldState::IN_WORLD) handleListStabledPets(packet);
    };
    dispatchTable_[Opcode::SMSG_STABLE_RESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint8_t result = packet.readUInt8();
        const char* msg = nullptr;
        switch (result) {
            case 0x01: msg = "Pet stored in stable."; break;
            case 0x06: msg = "Pet retrieved from stable."; break;
            case 0x07: msg = "Stable slot purchased."; break;
            case 0x08: msg = "Stable list updated."; break;
            case 0x09: msg = "Stable failed: not enough money or other error."; addUIError(msg); break;
            default: break;
        }
        if (msg) addSystemChatMessage(msg);
        LOG_INFO("SMSG_STABLE_RESULT: result=", static_cast<int>(result));
        if (stableWindowOpen_ && stableMasterGuid_ != 0 && socket && result <= 0x08) {
            auto refreshPkt = ListStabledPetsPacket::build(stableMasterGuid_);
            socket->send(refreshPkt);
        }
    };

    // -----------------------------------------------------------------------
    // Titles / achievements / character services
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_TITLE_EARNED] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint32_t titleBit = packet.readUInt32();
        uint32_t isLost   = packet.readUInt32();
        loadTitleNameCache();
        std::string titleStr;
        auto tit = titleNameCache_.find(titleBit);
        if (tit != titleNameCache_.end() && !tit->second.empty()) {
            const auto& ln = lookupName(playerGuid);
            const std::string& pName = ln.empty() ? std::string("you") : ln;
            const std::string& fmt = tit->second;
            size_t pos = fmt.find("%s");
            if (pos != std::string::npos)
                titleStr = fmt.substr(0, pos) + pName + fmt.substr(pos + 2);
            else
                titleStr = fmt;
        }
        std::string msg;
        if (!titleStr.empty()) {
            msg = isLost ? ("Title removed: " + titleStr + ".") : ("Title earned: " + titleStr + "!");
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), isLost ? "Title removed (bit %u)." : "Title earned (bit %u)!", titleBit);
            msg = buf;
        }
        if (isLost) knownTitleBits_.erase(titleBit);
        else        knownTitleBits_.insert(titleBit);
        addSystemChatMessage(msg);
        LOG_INFO("SMSG_TITLE_EARNED: bit=", titleBit, " lost=", isLost, " title='", titleStr, "'");
    };
    dispatchTable_[Opcode::SMSG_LEARNED_DANCE_MOVES] = [this](network::Packet& packet) {
        LOG_DEBUG("SMSG_LEARNED_DANCE_MOVES: ignored (size=", packet.getSize(), ")");
    };
    dispatchTable_[Opcode::SMSG_CHAR_RENAME] = [this](network::Packet& packet) {
        if (packet.hasRemaining(13)) {
            uint32_t result = packet.readUInt32();
            /*uint64_t guid =*/ packet.readUInt64();
            std::string newName = packet.readString();
            if (result == 0) {
                addSystemChatMessage("Character name changed to: " + newName);
            } else {
                static const char* kRenameErrors[] = {
                    nullptr, "Name already in use.", "Name too short.", "Name too long.",
                    "Name contains invalid characters.", "Name contains a profanity.",
                    "Name is reserved.", "Character name does not meet requirements.",
                };
                const char* errMsg = (result < 8) ? kRenameErrors[result] : nullptr;
                std::string renameErr = errMsg ? std::string("Rename failed: ") + errMsg : "Character rename failed.";
                addUIError(renameErr); addSystemChatMessage(renameErr);
            }
            LOG_INFO("SMSG_CHAR_RENAME: result=", result, " newName=", newName);
        }
    };

    // -----------------------------------------------------------------------
    // Bind / heartstone / phase / barber / corpse
    // -----------------------------------------------------------------------
    dispatchTable_[Opcode::SMSG_PLAYERBOUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        /*uint64_t binderGuid =*/ packet.readUInt64();
        uint32_t mapId  = packet.readUInt32();
        uint32_t zoneId = packet.readUInt32();
        homeBindMapId_  = mapId;
        homeBindZoneId_ = zoneId;
        std::string pbMsg = "Your home location has been set";
        std::string zoneName = getAreaName(zoneId);
        if (!zoneName.empty()) pbMsg += " to " + zoneName;
        pbMsg += '.';
        addSystemChatMessage(pbMsg);
    };
    registerSkipHandler(Opcode::SMSG_BINDER_CONFIRM);
    registerSkipHandler(Opcode::SMSG_SET_PHASE_SHIFT);
    dispatchTable_[Opcode::SMSG_TOGGLE_XP_GAIN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint8_t enabled = packet.readUInt8();
        addSystemChatMessage(enabled ? "XP gain enabled." : "XP gain disabled.");
    };
    dispatchTable_[Opcode::SMSG_BINDZONEREPLY] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            if (result == 0) addSystemChatMessage("Your home is now set to this location.");
            else { addUIError("You are too far from the innkeeper."); addSystemChatMessage("You are too far from the innkeeper."); }
        }
    };
    dispatchTable_[Opcode::SMSG_CHANGEPLAYER_DIFFICULTY_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            if (result == 0) {
                addSystemChatMessage("Difficulty changed.");
            } else {
                static const char* reasons[] = {
                    "", "Error", "Too many members", "Already in dungeon",
                    "You are in a battleground", "Raid not allowed in heroic",
                    "You must be in a raid group", "Player not in group"
                };
                const char* msg = (result < 8) ? reasons[result] : "Difficulty change failed.";
                addUIError(std::string("Cannot change difficulty: ") + msg);
                addSystemChatMessage(std::string("Cannot change difficulty: ") + msg);
            }
        }
    };
    dispatchTable_[Opcode::SMSG_CORPSE_NOT_IN_INSTANCE] = [this](network::Packet& /*packet*/) {
        addUIError("Your corpse is outside this instance.");
        addSystemChatMessage("Your corpse is outside this instance. Release spirit to retrieve it.");
    };
    dispatchTable_[Opcode::SMSG_CROSSED_INEBRIATION_THRESHOLD] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            uint64_t guid      = packet.readUInt64();
            uint32_t threshold = packet.readUInt32();
            if (guid == playerGuid && threshold > 0) addSystemChatMessage("You feel rather drunk.");
            LOG_DEBUG("SMSG_CROSSED_INEBRIATION_THRESHOLD: guid=0x", std::hex, guid, std::dec, " threshold=", threshold);
        }
    };
    dispatchTable_[Opcode::SMSG_CLEAR_FAR_SIGHT_IMMEDIATE] = [this](network::Packet& /*packet*/) {
        LOG_DEBUG("SMSG_CLEAR_FAR_SIGHT_IMMEDIATE");
    };
    registerSkipHandler(Opcode::SMSG_COMBAT_EVENT_FAILED);
    dispatchTable_[Opcode::SMSG_FORCE_ANIM] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint64_t animGuid = packet.readPackedGuid();
            if (packet.hasRemaining(4)) {
                uint32_t animId = packet.readUInt32();
                if (emoteAnimCallback_) emoteAnimCallback_(animGuid, animId);
            }
        }
    };
    // Consume silently — opcodes we receive but don't need to act on
    for (auto op : {
        Opcode::SMSG_GAMEOBJECT_DESPAWN_ANIM, Opcode::SMSG_GAMEOBJECT_RESET_STATE,
        Opcode::SMSG_FLIGHT_SPLINE_SYNC, Opcode::SMSG_FORCE_DISPLAY_UPDATE,
        Opcode::SMSG_FORCE_SEND_QUEUED_PACKETS, Opcode::SMSG_FORCE_SET_VEHICLE_REC_ID,
        Opcode::SMSG_CORPSE_MAP_POSITION_QUERY_RESPONSE, Opcode::SMSG_DAMAGE_CALC_LOG,
        Opcode::SMSG_DYNAMIC_DROP_ROLL_RESULT, Opcode::SMSG_DESTRUCTIBLE_BUILDING_DAMAGE,
    }) { registerSkipHandler(op); }
    dispatchTable_[Opcode::SMSG_FORCED_DEATH_UPDATE] = [this](network::Packet& packet) {
        playerDead_ = true;
        if (ghostStateCallback_) ghostStateCallback_(false);
        fireAddonEvent("PLAYER_DEAD", {});
        addSystemChatMessage("You have been killed.");
        LOG_INFO("SMSG_FORCED_DEATH_UPDATE: player force-killed");
        packet.skipAll();
    };
    // SMSG_DEFENSE_MESSAGE — moved to ChatHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_CORPSE_RECLAIM_DELAY] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t delayMs = packet.readUInt32();
            auto nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            corpseReclaimAvailableMs_ = nowMs + delayMs;
            LOG_INFO("SMSG_CORPSE_RECLAIM_DELAY: ", delayMs, "ms");
        }
    };
    dispatchTable_[Opcode::SMSG_DEATH_RELEASE_LOC] = [this](network::Packet& packet) {
        if (packet.hasRemaining(16)) {
            uint32_t relMapId = packet.readUInt32();
            float relX = packet.readFloat(), relY = packet.readFloat(), relZ = packet.readFloat();
            LOG_INFO("SMSG_DEATH_RELEASE_LOC (graveyard spawn): map=", relMapId, " x=", relX, " y=", relY, " z=", relZ);
        }
    };
    dispatchTable_[Opcode::SMSG_ENABLE_BARBER_SHOP] = [this](network::Packet& /*packet*/) {
        LOG_INFO("SMSG_ENABLE_BARBER_SHOP: barber shop available");
        barberShopOpen_ = true;
        fireAddonEvent("BARBER_SHOP_OPEN", {});
    };

    // ---- Batch 3: Corpse/gametime, combat clearing, mount, loot notify,
    //                movement/speed/flags, attack, spells, group ----

    dispatchTable_[Opcode::MSG_CORPSE_QUERY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(1)) return;
        uint8_t found = packet.readUInt8();
        if (found && packet.hasRemaining(20)) {
            /*uint32_t mapId =*/ packet.readUInt32();
            float cx = packet.readFloat();
            float cy = packet.readFloat();
            float cz = packet.readFloat();
            uint32_t corpseMapId = packet.readUInt32();
            corpseX_ = cx;
            corpseY_ = cy;
            corpseZ_ = cz;
            corpseMapId_ = corpseMapId;
            LOG_INFO("MSG_CORPSE_QUERY: corpse at (", cx, ",", cy, ",", cz, ") map=", corpseMapId);
        }
    };
    dispatchTable_[Opcode::SMSG_FEIGN_DEATH_RESISTED] = [this](network::Packet& /*packet*/) {
        addUIError("Your Feign Death was resisted.");
        addSystemChatMessage("Your Feign Death attempt was resisted.");
    };
    dispatchTable_[Opcode::SMSG_CHANNEL_MEMBER_COUNT] = [this](network::Packet& packet) {
        std::string chanName = packet.readString();
        if (packet.hasRemaining(5)) {
            /*uint8_t flags =*/ packet.readUInt8();
            uint32_t count = packet.readUInt32();
            LOG_DEBUG("SMSG_CHANNEL_MEMBER_COUNT: channel=", chanName, " members=", count);
        }
    };
    for (auto op : { Opcode::SMSG_GAMETIME_SET, Opcode::SMSG_GAMETIME_UPDATE }) {
        dispatchTable_[op] = [this](network::Packet& packet) {
            if (packet.hasRemaining(4)) {
                uint32_t gameTimePacked = packet.readUInt32();
                gameTime_ = static_cast<float>(gameTimePacked);
            }
            packet.skipAll();
        };
    }
    dispatchTable_[Opcode::SMSG_GAMESPEED_SET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t gameTimePacked = packet.readUInt32();
            float timeSpeed = packet.readFloat();
            gameTime_ = static_cast<float>(gameTimePacked);
            timeSpeed_ = timeSpeed;
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_GAMETIMEBIAS_SET] = [this](network::Packet& packet) {
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_ACHIEVEMENT_DELETED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t achId = packet.readUInt32();
            earnedAchievements_.erase(achId);
            achievementDates_.erase(achId);
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_CRITERIA_DELETED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t critId = packet.readUInt32();
            criteriaProgress_.erase(critId);
        }
        packet.skipAll();
    };

    // Combat clearing
    dispatchTable_[Opcode::SMSG_BREAK_TARGET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t bGuid = packet.readUInt64();
            if (bGuid == targetGuid) targetGuid = 0;
        }
    };
    dispatchTable_[Opcode::SMSG_CLEAR_TARGET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t cGuid = packet.readUInt64();
            if (cGuid == 0 || cGuid == targetGuid) targetGuid = 0;
        }
    };

    // Mount/dismount
    dispatchTable_[Opcode::SMSG_DISMOUNT] = [this](network::Packet& /*packet*/) {
        currentMountDisplayId_ = 0;
        if (mountCallback_) mountCallback_(0);
    };
    dispatchTable_[Opcode::SMSG_MOUNTRESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t result = packet.readUInt32();
        if (result != 4) {
            const char* msgs[] = { "Cannot mount here.", "Invalid mount spell.",
                                   "Too far away to mount.", "Already mounted." };
            std::string mountErr = result < 4 ? msgs[result] : "Cannot mount.";
            addUIError(mountErr);
            addSystemChatMessage(mountErr);
        }
    };
    dispatchTable_[Opcode::SMSG_DISMOUNTRESULT] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t result = packet.readUInt32();
        if (result != 0) {
            addUIError("Cannot dismount here.");
            addSystemChatMessage("Cannot dismount here.");
        }
    };

    // Camera shake
    dispatchTable_[Opcode::SMSG_CAMERA_SHAKE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint32_t shakeId   = packet.readUInt32();
            uint32_t shakeType = packet.readUInt32();
            (void)shakeType;
            float magnitude = (shakeId < 50) ? 0.04f : 0.08f;
            if (cameraShakeCallback_)
                cameraShakeCallback_(magnitude, 18.0f, 0.5f);
        }
    };

    // (SMSG_PLAY_SPELL_VISUAL, SMSG_CLEAR_COOLDOWN, SMSG_MODIFY_COOLDOWN → moved to SpellHandler)

    // ---- Batch 4: Ready check, duels, guild, loot/gossip/vendor, factions, spell mods ----

    // Guild
    registerHandler(Opcode::SMSG_PET_SPELLS, &GameHandler::handlePetSpells);

    // Loot/gossip/vendor delegates
    registerHandler(Opcode::SMSG_SUMMON_REQUEST, &GameHandler::handleSummonRequest);
    dispatchTable_[Opcode::SMSG_SUMMON_CANCEL] = [this](network::Packet& /*packet*/) {
        pendingSummonRequest_ = false;
        addSystemChatMessage("Summon cancelled.");
    };

    // Bind point
    dispatchTable_[Opcode::SMSG_BINDPOINTUPDATE] = [this](network::Packet& packet) {
        BindPointUpdateData data;
        if (BindPointUpdateParser::parse(packet, data)) {
            glm::vec3 canonical = core::coords::serverToCanonical(
                glm::vec3(data.x, data.y, data.z));
            bool wasSet = hasHomeBind_;
            hasHomeBind_ = true;
            homeBindMapId_ = data.mapId;
            homeBindZoneId_ = data.zoneId;
            homeBindPos_ = canonical;
            if (bindPointCallback_)
                bindPointCallback_(data.mapId, canonical.x, canonical.y, canonical.z);
            if (wasSet) {
                std::string bindMsg = "Your home has been set";
                std::string zoneName = getAreaName(data.zoneId);
                if (!zoneName.empty()) bindMsg += " to " + zoneName;
                bindMsg += '.';
                addSystemChatMessage(bindMsg);
            }
        }
    };

    // Spirit healer / resurrect
    dispatchTable_[Opcode::SMSG_SPIRIT_HEALER_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint64_t npcGuid = packet.readUInt64();
        if (npcGuid) {
            resurrectCasterGuid_ = npcGuid;
            resurrectCasterName_ = "";
            resurrectIsSpiritHealer_ = true;
            resurrectRequestPending_ = true;
        }
    };
    dispatchTable_[Opcode::SMSG_RESURRECT_REQUEST] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        uint64_t casterGuid = packet.readUInt64();
        std::string casterName;
        if (packet.hasData())
            casterName = packet.readString();
        if (casterGuid) {
            resurrectCasterGuid_ = casterGuid;
            resurrectIsSpiritHealer_ = false;
            if (!casterName.empty()) {
                resurrectCasterName_ = casterName;
            } else {
                resurrectCasterName_ = lookupName(casterGuid);
            }
            resurrectRequestPending_ = true;
                            fireAddonEvent("RESURRECT_REQUEST", {resurrectCasterName_});
        }
    };

    // Time sync
    dispatchTable_[Opcode::SMSG_TIME_SYNC_REQ] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t counter = packet.readUInt32();
        if (socket) {
            network::Packet resp(wireOpcode(Opcode::CMSG_TIME_SYNC_RESP));
            resp.writeUInt32(counter);
            resp.writeUInt32(nextMovementTimestampMs());
            socket->send(resp);
        }
    };

    // (SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED → moved to InventoryHandler)

    // Minimap ping
    dispatchTable_[Opcode::MSG_MINIMAP_PING] = [this](network::Packet& packet) {
        const bool mmTbcLike = isPreWotlk();
        if (!packet.hasRemaining(mmTbcLike ? 8u : 1u) ) return;
        uint64_t senderGuid = mmTbcLike
            ? packet.readUInt64() : packet.readPackedGuid();
        if (!packet.hasRemaining(8)) return;
        float pingX = packet.readFloat();
        float pingY = packet.readFloat();
        MinimapPing ping;
        ping.senderGuid = senderGuid;
        ping.wowX = pingY;
        ping.wowY = pingX;
        ping.age  = 0.0f;
        minimapPings_.push_back(ping);
        if (senderGuid != playerGuid) {
                            withSoundManager(&rendering::Renderer::getUiSoundManager, [](auto* sfx) { sfx->playMinimapPing(); });
        }
    };
    dispatchTable_[Opcode::SMSG_ZONE_UNDER_ATTACK] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t areaId = packet.readUInt32();
            std::string areaName = getAreaName(areaId);
            std::string msg = areaName.empty()
                ? std::string("A zone is under attack!")
                : (areaName + " is under attack!");
            addUIError(msg);
            addSystemChatMessage(msg);
        }
    };

    // Spirit healer time / durability
    dispatchTable_[Opcode::SMSG_AREA_SPIRIT_HEALER_TIME] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            /*uint64_t guid =*/ packet.readUInt64();
            uint32_t timeMs = packet.readUInt32();
            uint32_t secs = timeMs / 1000;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "You will be able to resurrect in %u seconds.", secs);
            addSystemChatMessage(buf);
        }
    };
    dispatchTable_[Opcode::SMSG_DURABILITY_DAMAGE_DEATH] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t pct = packet.readUInt32();
            char buf[80];
            std::snprintf(buf, sizeof(buf),
                "You have lost %u%% of your gear's durability due to death.", pct);
            addUIError(buf);
            addSystemChatMessage(buf);
        }
    };

    // (SMSG_INITIALIZE_FACTIONS, SMSG_SET_FACTION_STANDING,
    //  SMSG_SET_FACTION_ATWAR, SMSG_SET_FACTION_VISIBLE → moved to SocialHandler)
    dispatchTable_[Opcode::SMSG_FEATURE_SYSTEM_STATUS] = [this](network::Packet& packet) {
        packet.skipAll();
    };

    // (SMSG_SET_FLAT_SPELL_MODIFIER, SMSG_SET_PCT_SPELL_MODIFIER, SMSG_SPELL_DELAYED → moved to SpellHandler)

    // Proficiency
    dispatchTable_[Opcode::SMSG_SET_PROFICIENCY] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(5)) return;
        uint8_t  itemClass = packet.readUInt8();
        uint32_t mask      = packet.readUInt32();
        if (itemClass == 2) weaponProficiency_ = mask;
        else if (itemClass == 4) armorProficiency_ = mask;
    };

    // Loot money / misc consume
    for (auto op : { Opcode::SMSG_LOOT_CLEAR_MONEY, Opcode::SMSG_NPC_TEXT_UPDATE }) {
        dispatchTable_[op] = [](network::Packet& /*packet*/) {};
    }

    // Play sound
    dispatchTable_[Opcode::SMSG_PLAY_SOUND] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playSoundCallback_) playSoundCallback_(soundId);
        }
    };

    // SMSG_SERVER_MESSAGE — moved to ChatHandler::registerOpcodes
    // SMSG_CHAT_SERVER_MESSAGE — moved to ChatHandler::registerOpcodes
    // SMSG_AREA_TRIGGER_MESSAGE — moved to ChatHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_TRIGGER_CINEMATIC] = [this](network::Packet& packet) {
        packet.skipAll();
        network::Packet ack(wireOpcode(Opcode::CMSG_NEXT_CINEMATIC_CAMERA));
        socket->send(ack);
    };

    // ---- Batch 5: Teleport, taxi, BG, LFG, arena, movement relay, mail, bank, auction, quests ----

    // Teleport
    dispatchTable_[Opcode::SMSG_TRANSFER_PENDING] = [this](network::Packet& packet) {
        uint32_t pendingMapId = packet.readUInt32();
        if (packet.hasRemaining(8)) {
            packet.readUInt32(); // transportEntry
            packet.readUInt32(); // transportMapId
        }
        (void)pendingMapId;
    };
    dispatchTable_[Opcode::SMSG_TRANSFER_ABORTED] = [this](network::Packet& packet) {
        uint32_t mapId = packet.readUInt32();
        uint8_t reason = (packet.hasData()) ? packet.readUInt8() : 0;
        (void)mapId;
        const char* abortMsg = nullptr;
        switch (reason) {
            case 0x01: abortMsg = "Transfer aborted: difficulty unavailable."; break;
            case 0x02: abortMsg = "Transfer aborted: expansion required."; break;
            case 0x03: abortMsg = "Transfer aborted: instance not found."; break;
            case 0x04: abortMsg = "Transfer aborted: too many instances. Please wait before entering a new instance."; break;
            case 0x06: abortMsg = "Transfer aborted: instance is full."; break;
            case 0x07: abortMsg = "Transfer aborted: zone is in combat."; break;
            case 0x08: abortMsg = "Transfer aborted: you are already in this instance."; break;
            case 0x09: abortMsg = "Transfer aborted: not enough players."; break;
            default:   abortMsg = "Transfer aborted."; break;
        }
        addUIError(abortMsg);
        addSystemChatMessage(abortMsg);
    };

    // Taxi
    dispatchTable_[Opcode::SMSG_STANDSTATE_UPDATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            standState_ = packet.readUInt8();
            if (standStateCallback_) standStateCallback_(standState_);
        }
    };
    dispatchTable_[Opcode::SMSG_NEW_TAXI_PATH] = [this](network::Packet& /*packet*/) {
        addSystemChatMessage("New flight path discovered!");
    };

    // Arena
    dispatchTable_[Opcode::MSG_TALENT_WIPE_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
        talentWipeNpcGuid_ = packet.readUInt64();
        talentWipeCost_    = packet.readUInt32();
        talentWipePending_ = true;
                    fireAddonEvent("CONFIRM_TALENT_WIPE", {std::to_string(talentWipeCost_)});
    };

    // (SMSG_CHANNEL_LIST → moved to ChatHandler)
    // (SMSG_GROUP_SET_LEADER → moved to SocialHandler)

    // Gameobject / page text
    registerHandler(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE, &GameHandler::handleGameObjectQueryResponse);
    registerHandler(Opcode::SMSG_GAMEOBJECT_PAGETEXT, &GameHandler::handleGameObjectPageText);
    registerHandler(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE, &GameHandler::handlePageTextQueryResponse);
    dispatchTable_[Opcode::SMSG_GAMEOBJECT_CUSTOM_ANIM] = [this](network::Packet& packet) {
        if (packet.getSize() < 12) return;
        uint64_t guid = packet.readUInt64();
        uint32_t animId = packet.readUInt32();
        if (gameObjectCustomAnimCallback_)
            gameObjectCustomAnimCallback_(guid, animId);
        if (animId == 0) {
            auto goEnt = entityManager.getEntity(guid);
            if (goEnt && goEnt->getType() == ObjectType::GAMEOBJECT) {
                auto go = std::static_pointer_cast<GameObject>(goEnt);
                auto* info = getCachedGameObjectInfo(go->getEntry());
                if (info && info->type == 17) {
                    addUIError("A fish is on your line!");
                    addSystemChatMessage("A fish is on your line!");
                    withSoundManager(&rendering::Renderer::getUiSoundManager, [](auto* sfx) { sfx->playQuestUpdate(); });
                }
            }
        }
    };

    // Item refund / socket gems / item time
    dispatchTable_[Opcode::SMSG_ITEM_REFUND_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            packet.readUInt64(); // itemGuid
            uint32_t result = packet.readUInt32();
            addSystemChatMessage(result == 0 ? "Item returned. Refund processed."
                                             : "Could not return item for refund.");
        }
    };
    dispatchTable_[Opcode::SMSG_SOCKET_GEMS_RESULT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            if (result == 0) addSystemChatMessage("Gems socketed successfully.");
            else addSystemChatMessage("Failed to socket gems.");
        }
    };
    dispatchTable_[Opcode::SMSG_ITEM_TIME_UPDATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            packet.readUInt64(); // itemGuid
            packet.readUInt32(); // durationMs
        }
    };

    // ---- Batch 6: Spell miss / env damage / control / spell failure ----


    // ---- Achievement / fishing delegates ----
    dispatchTable_[Opcode::SMSG_ALL_ACHIEVEMENT_DATA] = [this](network::Packet& packet) {
        handleAllAchievementData(packet);
    };
    dispatchTable_[Opcode::SMSG_FISH_NOT_HOOKED] = [this](network::Packet& /*packet*/) {
        addSystemChatMessage("Your fish got away.");
    };
    dispatchTable_[Opcode::SMSG_FISH_ESCAPED] = [this](network::Packet& /*packet*/) {
        addSystemChatMessage("Your fish escaped!");
    };

    // ---- Auto-repeat / auras / dispel / totem ----
    dispatchTable_[Opcode::SMSG_CANCEL_AUTO_REPEAT] = [this](network::Packet& /*packet*/) {
        // Server signals to stop a repeating spell (wand/shoot); no client action needed
    };


    // ---- Batch 7: World states, action buttons, level-up, vendor, inventory ----

    // ---- SMSG_INIT_WORLD_STATES ----
    dispatchTable_[Opcode::SMSG_INIT_WORLD_STATES] = [this](network::Packet& packet) {
        // WotLK format: uint32 mapId, uint32 zoneId, uint32 areaId, uint16 count, N*(uint32 key, uint32 val)
        // Classic/TBC format: uint32 mapId, uint32 zoneId, uint16 count, N*(uint32 key, uint32 val)
        if (!packet.hasRemaining(10)) {
            LOG_WARNING("SMSG_INIT_WORLD_STATES too short: ", packet.getSize(), " bytes");
            return;
        }
        worldStateMapId_ = packet.readUInt32();
        {
            uint32_t newZoneId = packet.readUInt32();
            if (newZoneId != worldStateZoneId_ && newZoneId != 0) {
                worldStateZoneId_ = newZoneId;
                    fireAddonEvent("ZONE_CHANGED_NEW_AREA", {});
                    fireAddonEvent("ZONE_CHANGED", {});
            } else {
                worldStateZoneId_ = newZoneId;
            }
        }
        // WotLK adds areaId (uint32) before count; Classic/TBC/Turtle use the shorter format
        size_t remaining = packet.getRemainingSize();
        bool isWotLKFormat = isActiveExpansion("wotlk");
        if (isWotLKFormat && remaining >= 6) {
            packet.readUInt32(); // areaId (WotLK only)
        }
        uint16_t count = packet.readUInt16();
        size_t needed = static_cast<size_t>(count) * 8;
        size_t available = packet.getRemainingSize();
        if (available < needed) {
            // Be tolerant across expansion/private-core variants: if packet shape
            // still looks like N*(key,val) dwords, parse what is present.
            if ((available % 8) == 0) {
                uint16_t adjustedCount = static_cast<uint16_t>(available / 8);
                LOG_WARNING("SMSG_INIT_WORLD_STATES count mismatch: header=", count,
                            " adjusted=", adjustedCount, " (available=", available, ")");
                count = adjustedCount;
                needed = available;
            } else {
                LOG_WARNING("SMSG_INIT_WORLD_STATES truncated: expected ", needed,
                            " bytes of state pairs, got ", available);
                packet.skipAll();
                return;
            }
        }
        worldStates_.clear();
        worldStates_.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t key = packet.readUInt32();
            uint32_t val = packet.readUInt32();
            worldStates_[key] = val;
        }
    };

    // ---- SMSG_ACTION_BUTTONS ----
    dispatchTable_[Opcode::SMSG_ACTION_BUTTONS] = [this](network::Packet& packet) {
        // Slot encoding differs by expansion:
        //   Classic/Turtle: uint16 actionId + uint8 type + uint8 misc
        //     type: 0=spell, 1=item, 64=macro
        //   TBC/WotLK: uint32 packed = actionId | (type << 24)
        //     type: 0x00=spell, 0x80=item, 0x40=macro
        // Format differences:
        //   Classic 1.12: no mode byte, 120 slots (480 bytes)
        //   TBC 2.4.3:    no mode byte, 132 slots (528 bytes)
        //   WotLK 3.3.5a: uint8 mode + 144 slots (577 bytes)
        size_t rem = packet.getRemainingSize();
        const bool hasModeByteExp = isActiveExpansion("wotlk");
        int serverBarSlots;
        if (isClassicLikeExpansion()) {
            serverBarSlots = 120;
        } else if (isActiveExpansion("tbc")) {
            serverBarSlots = 132;
        } else {
            serverBarSlots = 144;
        }
        if (hasModeByteExp) {
            if (rem < 1) return;
            /*uint8_t mode =*/ packet.readUInt8();
            rem--;
        }
        for (int i = 0; i < serverBarSlots; ++i) {
            if (rem < 4) return;
            uint32_t packed = packet.readUInt32();
            rem -= 4;
            if (i >= ACTION_BAR_SLOTS) continue;  // only load bars 1 and 2
            if (packed == 0) {
                // Empty slot — only clear if not already set to Attack/Hearthstone defaults
                // so we don't wipe hardcoded fallbacks when the server sends zeros.
                continue;
            }
            uint8_t type = 0;
            uint32_t id = 0;
            if (isClassicLikeExpansion()) {
                id = packed & 0x0000FFFFu;
                type = static_cast<uint8_t>((packed >> 16) & 0xFF);
            } else {
                type = static_cast<uint8_t>((packed >> 24) & 0xFF);
                id = packed & 0x00FFFFFFu;
            }
            if (id == 0) continue;
            ActionBarSlot slot;
            switch (type) {
                case 0x00: slot.type = ActionBarSlot::SPELL; slot.id = id; break;
                case 0x01: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // Classic item
                case 0x80: slot.type = ActionBarSlot::ITEM;  slot.id = id; break;  // TBC/WotLK item
                case 0x40: slot.type = ActionBarSlot::MACRO; slot.id = id; break;  // macro (all expansions)
                default:   continue;  // unknown — leave as-is
            }
            actionBar[i] = slot;
        }
        // Apply any pending cooldowns from spellHandler's cooldowns to newly populated slots.
        // SMSG_SPELL_COOLDOWN often arrives before SMSG_ACTION_BUTTONS during login,
        // so the per-slot cooldownRemaining would be 0 without this sync.
        if (spellHandler_) {
            for (auto& slot : actionBar) {
                if (slot.type == ActionBarSlot::SPELL && slot.id != 0) {
                    auto cdIt = spellHandler_->spellCooldowns_.find(slot.id);
                    if (cdIt != spellHandler_->spellCooldowns_.end() && cdIt->second > 0.0f) {
                        slot.cooldownRemaining = cdIt->second;
                        slot.cooldownTotal     = cdIt->second;
                    }
                } else if (slot.type == ActionBarSlot::ITEM && slot.id != 0) {
                    // Items (potions, trinkets): look up the item's on-use spell
                    // and check if that spell has a pending cooldown.
                    const auto* qi = getItemInfo(slot.id);
                    if (qi && qi->valid) {
                        for (const auto& sp : qi->spells) {
                            if (sp.spellId == 0) continue;
                            auto cdIt = spellHandler_->spellCooldowns_.find(sp.spellId);
                            if (cdIt != spellHandler_->spellCooldowns_.end() && cdIt->second > 0.0f) {
                                slot.cooldownRemaining = cdIt->second;
                                slot.cooldownTotal     = cdIt->second;
                                break;
                            }
                        }
                    }
                }
            }
        }
        LOG_INFO("SMSG_ACTION_BUTTONS: populated action bar from server");
        fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {});
        packet.skipAll();
    };

    // ---- SMSG_LEVELUP_INFO / SMSG_LEVELUP_INFO_ALT (shared body) ----
    for (auto op : {Opcode::SMSG_LEVELUP_INFO, Opcode::SMSG_LEVELUP_INFO_ALT}) {
        dispatchTable_[op] = [this](network::Packet& packet) {
            // Server-authoritative level-up event.
            // WotLK layout: uint32 newLevel + uint32 hpDelta + uint32 manaDelta + 5x uint32 statDeltas
            if (packet.hasRemaining(4)) {
                uint32_t newLevel = packet.readUInt32();
                if (newLevel > 0) {
                    // Parse stat deltas (WotLK layout has 7 more uint32s)
                    lastLevelUpDeltas_ = {};
                    if (packet.hasRemaining(28)) {
                        lastLevelUpDeltas_.hp    = packet.readUInt32();
                        lastLevelUpDeltas_.mana  = packet.readUInt32();
                        lastLevelUpDeltas_.str   = packet.readUInt32();
                        lastLevelUpDeltas_.agi   = packet.readUInt32();
                        lastLevelUpDeltas_.sta   = packet.readUInt32();
                        lastLevelUpDeltas_.intel = packet.readUInt32();
                        lastLevelUpDeltas_.spi   = packet.readUInt32();
                    }
                    uint32_t oldLevel = serverPlayerLevel_;
                    serverPlayerLevel_ = std::max(serverPlayerLevel_, newLevel);
                    for (auto& ch : characters) {
                        if (ch.guid == playerGuid) {
                            ch.level = serverPlayerLevel_;
                            return;
                        }
                    }
                    if (newLevel > oldLevel) {
                        addSystemChatMessage("You have reached level " + std::to_string(newLevel) + "!");
                        withSoundManager(&rendering::Renderer::getUiSoundManager, [](auto* sfx) { sfx->playLevelUp(); });
                        if (levelUpCallback_) levelUpCallback_(newLevel);
                        fireAddonEvent("PLAYER_LEVEL_UP", {std::to_string(newLevel)});
                    }
                }
            }
            packet.skipAll();
        };
    }

    // ---- MSG_RAID_TARGET_UPDATE ----
    dispatchTable_[Opcode::MSG_RAID_TARGET_UPDATE] = [this](network::Packet& packet) {
        // uint8 type: 0 = full update (8 × (uint8 icon + uint64 guid)),
        //             1 = single update (uint8 icon + uint64 guid)
        size_t remRTU = packet.getRemainingSize();
        if (remRTU < 1) return;
        uint8_t rtuType = packet.readUInt8();
        if (rtuType == 0) {
            // Full update: always 8 entries
            for (uint32_t i = 0; i < kRaidMarkCount; ++i) {
                if (!packet.hasRemaining(9)) return;
                uint8_t  icon = packet.readUInt8();
                uint64_t guid = packet.readUInt64();
                if (socialHandler_)
                    socialHandler_->setRaidTargetGuid(icon, guid);
            }
        } else {
            // Single update
            if (packet.hasRemaining(9)) {
                uint8_t  icon = packet.readUInt8();
                uint64_t guid = packet.readUInt64();
                if (socialHandler_)
                    socialHandler_->setRaidTargetGuid(icon, guid);
            }
        }
        LOG_DEBUG("MSG_RAID_TARGET_UPDATE: type=", static_cast<int>(rtuType));
                    fireAddonEvent("RAID_TARGET_UPDATE", {});
    };

    // ---- SMSG_CRITERIA_UPDATE ----
    dispatchTable_[Opcode::SMSG_CRITERIA_UPDATE] = [this](network::Packet& packet) {
        // uint32 criteriaId + uint64 progress + uint32 elapsedTime + uint32 creationTime
        if (packet.hasRemaining(20)) {
            uint32_t criteriaId    = packet.readUInt32();
            uint64_t progress      = packet.readUInt64();
            packet.readUInt32(); // elapsedTime
            packet.readUInt32(); // creationTime
            uint64_t oldProgress = 0;
            auto cpit = criteriaProgress_.find(criteriaId);
            if (cpit != criteriaProgress_.end()) oldProgress = cpit->second;
            criteriaProgress_[criteriaId] = progress;
            LOG_DEBUG("SMSG_CRITERIA_UPDATE: id=", criteriaId, " progress=", progress);
            // Fire addon event for achievement tracking addons
            if (progress != oldProgress)
                fireAddonEvent("CRITERIA_UPDATE", {std::to_string(criteriaId), std::to_string(progress)});
        }
    };

    // ---- SMSG_BARBER_SHOP_RESULT ----
    dispatchTable_[Opcode::SMSG_BARBER_SHOP_RESULT] = [this](network::Packet& packet) {
        // uint32 result (0 = success, 1 = no money, 2 = not barber, 3 = sitting)
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            if (result == 0) {
                addSystemChatMessage("Hairstyle changed.");
                barberShopOpen_ = false;
                fireAddonEvent("BARBER_SHOP_CLOSE", {});
            } else {
                const char* msg = (result == 1) ? "Not enough money for new hairstyle."
                                : (result == 2) ? "You are not at a barber shop."
                                : (result == 3) ? "You must stand up to use the barber shop."
                                : "Barber shop unavailable.";
                addUIError(msg);
                addSystemChatMessage(msg);
            }
            LOG_DEBUG("SMSG_BARBER_SHOP_RESULT: result=", result);
        }
    };

    // -----------------------------------------------------------------------
    // Batch 8-12: Remaining opcodes (inspects, quests, auctions, spells,
    //             calendars, battlefields, voice, misc consume-only)
    // -----------------------------------------------------------------------
    // uint32 currentZoneLightId + uint32 overrideLightId + uint32 transitionMs
    dispatchTable_[Opcode::SMSG_OVERRIDE_LIGHT] = [this](network::Packet& packet) {
        // uint32 currentZoneLightId + uint32 overrideLightId + uint32 transitionMs
        if (packet.hasRemaining(12)) {
            uint32_t zoneLightId     = packet.readUInt32();
            uint32_t overrideLightId = packet.readUInt32();
            uint32_t transitionMs    = packet.readUInt32();
            overrideLightId_      = overrideLightId;
            overrideLightTransMs_ = transitionMs;
            LOG_DEBUG("SMSG_OVERRIDE_LIGHT: zone=", zoneLightId,
                      " override=", overrideLightId, " transition=", transitionMs, "ms");
        }
    };
    // Classic 1.12: uint32 weatherType + float intensity (8 bytes, no isAbrupt)
    // TBC 2.4.3 / WotLK 3.3.5a: uint32 weatherType + float intensity + uint8 isAbrupt (9 bytes)
    dispatchTable_[Opcode::SMSG_WEATHER] = [this](network::Packet& packet) {
        // Classic 1.12: uint32 weatherType + float intensity (8 bytes, no isAbrupt)
        // TBC 2.4.3 / WotLK 3.3.5a: uint32 weatherType + float intensity + uint8 isAbrupt (9 bytes)
        if (packet.hasRemaining(8)) {
            uint32_t wType = packet.readUInt32();
            float wIntensity = packet.readFloat();
            if (packet.hasRemaining(1))
                /*uint8_t isAbrupt =*/ packet.readUInt8();
            uint32_t prevWeatherType = weatherType_;
            weatherType_ = wType;
            weatherIntensity_ = wIntensity;
            const char* typeName = (wType == 1) ? "Rain" : (wType == 2) ? "Snow" : (wType == 3) ? "Storm" : "Clear";
            LOG_INFO("Weather changed: type=", wType, " (", typeName, "), intensity=", wIntensity);
            // Announce weather changes (including initial zone weather)
            if (wType != prevWeatherType) {
                const char* weatherMsg = nullptr;
                if (wIntensity < 0.05f || wType == 0) {
                    if (prevWeatherType != 0)
                        weatherMsg = "The weather clears.";
                } else if (wType == 1) {
                    weatherMsg = "It begins to rain.";
                } else if (wType == 2) {
                    weatherMsg = "It begins to snow.";
                } else if (wType == 3) {
                    weatherMsg = "A storm rolls in.";
                }
                if (weatherMsg) addSystemChatMessage(weatherMsg);
            }
            // Notify addons of weather change
                            fireAddonEvent("WEATHER_CHANGED", {std::to_string(wType), std::to_string(wIntensity)});
            // Storm transition: trigger a low-frequency thunder rumble shake
            if (wType == 3 && wIntensity > 0.3f && cameraShakeCallback_) {
                float mag = 0.03f + wIntensity * 0.04f; // 0.03–0.07 units
                cameraShakeCallback_(mag, 6.0f, 0.6f);
            }
        }
    };
    // Server-script text message — display in system chat
    dispatchTable_[Opcode::SMSG_SCRIPT_MESSAGE] = [this](network::Packet& packet) {
        // Server-script text message — display in system chat
        std::string msg = packet.readString();
        if (!msg.empty()) {
            addSystemChatMessage(msg);
            LOG_INFO("SMSG_SCRIPT_MESSAGE: ", msg);
        }
    };
    // uint64 targetGuid + uint64 casterGuid + uint32 spellId + uint32 displayId + uint32 animType
    dispatchTable_[Opcode::SMSG_ENCHANTMENTLOG] = [this](network::Packet& packet) {
        // uint64 targetGuid + uint64 casterGuid + uint32 spellId + uint32 displayId + uint32 animType
        if (packet.hasRemaining(28)) {
            uint64_t enchTargetGuid = packet.readUInt64();
            uint64_t enchCasterGuid = packet.readUInt64();
            uint32_t enchSpellId = packet.readUInt32();
            /*uint32_t displayId =*/ packet.readUInt32();
            /*uint32_t animType =*/ packet.readUInt32();
            LOG_DEBUG("SMSG_ENCHANTMENTLOG: spellId=", enchSpellId);
            // Show enchant message if the player is involved
            if (enchTargetGuid == playerGuid || enchCasterGuid == playerGuid) {
                const std::string& enchName = getSpellName(enchSpellId);
                std::string casterName = lookupName(enchCasterGuid);
                if (!enchName.empty()) {
                    std::string msg;
                    if (enchCasterGuid == playerGuid)
                        msg = "You enchant with " + enchName + ".";
                    else if (!casterName.empty())
                        msg = casterName + " enchants your item with " + enchName + ".";
                    else
                        msg = "Your item has been enchanted with " + enchName + ".";
                    addSystemChatMessage(msg);
                }
            }
        }
    };
    // WotLK: uint64 playerGuid + uint8 teamCount + per-team fields
    dispatchTable_[Opcode::MSG_INSPECT_ARENA_TEAMS] = [this](network::Packet& packet) {
        // WotLK: uint64 playerGuid + uint8 teamCount + per-team fields
        if (!packet.hasRemaining(9)) {
            packet.skipAll();
            return;
        }
        uint64_t inspGuid  = packet.readUInt64();
        uint8_t  teamCount = packet.readUInt8();
        if (teamCount > 3) teamCount = 3; // 2v2, 3v3, 5v5
        if (socialHandler_) {
            auto& ir = socialHandler_->mutableInspectResult();
            if (inspGuid == ir.guid || ir.guid == 0) {
                ir.guid = inspGuid;
                ir.arenaTeams.clear();
                for (uint8_t t = 0; t < teamCount; ++t) {
                    if (!packet.hasRemaining(21)) break;
                    SocialHandler::InspectArenaTeam team;
                    team.teamId         = packet.readUInt32();
                    team.type           = packet.readUInt8();
                    team.weekGames      = packet.readUInt32();
                    team.weekWins       = packet.readUInt32();
                    team.seasonGames    = packet.readUInt32();
                    team.seasonWins     = packet.readUInt32();
                    team.name           = packet.readString();
                    if (!packet.hasRemaining(4)) break;
                    team.personalRating = packet.readUInt32();
                    ir.arenaTeams.push_back(std::move(team));
                }
            }
        }
        LOG_DEBUG("MSG_INSPECT_ARENA_TEAMS: guid=0x", std::hex, inspGuid, std::dec,
                  " teams=", static_cast<int>(teamCount));
    };
    // auctionId(u32) + action(u32) + error(u32) + itemEntry(u32) + randomPropertyId(u32) + ...
    // action: 0=sold/won, 1=expired, 2=bid placed on your auction
    // auctionHouseId(u32) + auctionId(u32) + bidderGuid(u64) + bidAmount(u32) + outbidAmount(u32) + itemEntry(u32) + randomPropertyId(u32)
    // uint32 auctionId + uint32 itemEntry + uint32 itemRandom — auction expired/cancelled
    // uint64 containerGuid — tells client to open this container
    // The actual items come via update packets; we just log this.
    // PackedGuid (player guid) + uint32 vehicleId
    // vehicleId == 0 means the player left the vehicle
    dispatchTable_[Opcode::SMSG_PLAYER_VEHICLE_DATA] = [this](network::Packet& packet) {
        // PackedGuid (player guid) + uint32 vehicleId
        // vehicleId == 0 means the player left the vehicle
        if (packet.hasRemaining(1)) {
            (void)packet.readPackedGuid(); // player guid (unused)
        }
        if (packet.hasRemaining(4)) {
            vehicleId_ = packet.readUInt32();
        } else {
            vehicleId_ = 0;
        }
    };
    // guid(8) + status(1): status 1 = NPC has available/new routes for this player
    dispatchTable_[Opcode::SMSG_TAXINODE_STATUS] = [this](network::Packet& packet) {
        // guid(8) + status(1): status 1 = NPC has available/new routes for this player
        if (packet.hasRemaining(9)) {
            uint64_t npcGuid = packet.readUInt64();
            uint8_t  status  = packet.readUInt8();
            taxiNpcHasRoutes_[npcGuid] = (status != 0);
        }
    };
    // SMSG_GUILD_DECLINE — moved to SocialHandler::registerOpcodes
    // Clear cached talent data so the talent screen reflects the reset.
    dispatchTable_[Opcode::SMSG_TALENTS_INVOLUNTARILY_RESET] = [this](network::Packet& packet) {
        // Clear cached talent data so the talent screen reflects the reset.
        if (spellHandler_) spellHandler_->resetTalentState();
        addUIError("Your talents have been reset by the server.");
        addSystemChatMessage("Your talents have been reset by the server.");
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_SET_REST_START] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t restTrigger = packet.readUInt32();
            isResting_ = (restTrigger > 0);
            addSystemChatMessage(isResting_ ? "You are now resting."
                                            : "You are no longer resting.");
                            fireAddonEvent("PLAYER_UPDATE_RESTING", {});
        }
    };
    dispatchTable_[Opcode::SMSG_UPDATE_AURA_DURATION] = [this](network::Packet& packet) {
        if (packet.hasRemaining(5)) {
            uint8_t slot       = packet.readUInt8();
            uint32_t durationMs = packet.readUInt32();
            handleUpdateAuraDuration(slot, durationMs);
        }
    };
    dispatchTable_[Opcode::SMSG_ITEM_NAME_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t itemId = packet.readUInt32();
            std::string name = packet.readString();
            if (!itemInfoCache_.count(itemId) && !name.empty()) {
                ItemQueryResponseData stub;
                stub.entry = itemId;
                stub.name  = std::move(name);
                stub.valid = true;
                itemInfoCache_[itemId] = std::move(stub);
            }
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_MOUNTSPECIAL_ANIM] = [this](network::Packet& packet) { (void)packet.readPackedGuid(); };
    dispatchTable_[Opcode::SMSG_CHAR_CUSTOMIZE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t result = packet.readUInt8();
            addSystemChatMessage(result == 0 ? "Character customization complete."
                                             : "Character customization failed.");
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_CHAR_FACTION_CHANGE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t result = packet.readUInt8();
            addSystemChatMessage(result == 0 ? "Faction change complete."
                                             : "Faction change failed.");
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_INVALIDATE_PLAYER] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            uint64_t guid = packet.readUInt64();
            playerNameCache.erase(guid);
        }
    };
    // uint32 movieId — we don't play movies; acknowledge immediately.
    dispatchTable_[Opcode::SMSG_TRIGGER_MOVIE] = [this](network::Packet& packet) {
        // uint32 movieId — we don't play movies; acknowledge immediately.
        packet.skipAll();
        // WotLK servers expect CMSG_COMPLETE_MOVIE after the movie finishes;
        // without it, the server may hang or disconnect the client.
        uint16_t wire = wireOpcode(Opcode::CMSG_COMPLETE_MOVIE);
        if (wire != 0xFFFF) {
            network::Packet ack(wire);
            socket->send(ack);
            LOG_DEBUG("SMSG_TRIGGER_MOVIE: skipped, sent CMSG_COMPLETE_MOVIE");
        }
    };
    // Server-side LFG invite timed out (no response within time limit)
    dispatchTable_[Opcode::SMSG_LFG_TIMEDOUT] = [this](network::Packet& packet) {
        // Server-side LFG invite timed out (no response within time limit)
        addSystemChatMessage("Dungeon Finder: Invite timed out.");
        if (openLfgCallback_) openLfgCallback_();
        packet.skipAll();
    };
    // Another party member failed to respond to a LFG role-check in time
    dispatchTable_[Opcode::SMSG_LFG_OTHER_TIMEDOUT] = [this](network::Packet& packet) {
        // Another party member failed to respond to a LFG role-check in time
        addSystemChatMessage("Dungeon Finder: Another player's invite timed out.");
        if (openLfgCallback_) openLfgCallback_();
        packet.skipAll();
    };
    // uint32 result — LFG auto-join attempt failed (player selected auto-join at queue time)
    dispatchTable_[Opcode::SMSG_LFG_AUTOJOIN_FAILED] = [this](network::Packet& packet) {
        // uint32 result — LFG auto-join attempt failed (player selected auto-join at queue time)
        if (packet.hasRemaining(4)) {
            uint32_t result = packet.readUInt32();
            (void)result;
        }
        addUIError("Dungeon Finder: Auto-join failed.");
        addSystemChatMessage("Dungeon Finder: Auto-join failed.");
        packet.skipAll();
    };
    // No eligible players found for auto-join
    dispatchTable_[Opcode::SMSG_LFG_AUTOJOIN_FAILED_NO_PLAYER] = [this](network::Packet& packet) {
        // No eligible players found for auto-join
        addUIError("Dungeon Finder: No players available for auto-join.");
        addSystemChatMessage("Dungeon Finder: No players available for auto-join.");
        packet.skipAll();
    };
    // Party leader is currently set to Looking for More (LFM) mode
    dispatchTable_[Opcode::SMSG_LFG_LEADER_IS_LFM] = [this](network::Packet& packet) {
        // Party leader is currently set to Looking for More (LFM) mode
        addSystemChatMessage("Your party leader is currently Looking for More.");
        packet.skipAll();
    };
    // uint32 zoneId + uint8 level_min + uint8 level_max — player queued for meeting stone
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_SETQUEUE] = [this](network::Packet& packet) {
        // uint32 zoneId + uint8 level_min + uint8 level_max — player queued for meeting stone
        if (packet.hasRemaining(6)) {
            uint32_t zoneId   = packet.readUInt32();
            uint8_t  levelMin = packet.readUInt8();
            uint8_t  levelMax = packet.readUInt8();
            char buf[128];
            std::string zoneName = getAreaName(zoneId);
            if (!zoneName.empty())
                std::snprintf(buf, sizeof(buf),
                    "You are now in the Meeting Stone queue for %s (levels %u-%u).",
                    zoneName.c_str(), levelMin, levelMax);
            else
                std::snprintf(buf, sizeof(buf),
                    "You are now in the Meeting Stone queue for zone %u (levels %u-%u).",
                    zoneId, levelMin, levelMax);
            addSystemChatMessage(buf);
            LOG_INFO("SMSG_MEETINGSTONE_SETQUEUE: zone=", zoneId,
                     " levels=", static_cast<int>(levelMin), "-", static_cast<int>(levelMax));
        }
        packet.skipAll();
    };
    // Server confirms group found and teleport summon is ready
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_COMPLETE] = [this](network::Packet& packet) {
        // Server confirms group found and teleport summon is ready
        addSystemChatMessage("Meeting Stone: Your group is ready! Use the Meeting Stone to summon.");
        LOG_INFO("SMSG_MEETINGSTONE_COMPLETE");
        packet.skipAll();
    };
    // Meeting stone search is still ongoing
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_IN_PROGRESS] = [this](network::Packet& packet) {
        // Meeting stone search is still ongoing
        addSystemChatMessage("Meeting Stone: Searching for group members...");
        LOG_DEBUG("SMSG_MEETINGSTONE_IN_PROGRESS");
        packet.skipAll();
    };
    // uint64 memberGuid — a player was added to your group via meeting stone
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_MEMBER_ADDED] = [this](network::Packet& packet) {
        // uint64 memberGuid — a player was added to your group via meeting stone
        if (packet.hasRemaining(8)) {
            uint64_t memberGuid = packet.readUInt64();
            const auto& memberName = lookupName(memberGuid);
            if (!memberName.empty()) {
                addSystemChatMessage("Meeting Stone: " + memberName +
                                     " has been added to your group.");
            } else {
                addSystemChatMessage("Meeting Stone: A new player has been added to your group.");
            }
            LOG_INFO("SMSG_MEETINGSTONE_MEMBER_ADDED: guid=0x", std::hex, memberGuid, std::dec);
        }
    };
    // uint8 reason — failed to join group via meeting stone
    // 0=target_not_in_lfg, 1=target_in_party, 2=target_invalid_map, 3=target_not_available
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_JOINFAILED] = [this](network::Packet& packet) {
        // uint8 reason — failed to join group via meeting stone
        // 0=target_not_in_lfg, 1=target_in_party, 2=target_invalid_map, 3=target_not_available
        static const char* kMeetingstoneErrors[] = {
            "Target player is not using the Meeting Stone.",
            "Target player is already in a group.",
            "You are not in a valid zone for that Meeting Stone.",
            "Target player is not available.",
        };
        if (packet.hasRemaining(1)) {
            uint8_t reason = packet.readUInt8();
            const char* msg = (reason < 4) ? kMeetingstoneErrors[reason]
                                           : "Meeting Stone: Could not join group.";
            addSystemChatMessage(msg);
            LOG_INFO("SMSG_MEETINGSTONE_JOINFAILED: reason=", static_cast<int>(reason));
        }
    };
    // Player was removed from the meeting stone queue (left, or group disbanded)
    dispatchTable_[Opcode::SMSG_MEETINGSTONE_LEAVE] = [this](network::Packet& packet) {
        // Player was removed from the meeting stone queue (left, or group disbanded)
        addSystemChatMessage("You have left the Meeting Stone queue.");
        LOG_DEBUG("SMSG_MEETINGSTONE_LEAVE");
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_CREATE] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t res = packet.readUInt8();
            addSystemChatMessage(res == 1 ? "GM ticket submitted."
                                          : "Failed to submit GM ticket.");
        }
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_UPDATETEXT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t res = packet.readUInt8();
            addSystemChatMessage(res == 1 ? "GM ticket updated."
                                          : "Failed to update GM ticket.");
        }
    };
    dispatchTable_[Opcode::SMSG_GMTICKET_DELETETICKET] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t res = packet.readUInt8();
            addSystemChatMessage(res == 9 ? "GM ticket deleted."
                                          : "No ticket to delete.");
        }
    };
    // WotLK 3.3.5a format:
    //   uint8  status  — 1=no ticket, 6=has open ticket, 3=closed, 10=suspended
    // If status == 6 (GMTICKET_STATUS_HASTEXT):
    //   cstring ticketText
    //   uint32  ticketAge       (seconds old)
    //   uint32  daysUntilOld    (days remaining before escalation)
    //   float   waitTimeHours   (estimated GM wait time)
    dispatchTable_[Opcode::SMSG_GMTICKET_GETTICKET] = [this](network::Packet& packet) {
        // WotLK 3.3.5a format:
        //   uint8  status  — 1=no ticket, 6=has open ticket, 3=closed, 10=suspended
        // If status == 6 (GMTICKET_STATUS_HASTEXT):
        //   cstring ticketText
        //   uint32  ticketAge       (seconds old)
        //   uint32  daysUntilOld    (days remaining before escalation)
        //   float   waitTimeHours   (estimated GM wait time)
        if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
        uint8_t gmStatus = packet.readUInt8();
        // Status 6 = GMTICKET_STATUS_HASTEXT — open ticket with text
        if (gmStatus == 6 && packet.hasRemaining(1)) {
            gmTicketText_    = packet.readString();
            uint32_t ageSec  = (packet.hasRemaining(4)) ? packet.readUInt32() : 0;
            /*uint32_t daysLeft =*/ (packet.hasRemaining(4)) ? packet.readUInt32() : 0;
            gmTicketWaitHours_ = (packet.hasRemaining(4))
                ? packet.readFloat() : 0.0f;
            gmTicketActive_ = true;
            char buf[256];
            if (ageSec < 60) {
                std::snprintf(buf, sizeof(buf),
                    "You have an open GM ticket (submitted %us ago). Estimated wait: %.1f hours.",
                    ageSec, gmTicketWaitHours_);
            } else {
                uint32_t ageMin = ageSec / 60;
                std::snprintf(buf, sizeof(buf),
                    "You have an open GM ticket (submitted %um ago). Estimated wait: %.1f hours.",
                    ageMin, gmTicketWaitHours_);
            }
            addSystemChatMessage(buf);
            LOG_INFO("SMSG_GMTICKET_GETTICKET: open ticket age=", ageSec,
                     "s wait=", gmTicketWaitHours_, "h");
        } else if (gmStatus == 3) {
            gmTicketActive_ = false;
            gmTicketText_.clear();
            addSystemChatMessage("Your GM ticket has been closed.");
            LOG_INFO("SMSG_GMTICKET_GETTICKET: ticket closed");
        } else if (gmStatus == 10) {
            gmTicketActive_ = false;
            gmTicketText_.clear();
            addSystemChatMessage("Your GM ticket has been suspended.");
            LOG_INFO("SMSG_GMTICKET_GETTICKET: ticket suspended");
        } else {
            // Status 1 = no open ticket (default/no ticket)
            gmTicketActive_ = false;
            gmTicketText_.clear();
            LOG_DEBUG("SMSG_GMTICKET_GETTICKET: no open ticket (status=", static_cast<int>(gmStatus), ")");
        }
        packet.skipAll();
    };
    // uint32 status: 1 = GM support available, 0 = offline/unavailable
    dispatchTable_[Opcode::SMSG_GMTICKET_SYSTEMSTATUS] = [this](network::Packet& packet) {
        // uint32 status: 1 = GM support available, 0 = offline/unavailable
        if (packet.hasRemaining(4)) {
            uint32_t sysStatus = packet.readUInt32();
            gmSupportAvailable_ = (sysStatus != 0);
            addSystemChatMessage(gmSupportAvailable_
                ? "GM support is currently available."
                : "GM support is currently unavailable.");
            LOG_INFO("SMSG_GMTICKET_SYSTEMSTATUS: available=", gmSupportAvailable_);
        }
        packet.skipAll();
    };
    // uint8 runeIndex + uint8 newRuneType (0=Blood,1=Unholy,2=Frost,3=Death)
    dispatchTable_[Opcode::SMSG_CONVERT_RUNE] = [this](network::Packet& packet) {
        // uint8 runeIndex + uint8 newRuneType (0=Blood,1=Unholy,2=Frost,3=Death)
        if (!packet.hasRemaining(2)) {
            packet.skipAll();
            return;
        }
        uint8_t idx  = packet.readUInt8();
        uint8_t type = packet.readUInt8();
        if (idx < 6) playerRunes_[idx].type = static_cast<RuneType>(type & 0x3);
    };
    // uint8 runeReadyMask (bit i=1 → rune i is ready)
    // uint8[6] cooldowns (0=ready, 255=just used → readyFraction = 1 - val/255)
    dispatchTable_[Opcode::SMSG_RESYNC_RUNES] = [this](network::Packet& packet) {
        // uint8 runeReadyMask (bit i=1 → rune i is ready)
        // uint8[6] cooldowns (0=ready, 255=just used → readyFraction = 1 - val/255)
        if (!packet.hasRemaining(7)) {
            packet.skipAll();
            return;
        }
        uint8_t readyMask = packet.readUInt8();
        for (int i = 0; i < 6; i++) {
            uint8_t cd = packet.readUInt8();
            playerRunes_[i].ready = (readyMask & (1u << i)) != 0;
            playerRunes_[i].readyFraction = 1.0f - cd / 255.0f;
            if (playerRunes_[i].ready) playerRunes_[i].readyFraction = 1.0f;
        }
    };
    // uint32 runeMask (bit i=1 → rune i just became ready)
    dispatchTable_[Opcode::SMSG_ADD_RUNE_POWER] = [this](network::Packet& packet) {
        // uint32 runeMask (bit i=1 → rune i just became ready)
        if (!packet.hasRemaining(4)) {
            packet.skipAll();
            return;
        }
        uint32_t runeMask = packet.readUInt32();
        for (int i = 0; i < 6; i++) {
            if (runeMask & (1u << i)) {
                playerRunes_[i].ready = true;
                playerRunes_[i].readyFraction = 1.0f;
            }
        }
    };

    // uint8 result: 0=success, 1=failed, 2=disabled
    dispatchTable_[Opcode::SMSG_COMPLAIN_RESULT] = [this](network::Packet& packet) {
        // uint8 result: 0=success, 1=failed, 2=disabled
        if (packet.hasRemaining(1)) {
            uint8_t result = packet.readUInt8();
            if (result == 0)
                addSystemChatMessage("Your complaint has been submitted.");
            else if (result == 2)
                addUIError("Report a Player is currently disabled.");
        }
        packet.skipAll();
    };
    // uint32 slot + packed_guid unit (0 packed = clear slot)
    dispatchTable_[Opcode::SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT] = [this](network::Packet& packet) {
        // uint32 slot + packed_guid unit (0 packed = clear slot)
        if (!packet.hasRemaining(5)) {
            packet.skipAll();
            return;
        }
        uint32_t slot = packet.readUInt32();
        uint64_t unit = packet.readPackedGuid();
        if (socialHandler_) {
            socialHandler_->setEncounterUnitGuid(slot, unit);
            LOG_DEBUG("SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT: slot=", slot,
                      " guid=0x", std::hex, unit, std::dec);
        }
    };
    // charName (cstring) + guid (uint64) + achievementId (uint32) + ...
    dispatchTable_[Opcode::SMSG_SERVER_FIRST_ACHIEVEMENT] = [this](network::Packet& packet) {
        // charName (cstring) + guid (uint64) + achievementId (uint32) + ...
        if (packet.hasData()) {
            std::string charName = packet.readString();
            if (packet.hasRemaining(12)) {
                /*uint64_t guid =*/ packet.readUInt64();
                uint32_t achievementId = packet.readUInt32();
                loadAchievementNameCache();
                auto nit = achievementNameCache_.find(achievementId);
                char buf[256];
                if (nit != achievementNameCache_.end() && !nit->second.empty()) {
                    std::snprintf(buf, sizeof(buf),
                        "%s is the first on the realm to earn: %s!",
                        charName.c_str(), nit->second.c_str());
                } else {
                    std::snprintf(buf, sizeof(buf),
                        "%s is the first on the realm to earn achievement #%u!",
                        charName.c_str(), achievementId);
                }
                addSystemChatMessage(buf);
            }
        }
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_SUSPEND_COMMS] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t seqIdx = packet.readUInt32();
            if (socket) {
                network::Packet ack(wireOpcode(Opcode::CMSG_SUSPEND_COMMS_ACK));
                ack.writeUInt32(seqIdx);
                socket->send(ack);
            }
        }
    };
    // SMSG_PRE_RESURRECT: packed GUID of the player who can self-resurrect.
    // Sent when the dead player has Reincarnation (Shaman), Twisting Nether (Warlock),
    // or Deathpact (Death Knight passive). The client must send CMSG_SELF_RES to accept.
    dispatchTable_[Opcode::SMSG_PRE_RESURRECT] = [this](network::Packet& packet) {
        // SMSG_PRE_RESURRECT: packed GUID of the player who can self-resurrect.
        // Sent when the dead player has Reincarnation (Shaman), Twisting Nether (Warlock),
        // or Deathpact (Death Knight passive). The client must send CMSG_SELF_RES to accept.
        uint64_t targetGuid = packet.readPackedGuid();
        if (targetGuid == playerGuid || targetGuid == 0) {
            selfResAvailable_ = true;
            LOG_INFO("SMSG_PRE_RESURRECT: self-resurrection available (guid=0x",
                     std::hex, targetGuid, std::dec, ")");
        }
    };
    dispatchTable_[Opcode::SMSG_PLAYERBINDERROR] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t error = packet.readUInt32();
            if (error == 0) {
                addUIError("Your hearthstone is not bound.");
                addSystemChatMessage("Your hearthstone is not bound.");
            } else {
                addUIError("Hearthstone bind failed.");
                addSystemChatMessage("Hearthstone bind failed.");
            }
        }
    };
    dispatchTable_[Opcode::SMSG_RAID_GROUP_ONLY] = [this](network::Packet& packet) {
        addUIError("You must be in a raid group to enter this instance.");
        addSystemChatMessage("You must be in a raid group to enter this instance.");
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_RAID_READY_CHECK_ERROR] = [this](network::Packet& packet) {
        if (packet.hasRemaining(1)) {
            uint8_t err = packet.readUInt8();
            if (err == 0) { addUIError("Ready check failed: not in a group."); addSystemChatMessage("Ready check failed: not in a group."); }
            else if (err == 1) { addUIError("Ready check failed: in instance."); addSystemChatMessage("Ready check failed: in instance."); }
            else { addUIError("Ready check failed."); addSystemChatMessage("Ready check failed."); }
        }
    };
    dispatchTable_[Opcode::SMSG_RESET_FAILED_NOTIFY] = [this](network::Packet& packet) {
        addUIError("Cannot reset instance: another player is still inside.");
        addSystemChatMessage("Cannot reset instance: another player is still inside.");
        packet.skipAll();
    };
    // uint32 splitType + uint32 deferTime + string realmName
    // Client must respond with CMSG_REALM_SPLIT to avoid session timeout on some servers.
    dispatchTable_[Opcode::SMSG_REALM_SPLIT] = [this](network::Packet& packet) {
        // uint32 splitType + uint32 deferTime + string realmName
        // Client must respond with CMSG_REALM_SPLIT to avoid session timeout on some servers.
        uint32_t splitType = 0;
        if (packet.hasRemaining(4))
            splitType = packet.readUInt32();
        packet.skipAll();
        if (socket) {
            network::Packet resp(wireOpcode(Opcode::CMSG_REALM_SPLIT));
            resp.writeUInt32(splitType);
            resp.writeString("3.3.5");
            socket->send(resp);
            LOG_DEBUG("SMSG_REALM_SPLIT splitType=", splitType, " — sent CMSG_REALM_SPLIT ack");
        }
    };
    dispatchTable_[Opcode::SMSG_REAL_GROUP_UPDATE] = [this](network::Packet& packet) {
        auto rem = [&]() { return packet.getRemainingSize(); };
        if (rem() < 1) return;
        uint8_t newGroupType = packet.readUInt8();
        if (rem() < 4) return;
        uint32_t newMemberFlags = packet.readUInt32();
        if (rem() < 8) return;
        uint64_t newLeaderGuid = packet.readUInt64();

        if (socialHandler_) {
            auto& pd = socialHandler_->mutablePartyData();
            pd.groupType = newGroupType;
            pd.leaderGuid = newLeaderGuid;

            // Update local player's flags in the member list
            uint64_t localGuid = playerGuid;
            for (auto& m : pd.members) {
                if (m.guid == localGuid) {
                    m.flags = static_cast<uint8_t>(newMemberFlags & 0xFF);
                    break;
                }
            }
        }
        LOG_DEBUG("SMSG_REAL_GROUP_UPDATE groupType=", static_cast<int>(newGroupType),
                  " memberFlags=0x", std::hex, newMemberFlags, std::dec,
                  " leaderGuid=", newLeaderGuid);
        fireAddonEvent("PARTY_LEADER_CHANGED", {});
        fireAddonEvent("GROUP_ROSTER_UPDATE", {});
    };
    dispatchTable_[Opcode::SMSG_PLAY_MUSIC] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playMusicCallback_) playMusicCallback_(soundId);
        }
    };
    dispatchTable_[Opcode::SMSG_PLAY_OBJECT_SOUND] = [this](network::Packet& packet) {
        if (packet.hasRemaining(12)) {
            // uint32 soundId + uint64 sourceGuid
            uint32_t soundId = packet.readUInt32();
            uint64_t srcGuid = packet.readUInt64();
            LOG_DEBUG("SMSG_PLAY_OBJECT_SOUND: id=", soundId, " src=0x", std::hex, srcGuid, std::dec);
            if (playPositionalSoundCallback_) playPositionalSoundCallback_(soundId, srcGuid);
            else if (playSoundCallback_) playSoundCallback_(soundId);
        } else if (packet.hasRemaining(4)) {
            uint32_t soundId = packet.readUInt32();
            if (playSoundCallback_) playSoundCallback_(soundId);
        }
    };
    // uint64 targetGuid + uint32 visualId (same structure as SMSG_PLAY_SPELL_VISUAL)
    dispatchTable_[Opcode::SMSG_PLAY_SPELL_IMPACT] = [this](network::Packet& packet) {
        // uint64 targetGuid + uint32 visualId (same structure as SMSG_PLAY_SPELL_VISUAL)
        if (!packet.hasRemaining(12)) {
            packet.skipAll(); return;
        }
        uint64_t impTargetGuid = packet.readUInt64();
        uint32_t impVisualId   = packet.readUInt32();
        if (impVisualId == 0) return;
        auto* renderer = core::Application::getInstance().getRenderer();
        if (!renderer) return;
        glm::vec3 spawnPos;
        if (impTargetGuid == playerGuid) {
            spawnPos = renderer->getCharacterPosition();
        } else {
            auto entity = entityManager.getEntity(impTargetGuid);
            if (!entity) return;
            glm::vec3 canonical(entity->getLatestX(), entity->getLatestY(), entity->getLatestZ());
            spawnPos = core::coords::canonicalToRender(canonical);
        }
        renderer->playSpellVisual(impVisualId, spawnPos, /*useImpactKit=*/true);
    };
    // SMSG_READ_ITEM_OK — moved to InventoryHandler::registerOpcodes
    // SMSG_READ_ITEM_FAILED — moved to InventoryHandler::registerOpcodes
    // SMSG_QUERY_QUESTS_COMPLETED_RESPONSE — moved to QuestHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_NPC_WONT_TALK] = [this](network::Packet& packet) {
        addUIError("That creature can't talk to you right now.");
        addSystemChatMessage("That creature can't talk to you right now.");
        packet.skipAll();
    };

    // uint64 petGuid + uint32 cost (copper)
    for (auto op : { Opcode::SMSG_PET_GUIDS, Opcode::SMSG_PET_DISMISS_SOUND, Opcode::SMSG_PET_ACTION_SOUND, Opcode::SMSG_PET_UNLEARN_CONFIRM }) {
        dispatchTable_[op] = [this](network::Packet& packet) {
            // uint64 petGuid + uint32 cost (copper)
            if (packet.hasRemaining(12)) {
                petUnlearnGuid_ = packet.readUInt64();
                petUnlearnCost_ = packet.readUInt32();
                petUnlearnPending_ = true;
            }
            packet.skipAll();
        };
    }
    // Server signals that the pet can now be named (first tame)
    dispatchTable_[Opcode::SMSG_PET_RENAMEABLE] = [this](network::Packet& packet) {
        // Server signals that the pet can now be named (first tame)
        petRenameablePending_ = true;
        packet.skipAll();
    };
    dispatchTable_[Opcode::SMSG_PET_NAME_INVALID] = [this](network::Packet& packet) {
        addUIError("That pet name is invalid. Please choose a different name.");
        addSystemChatMessage("That pet name is invalid. Please choose a different name.");
        packet.skipAll();
    };
    // Classic 1.12: PackedGUID + 19×uint32 itemEntries (EQUIPMENT_SLOT_END=19)
    // This opcode is only reachable on Classic servers; TBC/WotLK wire 0x115 maps to
    // SMSG_INSPECT_RESULTS_UPDATE which is handled separately.
    dispatchTable_[Opcode::SMSG_INSPECT] = [this](network::Packet& packet) {
        // Classic 1.12: PackedGUID + 19×uint32 itemEntries (EQUIPMENT_SLOT_END=19)
        // This opcode is only reachable on Classic servers; TBC/WotLK wire 0x115 maps to
        // SMSG_INSPECT_RESULTS_UPDATE which is handled separately.
        if (!packet.hasRemaining(2)) {
            packet.skipAll(); return;
        }
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0) { packet.skipAll(); return; }

        constexpr int kGearSlots = 19;
        size_t needed = kGearSlots * sizeof(uint32_t);
        if (!packet.hasRemaining(needed)) {
            packet.skipAll(); return;
        }

        std::array<uint32_t, 19> items{};
        for (int s = 0; s < kGearSlots; ++s)
            items[s] = packet.readUInt32();

        // Resolve player name
        auto ent = entityManager.getEntity(guid);
        std::string playerName = "Target";
        if (ent) {
            auto pl = std::dynamic_pointer_cast<Player>(ent);
            if (pl && !pl->getName().empty()) playerName = pl->getName();
        }

        // Populate inspect result immediately (no talent data in Classic SMSG_INSPECT)
        if (socialHandler_) {
            auto& ir = socialHandler_->mutableInspectResult();
            ir.guid           = guid;
            ir.playerName     = playerName;
            ir.totalTalents   = 0;
            ir.unspentTalents = 0;
            ir.talentGroups   = 0;
            ir.activeTalentGroup = 0;
            ir.itemEntries    = items;
            ir.enchantIds     = {};
        }

        // Also cache for future talent-inspect cross-reference
        inspectedPlayerItemEntries_[guid] = items;

        // Trigger item queries for non-empty slots
        for (int s = 0; s < kGearSlots; ++s) {
            if (items[s] != 0) queryItemInfo(items[s], 0);
        }

        LOG_INFO("SMSG_INSPECT (Classic): ", playerName, " has gear in ",
                 std::count_if(items.begin(), items.end(),
                               [](uint32_t e) { return e != 0; }), "/19 slots");
        if (addonEventCallback_) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
            fireAddonEvent("INSPECT_READY", {guidBuf});
        }
    };
    // Same wire format as SMSG_COMPRESSED_MOVES: uint8 size + uint16 opcode + payload[]
    dispatchTable_[Opcode::SMSG_MULTIPLE_MOVES] = [this](network::Packet& packet) {
        // Same wire format as SMSG_COMPRESSED_MOVES: uint8 size + uint16 opcode + payload[]
        if (movementHandler_) movementHandler_->handleCompressedMoves(packet);
    };
    // Each sub-packet uses the standard WotLK server wire format:
    //   uint16_be subSize  (includes the 2-byte opcode; payload = subSize - 2)
    //   uint16_le subOpcode
    //   payload  (subSize - 2 bytes)
    dispatchTable_[Opcode::SMSG_MULTIPLE_PACKETS] = [this](network::Packet& packet) {
        // Each sub-packet uses the standard WotLK server wire format:
        //   uint16_be subSize  (includes the 2-byte opcode; payload = subSize - 2)
        //   uint16_le subOpcode
        //   payload  (subSize - 2 bytes)
        const auto& pdata = packet.getData();
        size_t dataLen = pdata.size();
        size_t pos = packet.getReadPos();
        static uint32_t multiPktWarnCount = 0;
        std::vector<network::Packet> subPackets;
        while (pos + 4 <= dataLen) {
            uint16_t subSize = static_cast<uint16_t>(
                (static_cast<uint16_t>(pdata[pos]) << 8) | pdata[pos + 1]);
            if (subSize < 2) break;
            size_t payloadLen = subSize - 2;
            if (pos + 4 + payloadLen > dataLen) {
                if (++multiPktWarnCount <= 10) {
                    LOG_WARNING("SMSG_MULTIPLE_PACKETS: sub-packet overruns buffer at pos=",
                                pos, " subSize=", subSize, " dataLen=", dataLen);
                }
                break;
            }
            uint16_t subOpcode = static_cast<uint16_t>(pdata[pos + 2]) |
                                 (static_cast<uint16_t>(pdata[pos + 3]) << 8);
            std::vector<uint8_t> subPayload(pdata.begin() + pos + 4,
                                            pdata.begin() + pos + 4 + payloadLen);
            subPackets.emplace_back(subOpcode, std::move(subPayload));
            pos += 4 + payloadLen;
        }
        for (auto it = subPackets.rbegin(); it != subPackets.rend(); ++it) {
            enqueueIncomingPacketFront(std::move(*it));
        }
        packet.skipAll();
    };
    // Recruit-A-Friend: a mentor is offering to grant you a level
    dispatchTable_[Opcode::SMSG_PROPOSE_LEVEL_GRANT] = [this](network::Packet& packet) {
        // Recruit-A-Friend: a mentor is offering to grant you a level
        if (packet.hasRemaining(8)) {
            uint64_t mentorGuid = packet.readUInt64();
            std::string mentorName;
            auto ent = entityManager.getEntity(mentorGuid);
            if (auto* unit = dynamic_cast<Unit*>(ent.get())) mentorName = unit->getName();
            if (mentorName.empty()) mentorName = lookupName(mentorGuid);
            addSystemChatMessage(mentorName.empty()
                ? "A player is offering to grant you a level."
                : (mentorName + " is offering to grant you a level."));
        }
        packet.skipAll();
    };
    // SMSG_REFER_A_FRIEND_EXPIRED — moved to SocialHandler::registerOpcodes
    // SMSG_REFER_A_FRIEND_FAILURE — moved to SocialHandler::registerOpcodes
    // SMSG_REPORT_PVP_AFK_RESULT — moved to SocialHandler::registerOpcodes
    dispatchTable_[Opcode::SMSG_RESPOND_INSPECT_ACHIEVEMENTS] = [this](network::Packet& packet) {
        loadAchievementNameCache();
        if (!packet.hasRemaining(1)) return;
        uint64_t inspectedGuid = packet.readPackedGuid();
        if (inspectedGuid == 0) { packet.skipAll(); return; }
        std::unordered_set<uint32_t> achievements;
        while (packet.hasRemaining(4)) {
            uint32_t id = packet.readUInt32();
            if (id == 0xFFFFFFFF) break;
            if (!packet.hasRemaining(4)) break;
            /*date*/ packet.readUInt32();
            achievements.insert(id);
        }
        while (packet.hasRemaining(4)) {
            uint32_t id = packet.readUInt32();
            if (id == 0xFFFFFFFF) break;
            if (!packet.hasRemaining(16)) break;
            packet.readUInt64(); packet.readUInt32(); packet.readUInt32();
        }
        inspectedPlayerAchievements_[inspectedGuid] = std::move(achievements);
        LOG_INFO("SMSG_RESPOND_INSPECT_ACHIEVEMENTS: guid=0x", std::hex, inspectedGuid, std::dec,
                 " achievements=", inspectedPlayerAchievements_[inspectedGuid].size());
    };
    dispatchTable_[Opcode::SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA] = [this](network::Packet& packet) {
        vehicleId_ = 0;  // Vehicle ride cancelled; clear UI
        packet.skipAll();
    };
    // uint32 type (0=normal, 1=heavy, 2=tired/restricted) + uint32 minutes played
    dispatchTable_[Opcode::SMSG_PLAY_TIME_WARNING] = [this](network::Packet& packet) {
        // uint32 type (0=normal, 1=heavy, 2=tired/restricted) + uint32 minutes played
        if (packet.hasRemaining(4)) {
            uint32_t warnType = packet.readUInt32();
            uint32_t minutesPlayed = (packet.hasRemaining(4))
                ? packet.readUInt32() : 0;
            const char* severity = (warnType >= 2) ? "[Tired] " : "[Play Time] ";
            char buf[128];
            if (minutesPlayed > 0) {
                uint32_t h = minutesPlayed / 60;
                uint32_t m = minutesPlayed % 60;
                if (h > 0)
                    std::snprintf(buf, sizeof(buf), "%sYou have been playing for %uh %um.", severity, h, m);
                else
                    std::snprintf(buf, sizeof(buf), "%sYou have been playing for %um.", severity, m);
            } else {
                std::snprintf(buf, sizeof(buf), "%sYou have been playing for a long time.", severity);
            }
            addSystemChatMessage(buf);
            addUIError(buf);
        }
    };
    // WotLK 3.3.5a format:
    //   uint64 mirrorGuid — GUID of the mirror image unit
    //   uint32 displayId  — display ID to render the image with
    //   uint8  raceId     — race of caster
    //   uint8  genderFlag — gender of caster
    //   uint8  classId    — class of caster
    //   uint64 casterGuid — GUID of the player who cast the spell
    //   Followed by equipped item display IDs (11 × uint32) if casterGuid != 0
    // Purpose: tells client how to render the image (same appearance as caster).
    // We parse the GUIDs so units render correctly via their existing display IDs.
    dispatchTable_[Opcode::SMSG_MIRRORIMAGE_DATA] = [this](network::Packet& packet) {
        // WotLK 3.3.5a format:
        //   uint64 mirrorGuid — GUID of the mirror image unit
        //   uint32 displayId  — display ID to render the image with
        //   uint8  raceId     — race of caster
        //   uint8  genderFlag — gender of caster
        //   uint8  classId    — class of caster
        //   uint64 casterGuid — GUID of the player who cast the spell
        //   Followed by equipped item display IDs (11 × uint32) if casterGuid != 0
        // Purpose: tells client how to render the image (same appearance as caster).
        // We parse the GUIDs so units render correctly via their existing display IDs.
        if (!packet.hasRemaining(8)) return;
        uint64_t mirrorGuid = packet.readUInt64();
        if (!packet.hasRemaining(4)) return;
        uint32_t displayId  = packet.readUInt32();
        if (!packet.hasRemaining(3)) return;
        /*uint8_t raceId   =*/ packet.readUInt8();
        /*uint8_t gender   =*/ packet.readUInt8();
        /*uint8_t classId  =*/ packet.readUInt8();
        // Apply display ID to the mirror image unit so it renders correctly
        if (mirrorGuid != 0 && displayId != 0) {
            auto entity = entityManager.getEntity(mirrorGuid);
            if (entity) {
                auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
                if (unit && unit->getDisplayId() == 0)
                    unit->setDisplayId(displayId);
            }
        }
        LOG_DEBUG("SMSG_MIRRORIMAGE_DATA: mirrorGuid=0x", std::hex, mirrorGuid,
                  " displayId=", std::dec, displayId);
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 zoneId + uint64 expireUnixTime (seconds)
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_ENTRY_INVITE] = [this](network::Packet& packet) {
        // uint64 battlefieldGuid + uint32 zoneId + uint64 expireUnixTime (seconds)
        if (!packet.hasRemaining(20)) {
            packet.skipAll(); return;
        }
        uint64_t bfGuid    = packet.readUInt64();
        uint32_t bfZoneId  = packet.readUInt32();
        uint64_t expireTime = packet.readUInt64();
        (void)bfGuid; (void)expireTime;
        // Store the invitation so the UI can show a prompt
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfZoneId;
        char buf[128];
        std::string bfZoneName = getAreaName(bfZoneId);
        if (!bfZoneName.empty())
            std::snprintf(buf, sizeof(buf),
                "You are invited to the outdoor battlefield in %s. Click to enter.",
                bfZoneName.c_str());
        else
            std::snprintf(buf, sizeof(buf),
                "You are invited to the outdoor battlefield in zone %u. Click to enter.",
                bfZoneId);
        addSystemChatMessage(buf);
        LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTRY_INVITE: zoneId=", bfZoneId);
    };
    // uint64 battlefieldGuid + uint8 isSafe (1=pvp zones enabled) + uint8 onQueue
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_ENTERED] = [this](network::Packet& packet) {
        // uint64 battlefieldGuid + uint8 isSafe (1=pvp zones enabled) + uint8 onQueue
        if (packet.hasRemaining(8)) {
            uint64_t bfGuid2 = packet.readUInt64();
            (void)bfGuid2;
            uint8_t isSafe  = (packet.hasRemaining(1)) ? packet.readUInt8() : 0;
            uint8_t onQueue = (packet.hasRemaining(1)) ? packet.readUInt8() : 0;
            bfMgrInvitePending_ = false;
            bfMgrActive_        = true;
            addSystemChatMessage(isSafe ? "You are in the battlefield zone (safe area)."
                                        : "You have entered the battlefield!");
            if (onQueue) addSystemChatMessage("You are in the battlefield queue.");
            LOG_INFO("SMSG_BATTLEFIELD_MGR_ENTERED: isSafe=", static_cast<int>(isSafe), " onQueue=", static_cast<int>(onQueue));
        }
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 battlefieldId + uint64 expireTime
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_INVITE] = [this](network::Packet& packet) {
        // uint64 battlefieldGuid + uint32 battlefieldId + uint64 expireTime
        if (!packet.hasRemaining(20)) {
            packet.skipAll(); return;
        }
        uint64_t bfGuid3   = packet.readUInt64();
        uint32_t bfId      = packet.readUInt32();
        uint64_t expTime   = packet.readUInt64();
        (void)bfGuid3; (void)expTime;
        bfMgrInvitePending_ = true;
        bfMgrZoneId_        = bfId;
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "A spot has opened in the battlefield queue (battlefield %u).", bfId);
        addSystemChatMessage(buf);
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_INVITE: bfId=", bfId);
    };
    // uint32 battlefieldId + uint32 teamId + uint8 accepted + uint8 loggingEnabled + uint8 result
    // result: 0=queued, 1=not_in_group, 2=too_high_level, 3=too_low_level,
    //         4=in_cooldown, 5=queued_other_bf, 6=bf_full
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE] = [this](network::Packet& packet) {
        // uint32 battlefieldId + uint32 teamId + uint8 accepted + uint8 loggingEnabled + uint8 result
        // result: 0=queued, 1=not_in_group, 2=too_high_level, 3=too_low_level,
        //         4=in_cooldown, 5=queued_other_bf, 6=bf_full
        if (!packet.hasRemaining(11)) {
            packet.skipAll(); return;
        }
        uint32_t bfId2    = packet.readUInt32();
        /*uint32_t teamId =*/ packet.readUInt32();
        uint8_t accepted  = packet.readUInt8();
        /*uint8_t logging =*/ packet.readUInt8();
        uint8_t result    = packet.readUInt8();
        (void)bfId2;
        if (accepted) {
            addSystemChatMessage("You have joined the battlefield queue.");
        } else {
            static const char* kBfQueueErrors[] = {
                "Queued for battlefield.", "Not in a group.", "Level too high.",
                "Level too low.", "Battlefield in cooldown.", "Already queued for another battlefield.",
                "Battlefield is full."
            };
            const char* msg = (result < 7) ? kBfQueueErrors[result]
                                           : "Battlefield queue request failed.";
            addSystemChatMessage(std::string("Battlefield: ") + msg);
        }
        LOG_INFO("SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE: accepted=", static_cast<int>(accepted),
                 " result=", static_cast<int>(result));
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint8 remove
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_EJECT_PENDING] = [this](network::Packet& packet) {
        // uint64 battlefieldGuid + uint8 remove
        if (packet.hasRemaining(9)) {
            uint64_t bfGuid4 = packet.readUInt64();
            uint8_t  remove  = packet.readUInt8();
            (void)bfGuid4;
            if (remove) {
                addSystemChatMessage("You will be removed from the battlefield shortly.");
            }
            LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECT_PENDING: remove=", static_cast<int>(remove));
        }
        packet.skipAll();
    };
    // uint64 battlefieldGuid + uint32 reason + uint32 battleStatus + uint8 relocated
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_EJECTED] = [this](network::Packet& packet) {
        // uint64 battlefieldGuid + uint32 reason + uint32 battleStatus + uint8 relocated
        if (packet.hasRemaining(17)) {
            uint64_t bfGuid5    = packet.readUInt64();
            uint32_t reason     = packet.readUInt32();
            /*uint32_t status  =*/ packet.readUInt32();
            uint8_t relocated   = packet.readUInt8();
            (void)bfGuid5;
            static const char* kEjectReasons[] = {
                "Removed from battlefield.", "Transported from battlefield.",
                "Left battlefield voluntarily.", "Offline.",
            };
            const char* msg = (reason < 4) ? kEjectReasons[reason]
                                           : "You have been ejected from the battlefield.";
            addSystemChatMessage(msg);
            if (relocated) addSystemChatMessage("You have been relocated outside the battlefield.");
            LOG_INFO("SMSG_BATTLEFIELD_MGR_EJECTED: reason=", reason, " relocated=", static_cast<int>(relocated));
        }
        bfMgrActive_        = false;
        bfMgrInvitePending_ = false;
        packet.skipAll();
    };
    // uint32 oldState + uint32 newState
    // States: 0=Waiting, 1=Starting, 2=InProgress, 3=Ending, 4=Cooldown
    dispatchTable_[Opcode::SMSG_BATTLEFIELD_MGR_STATE_CHANGE] = [this](network::Packet& packet) {
        // uint32 oldState + uint32 newState
        // States: 0=Waiting, 1=Starting, 2=InProgress, 3=Ending, 4=Cooldown
        if (packet.hasRemaining(8)) {
            /*uint32_t oldState =*/ packet.readUInt32();
            uint32_t newState   = packet.readUInt32();
            static const char* kBfStates[] = {
                "waiting", "starting", "in progress", "ending", "in cooldown"
            };
            const char* stateStr = (newState < 5) ? kBfStates[newState] : "unknown state";
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Battlefield is now %s.", stateStr);
            addSystemChatMessage(buf);
            LOG_INFO("SMSG_BATTLEFIELD_MGR_STATE_CHANGE: newState=", newState);
        }
        packet.skipAll();
    };
    // uint32 numPending — number of unacknowledged calendar invites
    dispatchTable_[Opcode::SMSG_CALENDAR_SEND_NUM_PENDING] = [this](network::Packet& packet) {
        // uint32 numPending — number of unacknowledged calendar invites
        if (packet.hasRemaining(4)) {
            uint32_t numPending = packet.readUInt32();
            calendarPendingInvites_ = numPending;
            if (numPending > 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                    "You have %u pending calendar invite%s.",
                    numPending, numPending == 1 ? "" : "s");
                addSystemChatMessage(buf);
            }
            LOG_DEBUG("SMSG_CALENDAR_SEND_NUM_PENDING: ", numPending, " pending invites");
        }
    };
    // uint32 command + uint8 result + cstring info
    // result 0 = success; non-zero = error code
    // command values: 0=add,1=get,2=guild_filter,3=arena_team,4=update,5=remove,
    //                 6=copy,7=invite,8=rsvp,9=remove_invite,10=status,11=moderator_status
    dispatchTable_[Opcode::SMSG_CALENDAR_COMMAND_RESULT] = [this](network::Packet& packet) {
        // uint32 command + uint8 result + cstring info
        // result 0 = success; non-zero = error code
        // command values: 0=add,1=get,2=guild_filter,3=arena_team,4=update,5=remove,
        //                 6=copy,7=invite,8=rsvp,9=remove_invite,10=status,11=moderator_status
        if (!packet.hasRemaining(5)) {
            packet.skipAll(); return;
        }
        /*uint32_t command =*/ packet.readUInt32();
        uint8_t result    = packet.readUInt8();
        std::string info  = (packet.hasData()) ? packet.readString() : "";
        if (result != 0) {
            // Map common calendar error codes to friendly strings
            static const char* kCalendarErrors[] = {
                "",
                "Calendar: Internal error.",           // 1 = CALENDAR_ERROR_INTERNAL
                "Calendar: Guild event limit reached.",// 2
                "Calendar: Event limit reached.",      // 3
                "Calendar: You cannot invite that player.", // 4
                "Calendar: No invites remaining.",     // 5
                "Calendar: Invalid date.",             // 6
                "Calendar: Cannot invite yourself.",   // 7
                "Calendar: Cannot modify this event.", // 8
                "Calendar: Not invited.",              // 9
                "Calendar: Already invited.",          // 10
                "Calendar: Player not found.",         // 11
                "Calendar: Not enough focus.",         // 12
                "Calendar: Event locked.",             // 13
                "Calendar: Event deleted.",            // 14
                "Calendar: Not a moderator.",          // 15
            };
            const char* errMsg = (result < 16) ? kCalendarErrors[result]
                                               : "Calendar: Command failed.";
            if (errMsg && errMsg[0] != '\0') addSystemChatMessage(errMsg);
            else if (!info.empty()) addSystemChatMessage("Calendar: " + info);
        }
        packet.skipAll();
    };
    // Rich notification: eventId(8) + title(cstring) + eventTime(8) + flags(4) +
    //                   eventType(1) + dungeonId(4) + inviteId(8) + status(1) + rank(1) +
    //                   isGuildEvent(1) + inviterGuid(8)
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_INVITE_ALERT] = [this](network::Packet& packet) {
        // Rich notification: eventId(8) + title(cstring) + eventTime(8) + flags(4) +
        //                   eventType(1) + dungeonId(4) + inviteId(8) + status(1) + rank(1) +
        //                   isGuildEvent(1) + inviterGuid(8)
        if (!packet.hasRemaining(9)) {
            packet.skipAll(); return;
        }
        /*uint64_t eventId =*/ packet.readUInt64();
        std::string title = (packet.hasData()) ? packet.readString() : "";
        packet.skipAll(); // consume remaining fields
        if (!title.empty()) {
            addSystemChatMessage("Calendar invite: " + title);
        } else {
            addSystemChatMessage("You have a new calendar invite.");
        }
        if (calendarPendingInvites_ < 255) ++calendarPendingInvites_;
        LOG_INFO("SMSG_CALENDAR_EVENT_INVITE_ALERT: title='", title, "'");
    };
    // Sent when an event invite's RSVP status changes for the local player
    // Format: inviteId(8) + eventId(8) + eventType(1) + flags(4) +
    //         inviteTime(8) + status(1) + rank(1) + isGuildEvent(1) + title(cstring)
    dispatchTable_[Opcode::SMSG_CALENDAR_EVENT_STATUS] = [this](network::Packet& packet) {
        // Sent when an event invite's RSVP status changes for the local player
        // Format: inviteId(8) + eventId(8) + eventType(1) + flags(4) +
        //         inviteTime(8) + status(1) + rank(1) + isGuildEvent(1) + title(cstring)
        if (!packet.hasRemaining(31)) {
            packet.skipAll(); return;
        }
        /*uint64_t inviteId =*/ packet.readUInt64();
        /*uint64_t eventId  =*/ packet.readUInt64();
        /*uint8_t  evType   =*/ packet.readUInt8();
        /*uint32_t flags    =*/ packet.readUInt32();
        /*uint64_t invTime  =*/ packet.readUInt64();
        uint8_t status     = packet.readUInt8();
        /*uint8_t rank      =*/ packet.readUInt8();
        /*uint8_t isGuild   =*/ packet.readUInt8();
        std::string evTitle = (packet.hasData()) ? packet.readString() : "";
        // status: 0=Invited,1=Accepted,2=Declined,3=Confirmed,4=Out,5=Standby,6=SignedUp,7=Not Signed Up,8=Tentative
        static const char* kRsvpStatus[] = {
            "invited", "accepted", "declined", "confirmed",
            "out", "on standby", "signed up", "not signed up", "tentative"
        };
        const char* statusStr = (status < 9) ? kRsvpStatus[status] : "unknown";
        if (!evTitle.empty()) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "Calendar event '%s': your RSVP is %s.",
                          evTitle.c_str(), statusStr);
            addSystemChatMessage(buf);
        }
        packet.skipAll();
    };
    // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty + uint64 resetTime
    dispatchTable_[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_ADDED] = [this](network::Packet& packet) {
        // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty + uint64 resetTime
        if (packet.hasRemaining(28)) {
            /*uint64_t inviteId =*/ packet.readUInt64();
            /*uint64_t eventId  =*/ packet.readUInt64();
            uint32_t mapId     = packet.readUInt32();
            uint32_t difficulty = packet.readUInt32();
            /*uint64_t resetTime =*/ packet.readUInt64();
            std::string mapLabel = getMapName(mapId);
            if (mapLabel.empty()) mapLabel = "map #" + std::to_string(mapId);
            static const char* kDiff[] = {"Normal","Heroic","25-Man","25-Man Heroic"};
            const char* diffStr = (difficulty < 4) ? kDiff[difficulty] : nullptr;
            std::string msg = "Calendar: Raid lockout added for " + mapLabel;
            if (diffStr) msg += std::string(" (") + diffStr + ")";
            msg += '.';
            addSystemChatMessage(msg);
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_ADDED: mapId=", mapId, " difficulty=", difficulty);
        }
        packet.skipAll();
    };
    // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty
    dispatchTable_[Opcode::SMSG_CALENDAR_RAID_LOCKOUT_REMOVED] = [this](network::Packet& packet) {
        // uint64 inviteId + uint64 eventId + uint32 mapId + uint32 difficulty
        if (packet.hasRemaining(20)) {
            /*uint64_t inviteId =*/ packet.readUInt64();
            /*uint64_t eventId  =*/ packet.readUInt64();
            uint32_t mapId     = packet.readUInt32();
            uint32_t difficulty = packet.readUInt32();
            std::string mapLabel = getMapName(mapId);
            if (mapLabel.empty()) mapLabel = "map #" + std::to_string(mapId);
            static const char* kDiff[] = {"Normal","Heroic","25-Man","25-Man Heroic"};
            const char* diffStr = (difficulty < 4) ? kDiff[difficulty] : nullptr;
            std::string msg = "Calendar: Raid lockout removed for " + mapLabel;
            if (diffStr) msg += std::string(" (") + diffStr + ")";
            msg += '.';
            addSystemChatMessage(msg);
            LOG_DEBUG("SMSG_CALENDAR_RAID_LOCKOUT_REMOVED: mapId=", mapId,
                      " difficulty=", difficulty);
        }
        packet.skipAll();
    };
    // uint32 unixTime — server's current unix timestamp; use to sync gameTime_
    dispatchTable_[Opcode::SMSG_SERVERTIME] = [this](network::Packet& packet) {
        // uint32 unixTime — server's current unix timestamp; use to sync gameTime_
        if (packet.hasRemaining(4)) {
            uint32_t srvTime = packet.readUInt32();
            if (srvTime > 0) {
                gameTime_ = static_cast<float>(srvTime);
                LOG_DEBUG("SMSG_SERVERTIME: serverTime=", srvTime);
            }
        }
    };
    // uint64 kickerGuid + uint32 kickReasonType + null-terminated reason string
    // kickReasonType: 0=other, 1=afk, 2=vote kick
    dispatchTable_[Opcode::SMSG_KICK_REASON] = [this](network::Packet& packet) {
        // uint64 kickerGuid + uint32 kickReasonType + null-terminated reason string
        // kickReasonType: 0=other, 1=afk, 2=vote kick
        if (!packet.hasRemaining(12)) {
            packet.skipAll();
            return;
        }
        uint64_t kickerGuid   = packet.readUInt64();
        uint32_t reasonType   = packet.readUInt32();
        std::string reason;
        if (packet.hasData())
            reason = packet.readString();
        (void)kickerGuid;
        (void)reasonType;
        std::string msg = "You have been removed from the group.";
        if (!reason.empty())
            msg = "You have been removed from the group: " + reason;
        else if (reasonType == 1)
            msg = "You have been removed from the group for being AFK.";
        else if (reasonType == 2)
            msg = "You have been removed from the group by vote.";
        addSystemChatMessage(msg);
        addUIError(msg);
        LOG_INFO("SMSG_KICK_REASON: reasonType=", reasonType,
                 " reason='", reason, "'");
    };
    // uint32 throttleMs — rate-limited group action; notify the player
    dispatchTable_[Opcode::SMSG_GROUPACTION_THROTTLED] = [this](network::Packet& packet) {
        // uint32 throttleMs — rate-limited group action; notify the player
        if (packet.hasRemaining(4)) {
            uint32_t throttleMs = packet.readUInt32();
            char buf[128];
            if (throttleMs > 0) {
                std::snprintf(buf, sizeof(buf),
                              "Group action throttled. Please wait %.1f seconds.",
                              throttleMs / 1000.0f);
            } else {
                std::snprintf(buf, sizeof(buf), "Group action throttled.");
            }
            addSystemChatMessage(buf);
            LOG_DEBUG("SMSG_GROUPACTION_THROTTLED: throttleMs=", throttleMs);
        }
    };
    // WotLK 3.3.5a: uint32 ticketId + string subject + string body + uint32 count
    //   per count: string responseText
    dispatchTable_[Opcode::SMSG_GMRESPONSE_RECEIVED] = [this](network::Packet& packet) {
        // WotLK 3.3.5a: uint32 ticketId + string subject + string body + uint32 count
        //   per count: string responseText
        if (!packet.hasRemaining(4)) {
            packet.skipAll();
            return;
        }
        uint32_t ticketId = packet.readUInt32();
        std::string subject;
        std::string body;
        if (packet.hasData()) subject = packet.readString();
        if (packet.hasData()) body    = packet.readString();
        uint32_t responseCount = 0;
        if (packet.hasRemaining(4))
            responseCount = packet.readUInt32();
        std::string responseText;
        for (uint32_t i = 0; i < responseCount && i < 10; ++i) {
            if (packet.hasData()) {
                std::string t = packet.readString();
                if (i == 0) responseText = t;
            }
        }
        (void)ticketId;
        std::string msg;
        if (!responseText.empty())
            msg = "[GM Response] " + responseText;
        else if (!body.empty())
            msg = "[GM Response] " + body;
        else if (!subject.empty())
            msg = "[GM Response] " + subject;
        else
            msg = "[GM Response] Your ticket has been answered.";
        addSystemChatMessage(msg);
        addUIError(msg);
        LOG_INFO("SMSG_GMRESPONSE_RECEIVED: ticketId=", ticketId,
                 " subject='", subject, "'");
    };
    // uint32 ticketId + uint8 status (1=open, 2=surveyed, 3=need_more_help)
    dispatchTable_[Opcode::SMSG_GMRESPONSE_STATUS_UPDATE] = [this](network::Packet& packet) {
        // uint32 ticketId + uint8 status (1=open, 2=surveyed, 3=need_more_help)
        if (packet.hasRemaining(5)) {
            uint32_t ticketId = packet.readUInt32();
            uint8_t  status   = packet.readUInt8();
            const char* statusStr = (status == 1) ? "open"
                                  : (status == 2) ? "answered"
                                  : (status == 3) ? "needs more info"
                                  : "updated";
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "[GM Ticket #%u] Status: %s.", ticketId, statusStr);
            addSystemChatMessage(buf);
            LOG_DEBUG("SMSG_GMRESPONSE_STATUS_UPDATE: ticketId=", ticketId,
                      " status=", static_cast<int>(status));
        }
    };
    // GM ticket status (new/updated); no ticket UI yet
    registerSkipHandler(Opcode::SMSG_GM_TICKET_STATUS_UPDATE);
    // Client uses this outbound; treat inbound variant as no-op for robustness.
    registerSkipHandler(Opcode::MSG_MOVE_WORLDPORT_ACK);
    // Observed custom server packet (8 bytes). Safe-consume for now.
    registerSkipHandler(Opcode::MSG_MOVE_TIME_SKIPPED);
    // loggingOut_ already cleared by cancelLogout(); this is server's confirmation
    registerSkipHandler(Opcode::SMSG_LOGOUT_CANCEL_ACK);
    // These packets are not damage-shield events. Consume them without
    // synthesizing reflected damage entries or misattributing GUIDs.
    registerSkipHandler(Opcode::SMSG_AURACASTLOG);
    // These packets are not damage-shield events. Consume them without
    // synthesizing reflected damage entries or misattributing GUIDs.
    registerSkipHandler(Opcode::SMSG_SPELLBREAKLOG);
    // Consume silently — informational, no UI action needed
    registerSkipHandler(Opcode::SMSG_ITEM_REFUND_INFO_RESPONSE);
    // Consume silently — informational, no UI action needed
    registerSkipHandler(Opcode::SMSG_LOOT_LIST);
    // Same format as LOCKOUT_ADDED; consume
    registerSkipHandler(Opcode::SMSG_CALENDAR_RAID_LOCKOUT_UPDATED);
    // Consume — remaining server notifications not yet parsed
    for (auto op : {
        Opcode::SMSG_AFK_MONITOR_INFO_RESPONSE,
        Opcode::SMSG_AUCTION_LIST_PENDING_SALES,
        Opcode::SMSG_AVAILABLE_VOICE_CHANNEL,
        Opcode::SMSG_CALENDAR_ARENA_TEAM,
        Opcode::SMSG_CALENDAR_CLEAR_PENDING_ACTION,
        Opcode::SMSG_CALENDAR_EVENT_INVITE,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_NOTES,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_NOTES_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_REMOVED_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_INVITE_STATUS_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_MODERATOR_STATUS_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_REMOVED_ALERT,
        Opcode::SMSG_CALENDAR_EVENT_UPDATED_ALERT,
        Opcode::SMSG_CALENDAR_FILTER_GUILD,
        Opcode::SMSG_CALENDAR_SEND_CALENDAR,
        Opcode::SMSG_CALENDAR_SEND_EVENT,
        Opcode::SMSG_CHEAT_DUMP_ITEMS_DEBUG_ONLY_RESPONSE,
        Opcode::SMSG_CHEAT_DUMP_ITEMS_DEBUG_ONLY_RESPONSE_WRITE_FILE,
        Opcode::SMSG_CHEAT_PLAYER_LOOKUP,
        Opcode::SMSG_CHECK_FOR_BOTS,
        Opcode::SMSG_COMMENTATOR_GET_PLAYER_INFO,
        Opcode::SMSG_COMMENTATOR_MAP_INFO,
        Opcode::SMSG_COMMENTATOR_PLAYER_INFO,
        Opcode::SMSG_COMMENTATOR_SKIRMISH_QUEUE_RESULT1,
        Opcode::SMSG_COMMENTATOR_SKIRMISH_QUEUE_RESULT2,
        Opcode::SMSG_COMMENTATOR_STATE_CHANGED,
        Opcode::SMSG_COOLDOWN_CHEAT,
        Opcode::SMSG_DANCE_QUERY_RESPONSE,
        Opcode::SMSG_DBLOOKUP,
        Opcode::SMSG_DEBUGAURAPROC,
        Opcode::SMSG_DEBUG_AISTATE,
        Opcode::SMSG_DEBUG_LIST_TARGETS,
        Opcode::SMSG_DEBUG_SERVER_GEO,
        Opcode::SMSG_DUMP_OBJECTS_DATA,
        Opcode::SMSG_FORCEACTIONSHOW,
        Opcode::SMSG_GM_PLAYER_INFO,
        Opcode::SMSG_GODMODE,
        Opcode::SMSG_IGNORE_DIMINISHING_RETURNS_CHEAT,
        Opcode::SMSG_IGNORE_REQUIREMENTS_CHEAT,
        Opcode::SMSG_INVALIDATE_DANCE,
        Opcode::SMSG_LFG_PENDING_INVITE,
        Opcode::SMSG_LFG_PENDING_MATCH,
        Opcode::SMSG_LFG_PENDING_MATCH_DONE,
        Opcode::SMSG_LFG_UPDATE,
        Opcode::SMSG_LFG_UPDATE_LFG,
        Opcode::SMSG_LFG_UPDATE_LFM,
        Opcode::SMSG_LFG_UPDATE_QUEUED,
        Opcode::SMSG_MOVE_CHARACTER_CHEAT,
        Opcode::SMSG_NOTIFY_DANCE,
        Opcode::SMSG_NOTIFY_DEST_LOC_SPELL_CAST,
        Opcode::SMSG_PETGODMODE,
        Opcode::SMSG_PET_UPDATE_COMBO_POINTS,
        Opcode::SMSG_PLAYER_SKINNED,
        Opcode::SMSG_PLAY_DANCE,
        Opcode::SMSG_PROFILEDATA_RESPONSE,
        Opcode::SMSG_PVP_QUEUE_STATS,
        Opcode::SMSG_QUERY_OBJECT_POSITION,
        Opcode::SMSG_QUERY_OBJECT_ROTATION,
        Opcode::SMSG_REDIRECT_CLIENT,
        Opcode::SMSG_RESET_RANGED_COMBAT_TIMER,
        Opcode::SMSG_SEND_ALL_COMBAT_LOG,
        Opcode::SMSG_SET_EXTRA_AURA_INFO_NEED_UPDATE,
        Opcode::SMSG_SET_PLAYER_DECLINED_NAMES_RESULT,
        Opcode::SMSG_SET_PROJECTILE_POSITION,
        Opcode::SMSG_SPELL_CHANCE_RESIST_PUSHBACK,
        Opcode::SMSG_SPELL_UPDATE_CHAIN_TARGETS,
        Opcode::SMSG_STOP_DANCE,
        Opcode::SMSG_TEST_DROP_RATE_RESULT,
        Opcode::SMSG_UPDATE_ACCOUNT_DATA,
        Opcode::SMSG_UPDATE_ACCOUNT_DATA_COMPLETE,
        Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP,
        Opcode::SMSG_UPDATE_LAST_INSTANCE,
        Opcode::SMSG_VOICESESSION_FULL,
        Opcode::SMSG_VOICE_CHAT_STATUS,
        Opcode::SMSG_VOICE_PARENTAL_CONTROLS,
        Opcode::SMSG_VOICE_SESSION_ADJUST_PRIORITY,
        Opcode::SMSG_VOICE_SESSION_ENABLE,
        Opcode::SMSG_VOICE_SESSION_LEAVE,
        Opcode::SMSG_VOICE_SESSION_ROSTER_UPDATE,
        Opcode::SMSG_VOICE_SET_TALKER_MUTED
    }) { registerSkipHandler(op); }

    // -----------------------------------------------------------------------
    // Domain handler registrations (override duplicate entries above)
    // -----------------------------------------------------------------------
    chatHandler_->registerOpcodes(dispatchTable_);
    movementHandler_->registerOpcodes(dispatchTable_);
    combatHandler_->registerOpcodes(dispatchTable_);
    spellHandler_->registerOpcodes(dispatchTable_);
    inventoryHandler_->registerOpcodes(dispatchTable_);
    socialHandler_->registerOpcodes(dispatchTable_);
    questHandler_->registerOpcodes(dispatchTable_);
    wardenHandler_->registerOpcodes(dispatchTable_);
}

void GameHandler::handlePacket(network::Packet& packet) {
    if (packet.getSize() < 1) {
        LOG_DEBUG("Received empty world packet (ignored)");
        return;
    }

    uint16_t opcode = packet.getOpcode();

    try {

    const bool allowVanillaAliases = isPreWotlk();

    // Vanilla compatibility aliases:
    // - 0x006B: can be SMSG_COMPRESSED_MOVES on some vanilla-family servers
    //           and SMSG_WEATHER on others
    // - 0x0103: SMSG_PLAY_MUSIC (some vanilla-family servers)
    //
    // We gate these by payload shape so expansion-native mappings remain intact.
    if (allowVanillaAliases && opcode == 0x006B) {
        // Try compressed movement batch first:
        // [u8 subSize][u16 subOpcode][subPayload...] ...
        // where subOpcode is typically SMSG_MONSTER_MOVE / SMSG_MONSTER_MOVE_TRANSPORT.
        const auto& data = packet.getData();
        if (packet.getReadPos() + 3 <= data.size()) {
            size_t pos = packet.getReadPos();
            uint8_t subSize = data[pos];
            if (subSize >= 2 && pos + 1 + subSize <= data.size()) {
                uint16_t subOpcode = static_cast<uint16_t>(data[pos + 1]) |
                                     (static_cast<uint16_t>(data[pos + 2]) << 8);
                uint16_t monsterMoveWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE);
                uint16_t monsterMoveTransportWire = wireOpcode(Opcode::SMSG_MONSTER_MOVE_TRANSPORT);
                if ((monsterMoveWire != 0xFFFF && subOpcode == monsterMoveWire) ||
                    (monsterMoveTransportWire != 0xFFFF && subOpcode == monsterMoveTransportWire)) {
                    LOG_INFO("Opcode 0x006B interpreted as SMSG_COMPRESSED_MOVES (subOpcode=0x",
                             std::hex, subOpcode, std::dec, ")");
                    if (movementHandler_) movementHandler_->handleCompressedMoves(packet);
                    return;
                }
            }
        }

        // Expected weather payload: uint32 weatherType, float intensity, uint8 abrupt
        if (packet.hasRemaining(9)) {
            uint32_t wType = packet.readUInt32();
            float wIntensity = packet.readFloat();
            uint8_t abrupt = packet.readUInt8();
            bool plausibleWeather =
                (wType <= 3) &&
                std::isfinite(wIntensity) &&
                (wIntensity >= 0.0f && wIntensity <= 1.5f) &&
                (abrupt <= 1);
            if (plausibleWeather) {
                weatherType_ = wType;
                weatherIntensity_ = wIntensity;
                const char* typeName =
                    (wType == 1) ? "Rain" :
                    (wType == 2) ? "Snow" :
                    (wType == 3) ? "Storm" : "Clear";
                LOG_INFO("Weather changed (0x006B alias): type=", wType,
                         " (", typeName, "), intensity=", wIntensity,
                         ", abrupt=", static_cast<int>(abrupt));
                return;
            }
            // Not weather-shaped: rewind and fall through to normal opcode table handling.
            packet.setReadPos(0);
        }
    } else if (allowVanillaAliases && opcode == 0x0103) {
        // Expected play-music payload: uint32 sound/music id
        if (packet.getRemainingSize() == 4) {
            uint32_t soundId = packet.readUInt32();
            LOG_INFO("SMSG_PLAY_MUSIC (0x0103 alias): soundId=", soundId);
            if (playMusicCallback_) playMusicCallback_(soundId);
            return;
        }
    } else if (opcode == 0x0480) {
        // Observed on this WotLK profile immediately after CMSG_BUYBACK_ITEM.
        // Treat as vendor/buyback transaction result (7-byte payload on this core).
        if (packet.hasRemaining(7)) {
            uint8_t opType = packet.readUInt8();
            uint8_t resultCode = packet.readUInt8();
            uint8_t slotOrCount = packet.readUInt8();
            uint32_t itemId = packet.readUInt32();
            LOG_INFO("Vendor txn result (0x480): opType=", static_cast<int>(opType),
                     " result=", static_cast<int>(resultCode),
                     " slot/count=", static_cast<int>(slotOrCount),
                     " itemId=", itemId,
                     " pendingBuybackSlot=", pendingBuybackSlot_,
                     " pendingBuyItemId=", pendingBuyItemId_,
                     " pendingBuyItemSlot=", pendingBuyItemSlot_);

            if (pendingBuybackSlot_ >= 0) {
                if (resultCode == 0) {
                    // Success: remove the bought-back slot from our local UI cache.
                    if (pendingBuybackSlot_ < static_cast<int>(buybackItems_.size())) {
                        buybackItems_.erase(buybackItems_.begin() + pendingBuybackSlot_);
                    }
                } else {
                    const char* msg = "Buyback failed.";
                    // Best-effort mapping; keep raw code visible for unknowns.
                    switch (resultCode) {
                        case 2: msg = "Buyback failed: not enough money."; break;
                        case 4: msg = "Buyback failed: vendor too far away."; break;
                        case 5: msg = "Buyback failed: item unavailable."; break;
                        case 6: msg = "Buyback failed: inventory full."; break;
                        case 8: msg = "Buyback failed: requirements not met."; break;
                        default: break;
                    }
                    addSystemChatMessage(std::string(msg) + " (code " + std::to_string(resultCode) + ")");
                }
                pendingBuybackSlot_ = -1;
                pendingBuybackWireSlot_ = 0;

                // Refresh vendor list so UI state stays in sync after buyback result.
                if (currentVendorItems.vendorGuid != 0 && socket && state == WorldState::IN_WORLD) {
                    auto pkt = ListInventoryPacket::build(currentVendorItems.vendorGuid);
                    socket->send(pkt);
                }
            } else if (pendingBuyItemId_ != 0) {
                if (resultCode != 0) {
                    const char* msg = "Purchase failed.";
                    switch (resultCode) {
                        case 2: msg = "Purchase failed: not enough money."; break;
                        case 4: msg = "Purchase failed: vendor too far away."; break;
                        case 5: msg = "Purchase failed: item sold out."; break;
                        case 6: msg = "Purchase failed: inventory full."; break;
                        case 8: msg = "Purchase failed: requirements not met."; break;
                        default: break;
                    }
                    addSystemChatMessage(std::string(msg) + " (code " + std::to_string(resultCode) + ")");
                }
                pendingBuyItemId_ = 0;
                pendingBuyItemSlot_ = 0;
            }
            return;
        }
    } else if (opcode == 0x046A) {
        // Server-specific vendor/buyback state packet (observed 25-byte records).
        // Consume to keep stream aligned; currently not used for gameplay logic.
        if (packet.hasRemaining(25)) {
            packet.setReadPos(packet.getReadPos() + 25);
            return;
        }
    }

    auto preLogicalOp = opcodeTable_.fromWire(opcode);
    if (wardenGateSeen_ && (!preLogicalOp || *preLogicalOp != Opcode::SMSG_WARDEN_DATA)) {
        ++wardenPacketsAfterGate_;
    }
    if (preLogicalOp && isAuthCharPipelineOpcode(*preLogicalOp)) {
        LOG_DEBUG("AUTH/CHAR RX opcode=0x", std::hex, opcode, std::dec,
                 " state=", worldStateName(state),
                 " size=", packet.getSize());
    }

    LOG_DEBUG("Received world packet: opcode=0x", std::hex, opcode, std::dec,
              " size=", packet.getSize(), " bytes");

    // Translate wire opcode to logical opcode via expansion table
    auto logicalOp = opcodeTable_.fromWire(opcode);

    if (!logicalOp) {
        static std::unordered_set<uint16_t> loggedUnknownWireOpcodes;
        if (loggedUnknownWireOpcodes.insert(opcode).second) {
            LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec,
                        " state=", static_cast<int>(state),
                        " size=", packet.getSize());
        }
        return;
    }

    // Dispatch via the opcode handler table
    auto it = dispatchTable_.find(*logicalOp);
    if (it != dispatchTable_.end()) {
        it->second(packet);
    } else {
        // In pre-world states we need full visibility (char create/login handshakes).
        // In-world we keep de-duplication to avoid heavy log I/O in busy areas.
        if (state != WorldState::IN_WORLD) {
            static std::unordered_set<uint32_t> loggedUnhandledByState;
            const uint32_t key = (static_cast<uint32_t>(static_cast<uint8_t>(state)) << 16) |
                                 static_cast<uint32_t>(opcode);
            if (loggedUnhandledByState.insert(key).second) {
                LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec,
                            " state=", static_cast<int>(state),
                            " size=", packet.getSize());
                const auto& data = packet.getData();
                std::string hex;
                size_t limit = std::min<size_t>(data.size(), 48);
                hex.reserve(limit * 3);
                for (size_t i = 0; i < limit; ++i) {
                    char b[4];
                    snprintf(b, sizeof(b), "%02x ", data[i]);
                    hex += b;
                }
                LOG_INFO("Unhandled opcode payload hex (first ", limit, " bytes): ", hex);
            }
        } else {
            static std::unordered_set<uint16_t> loggedUnhandledOpcodes;
            if (loggedUnhandledOpcodes.insert(static_cast<uint16_t>(opcode)).second) {
                LOG_WARNING("Unhandled world opcode: 0x", std::hex, opcode, std::dec);
            }
        }
    }
    } catch (const std::bad_alloc& e) {
        LOG_ERROR("OOM while handling world opcode=0x", std::hex, opcode, std::dec,
                  " state=", worldStateName(state),
                  " size=", packet.getSize(),
                  " readPos=", packet.getReadPos(),
                  " what=", e.what());
        if (socket && state == WorldState::IN_WORLD) {
            disconnect();
            fail("Out of memory while parsing world packet");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception while handling world opcode=0x", std::hex, opcode, std::dec,
                  " state=", worldStateName(state),
                  " size=", packet.getSize(),
                  " readPos=", packet.getReadPos(),
                  " what=", e.what());
    }
}

void GameHandler::enqueueIncomingPacket(const network::Packet& packet) {
    if (pendingIncomingPackets_.size() >= kMaxQueuedInboundPackets) {
        LOG_ERROR("Inbound packet queue overflow (", pendingIncomingPackets_.size(),
                  " packets); dropping oldest packet to preserve responsiveness");
        pendingIncomingPackets_.pop_front();
    }
    pendingIncomingPackets_.push_back(packet);
    lastRxTime_ = std::chrono::steady_clock::now();
    rxSilenceLogged_ = false;
}

void GameHandler::enqueueIncomingPacketFront(network::Packet&& packet) {
    if (pendingIncomingPackets_.size() >= kMaxQueuedInboundPackets) {
        LOG_ERROR("Inbound packet queue overflow while prepending (", pendingIncomingPackets_.size(),
                  " packets); dropping newest queued packet to preserve ordering");
        pendingIncomingPackets_.pop_back();
    }
    pendingIncomingPackets_.emplace_front(std::move(packet));
}

void GameHandler::enqueueUpdateObjectWork(UpdateObjectData&& data) {
    pendingUpdateObjectWork_.push_back(PendingUpdateObjectWork{std::move(data)});
}

void GameHandler::processPendingUpdateObjectWork(const std::chrono::steady_clock::time_point& start,
                                                 float budgetMs) {
    if (pendingUpdateObjectWork_.empty()) {
        return;
    }

    const int maxBlocksThisUpdate = updateObjectBlocksBudgetPerUpdate(state);
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
                  "/", work.data.blocks.size(), ", state=", worldStateName(state), ")");
    }
}

void GameHandler::processQueuedIncomingPackets() {
    if (pendingIncomingPackets_.empty() && pendingUpdateObjectWork_.empty()) {
        return;
    }

    const int maxPacketsThisUpdate = incomingPacketsBudgetPerUpdate(state);
    const float budgetMs = incomingPacketBudgetMs(state);
    const auto start = std::chrono::steady_clock::now();
    int processed = 0;

    while (processed < maxPacketsThisUpdate) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= budgetMs) {
            break;
        }

        if (!pendingUpdateObjectWork_.empty()) {
            processPendingUpdateObjectWork(start, budgetMs);
            if (!pendingUpdateObjectWork_.empty()) {
                break;
            }
            continue;
        }

        if (pendingIncomingPackets_.empty()) {
            break;
        }

        network::Packet packet = std::move(pendingIncomingPackets_.front());
        pendingIncomingPackets_.pop_front();
        const uint16_t wireOp = packet.getOpcode();
        const auto logicalOp = opcodeTable_.fromWire(wireOp);
        auto packetHandleStart = std::chrono::steady_clock::now();
        handlePacket(packet);
        float packetMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - packetHandleStart).count();
        if (packetMs > slowPacketLogThresholdMs()) {
            const char* logicalName = logicalOp
                ? OpcodeTable::logicalToName(*logicalOp)
                : "UNKNOWN";
            LOG_WARNING("SLOW packet handler: ", packetMs,
                        "ms wire=0x", std::hex, wireOp, std::dec,
                        " logical=", logicalName,
                        " size=", packet.getSize(),
                        " state=", worldStateName(state));
        }
        ++processed;
    }

    if (!pendingUpdateObjectWork_.empty()) {
        return;
    }

    if (!pendingIncomingPackets_.empty()) {
        LOG_DEBUG("GameHandler packet budget reached (processed=", processed,
                  ", remaining=", pendingIncomingPackets_.size(),
                  ", state=", worldStateName(state), ")");
    }
}

void GameHandler::handleAuthChallenge(network::Packet& packet) {
    LOG_INFO("Handling SMSG_AUTH_CHALLENGE");

    AuthChallengeData challenge;
    if (!AuthChallengeParser::parse(packet, challenge)) {
        fail("Failed to parse SMSG_AUTH_CHALLENGE");
        return;
    }

    if (!challenge.isValid()) {
        fail("Invalid auth challenge data");
        return;
    }

    // Store server seed
    serverSeed = challenge.serverSeed;
    LOG_DEBUG("Server seed: 0x", std::hex, serverSeed, std::dec);

    setState(WorldState::CHALLENGE_RECEIVED);

    // Send authentication session
    sendAuthSession();
}

void GameHandler::sendAuthSession() {
    LOG_INFO("Sending CMSG_AUTH_SESSION");

    // Build authentication packet
    auto packet = AuthSessionPacket::build(
        build,
        accountName,
        clientSeed,
        sessionKey,
        serverSeed,
        realmId_
    );

    LOG_DEBUG("CMSG_AUTH_SESSION packet size: ", packet.getSize(), " bytes");

    // Send packet (unencrypted - this is the last unencrypted packet)
    socket->send(packet);

    // Enable encryption IMMEDIATELY after sending AUTH_SESSION
    // AzerothCore enables encryption before sending AUTH_RESPONSE,
    // so we need to be ready to decrypt the response
    LOG_INFO("Enabling encryption immediately after AUTH_SESSION");
    socket->initEncryption(sessionKey, build);

    setState(WorldState::AUTH_SENT);
    LOG_INFO("CMSG_AUTH_SESSION sent, encryption enabled, waiting for AUTH_RESPONSE...");
}

void GameHandler::handleAuthResponse(network::Packet& packet) {
    LOG_INFO("Handling SMSG_AUTH_RESPONSE");

    AuthResponseData response;
    if (!AuthResponseParser::parse(packet, response)) {
        fail("Failed to parse SMSG_AUTH_RESPONSE");
        return;
    }

    if (!response.isSuccess()) {
        std::string reason = std::string("Authentication failed: ") +
                           getAuthResultString(response.result);
        fail(reason);
        return;
    }

    // Encryption was already enabled after sending AUTH_SESSION
    LOG_INFO("AUTH_RESPONSE OK - world authentication successful");

    setState(WorldState::AUTHENTICATED);

    LOG_INFO("========================================");
    LOG_INFO("   WORLD AUTHENTICATION SUCCESSFUL!");
    LOG_INFO("========================================");
    LOG_INFO("Connected to world server");
    LOG_INFO("Ready for character operations");

    setState(WorldState::READY);

    // Request character list automatically
    requestCharacterList();

    // Call success callback
    if (onSuccess) {
        onSuccess();
    }
}

void GameHandler::requestCharacterList() {
    if (requiresWarden_) {
        // Gate already surfaced via failure callback/chat; avoid per-frame warning spam.
        wardenCharEnumBlockedLogged_ = true;
        return;
    }

    if (state == WorldState::FAILED || !socket || !socket->isConnected()) {
        return;
    }

    if (state != WorldState::READY && state != WorldState::AUTHENTICATED &&
        state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot request character list in state: ", worldStateName(state));
        return;
    }

    LOG_INFO("Requesting character list from server...");

    // Prevent the UI from showing/selecting stale characters while we wait for the new SMSG_CHAR_ENUM.
    // This matters after character create/delete where the old list can linger for a few frames.
    characters.clear();

    // Build CMSG_CHAR_ENUM packet (no body, just opcode)
    auto packet = CharEnumPacket::build();

    // Send packet
    socket->send(packet);

    setState(WorldState::CHAR_LIST_REQUESTED);
    LOG_INFO("CMSG_CHAR_ENUM sent, waiting for character list...");
}

void GameHandler::handleCharEnum(network::Packet& packet) {
    LOG_INFO("Handling SMSG_CHAR_ENUM");

    CharEnumResponse response;
    // IMPORTANT: Do not infer packet formats from numeric build alone.
    // Turtle WoW uses a "high" build but classic-era world packet formats.
    bool parsed = packetParsers_ ? packetParsers_->parseCharEnum(packet, response)
                                 : CharEnumParser::parse(packet, response);
    if (!parsed) {
        fail("Failed to parse SMSG_CHAR_ENUM");
        return;
    }

    // Store characters
    characters = response.characters;

    setState(WorldState::CHAR_LIST_RECEIVED);

    LOG_INFO("========================================");
    LOG_INFO("   CHARACTER LIST RECEIVED");
    LOG_INFO("========================================");
    LOG_INFO("Found ", characters.size(), " character(s)");

    if (characters.empty()) {
        LOG_INFO("No characters on this account");
    } else {
        LOG_INFO("Characters:");
        for (size_t i = 0; i < characters.size(); ++i) {
            const auto& character = characters[i];
            LOG_INFO("  [", i + 1, "] ", character.name);
            LOG_INFO("      GUID: 0x", std::hex, character.guid, std::dec);
            LOG_INFO("      ", getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            LOG_INFO("      Level ", static_cast<int>(character.level));
        }
    }

    LOG_INFO("Ready to select character");
}

void GameHandler::createCharacter(const CharCreateData& data) {

    // Online mode: send packet to server
    if (!socket) {
        LOG_WARNING("Cannot create character: not connected");
        if (charCreateCallback_) {
            charCreateCallback_(false, "Not connected to server");
        }
        return;
    }

    if (requiresWarden_) {
        std::string msg = "Server requires anti-cheat/Warden; character creation blocked.";
        LOG_WARNING("Blocking CMSG_CHAR_CREATE while Warden gate is active");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
        return;
    }

    if (state != WorldState::CHAR_LIST_RECEIVED) {
        std::string msg = "Character list not ready yet. Wait for SMSG_CHAR_ENUM.";
        LOG_WARNING("Blocking CMSG_CHAR_CREATE in state=", worldStateName(state),
                    " (awaiting CHAR_LIST_RECEIVED)");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
        return;
    }

    auto packet = CharCreatePacket::build(data);
    socket->send(packet);
    LOG_INFO("CMSG_CHAR_CREATE sent for: ", data.name);
}

void GameHandler::handleCharCreateResponse(network::Packet& packet) {
    CharCreateResponseData data;
    if (!CharCreateResponseParser::parse(packet, data)) {
        LOG_ERROR("Failed to parse SMSG_CHAR_CREATE");
        return;
    }

    if (data.result == CharCreateResult::SUCCESS || data.result == CharCreateResult::IN_PROGRESS) {
        LOG_INFO("Character created successfully (code=", static_cast<int>(data.result), ")");
        requestCharacterList();
        if (charCreateCallback_) {
            charCreateCallback_(true, "Character created!");
        }
    } else {
        std::string msg;
        switch (data.result) {
            case CharCreateResult::CHAR_ERROR: msg = "Server error"; break;
            case CharCreateResult::FAILED: msg = "Creation failed"; break;
            case CharCreateResult::NAME_IN_USE: msg = "Name already in use"; break;
            case CharCreateResult::DISABLED: msg = "Character creation disabled"; break;
            case CharCreateResult::PVP_TEAMS_VIOLATION: msg = "PvP faction violation"; break;
            case CharCreateResult::SERVER_LIMIT: msg = "Server character limit reached"; break;
            case CharCreateResult::ACCOUNT_LIMIT: msg = "Account character limit reached"; break;
            case CharCreateResult::SERVER_QUEUE: msg = "Server is queued"; break;
            case CharCreateResult::ONLY_EXISTING: msg = "Only existing characters allowed"; break;
            case CharCreateResult::EXPANSION: msg = "Expansion required"; break;
            case CharCreateResult::EXPANSION_CLASS: msg = "Expansion required for this class"; break;
            case CharCreateResult::LEVEL_REQUIREMENT: msg = "Level requirement not met"; break;
            case CharCreateResult::UNIQUE_CLASS_LIMIT: msg = "Unique class limit reached"; break;
            case CharCreateResult::RESTRICTED_RACECLASS: msg = "Race/class combination not allowed"; break;
            case CharCreateResult::IN_PROGRESS: msg = "Character creation in progress..."; break;
            case CharCreateResult::CHARACTER_CHOOSE_RACE: msg = "Please choose a different race"; break;
            case CharCreateResult::CHARACTER_ARENA_LEADER: msg = "Arena team leader restriction"; break;
            case CharCreateResult::CHARACTER_DELETE_MAIL: msg = "Character has mail"; break;
            case CharCreateResult::CHARACTER_SWAP_FACTION: msg = "Faction swap restriction"; break;
            case CharCreateResult::CHARACTER_RACE_ONLY: msg = "Race-only restriction"; break;
            case CharCreateResult::CHARACTER_GOLD_LIMIT: msg = "Gold limit reached"; break;
            case CharCreateResult::FORCE_LOGIN: msg = "Force login required"; break;
            case CharCreateResult::CHARACTER_IN_GUILD: msg = "Character is in a guild"; break;
            // Name validation errors
            case CharCreateResult::NAME_FAILURE: msg = "Invalid name"; break;
            case CharCreateResult::NAME_NO_NAME: msg = "Please enter a name"; break;
            case CharCreateResult::NAME_TOO_SHORT: msg = "Name is too short"; break;
            case CharCreateResult::NAME_TOO_LONG: msg = "Name is too long"; break;
            case CharCreateResult::NAME_INVALID_CHARACTER: msg = "Name contains invalid characters"; break;
            case CharCreateResult::NAME_MIXED_LANGUAGES: msg = "Name mixes languages"; break;
            case CharCreateResult::NAME_PROFANE: msg = "Name contains profanity"; break;
            case CharCreateResult::NAME_RESERVED: msg = "Name is reserved"; break;
            case CharCreateResult::NAME_INVALID_APOSTROPHE: msg = "Invalid apostrophe in name"; break;
            case CharCreateResult::NAME_MULTIPLE_APOSTROPHES: msg = "Name has multiple apostrophes"; break;
            case CharCreateResult::NAME_THREE_CONSECUTIVE: msg = "Name has 3+ consecutive same letters"; break;
            case CharCreateResult::NAME_INVALID_SPACE: msg = "Invalid space in name"; break;
            case CharCreateResult::NAME_CONSECUTIVE_SPACES: msg = "Name has consecutive spaces"; break;
            default: msg = "Unknown error (code " + std::to_string(static_cast<int>(data.result)) + ")"; break;
        }
        LOG_WARNING("Character creation failed: ", msg, " (code=", static_cast<int>(data.result), ")");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
    }
}

void GameHandler::deleteCharacter(uint64_t characterGuid) {
    if (!socket) {
        if (charDeleteCallback_) charDeleteCallback_(false);
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_CHAR_DELETE));
    packet.writeUInt64(characterGuid);
    socket->send(packet);
    LOG_INFO("CMSG_CHAR_DELETE sent for GUID: 0x", std::hex, characterGuid, std::dec);
}

const Character* GameHandler::getActiveCharacter() const {
    if (activeCharacterGuid_ == 0) return nullptr;
    for (const auto& ch : characters) {
        if (ch.guid == activeCharacterGuid_) return &ch;
    }
    return nullptr;
}

const Character* GameHandler::getFirstCharacter() const {
    if (characters.empty()) return nullptr;
    return &characters.front();
}

void GameHandler::handleCharLoginFailed(network::Packet& packet) {
    uint8_t reason = packet.readUInt8();

    static const char* reasonNames[] = {
        "Login failed",          // 0
        "World server is down",  // 1
        "Duplicate character",   // 2 (session still active)
        "No instance servers",   // 3
        "Login disabled",        // 4
        "Character not found",   // 5
        "Locked for transfer",   // 6
        "Locked by billing",     // 7
        "Using remote",          // 8
    };
    const char* msg = (reason < 9) ? reasonNames[reason] : "Unknown reason";

    LOG_ERROR("SMSG_CHARACTER_LOGIN_FAILED: reason=", static_cast<int>(reason), " (", msg, ")");

    // Allow the player to re-select a character
    setState(WorldState::CHAR_LIST_RECEIVED);

    if (charLoginFailCallback_) {
        charLoginFailCallback_(msg);
    }
}

void GameHandler::selectCharacter(uint64_t characterGuid) {
    if (state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot select character in state: ", static_cast<int>(state));
        return;
    }

    // Make the selected character authoritative in GameHandler.
    // This avoids relying on UI/Application ordering for appearance-dependent logic.
    activeCharacterGuid_ = characterGuid;

    LOG_INFO("========================================");
    LOG_INFO("   ENTERING WORLD");
    LOG_INFO("========================================");
    LOG_INFO("Character GUID: 0x", std::hex, characterGuid, std::dec);

    // Find character name for logging
    for (const auto& character : characters) {
        if (character.guid == characterGuid) {
            LOG_INFO("Character: ", character.name);
            LOG_INFO("Level ", static_cast<int>(character.level), " ",
                     getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            playerRace_ = character.race;
            break;
        }
    }

    // Store player GUID
    playerGuid = characterGuid;

    // Reset per-character state so previous character data doesn't bleed through
    inventory = Inventory();
    onlineItems_.clear();
    itemInfoCache_.clear();
    pendingItemQueries_.clear();
    equipSlotGuids_ = {};
    backpackSlotGuids_ = {};
    keyringSlotGuids_ = {};
    invSlotBase_ = -1;
    packSlotBase_ = -1;
    lastPlayerFields_.clear();
    onlineEquipDirty_ = false;
    playerMoneyCopper_ = 0;
    playerArmorRating_ = 0;
    std::fill(std::begin(playerResistances_), std::end(playerResistances_), 0);
    std::fill(std::begin(playerStats_), std::end(playerStats_), -1);
    playerMeleeAP_ = -1;
    playerRangedAP_ = -1;
    std::fill(std::begin(playerSpellDmgBonus_), std::end(playerSpellDmgBonus_), -1);
    playerHealBonus_ = -1;
    playerDodgePct_ = -1.0f;
    playerParryPct_ = -1.0f;
    playerBlockPct_ = -1.0f;
    playerCritPct_  = -1.0f;
    playerRangedCritPct_ = -1.0f;
    std::fill(std::begin(playerSpellCritPct_), std::end(playerSpellCritPct_), -1.0f);
    std::fill(std::begin(playerCombatRatings_), std::end(playerCombatRatings_), -1);
    if (spellHandler_) spellHandler_->knownSpells_.clear();
    if (spellHandler_) spellHandler_->spellCooldowns_.clear();
    spellFlatMods_.clear();
    spellPctMods_.clear();
    actionBar = {};
    if (spellHandler_) {
        spellHandler_->playerAuras_.clear();
        spellHandler_->targetAuras_.clear();
        spellHandler_->unitAurasCache_.clear();
    }
    if (spellHandler_) spellHandler_->unitCastStates_.clear();
    petGuid_ = 0;
    stableWindowOpen_  = false;
    stableMasterGuid_  = 0;
    stableNumSlots_    = 0;
    stabledPets_.clear();
    playerXp_ = 0;
    playerNextLevelXp_ = 0;
    serverPlayerLevel_ = 1;
    std::fill(playerExploredZones_.begin(), playerExploredZones_.end(), 0u);
    hasPlayerExploredZones_ = false;
    playerSkills_.clear();
    questLog_.clear();
    pendingQuestQueryIds_.clear();
    pendingLoginQuestResync_ = false;
    pendingLoginQuestResyncTimeout_ = 0.0f;
    pendingQuestAcceptTimeouts_.clear();
    pendingQuestAcceptNpcGuids_.clear();
    npcQuestStatus_.clear();
    if (combatHandler_) combatHandler_->resetAllCombatState();
    if (spellHandler_) { spellHandler_->casting_ = false; spellHandler_->castIsChannel_ = false; spellHandler_->currentCastSpellId_ = 0; }
    pendingGameObjectInteractGuid_ = 0;
    lastInteractedGoGuid_ = 0;
    if (spellHandler_) { spellHandler_->castTimeRemaining_ = 0.0f; spellHandler_->castTimeTotal_ = 0.0f; }
    craftQueueSpellId_ = 0;
    craftQueueRemaining_ = 0;
    queuedSpellId_ = 0;
    queuedSpellTarget_ = 0;
    playerDead_ = false;
    releasedSpirit_ = false;
    corpseGuid_ = 0;
    corpseReclaimAvailableMs_ = 0;
    targetGuid = 0;
    focusGuid = 0;
    lastTargetGuid = 0;
    tabCycleStale = true;
    entityManager = EntityManager();

    // Build CMSG_PLAYER_LOGIN packet
    auto packet = PlayerLoginPacket::build(characterGuid);

    // Send packet
    socket->send(packet);

    setState(WorldState::ENTERING_WORLD);
    LOG_INFO("CMSG_PLAYER_LOGIN sent, entering world...");
}

void GameHandler::handleLoginSetTimeSpeed(network::Packet& packet) {
    // SMSG_LOGIN_SETTIMESPEED (0x042)
    // Structure: uint32 gameTime, float timeScale
    // gameTime: Game time in seconds since epoch
    // timeScale: Time speed multiplier (typically 0.0166 for 1 day = 1 hour)

    if (packet.getSize() < 8) {
        LOG_WARNING("SMSG_LOGIN_SETTIMESPEED: packet too small (", packet.getSize(), " bytes)");
        return;
    }

    uint32_t gameTimePacked = packet.readUInt32();
    float timeScale = packet.readFloat();

    // Store for celestial/sky system use
    gameTime_ = static_cast<float>(gameTimePacked);
    timeSpeed_ = timeScale;

    LOG_INFO("Server time: gameTime=", gameTime_, "s, timeSpeed=", timeSpeed_);
    LOG_INFO("  (1 game day = ", (1.0f / timeSpeed_) / 60.0f, " real minutes)");
}

void GameHandler::handleLoginVerifyWorld(network::Packet& packet) {
    LOG_INFO("Handling SMSG_LOGIN_VERIFY_WORLD");
    const bool initialWorldEntry = (state == WorldState::ENTERING_WORLD);

    LoginVerifyWorldData data;
    if (!LoginVerifyWorldParser::parse(packet, data)) {
        fail("Failed to parse SMSG_LOGIN_VERIFY_WORLD");
        return;
    }

    if (!data.isValid()) {
        fail("Invalid world entry data");
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
    const bool alreadyInWorld = (state == WorldState::IN_WORLD);
    const bool sameMap = alreadyInWorld && (currentMapId_ == data.mapId);
    const float dxCurrent = movementInfo.x - canonical.x;
    const float dyCurrent = movementInfo.y - canonical.y;
    const float dzCurrent = movementInfo.z - canonical.z;
    const float distSqCurrent = dxCurrent * dxCurrent + dyCurrent * dyCurrent + dzCurrent * dzCurrent;

    // Some realms emit a late duplicate LOGIN_VERIFY_WORLD after the client is already
    // in-world. Re-running full world-entry handling here can trigger an expensive
    // same-map reload/reset path and starve networking for tens of seconds.
    if (!initialWorldEntry && sameMap && distSqCurrent <= (5.0f * 5.0f)) {
        LOG_INFO("Ignoring duplicate SMSG_LOGIN_VERIFY_WORLD while already in world: mapId=",
                 data.mapId, " dist=", std::sqrt(distSqCurrent));
        return;
    }

    // Successfully entered the world (or teleported)
    currentMapId_ = data.mapId;
    setState(WorldState::IN_WORLD);
    if (socket) {
        socket->tracePacketsFor(std::chrono::seconds(12), "login_verify_world");
    }

    LOG_INFO("========================================");
    LOG_INFO("   SUCCESSFULLY ENTERED WORLD!");
    LOG_INFO("========================================");
    LOG_INFO("Map ID: ", data.mapId);
    LOG_INFO("Position: (", data.x, ", ", data.y, ", ", data.z, ")");
    LOG_INFO("Orientation: ", data.orientation, " radians");
    LOG_INFO("Player is now in the game world");

    // Initialize movement info with world entry position (server → canonical)
    LOG_DEBUG("LOGIN_VERIFY_WORLD: server=(", data.x, ", ", data.y, ", ", data.z,
             ") canonical=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ") mapId=", data.mapId);
    movementInfo.x = canonical.x;
    movementInfo.y = canonical.y;
    movementInfo.z = canonical.z;
    movementInfo.orientation = core::coords::serverToCanonicalYaw(data.orientation);
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    if (movementHandler_) {
        movementHandler_->movementClockStart_ = std::chrono::steady_clock::now();
        movementHandler_->lastMovementTimestampMs_ = 0;
    }
    movementInfo.time = nextMovementTimestampMs();
    if (movementHandler_) {
        movementHandler_->isFalling_ = false;
        movementHandler_->fallStartMs_ = 0;
    }
    movementInfo.fallTime = 0;
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = 0.0f;
    movementInfo.jumpCosAngle = 0.0f;
    movementInfo.jumpXYSpeed = 0.0f;
    resurrectPending_ = false;
    resurrectRequestPending_ = false;
    selfResAvailable_ = false;
    onTaxiFlight_ = false;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    taxiClientActive_ = false;
    taxiClientPath_.clear();
    taxiRecoverPending_ = false;
    taxiStartGrace_ = 0.0f;
    currentMountDisplayId_ = 0;
    taxiMountDisplayId_ = 0;
    vehicleId_ = 0;
    if (mountCallback_) {
        mountCallback_(0);
    }

    // Clear boss encounter unit slots and raid marks on world transfer
    if (socialHandler_) socialHandler_->resetTransferState();

    // Suppress area triggers on initial login — prevents exit portals from
    // immediately firing when spawning inside a dungeon/instance.
    activeAreaTriggers_.clear();
    areaTriggerCheckTimer_ = -5.0f;
    areaTriggerSuppressFirst_ = true;

    // Notify application to load terrain for this map/position (online mode)
    if (worldEntryCallback_) {
        worldEntryCallback_(data.mapId, data.x, data.y, data.z, initialWorldEntry);
    }

    // Send CMSG_SET_ACTIVE_MOVER on initial world entry and world transfers.
    if (playerGuid != 0 && socket) {
        auto activeMoverPacket = SetActiveMoverPacket::build(playerGuid);
        socket->send(activeMoverPacket);
        LOG_INFO("Sent CMSG_SET_ACTIVE_MOVER for player 0x", std::hex, playerGuid, std::dec);
    }

    // Kick the first keepalive immediately on world entry. Classic-like realms
    // can close the session before our default 30s ping cadence fires.
    timeSinceLastPing = 0.0f;
    if (socket) {
        LOG_DEBUG("World entry keepalive: sending immediate ping after LOGIN_VERIFY_WORLD");
        sendPing();
    }

    // If we disconnected mid-taxi, attempt to recover to destination after login.
    if (taxiRecoverPending_ && taxiRecoverMapId_ == data.mapId) {
        float dx = movementInfo.x - taxiRecoverPos_.x;
        float dy = movementInfo.y - taxiRecoverPos_.y;
        float dz = movementInfo.z - taxiRecoverPos_.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 5.0f) {
            // Keep pending until player entity exists; update() will apply.
            LOG_INFO("Taxi recovery pending: dist=", dist);
        } else {
            taxiRecoverPending_ = false;
        }
    }

    if (initialWorldEntry) {
        // Clear inspect caches on world entry to avoid showing stale data.
        inspectedPlayerAchievements_.clear();

        // Reset talent initialization so the first SMSG_TALENTS_INFO after login
        // correctly sets the active spec (static locals don't reset across logins).
        if (spellHandler_) spellHandler_->resetTalentState();

        // Auto-join default chat channels only on first world entry.
        autoJoinDefaultChannels();

        // Auto-query guild info on login.
        const Character* activeChar = getActiveCharacter();
        if (activeChar && activeChar->hasGuild() && socket) {
            auto gqPacket = GuildQueryPacket::build(activeChar->guildId);
            socket->send(gqPacket);
            auto grPacket = GuildRosterPacket::build();
            socket->send(grPacket);
            LOG_INFO("Auto-queried guild info (guildId=", activeChar->guildId, ")");
        }

        pendingQuestAcceptTimeouts_.clear();
        pendingQuestAcceptNpcGuids_.clear();
        pendingQuestQueryIds_.clear();
        pendingLoginQuestResync_ = true;
        pendingLoginQuestResyncTimeout_ = 10.0f;
        completedQuests_.clear();
        LOG_INFO("Queued quest log resync for login (from server quest slots)");

        // Request completed quest IDs when the expansion supports it. Classic-like
        // opcode tables do not define this packet, and sending 0xFFFF during world
        // entry can desync the early session handshake.
        if (socket) {
            const uint16_t queryCompletedWire = wireOpcode(Opcode::CMSG_QUERY_QUESTS_COMPLETED);
            if (queryCompletedWire != 0xFFFF) {
                network::Packet cqcPkt(queryCompletedWire);
                socket->send(cqcPkt);
                LOG_INFO("Sent CMSG_QUERY_QUESTS_COMPLETED");
            } else {
                LOG_INFO("Skipping CMSG_QUERY_QUESTS_COMPLETED: opcode not mapped for current expansion");
            }
        }

        // Auto-request played time on login so the character Stats tab is
        // populated immediately without requiring /played.
        if (socket) {
            auto ptPkt = RequestPlayedTimePacket::build(false);  // false = don't show in chat
            socket->send(ptPkt);
            LOG_INFO("Auto-requested played time on login");
        }
    }

    // Pre-load DBC name caches during world entry so the first packet that
    // needs spell/title/achievement data doesn't stall mid-gameplay (the
    // Spell.dbc cache alone is ~170ms on a cold load).
    if (initialWorldEntry) {
        preloadDBCCaches();
    }

    // Fire PLAYER_ENTERING_WORLD — THE most important event for addon initialization.
    // Fires on initial login, teleports, instance transitions, and zone changes.
    if (addonEventCallback_) {
        fireAddonEvent("PLAYER_ENTERING_WORLD", {initialWorldEntry ? "1" : "0"});
        // Also fire ZONE_CHANGED_NEW_AREA and UPDATE_WORLD_STATES so map/BG addons refresh
        fireAddonEvent("ZONE_CHANGED_NEW_AREA", {});
        fireAddonEvent("UPDATE_WORLD_STATES", {});
        // PLAYER_LOGIN fires only on initial login (not teleports)
        if (initialWorldEntry) {
            fireAddonEvent("PLAYER_LOGIN", {});
        }
    }
}

void GameHandler::handleClientCacheVersion(network::Packet& packet) {
    if (packet.getSize() < 4) {
        LOG_WARNING("SMSG_CLIENTCACHE_VERSION too short: ", packet.getSize(), " bytes");
        return;
    }

    uint32_t version = packet.readUInt32();
    LOG_INFO("SMSG_CLIENTCACHE_VERSION: ", version);
}

void GameHandler::handleTutorialFlags(network::Packet& packet) {
    if (packet.getSize() < 32) {
        LOG_WARNING("SMSG_TUTORIAL_FLAGS too short: ", packet.getSize(), " bytes");
        return;
    }

    std::array<uint32_t, 8> flags{};
    for (uint32_t& v : flags) {
        v = packet.readUInt32();
    }

    LOG_INFO("SMSG_TUTORIAL_FLAGS: [",
             flags[0], ", ", flags[1], ", ", flags[2], ", ", flags[3], ", ",
             flags[4], ", ", flags[5], ", ", flags[6], ", ", flags[7], "]");
}

void GameHandler::handleAccountDataTimes(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_ACCOUNT_DATA_TIMES");

    AccountDataTimesData data;
    if (!AccountDataTimesParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_ACCOUNT_DATA_TIMES");
        return;
    }

    LOG_DEBUG("Account data times received (server time: ", data.serverTime, ")");
}

void GameHandler::handleMotd(network::Packet& packet) {
    if (chatHandler_) chatHandler_->handleMotd(packet);
}

void GameHandler::handleNotification(network::Packet& packet) {
    // SMSG_NOTIFICATION: single null-terminated string
    std::string message = packet.readString();
    if (!message.empty()) {
        LOG_INFO("Server notification: ", message);
        addSystemChatMessage(message);
    }
}

void GameHandler::sendPing() {
    if (state != WorldState::IN_WORLD) {
        return;
    }

    // Increment sequence number
    pingSequence++;

    LOG_DEBUG("Sending CMSG_PING: sequence=", pingSequence,
              " latencyHintMs=", lastLatency);

    // Record send time for RTT measurement
    pingTimestamp_ = std::chrono::steady_clock::now();

    // Build and send ping packet
    auto packet = PingPacket::build(pingSequence, lastLatency);
    socket->send(packet);
}

void GameHandler::sendRequestVehicleExit() {
    if (state != WorldState::IN_WORLD || vehicleId_ == 0) return;
    // CMSG_REQUEST_VEHICLE_EXIT has no payload — opcode only
    network::Packet pkt(wireOpcode(Opcode::CMSG_REQUEST_VEHICLE_EXIT));
    socket->send(pkt);
    vehicleId_ = 0;  // Optimistically clear; server will confirm via SMSG_PLAYER_VEHICLE_DATA(0)
}

const std::vector<GameHandler::EquipmentSetInfo>& GameHandler::getEquipmentSets() const {
    if (inventoryHandler_) return inventoryHandler_->getEquipmentSets();
    static const std::vector<EquipmentSetInfo> empty;
    return empty;
}

bool GameHandler::supportsEquipmentSets() const {
    return inventoryHandler_ && inventoryHandler_->supportsEquipmentSets();
}

void GameHandler::useEquipmentSet(uint32_t setId) {
    if (inventoryHandler_) inventoryHandler_->useEquipmentSet(setId);
}

void GameHandler::saveEquipmentSet(const std::string& name, const std::string& iconName,
                                    uint64_t existingGuid, uint32_t setIndex) {
    if (inventoryHandler_) inventoryHandler_->saveEquipmentSet(name, iconName, existingGuid, setIndex);
}

void GameHandler::deleteEquipmentSet(uint64_t setGuid) {
    if (inventoryHandler_) inventoryHandler_->deleteEquipmentSet(setGuid);
}

void GameHandler::sendMinimapPing(float wowX, float wowY) {
    if (socialHandler_) socialHandler_->sendMinimapPing(wowX, wowY);
}

void GameHandler::handlePong(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_PONG");

    PongData data;
    if (!PongParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_PONG");
        return;
    }

    // Verify sequence matches
    if (data.sequence != pingSequence) {
        LOG_WARNING("SMSG_PONG sequence mismatch: expected ", pingSequence,
                    ", got ", data.sequence);
        return;
    }

    // Measure round-trip time
    auto rtt = std::chrono::steady_clock::now() - pingTimestamp_;
    lastLatency = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count());

    LOG_DEBUG("SMSG_PONG acknowledged: sequence=", data.sequence,
              " latencyMs=", lastLatency);
}

bool GameHandler::isServerMovementAllowed() const {
    return movementHandler_ ? movementHandler_->isServerMovementAllowed() : true;
}

uint32_t GameHandler::nextMovementTimestampMs() {
    if (movementHandler_) return movementHandler_->nextMovementTimestampMs();
    return 0;
}

void GameHandler::sendMovement(Opcode opcode) {
    if (movementHandler_) movementHandler_->sendMovement(opcode);
}

void GameHandler::sanitizeMovementForTaxi() {
    if (movementHandler_) movementHandler_->sanitizeMovementForTaxi();
}

void GameHandler::forceClearTaxiAndMovementState() {
    if (movementHandler_) movementHandler_->forceClearTaxiAndMovementState();
}

void GameHandler::setPosition(float x, float y, float z) {
    if (movementHandler_) movementHandler_->setPosition(x, y, z);
}

void GameHandler::setOrientation(float orientation) {
    if (movementHandler_) movementHandler_->setOrientation(orientation);
}

void GameHandler::handleUpdateObject(network::Packet& packet) {
    UpdateObjectData data;
    if (!packetParsers_->parseUpdateObject(packet, data)) {
        static int updateObjErrors = 0;
        if (++updateObjErrors <= 5)
            LOG_WARNING("Failed to parse SMSG_UPDATE_OBJECT");
        if (data.blocks.empty()) return;
        // Fall through: process any blocks that were successfully parsed before the failure.
    }

    enqueueUpdateObjectWork(std::move(data));
}

void GameHandler::processOutOfRangeObjects(const std::vector<uint64_t>& guids) {
    // Process out-of-range objects first
    for (uint64_t guid : guids) {
        auto entity = entityManager.getEntity(guid);
        if (!entity) continue;

        const bool isKnownTransport = transportGuids_.count(guid) > 0;
        if (isKnownTransport) {
            // Keep transports alive across out-of-range flapping.
            // Boats/zeppelins are global movers and removing them here can make
            // them disappear until a later movement snapshot happens to recreate them.
            const bool playerAboardNow = (playerTransportGuid_ == guid);
            const bool stickyAboard = (playerTransportStickyGuid_ == guid && playerTransportStickyTimer_ > 0.0f);
            const bool movementSaysAboard = (movementInfo.transportGuid == guid);
            LOG_INFO("Preserving transport on out-of-range: 0x",
                     std::hex, guid, std::dec,
                     " now=", playerAboardNow,
                     " sticky=", stickyAboard,
                     " movement=", movementSaysAboard);
            continue;
        }

        LOG_DEBUG("Entity went out of range: 0x", std::hex, guid, std::dec);
        // Trigger despawn callbacks before removing entity
        if (entity->getType() == ObjectType::UNIT && creatureDespawnCallback_) {
            creatureDespawnCallback_(guid);
        } else if (entity->getType() == ObjectType::PLAYER && playerDespawnCallback_) {
            playerDespawnCallback_(guid);
            otherPlayerVisibleItemEntries_.erase(guid);
            otherPlayerVisibleDirty_.erase(guid);
            otherPlayerMoveTimeMs_.erase(guid);
            inspectedPlayerItemEntries_.erase(guid);
            pendingAutoInspect_.erase(guid);
            // Clear pending name query so the query is re-sent when this player
            // comes back into range (entity is recreated as a new object).
            pendingNameQueries.erase(guid);
        } else if (entity->getType() == ObjectType::GAMEOBJECT && gameObjectDespawnCallback_) {
            gameObjectDespawnCallback_(guid);
        }
        transportGuids_.erase(guid);
        serverUpdatedTransportGuids_.erase(guid);
        clearTransportAttachment(guid);
        if (playerTransportGuid_ == guid) {
            clearPlayerTransport();
        }
        entityManager.removeEntity(guid);
    }

}

void GameHandler::applyUpdateObjectBlock(const UpdateBlock& block, bool& newItemCreated) {
    static const bool kVerboseUpdateObject = envFlagEnabled("WOWEE_LOG_UPDATE_OBJECT_VERBOSE", false);
    auto extractPlayerAppearance = [&](const std::map<uint16_t, uint32_t>& fields,
                                       uint8_t& outRace,
                                       uint8_t& outGender,
                                       uint32_t& outAppearanceBytes,
                                       uint8_t& outFacial) -> bool {
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
    };

    auto maybeDetectCoinageIndex = [&](const std::map<uint16_t, uint32_t>& oldFields,
                                       const std::map<uint16_t, uint32_t>& newFields) {
        if (pendingMoneyDelta_ == 0 || pendingMoneyDeltaTimer_ <= 0.0f) return;
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
            if (delta != pendingMoneyDelta_) continue;
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
            updateFieldTable_.setIndex(UF::PLAYER_FIELD_COINAGE, chosen);
            LOG_WARNING("Auto-detected PLAYER_FIELD_COINAGE index: ", chosen, " (was ", current, ")");
        }

        pendingMoneyDelta_ = 0;
        pendingMoneyDeltaTimer_ = 0.0f;
    };

    switch (block.updateType) {
        case UpdateType::CREATE_OBJECT:
        case UpdateType::CREATE_OBJECT2: {
            // Create new entity
            std::shared_ptr<Entity> entity;

            switch (block.objectType) {
                case ObjectType::PLAYER:
                    entity = std::make_shared<Player>(block.guid);
                    break;

                case ObjectType::UNIT:
                    entity = std::make_shared<Unit>(block.guid);
                    break;

                case ObjectType::GAMEOBJECT:
                    entity = std::make_shared<GameObject>(block.guid);
                    break;

                default:
                    entity = std::make_shared<Entity>(block.guid);
                    entity->setType(block.objectType);
                    break;
            }

            // Set position from movement block (server → canonical)
            if (block.hasMovement) {
                glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
                float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
                entity->setPosition(pos.x, pos.y, pos.z, oCanonical);
                LOG_DEBUG("  Position: (", pos.x, ", ", pos.y, ", ", pos.z, ")");
                if (block.guid == playerGuid && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
                    serverRunSpeed_ = block.runSpeed;
                }
                // Track player-on-transport state
                if (block.guid == playerGuid) {
                    if (block.onTransport) {
                        // Convert transport offset from server → canonical coordinates
                        glm::vec3 serverOffset(block.transportX, block.transportY, block.transportZ);
                        glm::vec3 canonicalOffset = core::coords::serverToCanonical(serverOffset);
                        setPlayerOnTransport(block.transportGuid, canonicalOffset);
                        if (transportManager_ && transportManager_->getTransport(playerTransportGuid_)) {
                            glm::vec3 composed = transportManager_->getPlayerWorldPosition(playerTransportGuid_, playerTransportOffset_);
                            entity->setPosition(composed.x, composed.y, composed.z, oCanonical);
                            movementInfo.x = composed.x;
                            movementInfo.y = composed.y;
                            movementInfo.z = composed.z;
                        }
                        LOG_INFO("Player on transport: 0x", std::hex, playerTransportGuid_, std::dec,
                                " offset=(", playerTransportOffset_.x, ", ", playerTransportOffset_.y, ", ", playerTransportOffset_.z, ")");
                    } else {
                        // Don't clear client-side M2 transport boarding (trams) —
                        // the server doesn't know about client-detected transport attachment.
                        bool isClientM2Transport = false;
                        if (playerTransportGuid_ != 0 && transportManager_) {
                            auto* tr = transportManager_->getTransport(playerTransportGuid_);
                            isClientM2Transport = (tr && tr->isM2);
                        }
                        if (playerTransportGuid_ != 0 && !isClientM2Transport) {
                            LOG_INFO("Player left transport");
                            clearPlayerTransport();
                        }
                    }
                }

                // Track transport-relative children so they follow parent transport motion.
                if (block.guid != playerGuid &&
                    (block.objectType == ObjectType::UNIT || block.objectType == ObjectType::GAMEOBJECT)) {
                    if (block.onTransport && block.transportGuid != 0) {
                        glm::vec3 localOffset = core::coords::serverToCanonical(
                            glm::vec3(block.transportX, block.transportY, block.transportZ));
                        const bool hasLocalOrientation = (block.updateFlags & 0x0020) != 0; // UPDATEFLAG_LIVING
                        float localOriCanonical = core::coords::normalizeAngleRad(-block.transportO);
                        setTransportAttachment(block.guid, block.objectType, block.transportGuid,
                                               localOffset, hasLocalOrientation, localOriCanonical);
                        if (transportManager_ && transportManager_->getTransport(block.transportGuid)) {
                            glm::vec3 composed = transportManager_->getPlayerWorldPosition(block.transportGuid, localOffset);
                            entity->setPosition(composed.x, composed.y, composed.z, entity->getOrientation());
                        }
                    } else {
                        clearTransportAttachment(block.guid);
                    }
                }
            }

            // Set fields
            for (const auto& field : block.fields) {
                entity->setField(field.first, field.second);
            }

            // Add to manager
            entityManager.addEntity(block.guid, entity);

            // For the local player, capture the full initial field state (CREATE_OBJECT carries the
            // large baseline update-field set, including visible item fields on many cores).
            // Later VALUES updates often only include deltas and may never touch visible item fields.
            if (block.guid == playerGuid && block.objectType == ObjectType::PLAYER) {
                lastPlayerFields_ = entity->getFields();
                maybeDetectVisibleItemLayout();
            }

            // Auto-query names (Phase 1)
            if (block.objectType == ObjectType::PLAYER) {
                queryPlayerName(block.guid);
                if (block.guid != playerGuid) {
                    updateOtherPlayerVisibleItems(block.guid, entity->getFields());
                }
            } else if (block.objectType == ObjectType::UNIT) {
                auto it = block.fields.find(fieldIndex(UF::OBJECT_FIELD_ENTRY));
                if (it != block.fields.end() && it->second != 0) {
                    auto unit = std::static_pointer_cast<Unit>(entity);
                    unit->setEntry(it->second);
                    // Set name from cache immediately if available
                    std::string cached = getCachedCreatureName(it->second);
                    if (!cached.empty()) {
                        unit->setName(cached);
                    }
                    queryCreatureInfo(it->second, block.guid);
                }
            }

            // Extract health/mana/power from fields (Phase 2) — single pass
            if (block.objectType == ObjectType::UNIT || block.objectType == ObjectType::PLAYER) {
                auto unit = std::static_pointer_cast<Unit>(entity);
                constexpr uint32_t UNIT_DYNFLAG_DEAD = 0x0008;
                constexpr uint32_t UNIT_DYNFLAG_LOOTABLE = 0x0001;
                bool unitInitiallyDead = false;
                const uint16_t ufHealth = fieldIndex(UF::UNIT_FIELD_HEALTH);
                const uint16_t ufPowerBase = fieldIndex(UF::UNIT_FIELD_POWER1);
                const uint16_t ufMaxHealth = fieldIndex(UF::UNIT_FIELD_MAXHEALTH);
                const uint16_t ufMaxPowerBase = fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
                const uint16_t ufLevel = fieldIndex(UF::UNIT_FIELD_LEVEL);
                const uint16_t ufFaction = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE);
                const uint16_t ufFlags = fieldIndex(UF::UNIT_FIELD_FLAGS);
                const uint16_t ufDynFlags = fieldIndex(UF::UNIT_DYNAMIC_FLAGS);
                const uint16_t ufDisplayId = fieldIndex(UF::UNIT_FIELD_DISPLAYID);
                const uint16_t ufMountDisplayId = fieldIndex(UF::UNIT_FIELD_MOUNTDISPLAYID);
                const uint16_t ufNpcFlags = fieldIndex(UF::UNIT_NPC_FLAGS);
                const uint16_t ufBytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
                for (const auto& [key, val] : block.fields) {
                    // Check all specific fields BEFORE power/maxpower range checks.
                    // In Classic, power indices (23-27) are adjacent to maxHealth (28),
                    // and maxPower indices (29-33) are adjacent to level (34) and faction (35).
                    // A range check like "key >= powerBase && key < powerBase+7" would
                    // incorrectly capture maxHealth/level/faction in Classic's tight layout.
                    if (key == ufHealth) {
                        unit->setHealth(val);
                        if (block.objectType == ObjectType::UNIT && val == 0) {
                            unitInitiallyDead = true;
                        }
                        if (block.guid == playerGuid && val == 0) {
                            playerDead_ = true;
                            LOG_INFO("Player logged in dead");
                        }
                    } else if (key == ufMaxHealth) { unit->setMaxHealth(val); }
                    else if (key == ufLevel) {
                        unit->setLevel(val);
                    } else if (key == ufFaction) {
                        unit->setFactionTemplate(val);
                        if (addonEventCallback_) {
                            auto uid = guidToUnitId(block.guid);
                            if (!uid.empty())
                                fireAddonEvent("UNIT_FACTION", {uid});
                        }
                    }
                    else if (key == ufFlags) {
                        unit->setUnitFlags(val);
                        if (addonEventCallback_) {
                            auto uid = guidToUnitId(block.guid);
                            if (!uid.empty())
                                fireAddonEvent("UNIT_FLAGS", {uid});
                        }
                    }
                    else if (key == ufBytes0) {
                        unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
                    } else if (key == ufDisplayId) {
                        unit->setDisplayId(val);
                        if (addonEventCallback_) {
                            auto uid = guidToUnitId(block.guid);
                            if (!uid.empty())
                                fireAddonEvent("UNIT_MODEL_CHANGED", {uid});
                        }
                    }
                    else if (key == ufNpcFlags) { unit->setNpcFlags(val); }
                    else if (key == ufDynFlags) {
                        unit->setDynamicFlags(val);
                        if (block.objectType == ObjectType::UNIT &&
                            ((val & UNIT_DYNFLAG_DEAD) != 0 || (val & UNIT_DYNFLAG_LOOTABLE) != 0)) {
                            unitInitiallyDead = true;
                        }
                    }
                    // Power/maxpower range checks AFTER all specific fields
                    else if (key >= ufPowerBase && key < ufPowerBase + 7) {
                        unit->setPowerByType(static_cast<uint8_t>(key - ufPowerBase), val);
                    } else if (key >= ufMaxPowerBase && key < ufMaxPowerBase + 7) {
                        unit->setMaxPowerByType(static_cast<uint8_t>(key - ufMaxPowerBase), val);
                    }
                    else if (key == ufMountDisplayId) {
                        if (block.guid == playerGuid) {
                            uint32_t old = currentMountDisplayId_;
                            currentMountDisplayId_ = val;
                            if (val != old && mountCallback_) mountCallback_(val);
                            if (val != old)
                                fireAddonEvent("UNIT_MODEL_CHANGED", {"player"});
                            if (old == 0 && val != 0) {
                                // Just mounted — find the mount aura (indefinite duration, self-cast)
                                mountAuraSpellId_ = 0;
                                if (spellHandler_) for (const auto& a : spellHandler_->playerAuras_) {
                                    if (!a.isEmpty() && a.maxDurationMs < 0 && a.casterGuid == playerGuid) {
                                        mountAuraSpellId_ = a.spellId;
                                    }
                                }
                                // Classic/vanilla fallback: scan UNIT_FIELD_AURAS from same update block
                                if (mountAuraSpellId_ == 0) {
                                    const uint16_t ufAuras = fieldIndex(UF::UNIT_FIELD_AURAS);
                                    if (ufAuras != 0xFFFF) {
                                        for (const auto& [fk, fv] : block.fields) {
                                            if (fk >= ufAuras && fk < ufAuras + 48 && fv != 0) {
                                                mountAuraSpellId_ = fv;
                                                break;
                                            }
                                        }
                                    }
                                }
                                LOG_INFO("Mount detected: displayId=", val, " auraSpellId=", mountAuraSpellId_);
                            }
                            if (old != 0 && val == 0) {
                                mountAuraSpellId_ = 0;
                                if (spellHandler_) for (auto& a : spellHandler_->playerAuras_)
                                    if (!a.isEmpty() && a.maxDurationMs < 0) a = AuraSlot{};
                            }
                        }
                        unit->setMountDisplayId(val);
                    }
                }
                if (block.guid == playerGuid) {
                    constexpr uint32_t UNIT_FLAG_TAXI_FLIGHT = 0x00000100;
                    if ((unit->getUnitFlags() & UNIT_FLAG_TAXI_FLIGHT) != 0 && !onTaxiFlight_ && taxiLandingCooldown_ <= 0.0f) {
                        onTaxiFlight_ = true;
                        taxiStartGrace_ = std::max(taxiStartGrace_, 2.0f);
                        sanitizeMovementForTaxi();
                        if (movementHandler_) movementHandler_->applyTaxiMountForCurrentNode();
                    }
                }
                if (block.guid == playerGuid &&
                    (unit->getDynamicFlags() & UNIT_DYNFLAG_DEAD) != 0) {
                    playerDead_ = true;
                    LOG_INFO("Player logged in dead (dynamic flags)");
                }
                // Detect ghost state on login via PLAYER_FLAGS
                if (block.guid == playerGuid) {
                    constexpr uint32_t PLAYER_FLAGS_GHOST = 0x00000010;
                    auto pfIt = block.fields.find(fieldIndex(UF::PLAYER_FLAGS));
                    if (pfIt != block.fields.end() && (pfIt->second & PLAYER_FLAGS_GHOST) != 0) {
                        releasedSpirit_ = true;
                        playerDead_ = true;
                        LOG_INFO("Player logged in as ghost (PLAYER_FLAGS)");
                        if (ghostStateCallback_) ghostStateCallback_(true);
                        // Query corpse position so minimap marker is accurate on reconnect
                        if (socket) {
                            network::Packet cq(wireOpcode(Opcode::MSG_CORPSE_QUERY));
                            socket->send(cq);
                        }
                    }
                }
                // Classic: rebuild spellHandler_->playerAuras_ from UNIT_FIELD_AURAS on initial object create
                if (block.guid == playerGuid && isClassicLikeExpansion() && spellHandler_) {
                    const uint16_t ufAuras     = fieldIndex(UF::UNIT_FIELD_AURAS);
                    const uint16_t ufAuraFlags = fieldIndex(UF::UNIT_FIELD_AURAFLAGS);
                    if (ufAuras != 0xFFFF) {
                        bool hasAuraField = false;
                        for (const auto& [fk, fv] : block.fields) {
                            if (fk >= ufAuras && fk < ufAuras + 48) { hasAuraField = true; break; }
                        }
                        if (hasAuraField) {
                            spellHandler_->playerAuras_.clear();
                            spellHandler_->playerAuras_.resize(48);
                            uint64_t nowMs = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count());
                            const auto& allFields = entity->getFields();
                            for (int slot = 0; slot < 48; ++slot) {
                                auto it = allFields.find(static_cast<uint16_t>(ufAuras + slot));
                                if (it != allFields.end() && it->second != 0) {
                                    AuraSlot& a = spellHandler_->playerAuras_[slot];
                                    a.spellId = it->second;
                                    // Read aura flag byte: packed 4-per-uint32 at ufAuraFlags
                                    // Classic flags: 0x01=cancelable, 0x02=harmful, 0x04=helpful
                                    // Normalize to WotLK convention: 0x80 = negative (debuff)
                                    uint8_t classicFlag = 0;
                                    if (ufAuraFlags != 0xFFFF) {
                                        auto fit = allFields.find(static_cast<uint16_t>(ufAuraFlags + slot / 4));
                                        if (fit != allFields.end())
                                            classicFlag = static_cast<uint8_t>((fit->second >> ((slot % 4) * 8)) & 0xFF);
                                    }
                                    // Map Classic harmful bit (0x02) → WotLK debuff bit (0x80)
                                    a.flags = (classicFlag & 0x02) ? 0x80u : 0u;
                                    a.durationMs = -1;
                                    a.maxDurationMs = -1;
                                    a.casterGuid = playerGuid;
                                    a.receivedAtMs = nowMs;
                                }
                            }
                            LOG_DEBUG("[Classic] Rebuilt playerAuras from UNIT_FIELD_AURAS (CREATE_OBJECT)");
                                                            fireAddonEvent("UNIT_AURA", {"player"});
                        }
                    }
                }
                // Determine hostility from faction template for online creatures.
                // Always call isHostileFaction — factionTemplate=0 defaults to hostile
                // in the lookup rather than silently staying at the struct default (false).
                unit->setHostile(isHostileFaction(unit->getFactionTemplate()));
            // Trigger creature spawn callback for units/players with displayId
                if (block.objectType == ObjectType::UNIT && unit->getDisplayId() == 0) {
                    LOG_WARNING("[Spawn] UNIT guid=0x", std::hex, block.guid, std::dec,
                              " has displayId=0 — no spawn (entry=", unit->getEntry(),
                              " at ", unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
                }
                if ((block.objectType == ObjectType::UNIT || block.objectType == ObjectType::PLAYER) && unit->getDisplayId() != 0) {
                    if (block.objectType == ObjectType::PLAYER && block.guid == playerGuid) {
                        // Skip local player — spawned separately via spawnPlayerCharacter()
                    } else if (block.objectType == ObjectType::PLAYER) {
                        if (playerSpawnCallback_) {
                            uint8_t race = 0, gender = 0, facial = 0;
                            uint32_t appearanceBytes = 0;
                            // Use the entity's accumulated field state, not just this block's changed fields.
                            if (extractPlayerAppearance(entity->getFields(), race, gender, appearanceBytes, facial)) {
                                playerSpawnCallback_(block.guid, unit->getDisplayId(), race, gender,
                                                    appearanceBytes, facial,
                                                    unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation());
                            } else {
                                LOG_WARNING("[Spawn] PLAYER guid=0x", std::hex, block.guid, std::dec,
                                          " displayId=", unit->getDisplayId(), " appearance extraction failed — model will not render");
                            }
                        }
                        if (unitInitiallyDead && npcDeathCallback_) {
                            npcDeathCallback_(block.guid);
                        }
                    } else if (creatureSpawnCallback_) {
                        LOG_DEBUG("[Spawn] UNIT guid=0x", std::hex, block.guid, std::dec,
                                  " displayId=", unit->getDisplayId(), " at (",
                                  unit->getX(), ",", unit->getY(), ",", unit->getZ(), ")");
                        float unitScale = 1.0f;
                        {
                            uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
                            if (scaleIdx != 0xFFFF) {
                                uint32_t raw = entity->getField(scaleIdx);
                                if (raw != 0) {
                                    std::memcpy(&unitScale, &raw, sizeof(float));
                                    if (unitScale <= 0.01f || unitScale > 100.0f) unitScale = 1.0f;
                                }
                            }
                        }
                        creatureSpawnCallback_(block.guid, unit->getDisplayId(),
                            unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation(), unitScale);
                        if (unitInitiallyDead && npcDeathCallback_) {
                            npcDeathCallback_(block.guid);
                        }
                    }
                    // Initialise swim/walk state from spawn-time movement flags (cold-join fix).
                    // Without this, an entity already swimming/walking when the client joins
                    // won't get its animation state set until the next MSG_MOVE_* heartbeat.
                    if (block.hasMovement && block.moveFlags != 0 && unitMoveFlagsCallback_ &&
                        block.guid != playerGuid) {
                        unitMoveFlagsCallback_(block.guid, block.moveFlags);
                    }
                    // Query quest giver status for NPCs with questgiver flag (0x02)
                    if (block.objectType == ObjectType::UNIT && (unit->getNpcFlags() & 0x02) && socket) {
                        network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                        qsPkt.writeUInt64(block.guid);
                        socket->send(qsPkt);
                    }
                }
            }
            // Extract displayId and entry for gameobjects (3.3.5a: GAMEOBJECT_DISPLAYID = field 8)
            if (block.objectType == ObjectType::GAMEOBJECT) {
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
                if (go->getDisplayId() != 0 && gameObjectSpawnCallback_) {
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
                    gameObjectSpawnCallback_(block.guid, go->getEntry(), go->getDisplayId(),
                        go->getX(), go->getY(), go->getZ(), go->getOrientation(), goScale);
                }
                // Fire transport move callback for transports (position update on re-creation)
                if (transportGuids_.count(block.guid) && transportMoveCallback_) {
                    serverUpdatedTransportGuids_.insert(block.guid);
                    transportMoveCallback_(block.guid,
                        go->getX(), go->getY(), go->getZ(), go->getOrientation());
                }
            }
            // Detect player's own corpse object so we have the position even when
            // SMSG_DEATH_RELEASE_LOC hasn't been received (e.g. login as ghost).
            if (block.objectType == ObjectType::CORPSE && block.hasMovement) {
                // CORPSE_FIELD_OWNER is at index 6 (uint64, low word at 6, high at 7)
                uint16_t ownerLowIdx = 6;
                auto ownerLowIt = block.fields.find(ownerLowIdx);
                uint32_t ownerLow = (ownerLowIt != block.fields.end()) ? ownerLowIt->second : 0;
                auto ownerHighIt = block.fields.find(ownerLowIdx + 1);
                uint32_t ownerHigh = (ownerHighIt != block.fields.end()) ? ownerHighIt->second : 0;
                uint64_t ownerGuid = (static_cast<uint64_t>(ownerHigh) << 32) | ownerLow;
                if (ownerGuid == playerGuid || ownerLow == static_cast<uint32_t>(playerGuid)) {
                    // Server coords from movement block
                    corpseGuid_  = block.guid;
                    corpseX_     = block.x;
                    corpseY_     = block.y;
                    corpseZ_     = block.z;
                    corpseMapId_ = currentMapId_;
                    LOG_INFO("Corpse object detected: guid=0x", std::hex, corpseGuid_, std::dec,
                             " server=(", block.x, ", ", block.y, ", ", block.z,
                             ") map=", corpseMapId_);
                }
            }

            // Track online item objects (CONTAINER = bags, also tracked as items)
            if (block.objectType == ObjectType::ITEM || block.objectType == ObjectType::CONTAINER) {
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
                    OnlineItemInfo info = onlineItems_.count(block.guid)
                        ? onlineItems_[block.guid] : OnlineItemInfo{};
                    info.entry = entryIt->second;
                    if (stackIt    != block.fields.end()) info.stackCount            = stackIt->second;
                    if (durIt      != block.fields.end()) info.curDurability         = durIt->second;
                    if (maxDurIt   != block.fields.end()) info.maxDurability         = maxDurIt->second;
                    if (permEnchIt != block.fields.end()) info.permanentEnchantId    = permEnchIt->second;
                    if (tempEnchIt != block.fields.end()) info.temporaryEnchantId    = tempEnchIt->second;
                    if (sock1EnchIt != block.fields.end()) info.socketEnchantIds[0]  = sock1EnchIt->second;
                    if (sock2EnchIt != block.fields.end()) info.socketEnchantIds[1]  = sock2EnchIt->second;
                    if (sock3EnchIt != block.fields.end()) info.socketEnchantIds[2]  = sock3EnchIt->second;
                    auto [itemIt, isNew] = onlineItems_.insert_or_assign(block.guid, info);
                    if (isNew) newItemCreated = true;
                    queryItemInfo(info.entry, block.guid);
                }
                // Extract container slot GUIDs for bags
                if (block.objectType == ObjectType::CONTAINER) {
                    extractContainerFields(block.guid, block.fields);
                }
            }

            // Extract XP / inventory slot / skill fields for player entity
            if (block.guid == playerGuid && block.objectType == ObjectType::PLAYER) {
                // Auto-detect coinage index using the previous snapshot vs this full snapshot.
                maybeDetectCoinageIndex(lastPlayerFields_, block.fields);

                lastPlayerFields_ = block.fields;
                detectInventorySlotBases(block.fields);

                if (kVerboseUpdateObject) {
                    uint16_t maxField = 0;
                    for (const auto& [key, _val] : block.fields) {
                        if (key > maxField) maxField = key;
                    }
                    LOG_INFO("Player update with ", block.fields.size(),
                             " fields (max index=", maxField, ")");
                }

                bool slotsChanged = false;
                const uint16_t ufPlayerXp = fieldIndex(UF::PLAYER_XP);
                const uint16_t ufPlayerNextXp = fieldIndex(UF::PLAYER_NEXT_LEVEL_XP);
                const uint16_t ufPlayerRestedXp = fieldIndex(UF::PLAYER_REST_STATE_EXPERIENCE);
                const uint16_t ufPlayerLevel = fieldIndex(UF::UNIT_FIELD_LEVEL);
                const uint16_t ufCoinage = fieldIndex(UF::PLAYER_FIELD_COINAGE);
                const uint16_t ufHonor   = fieldIndex(UF::PLAYER_FIELD_HONOR_CURRENCY);
                const uint16_t ufArena   = fieldIndex(UF::PLAYER_FIELD_ARENA_CURRENCY);
                const uint16_t ufArmor = fieldIndex(UF::UNIT_FIELD_RESISTANCES);
                const uint16_t ufPBytes2 = fieldIndex(UF::PLAYER_BYTES_2);
                const uint16_t ufChosenTitle = fieldIndex(UF::PLAYER_CHOSEN_TITLE);
                const uint16_t ufStats[5] = {
                    fieldIndex(UF::UNIT_FIELD_STAT0), fieldIndex(UF::UNIT_FIELD_STAT1),
                    fieldIndex(UF::UNIT_FIELD_STAT2), fieldIndex(UF::UNIT_FIELD_STAT3),
                    fieldIndex(UF::UNIT_FIELD_STAT4)
                };
                const uint16_t ufMeleeAP   = fieldIndex(UF::UNIT_FIELD_ATTACK_POWER);
                const uint16_t ufRangedAP  = fieldIndex(UF::UNIT_FIELD_RANGED_ATTACK_POWER);
                const uint16_t ufSpDmg1    = fieldIndex(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS);
                const uint16_t ufHealBonus = fieldIndex(UF::PLAYER_FIELD_MOD_HEALING_DONE_POS);
                const uint16_t ufBlockPct  = fieldIndex(UF::PLAYER_BLOCK_PERCENTAGE);
                const uint16_t ufDodgePct  = fieldIndex(UF::PLAYER_DODGE_PERCENTAGE);
                const uint16_t ufParryPct  = fieldIndex(UF::PLAYER_PARRY_PERCENTAGE);
                const uint16_t ufCritPct   = fieldIndex(UF::PLAYER_CRIT_PERCENTAGE);
                const uint16_t ufRCritPct  = fieldIndex(UF::PLAYER_RANGED_CRIT_PERCENTAGE);
                const uint16_t ufSCrit1    = fieldIndex(UF::PLAYER_SPELL_CRIT_PERCENTAGE1);
                const uint16_t ufRating1   = fieldIndex(UF::PLAYER_FIELD_COMBAT_RATING_1);
                for (const auto& [key, val] : block.fields) {
                    if (key == ufPlayerXp) { playerXp_ = val; }
                    else if (key == ufPlayerNextXp) { playerNextLevelXp_ = val; }
                    else if (ufPlayerRestedXp != 0xFFFF && key == ufPlayerRestedXp) { playerRestedXp_ = val; }
                    else if (key == ufPlayerLevel) {
                        serverPlayerLevel_ = val;
                        for (auto& ch : characters) {
                            if (ch.guid == playerGuid) { ch.level = val; break; }
                        }
                    }
                    else if (key == ufCoinage) {
                        uint64_t oldMoney = playerMoneyCopper_;
                        playerMoneyCopper_ = val;
                        LOG_DEBUG("Money set from update fields: ", val, " copper");
                        if (val != oldMoney)
                            fireAddonEvent("PLAYER_MONEY", {});
                    }
                    else if (ufHonor != 0xFFFF && key == ufHonor) {
                        playerHonorPoints_ = val;
                        LOG_DEBUG("Honor points from update fields: ", val);
                    }
                    else if (ufArena != 0xFFFF && key == ufArena) {
                        playerArenaPoints_ = val;
                        LOG_DEBUG("Arena points from update fields: ", val);
                    }
                    else if (ufArmor != 0xFFFF && key == ufArmor) {
                        playerArmorRating_ = static_cast<int32_t>(val);
                        LOG_DEBUG("Armor rating from update fields: ", playerArmorRating_);
                    }
                    else if (ufArmor != 0xFFFF && key > ufArmor && key <= ufArmor + 6) {
                        playerResistances_[key - ufArmor - 1] = static_cast<int32_t>(val);
                    }
                    else if (ufPBytes2 != 0xFFFF && key == ufPBytes2) {
                        uint8_t bankBagSlots = static_cast<uint8_t>((val >> 16) & 0xFF);
                        LOG_WARNING("PLAYER_BYTES_2 (CREATE): raw=0x", std::hex, val, std::dec,
                                   " bankBagSlots=", static_cast<int>(bankBagSlots));
                        inventory.setPurchasedBankBagSlots(bankBagSlots);
                        // Byte 3 (bits 24-31): REST_STATE
                        // 0 = not resting, 1 = REST_TYPE_IN_TAVERN, 2 = REST_TYPE_IN_CITY
                        uint8_t restStateByte = static_cast<uint8_t>((val >> 24) & 0xFF);
                        bool wasResting = isResting_;
                        isResting_ = (restStateByte != 0);
                        if (isResting_ != wasResting) {
                            fireAddonEvent("UPDATE_EXHAUSTION", {});
                            fireAddonEvent("PLAYER_UPDATE_RESTING", {});
                        }
                    }
                    else if (ufChosenTitle != 0xFFFF && key == ufChosenTitle) {
                        chosenTitleBit_ = static_cast<int32_t>(val);
                        LOG_DEBUG("PLAYER_CHOSEN_TITLE from update fields: ", chosenTitleBit_);
                    }
                    else if (ufMeleeAP  != 0xFFFF && key == ufMeleeAP)  { playerMeleeAP_  = static_cast<int32_t>(val); }
                    else if (ufRangedAP != 0xFFFF && key == ufRangedAP) { playerRangedAP_ = static_cast<int32_t>(val); }
                    else if (ufSpDmg1   != 0xFFFF && key >= ufSpDmg1 && key < ufSpDmg1 + 7) {
                        playerSpellDmgBonus_[key - ufSpDmg1] = static_cast<int32_t>(val);
                    }
                    else if (ufHealBonus != 0xFFFF && key == ufHealBonus) { playerHealBonus_ = static_cast<int32_t>(val); }
                    else if (ufBlockPct != 0xFFFF && key == ufBlockPct) { std::memcpy(&playerBlockPct_, &val, 4); }
                    else if (ufDodgePct != 0xFFFF && key == ufDodgePct) { std::memcpy(&playerDodgePct_, &val, 4); }
                    else if (ufParryPct != 0xFFFF && key == ufParryPct) { std::memcpy(&playerParryPct_, &val, 4); }
                    else if (ufCritPct  != 0xFFFF && key == ufCritPct)  { std::memcpy(&playerCritPct_,  &val, 4); }
                    else if (ufRCritPct != 0xFFFF && key == ufRCritPct) { std::memcpy(&playerRangedCritPct_, &val, 4); }
                    else if (ufSCrit1   != 0xFFFF && key >= ufSCrit1 && key < ufSCrit1 + 7) {
                        std::memcpy(&playerSpellCritPct_[key - ufSCrit1], &val, 4);
                    }
                    else if (ufRating1  != 0xFFFF && key >= ufRating1 && key < ufRating1 + 25) {
                        playerCombatRatings_[key - ufRating1] = static_cast<int32_t>(val);
                    }
                    else {
                        for (int si = 0; si < 5; ++si) {
                            if (ufStats[si] != 0xFFFF && key == ufStats[si]) {
                                playerStats_[si] = static_cast<int32_t>(val);
                                break;
                            }
                        }
                    }
                    // Do not synthesize quest-log entries from raw update-field slots.
                    // Slot layouts differ on some classic-family realms and can produce
                    // phantom "already accepted" quests that block quest acceptance.
                }
                if (applyInventoryFields(block.fields)) slotsChanged = true;
                if (slotsChanged) rebuildOnlineInventory();
                maybeDetectVisibleItemLayout();
                extractSkillFields(lastPlayerFields_);
                extractExploredZoneFields(lastPlayerFields_);
                applyQuestStateFromFields(lastPlayerFields_);
            }
            break;
        }

        case UpdateType::VALUES: {
            // Update existing entity fields
            auto entity = entityManager.getEntity(block.guid);
            if (entity) {
                if (block.hasMovement) {
                    glm::vec3 pos = core::coords::serverToCanonical(glm::vec3(block.x, block.y, block.z));
                    float oCanonical = core::coords::serverToCanonicalYaw(block.orientation);
                    entity->setPosition(pos.x, pos.y, pos.z, oCanonical);

                    if (block.guid != playerGuid &&
                        (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::GAMEOBJECT)) {
                        if (block.onTransport && block.transportGuid != 0) {
                            glm::vec3 localOffset = core::coords::serverToCanonical(
                                glm::vec3(block.transportX, block.transportY, block.transportZ));
                            const bool hasLocalOrientation = (block.updateFlags & 0x0020) != 0; // UPDATEFLAG_LIVING
                            float localOriCanonical = core::coords::normalizeAngleRad(-block.transportO);
                            setTransportAttachment(block.guid, entity->getType(), block.transportGuid,
                                                   localOffset, hasLocalOrientation, localOriCanonical);
                            if (transportManager_ && transportManager_->getTransport(block.transportGuid)) {
                                glm::vec3 composed = transportManager_->getPlayerWorldPosition(block.transportGuid, localOffset);
                                entity->setPosition(composed.x, composed.y, composed.z, entity->getOrientation());
                            }
                        } else {
                            clearTransportAttachment(block.guid);
                        }
                    }
                }

                for (const auto& field : block.fields) {
                    entity->setField(field.first, field.second);
                }

                if (entity->getType() == ObjectType::PLAYER && block.guid != playerGuid) {
                    updateOtherPlayerVisibleItems(block.guid, entity->getFields());
                }

                // Update cached health/mana/power values (Phase 2) — single pass
                if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                    auto unit = std::static_pointer_cast<Unit>(entity);
                    constexpr uint32_t UNIT_DYNFLAG_DEAD = 0x0008;
                    constexpr uint32_t UNIT_DYNFLAG_LOOTABLE = 0x0001;
                    uint32_t oldDisplayId = unit->getDisplayId();
                    bool displayIdChanged = false;
                    bool npcDeathNotified = false;
                    bool npcRespawnNotified = false;
                    bool healthChanged = false;
                    bool powerChanged = false;
                    const uint16_t ufHealth = fieldIndex(UF::UNIT_FIELD_HEALTH);
                    const uint16_t ufPowerBase = fieldIndex(UF::UNIT_FIELD_POWER1);
                    const uint16_t ufMaxHealth = fieldIndex(UF::UNIT_FIELD_MAXHEALTH);
                    const uint16_t ufMaxPowerBase = fieldIndex(UF::UNIT_FIELD_MAXPOWER1);
                    const uint16_t ufLevel = fieldIndex(UF::UNIT_FIELD_LEVEL);
                    const uint16_t ufFaction = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE);
                    const uint16_t ufFlags = fieldIndex(UF::UNIT_FIELD_FLAGS);
                    const uint16_t ufDynFlags = fieldIndex(UF::UNIT_DYNAMIC_FLAGS);
                    const uint16_t ufDisplayId = fieldIndex(UF::UNIT_FIELD_DISPLAYID);
                    const uint16_t ufMountDisplayId = fieldIndex(UF::UNIT_FIELD_MOUNTDISPLAYID);
                    const uint16_t ufNpcFlags = fieldIndex(UF::UNIT_NPC_FLAGS);
                    const uint16_t ufBytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
                    const uint16_t ufBytes1 = fieldIndex(UF::UNIT_FIELD_BYTES_1);
                    for (const auto& [key, val] : block.fields) {
                        if (key == ufHealth) {
                            uint32_t oldHealth = unit->getHealth();
                            unit->setHealth(val);
                            healthChanged = true;
                            if (val == 0) {
                                if (combatHandler_ && block.guid == combatHandler_->getAutoAttackTargetGuid()) {
                                    stopAutoAttack();
                                }
                                if (combatHandler_) combatHandler_->removeHostileAttacker(block.guid);
                                if (block.guid == playerGuid) {
                                    playerDead_ = true;
                                    releasedSpirit_ = false;
                                    stopAutoAttack();
                                    // Cache death position as corpse location.
                                    // Classic WoW does not send SMSG_DEATH_RELEASE_LOC, so
                                    // this is the primary source for canReclaimCorpse().
                                    // movementInfo is canonical (x=north, y=west); corpseX_/Y_
                                    // are raw server coords (x=west, y=north) — swap axes.
                                    corpseX_     = movementInfo.y;   // canonical west  = server X
                                    corpseY_     = movementInfo.x;   // canonical north = server Y
                                    corpseZ_     = movementInfo.z;
                                    corpseMapId_ = currentMapId_;
                                    LOG_INFO("Player died! Corpse position cached at server=(",
                                             corpseX_, ",", corpseY_, ",", corpseZ_,
                                             ") map=", corpseMapId_);
                                                                            fireAddonEvent("PLAYER_DEAD", {});
                                }
                                if ((entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) && npcDeathCallback_) {
                                    npcDeathCallback_(block.guid);
                                    npcDeathNotified = true;
                                }
                            } else if (oldHealth == 0 && val > 0) {
                                if (block.guid == playerGuid) {
                                    bool wasGhost = releasedSpirit_;
                                    playerDead_ = false;
                                    if (!wasGhost) {
                                        LOG_INFO("Player resurrected!");
                                                                                    fireAddonEvent("PLAYER_ALIVE", {});
                                    } else {
                                        LOG_INFO("Player entered ghost form");
                                        releasedSpirit_ = false;
                                                                                    fireAddonEvent("PLAYER_UNGHOST", {});
                                    }
                                }
                                if ((entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) && npcRespawnCallback_) {
                                    npcRespawnCallback_(block.guid);
                                    npcRespawnNotified = true;
                                }
                            }
                        // Specific fields checked BEFORE power/maxpower range checks
                        // (Classic packs maxHealth/level/faction adjacent to power indices)
                        } else if (key == ufMaxHealth) { unit->setMaxHealth(val); healthChanged = true; }
                        else if (key == ufBytes0) {
                            uint8_t oldPT = unit->getPowerType();
                            unit->setPowerType(static_cast<uint8_t>((val >> 24) & 0xFF));
                            if (unit->getPowerType() != oldPT) {
                                auto uid = guidToUnitId(block.guid);
                                if (!uid.empty())
                                    fireAddonEvent("UNIT_DISPLAYPOWER", {uid});
                            }
                        } else if (key == ufFlags) { unit->setUnitFlags(val); }
                        else if (ufBytes1 != 0xFFFF && key == ufBytes1 && block.guid == playerGuid) {
                            uint8_t newForm = static_cast<uint8_t>((val >> 24) & 0xFF);
                            if (newForm != shapeshiftFormId_) {
                                shapeshiftFormId_ = newForm;
                                LOG_INFO("Shapeshift form changed: ", static_cast<int>(newForm));
                                    fireAddonEvent("UPDATE_SHAPESHIFT_FORM", {});
                                    fireAddonEvent("UPDATE_SHAPESHIFT_FORMS", {});
                            }
                        }
                        else if (key == ufDynFlags) {
                            uint32_t oldDyn = unit->getDynamicFlags();
                            unit->setDynamicFlags(val);
                            if (block.guid == playerGuid) {
                                bool wasDead = (oldDyn & UNIT_DYNFLAG_DEAD) != 0;
                                bool nowDead = (val & UNIT_DYNFLAG_DEAD) != 0;
                                if (!wasDead && nowDead) {
                                    playerDead_ = true;
                                    releasedSpirit_ = false;
                                    corpseX_     = movementInfo.y;
                                    corpseY_     = movementInfo.x;
                                    corpseZ_     = movementInfo.z;
                                    corpseMapId_ = currentMapId_;
                                    LOG_INFO("Player died (dynamic flags). Corpse cached map=", corpseMapId_);
                                } else if (wasDead && !nowDead) {
                                    playerDead_ = false;
                                    releasedSpirit_ = false;
                                    selfResAvailable_ = false;
                                    LOG_INFO("Player resurrected (dynamic flags)");
                                }
                            } else if (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) {
                                bool wasDead = (oldDyn & UNIT_DYNFLAG_DEAD) != 0;
                                bool nowDead = (val & UNIT_DYNFLAG_DEAD) != 0;
                                if (!wasDead && nowDead) {
                                    if (!npcDeathNotified && npcDeathCallback_) {
                                        npcDeathCallback_(block.guid);
                                        npcDeathNotified = true;
                                    }
                                } else if (wasDead && !nowDead) {
                                    if (!npcRespawnNotified && npcRespawnCallback_) {
                                        npcRespawnCallback_(block.guid);
                                        npcRespawnNotified = true;
                                    }
                                }
                            }
                        } else if (key == ufLevel) {
                            uint32_t oldLvl = unit->getLevel();
                            unit->setLevel(val);
                            if (val != oldLvl) {
                                auto uid = guidToUnitId(block.guid);
                                if (!uid.empty())
                                    fireAddonEvent("UNIT_LEVEL", {uid});
                            }
                            if (block.guid != playerGuid &&
                                entity->getType() == ObjectType::PLAYER &&
                                val > oldLvl && oldLvl > 0 &&
                                otherPlayerLevelUpCallback_) {
                                otherPlayerLevelUpCallback_(block.guid, val);
                            }
                        }
                        else if (key == ufFaction) {
                            unit->setFactionTemplate(val);
                            unit->setHostile(isHostileFaction(val));
                        } else if (key == ufDisplayId) {
                            if (val != unit->getDisplayId()) {
                                unit->setDisplayId(val);
                                displayIdChanged = true;
                            }
                        } else if (key == ufMountDisplayId) {
                            if (block.guid == playerGuid) {
                                uint32_t old = currentMountDisplayId_;
                                currentMountDisplayId_ = val;
                                if (val != old && mountCallback_) mountCallback_(val);
                                if (val != old)
                                    fireAddonEvent("UNIT_MODEL_CHANGED", {"player"});
                                if (old == 0 && val != 0) {
                                    mountAuraSpellId_ = 0;
                                    if (spellHandler_) for (const auto& a : spellHandler_->playerAuras_) {
                                        if (!a.isEmpty() && a.maxDurationMs < 0 && a.casterGuid == playerGuid) {
                                            mountAuraSpellId_ = a.spellId;
                                        }
                                    }
                                    // Classic/vanilla fallback: scan UNIT_FIELD_AURAS from same update block
                                    if (mountAuraSpellId_ == 0) {
                                        const uint16_t ufAuras = fieldIndex(UF::UNIT_FIELD_AURAS);
                                        if (ufAuras != 0xFFFF) {
                                            for (const auto& [fk, fv] : block.fields) {
                                                if (fk >= ufAuras && fk < ufAuras + 48 && fv != 0) {
                                                    mountAuraSpellId_ = fv;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    LOG_INFO("Mount detected (values update): displayId=", val, " auraSpellId=", mountAuraSpellId_);
                                }
                                if (old != 0 && val == 0) {
                                    mountAuraSpellId_ = 0;
                                    if (spellHandler_) for (auto& a : spellHandler_->playerAuras_)
                                        if (!a.isEmpty() && a.maxDurationMs < 0) a = AuraSlot{};
                                }
                            }
                            unit->setMountDisplayId(val);
                        } else if (key == ufNpcFlags) { unit->setNpcFlags(val); }
                        // Power/maxpower range checks AFTER all specific fields
                        else if (key >= ufPowerBase && key < ufPowerBase + 7) {
                            unit->setPowerByType(static_cast<uint8_t>(key - ufPowerBase), val);
                            powerChanged = true;
                        } else if (key >= ufMaxPowerBase && key < ufMaxPowerBase + 7) {
                            unit->setMaxPowerByType(static_cast<uint8_t>(key - ufMaxPowerBase), val);
                            powerChanged = true;
                        }
                    }

                    // Fire UNIT_HEALTH / UNIT_POWER events for Lua addons
                    if ((healthChanged || powerChanged)) {
                        auto unitId = guidToUnitId(block.guid);
                        if (!unitId.empty()) {
                            if (healthChanged) fireAddonEvent("UNIT_HEALTH", {unitId});
                            if (powerChanged) {
                                fireAddonEvent("UNIT_POWER", {unitId});
                                // When player power changes, action bar usability may change
                                if (block.guid == playerGuid) {
                                    fireAddonEvent("ACTIONBAR_UPDATE_USABLE", {});
                                    fireAddonEvent("SPELL_UPDATE_USABLE", {});
                                }
                            }
                        }
                    }

                    // Classic: sync spellHandler_->playerAuras_ from UNIT_FIELD_AURAS when those fields are updated
                    if (block.guid == playerGuid && isClassicLikeExpansion() && spellHandler_) {
                        const uint16_t ufAuras     = fieldIndex(UF::UNIT_FIELD_AURAS);
                        const uint16_t ufAuraFlags = fieldIndex(UF::UNIT_FIELD_AURAFLAGS);
                        if (ufAuras != 0xFFFF) {
                            bool hasAuraUpdate = false;
                            for (const auto& [fk, fv] : block.fields) {
                                if (fk >= ufAuras && fk < ufAuras + 48) { hasAuraUpdate = true; break; }
                            }
                            if (hasAuraUpdate) {
                                spellHandler_->playerAuras_.clear();
                                spellHandler_->playerAuras_.resize(48);
                                uint64_t nowMs = static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch()).count());
                                const auto& allFields = entity->getFields();
                                for (int slot = 0; slot < 48; ++slot) {
                                    auto it = allFields.find(static_cast<uint16_t>(ufAuras + slot));
                                    if (it != allFields.end() && it->second != 0) {
                                        AuraSlot& a = spellHandler_->playerAuras_[slot];
                                        a.spellId = it->second;
                                        // Read aura flag byte: packed 4-per-uint32 at ufAuraFlags
                                        uint8_t aFlag = 0;
                                        if (ufAuraFlags != 0xFFFF) {
                                            auto fit = allFields.find(static_cast<uint16_t>(ufAuraFlags + slot / 4));
                                            if (fit != allFields.end())
                                                aFlag = static_cast<uint8_t>((fit->second >> ((slot % 4) * 8)) & 0xFF);
                                        }
                                        a.flags = aFlag;
                                        a.durationMs = -1;
                                        a.maxDurationMs = -1;
                                        a.casterGuid = playerGuid;
                                        a.receivedAtMs = nowMs;
                                    }
                                }
                                LOG_DEBUG("[Classic] Rebuilt playerAuras from UNIT_FIELD_AURAS (VALUES)");
                                                                    fireAddonEvent("UNIT_AURA", {"player"});
                            }
                        }
                    }

                    // Some units/players are created without displayId and get it later via VALUES.
                    if ((entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::PLAYER) &&
                        displayIdChanged &&
                        unit->getDisplayId() != 0 &&
                        unit->getDisplayId() != oldDisplayId) {
                        if (entity->getType() == ObjectType::PLAYER && block.guid == playerGuid) {
                            // Skip local player — spawned separately
                        } else if (entity->getType() == ObjectType::PLAYER) {
                            if (playerSpawnCallback_) {
                                uint8_t race = 0, gender = 0, facial = 0;
                                uint32_t appearanceBytes = 0;
                                // Use the entity's accumulated field state, not just this block's changed fields.
                                if (extractPlayerAppearance(entity->getFields(), race, gender, appearanceBytes, facial)) {
                                    playerSpawnCallback_(block.guid, unit->getDisplayId(), race, gender,
                                                        appearanceBytes, facial,
                                                        unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation());
                                } else {
                                    LOG_WARNING("[Spawn] PLAYER guid=0x", std::hex, block.guid, std::dec,
                                              " displayId=", unit->getDisplayId(), " appearance extraction failed (VALUES update) — model will not render");
                                }
                            }
                            bool isDeadNow = (unit->getHealth() == 0) ||
                                ((unit->getDynamicFlags() & (UNIT_DYNFLAG_DEAD | UNIT_DYNFLAG_LOOTABLE)) != 0);
                            if (isDeadNow && !npcDeathNotified && npcDeathCallback_) {
                                npcDeathCallback_(block.guid);
                                npcDeathNotified = true;
                            }
                        } else if (creatureSpawnCallback_) {
                            float unitScale2 = 1.0f;
                            {
                                uint16_t scaleIdx = fieldIndex(UF::OBJECT_FIELD_SCALE_X);
                                if (scaleIdx != 0xFFFF) {
                                    uint32_t raw = entity->getField(scaleIdx);
                                    if (raw != 0) {
                                        std::memcpy(&unitScale2, &raw, sizeof(float));
                                        if (unitScale2 <= 0.01f || unitScale2 > 100.0f) unitScale2 = 1.0f;
                                    }
                                }
                            }
                            creatureSpawnCallback_(block.guid, unit->getDisplayId(),
                                unit->getX(), unit->getY(), unit->getZ(), unit->getOrientation(), unitScale2);
                            bool isDeadNow = (unit->getHealth() == 0) ||
                                ((unit->getDynamicFlags() & (UNIT_DYNFLAG_DEAD | UNIT_DYNFLAG_LOOTABLE)) != 0);
                            if (isDeadNow && !npcDeathNotified && npcDeathCallback_) {
                                npcDeathCallback_(block.guid);
                                npcDeathNotified = true;
                            }
                        }
                        if (entity->getType() == ObjectType::UNIT && (unit->getNpcFlags() & 0x02) && socket) {
                            network::Packet qsPkt(wireOpcode(Opcode::CMSG_QUESTGIVER_STATUS_QUERY));
                            qsPkt.writeUInt64(block.guid);
                            socket->send(qsPkt);
                        }
                        // Fire UNIT_MODEL_CHANGED for addons that track model swaps
                        if (addonEventCallback_) {
                            std::string uid;
                            if (block.guid == targetGuid) uid = "target";
                            else if (block.guid == focusGuid) uid = "focus";
                            else if (block.guid == petGuid_) uid = "pet";
                            if (!uid.empty())
                                fireAddonEvent("UNIT_MODEL_CHANGED", {uid});
                        }
                    }
                }
                // Update XP / inventory slot / skill fields for player entity
                if (block.guid == playerGuid) {
                    const bool needCoinageDetectSnapshot =
                        (pendingMoneyDelta_ != 0 && pendingMoneyDeltaTimer_ > 0.0f);
                    std::map<uint16_t, uint32_t> oldFieldsSnapshot;
                    if (needCoinageDetectSnapshot) {
                        oldFieldsSnapshot = lastPlayerFields_;
                    }
                    if (block.hasMovement && block.runSpeed > 0.1f && block.runSpeed < 100.0f) {
                        serverRunSpeed_ = block.runSpeed;
                        // Some server dismount paths update run speed without updating mount display field.
                        if (!onTaxiFlight_ && !taxiMountActive_ &&
                            currentMountDisplayId_ != 0 && block.runSpeed <= 8.5f) {
                            LOG_INFO("Auto-clearing mount from movement speed update: speed=", block.runSpeed,
                                     " displayId=", currentMountDisplayId_);
                            currentMountDisplayId_ = 0;
                            if (mountCallback_) {
                                mountCallback_(0);
                            }
                        }
                    }
                    auto mergeHint = lastPlayerFields_.end();
                    for (const auto& [key, val] : block.fields) {
                        mergeHint = lastPlayerFields_.insert_or_assign(mergeHint, key, val);
                    }
                    if (needCoinageDetectSnapshot) {
                        maybeDetectCoinageIndex(oldFieldsSnapshot, lastPlayerFields_);
                    }
                    maybeDetectVisibleItemLayout();
                    detectInventorySlotBases(block.fields);
                    bool slotsChanged = false;
                    const uint16_t ufPlayerXp = fieldIndex(UF::PLAYER_XP);
                    const uint16_t ufPlayerNextXp = fieldIndex(UF::PLAYER_NEXT_LEVEL_XP);
                    const uint16_t ufPlayerRestedXpV = fieldIndex(UF::PLAYER_REST_STATE_EXPERIENCE);
                    const uint16_t ufPlayerLevel = fieldIndex(UF::UNIT_FIELD_LEVEL);
                    const uint16_t ufCoinage = fieldIndex(UF::PLAYER_FIELD_COINAGE);
                    const uint16_t ufHonorV  = fieldIndex(UF::PLAYER_FIELD_HONOR_CURRENCY);
                    const uint16_t ufArenaV  = fieldIndex(UF::PLAYER_FIELD_ARENA_CURRENCY);
                    const uint16_t ufPlayerFlags = fieldIndex(UF::PLAYER_FLAGS);
                    const uint16_t ufArmor = fieldIndex(UF::UNIT_FIELD_RESISTANCES);
                    const uint16_t ufPBytesV = fieldIndex(UF::PLAYER_BYTES);
                    const uint16_t ufPBytes2v = fieldIndex(UF::PLAYER_BYTES_2);
                    const uint16_t ufChosenTitle = fieldIndex(UF::PLAYER_CHOSEN_TITLE);
                    const uint16_t ufStatsV[5] = {
                        fieldIndex(UF::UNIT_FIELD_STAT0), fieldIndex(UF::UNIT_FIELD_STAT1),
                        fieldIndex(UF::UNIT_FIELD_STAT2), fieldIndex(UF::UNIT_FIELD_STAT3),
                        fieldIndex(UF::UNIT_FIELD_STAT4)
                    };
                    const uint16_t ufMeleeAPV  = fieldIndex(UF::UNIT_FIELD_ATTACK_POWER);
                    const uint16_t ufRangedAPV = fieldIndex(UF::UNIT_FIELD_RANGED_ATTACK_POWER);
                    const uint16_t ufSpDmg1V   = fieldIndex(UF::PLAYER_FIELD_MOD_DAMAGE_DONE_POS);
                    const uint16_t ufHealBonusV= fieldIndex(UF::PLAYER_FIELD_MOD_HEALING_DONE_POS);
                    const uint16_t ufBlockPctV = fieldIndex(UF::PLAYER_BLOCK_PERCENTAGE);
                    const uint16_t ufDodgePctV = fieldIndex(UF::PLAYER_DODGE_PERCENTAGE);
                    const uint16_t ufParryPctV = fieldIndex(UF::PLAYER_PARRY_PERCENTAGE);
                    const uint16_t ufCritPctV  = fieldIndex(UF::PLAYER_CRIT_PERCENTAGE);
                    const uint16_t ufRCritPctV = fieldIndex(UF::PLAYER_RANGED_CRIT_PERCENTAGE);
                    const uint16_t ufSCrit1V   = fieldIndex(UF::PLAYER_SPELL_CRIT_PERCENTAGE1);
                    const uint16_t ufRating1V  = fieldIndex(UF::PLAYER_FIELD_COMBAT_RATING_1);
                    for (const auto& [key, val] : block.fields) {
                        if (key == ufPlayerXp) {
                            playerXp_ = val;
                            LOG_DEBUG("XP updated: ", val);
                                                            fireAddonEvent("PLAYER_XP_UPDATE", {std::to_string(val)});
                        }
                        else if (key == ufPlayerNextXp) {
                            playerNextLevelXp_ = val;
                            LOG_DEBUG("Next level XP updated: ", val);
                        }
                        else if (ufPlayerRestedXpV != 0xFFFF && key == ufPlayerRestedXpV) {
                            playerRestedXp_ = val;
                                                            fireAddonEvent("UPDATE_EXHAUSTION", {});
                        }
                        else if (key == ufPlayerLevel) {
                            serverPlayerLevel_ = val;
                            LOG_DEBUG("Level updated: ", val);
                            for (auto& ch : characters) {
                                if (ch.guid == playerGuid) {
                                    ch.level = val;
                                    break;
                                }
                            }
                        }
                        else if (key == ufCoinage) {
                            uint64_t oldM = playerMoneyCopper_;
                            playerMoneyCopper_ = val;
                            LOG_DEBUG("Money updated via VALUES: ", val, " copper");
                            if (val != oldM)
                                fireAddonEvent("PLAYER_MONEY", {});
                        }
                        else if (ufHonorV != 0xFFFF && key == ufHonorV) {
                            playerHonorPoints_ = val;
                            LOG_DEBUG("Honor points updated: ", val);
                        }
                        else if (ufArenaV != 0xFFFF && key == ufArenaV) {
                            playerArenaPoints_ = val;
                            LOG_DEBUG("Arena points updated: ", val);
                        }
                        else if (ufArmor != 0xFFFF && key == ufArmor) {
                            playerArmorRating_ = static_cast<int32_t>(val);
                        }
                        else if (ufArmor != 0xFFFF && key > ufArmor && key <= ufArmor + 6) {
                            playerResistances_[key - ufArmor - 1] = static_cast<int32_t>(val);
                        }
                        else if (ufPBytesV != 0xFFFF && key == ufPBytesV) {
                            // PLAYER_BYTES changed (barber shop, polymorph, etc.)
                            // Update the Character struct so inventory preview refreshes
                            for (auto& ch : characters) {
                                if (ch.guid == playerGuid) {
                                    ch.appearanceBytes = val;
                                    break;
                                }
                            }
                            if (appearanceChangedCallback_)
                                appearanceChangedCallback_();
                        }
                        else if (ufPBytes2v != 0xFFFF && key == ufPBytes2v) {
                            // Byte 0 (bits 0-7): facial hair / piercings
                            uint8_t facialHair = static_cast<uint8_t>(val & 0xFF);
                            for (auto& ch : characters) {
                                if (ch.guid == playerGuid) {
                                    ch.facialFeatures = facialHair;
                                    break;
                                }
                            }
                            uint8_t bankBagSlots = static_cast<uint8_t>((val >> 16) & 0xFF);
                            LOG_DEBUG("PLAYER_BYTES_2 (VALUES): raw=0x", std::hex, val, std::dec,
                                       " bankBagSlots=", static_cast<int>(bankBagSlots),
                                       " facial=", static_cast<int>(facialHair));
                            inventory.setPurchasedBankBagSlots(bankBagSlots);
                            // Byte 3 (bits 24-31): REST_STATE
                            // 0 = not resting, 1 = REST_TYPE_IN_TAVERN, 2 = REST_TYPE_IN_CITY
                            uint8_t restStateByte = static_cast<uint8_t>((val >> 24) & 0xFF);
                            isResting_ = (restStateByte != 0);
                            if (appearanceChangedCallback_)
                                appearanceChangedCallback_();
                        }
                        else if (ufChosenTitle != 0xFFFF && key == ufChosenTitle) {
                            chosenTitleBit_ = static_cast<int32_t>(val);
                            LOG_DEBUG("PLAYER_CHOSEN_TITLE updated: ", chosenTitleBit_);
                        }
                        else if (key == ufPlayerFlags) {
                            constexpr uint32_t PLAYER_FLAGS_GHOST = 0x00000010;
                            bool wasGhost = releasedSpirit_;
                            bool nowGhost = (val & PLAYER_FLAGS_GHOST) != 0;
                            if (!wasGhost && nowGhost) {
                                releasedSpirit_ = true;
                                LOG_INFO("Player entered ghost form (PLAYER_FLAGS)");
                                if (ghostStateCallback_) ghostStateCallback_(true);
                            } else if (wasGhost && !nowGhost) {
                                releasedSpirit_ = false;
                                playerDead_ = false;
                                repopPending_ = false;
                                resurrectPending_ = false;
                                selfResAvailable_ = false;
                                corpseMapId_ = 0;  // corpse reclaimed
                                corpseGuid_ = 0;
                                corpseReclaimAvailableMs_ = 0;
                                LOG_INFO("Player resurrected (PLAYER_FLAGS ghost cleared)");
                                fireAddonEvent("PLAYER_ALIVE", {});
                                if (ghostStateCallback_) ghostStateCallback_(false);
                            }
                                                            fireAddonEvent("PLAYER_FLAGS_CHANGED", {});
                        }
                        else if (ufMeleeAPV  != 0xFFFF && key == ufMeleeAPV)  { playerMeleeAP_  = static_cast<int32_t>(val); }
                        else if (ufRangedAPV != 0xFFFF && key == ufRangedAPV) { playerRangedAP_ = static_cast<int32_t>(val); }
                        else if (ufSpDmg1V   != 0xFFFF && key >= ufSpDmg1V && key < ufSpDmg1V + 7) {
                            playerSpellDmgBonus_[key - ufSpDmg1V] = static_cast<int32_t>(val);
                        }
                        else if (ufHealBonusV != 0xFFFF && key == ufHealBonusV) { playerHealBonus_ = static_cast<int32_t>(val); }
                        else if (ufBlockPctV != 0xFFFF && key == ufBlockPctV) { std::memcpy(&playerBlockPct_, &val, 4); }
                        else if (ufDodgePctV != 0xFFFF && key == ufDodgePctV) { std::memcpy(&playerDodgePct_, &val, 4); }
                        else if (ufParryPctV != 0xFFFF && key == ufParryPctV) { std::memcpy(&playerParryPct_, &val, 4); }
                        else if (ufCritPctV  != 0xFFFF && key == ufCritPctV)  { std::memcpy(&playerCritPct_,  &val, 4); }
                        else if (ufRCritPctV != 0xFFFF && key == ufRCritPctV) { std::memcpy(&playerRangedCritPct_, &val, 4); }
                        else if (ufSCrit1V   != 0xFFFF && key >= ufSCrit1V && key < ufSCrit1V + 7) {
                            std::memcpy(&playerSpellCritPct_[key - ufSCrit1V], &val, 4);
                        }
                        else if (ufRating1V  != 0xFFFF && key >= ufRating1V && key < ufRating1V + 25) {
                            playerCombatRatings_[key - ufRating1V] = static_cast<int32_t>(val);
                        }
                        else {
                            for (int si = 0; si < 5; ++si) {
                                if (ufStatsV[si] != 0xFFFF && key == ufStatsV[si]) {
                                    playerStats_[si] = static_cast<int32_t>(val);
                                    break;
                                }
                            }
                        }
                    }
                    // Do not auto-create quests from VALUES quest-log slot fields for the
                    // same reason as CREATE_OBJECT2 above (can be misaligned per realm).
                    if (applyInventoryFields(block.fields)) slotsChanged = true;
                    if (slotsChanged) {
                        rebuildOnlineInventory();
                                                    fireAddonEvent("PLAYER_EQUIPMENT_CHANGED", {});
                    }
                    extractSkillFields(lastPlayerFields_);
                    extractExploredZoneFields(lastPlayerFields_);
                    applyQuestStateFromFields(lastPlayerFields_);
                }

                // Update item stack count / durability for online items
                if (entity->getType() == ObjectType::ITEM || entity->getType() == ObjectType::CONTAINER) {
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

                    auto it = onlineItems_.find(block.guid);
                    bool isItemInInventory = (it != onlineItems_.end());

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
                                    for (uint64_t slotGuid : equipSlotGuids_) {
                                        if (slotGuid == block.guid) { isEquipped = true; break; }
                                    }
                                    if (isEquipped) {
                                        std::string itemName;
                                        const auto* info = getItemInfo(it->second.entry);
                                        if (info) itemName = info->name;
                                        char buf[128];
                                        if (!itemName.empty())
                                            std::snprintf(buf, sizeof(buf), "%s is about to break!", itemName.c_str());
                                        else
                                            std::snprintf(buf, sizeof(buf), "An equipped item is about to break!");
                                        addUIError(buf);
                                        addSystemChatMessage(buf);
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
                        extractContainerFields(block.guid, block.fields);
                    }
                    if (inventoryChanged) {
                        rebuildOnlineInventory();
                            fireAddonEvent("BAG_UPDATE", {});
                            fireAddonEvent("UNIT_INVENTORY_CHANGED", {"player"});
                    }
                }
                if (block.hasMovement && entity->getType() == ObjectType::GAMEOBJECT) {
                    if (transportGuids_.count(block.guid) && transportMoveCallback_) {
                        serverUpdatedTransportGuids_.insert(block.guid);
                        transportMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                               entity->getZ(), entity->getOrientation());
                    } else if (gameObjectMoveCallback_) {
                        gameObjectMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                                entity->getZ(), entity->getOrientation());
                    }
                }

                LOG_DEBUG("Updated entity fields: 0x", std::hex, block.guid, std::dec);
            } else {
            }
            break;
        }

        case UpdateType::MOVEMENT: {
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

                if (block.guid != playerGuid &&
                    (entity->getType() == ObjectType::UNIT || entity->getType() == ObjectType::GAMEOBJECT)) {
                    if (block.onTransport && block.transportGuid != 0) {
                        glm::vec3 localOffset = core::coords::serverToCanonical(
                            glm::vec3(block.transportX, block.transportY, block.transportZ));
                        const bool hasLocalOrientation = (block.updateFlags & 0x0020) != 0; // UPDATEFLAG_LIVING
                        float localOriCanonical = core::coords::normalizeAngleRad(-block.transportO);
                        setTransportAttachment(block.guid, entity->getType(), block.transportGuid,
                                               localOffset, hasLocalOrientation, localOriCanonical);
                        if (transportManager_ && transportManager_->getTransport(block.transportGuid)) {
                            glm::vec3 composed = transportManager_->getPlayerWorldPosition(block.transportGuid, localOffset);
                            entity->setPosition(composed.x, composed.y, composed.z, entity->getOrientation());
                        }
                    } else {
                        clearTransportAttachment(block.guid);
                    }
                }

                if (block.guid == playerGuid) {
                    movementInfo.orientation = oCanonical;

                    // Track player-on-transport state from MOVEMENT updates
                    if (block.onTransport) {
                        // Convert transport offset from server → canonical coordinates
                        glm::vec3 serverOffset(block.transportX, block.transportY, block.transportZ);
                        glm::vec3 canonicalOffset = core::coords::serverToCanonical(serverOffset);
                        setPlayerOnTransport(block.transportGuid, canonicalOffset);
                        if (transportManager_ && transportManager_->getTransport(playerTransportGuid_)) {
                            glm::vec3 composed = transportManager_->getPlayerWorldPosition(playerTransportGuid_, playerTransportOffset_);
                            entity->setPosition(composed.x, composed.y, composed.z, oCanonical);
                            movementInfo.x = composed.x;
                            movementInfo.y = composed.y;
                            movementInfo.z = composed.z;
                        } else {
                            movementInfo.x = pos.x;
                            movementInfo.y = pos.y;
                            movementInfo.z = pos.z;
                        }
                        LOG_INFO("Player on transport (MOVEMENT): 0x", std::hex, playerTransportGuid_, std::dec);
                    } else {
                        movementInfo.x = pos.x;
                        movementInfo.y = pos.y;
                        movementInfo.z = pos.z;
                        // Don't clear client-side M2 transport boarding
                        bool isClientM2Transport = false;
                        if (playerTransportGuid_ != 0 && transportManager_) {
                            auto* tr = transportManager_->getTransport(playerTransportGuid_);
                            isClientM2Transport = (tr && tr->isM2);
                        }
                        if (playerTransportGuid_ != 0 && !isClientM2Transport) {
                            LOG_INFO("Player left transport (MOVEMENT)");
                            clearPlayerTransport();
                        }
                    }
                }

                // Fire transport move callback if this is a known transport
                if (transportGuids_.count(block.guid) && transportMoveCallback_) {
                    serverUpdatedTransportGuids_.insert(block.guid);
                    transportMoveCallback_(block.guid, pos.x, pos.y, pos.z, oCanonical);
                }
                // Fire move callback for non-transport gameobjects.
                if (entity->getType() == ObjectType::GAMEOBJECT &&
                    transportGuids_.count(block.guid) == 0 &&
                    gameObjectMoveCallback_) {
                    gameObjectMoveCallback_(block.guid, entity->getX(), entity->getY(),
                                            entity->getZ(), entity->getOrientation());
                }
                // Fire move callback for non-player units (creatures).
                // SMSG_MONSTER_MOVE handles smooth interpolated movement, but many
                // servers (especially vanilla/Turtle WoW) communicate NPC positions
                // via MOVEMENT blocks instead. Use duration=0 for an instant snap.
                if (block.guid != playerGuid &&
                    entity->getType() == ObjectType::UNIT &&
                    transportGuids_.count(block.guid) == 0 &&
                    creatureMoveCallback_) {
                    creatureMoveCallback_(block.guid, pos.x, pos.y, pos.z, 0);
                }
            } else {
                LOG_WARNING("MOVEMENT update for unknown entity: 0x", std::hex, block.guid, std::dec);
            }
            break;
        }

        default:
            break;
    }
}

void GameHandler::finalizeUpdateObjectBatch(bool newItemCreated) {
    tabCycleStale = true;
    // Entity count logging disabled

    // Deferred rebuild: if new item objects were created in this packet, rebuild
    // inventory so that slot GUIDs updated earlier in the same packet can resolve.
    if (newItemCreated) {
        rebuildOnlineInventory();
    }

    // Late inventory base detection once items are known
    if (playerGuid != 0 && invSlotBase_ < 0 && !lastPlayerFields_.empty() && !onlineItems_.empty()) {
        detectInventorySlotBases(lastPlayerFields_);
        if (invSlotBase_ >= 0) {
            if (applyInventoryFields(lastPlayerFields_)) {
                rebuildOnlineInventory();
            }
        }
    }
}

void GameHandler::handleCompressedUpdateObject(network::Packet& packet) {
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

void GameHandler::handleDestroyObject(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_DESTROY_OBJECT");

    DestroyObjectData data;
    if (!DestroyObjectParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_DESTROY_OBJECT");
        return;
    }

    // Remove entity
    if (entityManager.hasEntity(data.guid)) {
        if (transportGuids_.count(data.guid) > 0) {
            const bool playerAboardNow = (playerTransportGuid_ == data.guid);
            const bool stickyAboard = (playerTransportStickyGuid_ == data.guid && playerTransportStickyTimer_ > 0.0f);
            const bool movementSaysAboard = (movementInfo.transportGuid == data.guid);
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
            if (entity->getType() == ObjectType::UNIT && creatureDespawnCallback_) {
                creatureDespawnCallback_(data.guid);
            } else if (entity->getType() == ObjectType::PLAYER && playerDespawnCallback_) {
                // Player entities also need renderer cleanup on DESTROY_OBJECT, not just out-of-range.
                playerDespawnCallback_(data.guid);
                otherPlayerVisibleItemEntries_.erase(data.guid);
                otherPlayerVisibleDirty_.erase(data.guid);
                otherPlayerMoveTimeMs_.erase(data.guid);
                inspectedPlayerItemEntries_.erase(data.guid);
                pendingAutoInspect_.erase(data.guid);
                pendingNameQueries.erase(data.guid);
            } else if (entity->getType() == ObjectType::GAMEOBJECT && gameObjectDespawnCallback_) {
                gameObjectDespawnCallback_(data.guid);
            }
        }
        if (transportGuids_.count(data.guid) > 0) {
            transportGuids_.erase(data.guid);
            serverUpdatedTransportGuids_.erase(data.guid);
            if (playerTransportGuid_ == data.guid) {
                clearPlayerTransport();
            }
        }
        clearTransportAttachment(data.guid);
        entityManager.removeEntity(data.guid);
        LOG_INFO("Destroyed entity: 0x", std::hex, data.guid, std::dec,
                 " (", (data.isDeath ? "death" : "despawn"), ")");
    } else {
        LOG_DEBUG("Destroy object for unknown entity: 0x", std::hex, data.guid, std::dec);
    }

    // Clean up auto-attack and target if destroyed entity was our target
    if (combatHandler_ && data.guid == combatHandler_->getAutoAttackTargetGuid()) {
        stopAutoAttack();
    }
    if (data.guid == targetGuid) {
        targetGuid = 0;
    }
    if (combatHandler_) combatHandler_->removeHostileAttacker(data.guid);

    // Remove online item/container tracking
    containerContents_.erase(data.guid);
    if (onlineItems_.erase(data.guid)) {
        rebuildOnlineInventory();
    }

    // Clean up quest giver status
    npcQuestStatus_.erase(data.guid);

    // Remove combat text entries referencing the destroyed entity so floating
    // damage numbers don't linger after the source/target despawns.
    if (combatHandler_) combatHandler_->removeCombatTextForGuid(data.guid);

    // Clean up unit cast state (cast bar) for the destroyed unit
    if (spellHandler_) spellHandler_->unitCastStates_.erase(data.guid);
    // Clean up cached auras
    if (spellHandler_) spellHandler_->unitAurasCache_.erase(data.guid);

    tabCycleStale = true;
}

void GameHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target) {
    if (chatHandler_) chatHandler_->sendChatMessage(type, message, target);
}

void GameHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    if (chatHandler_) chatHandler_->sendTextEmote(textEmoteId, targetGuid);
}

void GameHandler::joinChannel(const std::string& channelName, const std::string& password) {
    if (chatHandler_) chatHandler_->joinChannel(channelName, password);
}

void GameHandler::leaveChannel(const std::string& channelName) {
    if (chatHandler_) chatHandler_->leaveChannel(channelName);
}

std::string GameHandler::getChannelByIndex(int index) const {
    return chatHandler_ ? chatHandler_->getChannelByIndex(index) : "";
}

int GameHandler::getChannelIndex(const std::string& channelName) const {
    return chatHandler_ ? chatHandler_->getChannelIndex(channelName) : 0;
}

void GameHandler::autoJoinDefaultChannels() {
    if (chatHandler_) {
        chatHandler_->chatAutoJoin.general = chatAutoJoin.general;
        chatHandler_->chatAutoJoin.trade = chatAutoJoin.trade;
        chatHandler_->chatAutoJoin.localDefense = chatAutoJoin.localDefense;
        chatHandler_->chatAutoJoin.lfg = chatAutoJoin.lfg;
        chatHandler_->chatAutoJoin.local = chatAutoJoin.local;
        chatHandler_->autoJoinDefaultChannels();
    }
}

void GameHandler::setTarget(uint64_t guid) {
    if (combatHandler_) combatHandler_->setTarget(guid);
}

void GameHandler::clearTarget() {
    if (combatHandler_) combatHandler_->clearTarget();
}

std::shared_ptr<Entity> GameHandler::getTarget() const {
    return combatHandler_ ? combatHandler_->getTarget() : nullptr;
}

void GameHandler::setFocus(uint64_t guid) {
    if (combatHandler_) combatHandler_->setFocus(guid);
}

void GameHandler::clearFocus() {
    if (combatHandler_) combatHandler_->clearFocus();
}

void GameHandler::setMouseoverGuid(uint64_t guid) {
    if (combatHandler_) combatHandler_->setMouseoverGuid(guid);
}

std::shared_ptr<Entity> GameHandler::getFocus() const {
    return combatHandler_ ? combatHandler_->getFocus() : nullptr;
}

void GameHandler::targetLastTarget() {
    if (combatHandler_) combatHandler_->targetLastTarget();
}

void GameHandler::targetEnemy(bool reverse) {
    if (combatHandler_) combatHandler_->targetEnemy(reverse);
}

void GameHandler::targetFriend(bool reverse) {
    if (combatHandler_) combatHandler_->targetFriend(reverse);
}

void GameHandler::inspectTarget() {
    if (socialHandler_) socialHandler_->inspectTarget();
}

void GameHandler::queryServerTime() {
    if (socialHandler_) socialHandler_->queryServerTime();
}

void GameHandler::requestPlayedTime() {
    if (socialHandler_) socialHandler_->requestPlayedTime();
}

void GameHandler::queryWho(const std::string& playerName) {
    if (socialHandler_) socialHandler_->queryWho(playerName);
}

void GameHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->addFriend(playerName, note);
}

void GameHandler::removeFriend(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeFriend(playerName);
}

void GameHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->setFriendNote(playerName, note);
}

void GameHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (socialHandler_) socialHandler_->randomRoll(minRoll, maxRoll);
}

void GameHandler::addIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->addIgnore(playerName);
}

void GameHandler::removeIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeIgnore(playerName);
}

void GameHandler::requestLogout() {
    if (socialHandler_) socialHandler_->requestLogout();
}

void GameHandler::cancelLogout() {
    if (socialHandler_) socialHandler_->cancelLogout();
}

void GameHandler::sendSetDifficulty(uint32_t difficulty) {
    if (socialHandler_) socialHandler_->sendSetDifficulty(difficulty);
}

void GameHandler::setStandState(uint8_t standState) {
    if (socialHandler_) socialHandler_->setStandState(standState);
}

void GameHandler::toggleHelm() {
    if (socialHandler_) socialHandler_->toggleHelm();
}

void GameHandler::toggleCloak() {
    if (socialHandler_) socialHandler_->toggleCloak();
}

void GameHandler::followTarget() {
    if (movementHandler_) movementHandler_->followTarget();
}

void GameHandler::cancelFollow() {
    if (movementHandler_) movementHandler_->cancelFollow();
}

void GameHandler::assistTarget() {
    if (combatHandler_) combatHandler_->assistTarget();
}

void GameHandler::togglePvp() {
    if (combatHandler_) combatHandler_->togglePvp();
}

void GameHandler::requestGuildInfo() {
    if (socialHandler_) socialHandler_->requestGuildInfo();
}

void GameHandler::requestGuildRoster() {
    if (socialHandler_) socialHandler_->requestGuildRoster();
}

void GameHandler::setGuildMotd(const std::string& motd) {
    if (socialHandler_) socialHandler_->setGuildMotd(motd);
}

void GameHandler::promoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->promoteGuildMember(playerName);
}

void GameHandler::demoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->demoteGuildMember(playerName);
}

void GameHandler::leaveGuild() {
    if (socialHandler_) socialHandler_->leaveGuild();
}

void GameHandler::inviteToGuild(const std::string& playerName) {
    if (socialHandler_) socialHandler_->inviteToGuild(playerName);
}

void GameHandler::initiateReadyCheck() {
    if (socialHandler_) socialHandler_->initiateReadyCheck();
}

void GameHandler::respondToReadyCheck(bool ready) {
    if (socialHandler_) socialHandler_->respondToReadyCheck(ready);
}

void GameHandler::acceptDuel() {
    if (socialHandler_) socialHandler_->acceptDuel();
}

void GameHandler::forfeitDuel() {
    if (socialHandler_) socialHandler_->forfeitDuel();
}

void GameHandler::toggleAfk(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleAfk(message);
}

void GameHandler::toggleDnd(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleDnd(message);
}

void GameHandler::replyToLastWhisper(const std::string& message) {
    if (chatHandler_) chatHandler_->replyToLastWhisper(message);
}

void GameHandler::uninvitePlayer(const std::string& playerName) {
    if (socialHandler_) socialHandler_->uninvitePlayer(playerName);
}

void GameHandler::leaveParty() {
    if (socialHandler_) socialHandler_->leaveParty();
}

void GameHandler::setMainTank(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainTank(targetGuid);
}

void GameHandler::setMainAssist(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainAssist(targetGuid);
}

void GameHandler::clearMainTank() {
    if (socialHandler_) socialHandler_->clearMainTank();
}

void GameHandler::clearMainAssist() {
    if (socialHandler_) socialHandler_->clearMainAssist();
}

void GameHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (socialHandler_) socialHandler_->setRaidMark(guid, icon);
}

void GameHandler::requestRaidInfo() {
    if (socialHandler_) socialHandler_->requestRaidInfo();
}

void GameHandler::proposeDuel(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->proposeDuel(targetGuid);
}

void GameHandler::initiateTrade(uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->initiateTrade(targetGuid);
}

void GameHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (socialHandler_) socialHandler_->reportPlayer(targetGuid, reason);
}

void GameHandler::stopCasting() {
    if (spellHandler_) spellHandler_->stopCasting();
}

void GameHandler::resetCastState() {
    if (spellHandler_) spellHandler_->resetCastState();
}

void GameHandler::clearUnitCaches() {
    if (spellHandler_) spellHandler_->clearUnitCaches();
}

void GameHandler::releaseSpirit() {
    if (combatHandler_) combatHandler_->releaseSpirit();
}

bool GameHandler::canReclaimCorpse() const {
    return combatHandler_ ? combatHandler_->canReclaimCorpse() : false;
}

float GameHandler::getCorpseReclaimDelaySec() const {
    return combatHandler_ ? combatHandler_->getCorpseReclaimDelaySec() : 0.0f;
}

void GameHandler::reclaimCorpse() {
    if (combatHandler_) combatHandler_->reclaimCorpse();
}

void GameHandler::useSelfRes() {
    if (combatHandler_) combatHandler_->useSelfRes();
}

void GameHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (combatHandler_) combatHandler_->activateSpiritHealer(npcGuid);
}

void GameHandler::acceptResurrect() {
    if (combatHandler_) combatHandler_->acceptResurrect();
}

void GameHandler::declineResurrect() {
    if (combatHandler_) combatHandler_->declineResurrect();
}

void GameHandler::tabTarget(float playerX, float playerY, float playerZ) {
    if (combatHandler_) combatHandler_->tabTarget(playerX, playerY, playerZ);
}

void GameHandler::addLocalChatMessage(const MessageChatData& msg) {
    if (chatHandler_) chatHandler_->addLocalChatMessage(msg);
}

const std::deque<MessageChatData>& GameHandler::getChatHistory() const {
    if (chatHandler_) return chatHandler_->getChatHistory();
    static const std::deque<MessageChatData> kEmpty;
    return kEmpty;
}

void GameHandler::clearChatHistory() {
    if (chatHandler_) chatHandler_->getChatHistory().clear();
}

const std::vector<std::string>& GameHandler::getJoinedChannels() const {
    if (chatHandler_) return chatHandler_->getJoinedChannels();
    static const std::vector<std::string> kEmpty;
    return kEmpty;
}

// ============================================================
// Phase 1: Name Queries
// ============================================================

void GameHandler::queryPlayerName(uint64_t guid) {
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
    if (!isInWorld()) {
        LOG_INFO("queryPlayerName: skipped guid=0x", std::hex, guid, std::dec,
                 " state=", worldStateName(state), " socket=", (socket ? "yes" : "no"));
        return;
    }

    LOG_INFO("queryPlayerName: sending CMSG_NAME_QUERY for guid=0x", std::hex, guid, std::dec);
    pendingNameQueries.insert(guid);
    auto packet = NameQueryPacket::build(guid);
    socket->send(packet);
}

void GameHandler::queryCreatureInfo(uint32_t entry, uint64_t guid) {
    if (creatureInfoCache.count(entry) || pendingCreatureQueries.count(entry)) return;
    if (!isInWorld()) return;

    pendingCreatureQueries.insert(entry);
    auto packet = CreatureQueryPacket::build(entry, guid);
    socket->send(packet);
}

void GameHandler::queryGameObjectInfo(uint32_t entry, uint64_t guid) {
    if (gameObjectInfoCache_.count(entry) || pendingGameObjectQueries_.count(entry)) return;
    if (!isInWorld()) return;

    pendingGameObjectQueries_.insert(entry);
    auto packet = GameObjectQueryPacket::build(entry, guid);
    socket->send(packet);
}

std::string GameHandler::getCachedPlayerName(uint64_t guid) const {
    return std::string(lookupName(guid));
}

std::string GameHandler::getCachedCreatureName(uint32_t entry) const {
    auto it = creatureInfoCache.find(entry);
    return (it != creatureInfoCache.end()) ? it->second.name : "";
}

void GameHandler::handleNameQueryResponse(network::Packet& packet) {
    NameQueryResponseData data;
    if (!packetParsers_ || !packetParsers_->parseNameQueryResponse(packet, data)) {
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
        if (chatHandler_) {
            for (auto& msg : chatHandler_->getChatHistory()) {
                if (msg.senderGuid == data.guid && msg.senderName.empty()) {
                    msg.senderName = data.name;
                }
            }
        }

        // Backfill mail inbox sender names
        for (auto& mail : mailInbox_) {
            if (mail.messageType == 0 && mail.senderGuid == data.guid) {
                mail.senderName = data.name;
            }
        }

        // Backfill friend list: if this GUID came from a friend list packet,
        // register the name in friendsCache now that we know it.
        if (friendGuids_.count(data.guid)) {
            friendsCache[data.name] = data.guid;
        }

        // Fire UNIT_NAME_UPDATE so nameplate/unit frame addons know the name is available
        if (addonEventCallback_) {
            std::string unitId;
            if (data.guid == targetGuid) unitId = "target";
            else if (data.guid == focusGuid) unitId = "focus";
            else if (data.guid == playerGuid) unitId = "player";
            if (!unitId.empty())
                fireAddonEvent("UNIT_NAME_UPDATE", {unitId});
        }
    }
}

void GameHandler::handleCreatureQueryResponse(network::Packet& packet) {
    CreatureQueryResponseData data;
    if (!packetParsers_->parseCreatureQueryResponse(packet, data)) return;

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

void GameHandler::handleGameObjectQueryResponse(network::Packet& packet) {
    GameObjectQueryResponseData data;
    bool ok = packetParsers_ ? packetParsers_->parseGameObjectQueryResponse(packet, data)
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
        if (data.type == 15 && data.hasData && data.data[0] != 0 && transportManager_) {
            uint32_t taxiPathId = data.data[0];
            if (transportManager_->hasTaxiPath(taxiPathId)) {
                if (transportManager_->assignTaxiPathToTransport(data.entry, taxiPathId)) {
                    LOG_DEBUG("MO_TRANSPORT entry=", data.entry, " assigned TaxiPathNode path ", taxiPathId);
                }
            } else {
                LOG_DEBUG("MO_TRANSPORT entry=", data.entry, " taxiPathId=", taxiPathId,
                         " not found in TaxiPathNode.dbc");
            }
        }
    }
}

void GameHandler::handleGameObjectPageText(network::Packet& packet) {
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

    if (pageId != 0 && socket && state == WorldState::IN_WORLD) {
        bookPages_.clear();  // start a fresh book for this interaction
        auto req = PageTextQueryPacket::build(pageId, guid);
        socket->send(req);
        return;
    }

    if (!info.name.empty()) {
        addSystemChatMessage(info.name);
    }
}

void GameHandler::handlePageTextQueryResponse(network::Packet& packet) {
    PageTextQueryResponseData data;
    if (!PageTextQueryResponseParser::parse(packet, data)) return;

    if (!data.isValid()) return;

    // Append page if not already collected
    bool alreadyHave = false;
    for (const auto& bp : bookPages_) {
        if (bp.pageId == data.pageId) { alreadyHave = true; break; }
    }
    if (!alreadyHave) {
        bookPages_.push_back({data.pageId, data.text});
    }

    // Follow the chain: if there's a next page we haven't fetched yet, request it
    if (data.nextPageId != 0) {
        bool nextHave = false;
        for (const auto& bp : bookPages_) {
            if (bp.pageId == data.nextPageId) { nextHave = true; break; }
        }
        if (!nextHave && socket && state == WorldState::IN_WORLD) {
            auto req = PageTextQueryPacket::build(data.nextPageId, playerGuid);
            socket->send(req);
        }
    }
    LOG_DEBUG("handlePageTextQueryResponse: pageId=", data.pageId,
              " nextPage=", data.nextPageId,
              " totalPages=", bookPages_.size());
}

// ============================================================
// Item Query (forwarded to InventoryHandler)
// ============================================================

void GameHandler::queryItemInfo(uint32_t entry, uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->queryItemInfo(entry, guid);
}

void GameHandler::handleItemQueryResponse(network::Packet& packet) {
    if (inventoryHandler_) inventoryHandler_->handleItemQueryResponse(packet);
}

uint64_t GameHandler::resolveOnlineItemGuid(uint32_t itemId) const {
    return inventoryHandler_ ? inventoryHandler_->resolveOnlineItemGuid(itemId) : 0;
}

void GameHandler::detectInventorySlotBases(const std::map<uint16_t, uint32_t>& fields) {
    if (inventoryHandler_) inventoryHandler_->detectInventorySlotBases(fields);
}

bool GameHandler::applyInventoryFields(const std::map<uint16_t, uint32_t>& fields) {
    return inventoryHandler_ ? inventoryHandler_->applyInventoryFields(fields) : false;
}

void GameHandler::extractContainerFields(uint64_t containerGuid, const std::map<uint16_t, uint32_t>& fields) {
    if (inventoryHandler_) inventoryHandler_->extractContainerFields(containerGuid, fields);
}

void GameHandler::rebuildOnlineInventory() {
    if (inventoryHandler_) inventoryHandler_->rebuildOnlineInventory();
}

void GameHandler::maybeDetectVisibleItemLayout() {
    if (inventoryHandler_) inventoryHandler_->maybeDetectVisibleItemLayout();
}

void GameHandler::updateOtherPlayerVisibleItems(uint64_t guid, const std::map<uint16_t, uint32_t>& fields) {
    if (inventoryHandler_) inventoryHandler_->updateOtherPlayerVisibleItems(guid, fields);
}

void GameHandler::emitOtherPlayerEquipment(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->emitOtherPlayerEquipment(guid);
}

void GameHandler::emitAllOtherPlayerEquipment() {
    if (inventoryHandler_) inventoryHandler_->emitAllOtherPlayerEquipment();
}

// ============================================================
// Phase 2: Combat (delegated to CombatHandler)
// ============================================================

void GameHandler::startAutoAttack(uint64_t targetGuid) {
    if (combatHandler_) combatHandler_->startAutoAttack(targetGuid);
}

void GameHandler::stopAutoAttack() {
    if (combatHandler_) combatHandler_->stopAutoAttack();
}

void GameHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                uint64_t srcGuid, uint64_t dstGuid) {
    if (combatHandler_) combatHandler_->addCombatText(type, amount, spellId, isPlayerSource, powerType, srcGuid, dstGuid);
}

bool GameHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    return combatHandler_ ? combatHandler_->shouldLogSpellstealAura(casterGuid, victimGuid, spellId) : false;
}

void GameHandler::updateCombatText(float deltaTime) {
    if (combatHandler_) combatHandler_->updateCombatText(deltaTime);
}

bool GameHandler::isAutoAttacking() const {
    return combatHandler_ ? combatHandler_->isAutoAttacking() : false;
}

bool GameHandler::hasAutoAttackIntent() const {
    return combatHandler_ ? combatHandler_->hasAutoAttackIntent() : false;
}

bool GameHandler::isInCombat() const {
    return combatHandler_ ? combatHandler_->isInCombat() : false;
}

bool GameHandler::isInCombatWith(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isInCombatWith(guid) : false;
}

uint64_t GameHandler::getAutoAttackTargetGuid() const {
    return combatHandler_ ? combatHandler_->getAutoAttackTargetGuid() : 0;
}

bool GameHandler::isAggressiveTowardPlayer(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isAggressiveTowardPlayer(guid) : false;
}

uint64_t GameHandler::getLastMeleeSwingMs() const {
    return combatHandler_ ? combatHandler_->getLastMeleeSwingMs() : 0;
}

const std::vector<CombatTextEntry>& GameHandler::getCombatText() const {
    static const std::vector<CombatTextEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatText() : empty;
}

const std::deque<CombatLogEntry>& GameHandler::getCombatLog() const {
    static const std::deque<CombatLogEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatLog() : empty;
}

void GameHandler::clearCombatLog() {
    if (combatHandler_) combatHandler_->clearCombatLog();
}

void GameHandler::clearCombatText() {
    if (combatHandler_) combatHandler_->clearCombatText();
}

void GameHandler::clearHostileAttackers() {
    if (combatHandler_) combatHandler_->clearHostileAttackers();
}

const std::vector<GameHandler::ThreatEntry>* GameHandler::getThreatList(uint64_t unitGuid) const {
    return combatHandler_ ? combatHandler_->getThreatList(unitGuid) : nullptr;
}

const std::vector<GameHandler::ThreatEntry>* GameHandler::getTargetThreatList() const {
    return targetGuid ? getThreatList(targetGuid) : nullptr;
}

bool GameHandler::isHostileAttacker(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isHostileAttacker(guid) : false;
}

void GameHandler::dismount() {
    if (movementHandler_) movementHandler_->dismount();
}

// ============================================================
// Arena / Battleground Handlers
// ============================================================

void GameHandler::declineBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->declineBattlefield(queueSlot);
}

bool GameHandler::hasPendingBgInvite() const {
    return socialHandler_ && socialHandler_->hasPendingBgInvite();
}

void GameHandler::acceptBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->acceptBattlefield(queueSlot);
}

// ---------------------------------------------------------------------------
// LFG / Dungeon Finder handlers (WotLK 3.3.5a)
// ---------------------------------------------------------------------------

static const char* lfgJoinResultString(uint8_t result) {
    switch (result) {
        case 0:  return nullptr; // success
        case 1:  return "Role check failed.";
        case 2:  return "No LFG slots available for your group.";
        case 3:  return "No LFG object found.";
        case 4:  return "No slots available (player).";
        case 5:  return "No slots available (party).";
        case 6:  return "Dungeon requirements not met by all members.";
        case 7:  return "Party members are from different realms.";
        case 8:  return "Not all members are present.";
        case 9:  return "Get info timeout.";
        case 10: return "Invalid dungeon slot.";
        case 11: return "You are marked as a deserter.";
        case 12: return "A party member is marked as a deserter.";
        case 13: return "You are on a random dungeon cooldown.";
        case 14: return "A party member is on a random dungeon cooldown.";
        case 16: return "No spec/role available.";
        default: return "Cannot join dungeon finder.";
    }
}

static const char* lfgTeleportDeniedString(uint8_t reason) {
    switch (reason) {
        case 0:  return "You are not in a LFG group.";
        case 1:  return "You are not in the dungeon.";
        case 2:  return "You have a summon pending.";
        case 3:  return "You are dead.";
        case 4:  return "You have Deserter.";
        case 5:  return "You do not meet the requirements.";
        default: return "Teleport to dungeon denied.";
    }
}

// ---------------------------------------------------------------------------
// LFG outgoing packets
// ---------------------------------------------------------------------------

void GameHandler::lfgJoin(uint32_t dungeonId, uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgJoin(dungeonId, roles);
}

void GameHandler::lfgLeave() {
    if (socialHandler_) socialHandler_->lfgLeave();
}

void GameHandler::lfgSetRoles(uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgSetRoles(roles);
}

void GameHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (socialHandler_) socialHandler_->lfgAcceptProposal(proposalId, accept);
}

void GameHandler::lfgTeleport(bool toLfgDungeon) {
    if (socialHandler_) socialHandler_->lfgTeleport(toLfgDungeon);
}

void GameHandler::lfgSetBootVote(bool vote) {
    if (socialHandler_) socialHandler_->lfgSetBootVote(vote);
}

void GameHandler::loadAreaTriggerDbc() {
    if (movementHandler_) movementHandler_->loadAreaTriggerDbc();
}

void GameHandler::checkAreaTriggers() {
    if (movementHandler_) movementHandler_->checkAreaTriggers();
}

void GameHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (socialHandler_) socialHandler_->requestArenaTeamRoster(teamId);
}

void GameHandler::requestPvpLog() {
    if (socialHandler_) socialHandler_->requestPvpLog();
}

// ============================================================
// Phase 3: Spells
// ============================================================

void GameHandler::castSpell(uint32_t spellId, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->castSpell(spellId, targetGuid);
}

void GameHandler::cancelCast() {
    if (spellHandler_) spellHandler_->cancelCast();
}

void GameHandler::startCraftQueue(uint32_t spellId, int count) {
    if (spellHandler_) spellHandler_->startCraftQueue(spellId, count);
}

void GameHandler::cancelCraftQueue() {
    if (spellHandler_) spellHandler_->cancelCraftQueue();
}

void GameHandler::cancelAura(uint32_t spellId) {
    if (spellHandler_) spellHandler_->cancelAura(spellId);
}

uint32_t GameHandler::getTempEnchantRemainingMs(uint32_t slot) const {
    return inventoryHandler_ ? inventoryHandler_->getTempEnchantRemainingMs(slot) : 0u;
}

void GameHandler::handlePetSpells(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handlePetSpells(packet);
}

void GameHandler::sendPetAction(uint32_t action, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->sendPetAction(action, targetGuid);
}

void GameHandler::dismissPet() {
    if (spellHandler_) spellHandler_->dismissPet();
}

void GameHandler::togglePetSpellAutocast(uint32_t spellId) {
    if (spellHandler_) spellHandler_->togglePetSpellAutocast(spellId);
}

void GameHandler::renamePet(const std::string& newName) {
    if (spellHandler_) spellHandler_->renamePet(newName);
}

void GameHandler::requestStabledPetList() {
    if (spellHandler_) spellHandler_->requestStabledPetList();
}

void GameHandler::stablePet(uint8_t slot) {
    if (spellHandler_) spellHandler_->stablePet(slot);
}

void GameHandler::unstablePet(uint32_t petNumber) {
    if (spellHandler_) spellHandler_->unstablePet(petNumber);
}

void GameHandler::handleListStabledPets(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handleListStabledPets(packet);
}

void GameHandler::setActionBarSlot(int slot, ActionBarSlot::Type type, uint32_t id) {
    if (slot < 0 || slot >= ACTION_BAR_SLOTS) return;
    actionBar[slot].type = type;
    actionBar[slot].id = id;
    // Pre-query item information so action bar displays item name instead of "Item" placeholder
    if (type == ActionBarSlot::ITEM && id != 0) {
        queryItemInfo(id, 0);
    }
    saveCharacterConfig();
    // Notify Lua addons that the action bar changed
        fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(slot + 1)});
        fireAddonEvent("ACTIONBAR_UPDATE_STATE", {});
    // Notify the server so the action bar persists across relogs.
    if (isInWorld()) {
        const bool classic = isClassicLikeExpansion();
        auto pkt = SetActionButtonPacket::build(
            static_cast<uint8_t>(slot),
            static_cast<uint8_t>(type),
            id,
            classic);
        socket->send(pkt);
    }
}

float GameHandler::getSpellCooldown(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellCooldown(spellId);
    return 0;
}

static audio::SpellSoundManager::MagicSchool schoolMaskToMagicSchool(uint32_t mask) {
    if (mask & 0x04) return audio::SpellSoundManager::MagicSchool::FIRE;
    if (mask & 0x10) return audio::SpellSoundManager::MagicSchool::FROST;
    if (mask & 0x02) return audio::SpellSoundManager::MagicSchool::HOLY;
    if (mask & 0x08) return audio::SpellSoundManager::MagicSchool::NATURE;
    if (mask & 0x20) return audio::SpellSoundManager::MagicSchool::SHADOW;
    if (mask & 0x40) return audio::SpellSoundManager::MagicSchool::ARCANE;
    return audio::SpellSoundManager::MagicSchool::ARCANE;
}

// ============================================================
// Talents
// ============================================================

void GameHandler::learnTalent(uint32_t talentId, uint32_t requestedRank) {
    if (spellHandler_) spellHandler_->learnTalent(talentId, requestedRank);
}

void GameHandler::switchTalentSpec(uint8_t newSpec) {
    if (spellHandler_) spellHandler_->switchTalentSpec(newSpec);
}

void GameHandler::confirmPetUnlearn() {
    if (spellHandler_) spellHandler_->confirmPetUnlearn();
}

void GameHandler::confirmTalentWipe() {
    if (spellHandler_) spellHandler_->confirmTalentWipe();
}

void GameHandler::sendAlterAppearance(uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair) {
    if (socialHandler_) socialHandler_->sendAlterAppearance(hairStyle, hairColor, facialHair);
}

// ============================================================
// Phase 4: Group/Party
// ============================================================

void GameHandler::inviteToGroup(const std::string& playerName) {
    if (socialHandler_) socialHandler_->inviteToGroup(playerName);
}

void GameHandler::acceptGroupInvite() {
    if (socialHandler_) socialHandler_->acceptGroupInvite();
}

void GameHandler::declineGroupInvite() {
    if (socialHandler_) socialHandler_->declineGroupInvite();
}

void GameHandler::leaveGroup() {
    if (socialHandler_) socialHandler_->leaveGroup();
}

void GameHandler::convertToRaid() {
    if (socialHandler_) socialHandler_->convertToRaid();
}

void GameHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (socialHandler_) socialHandler_->sendSetLootMethod(method, threshold, masterLooterGuid);
}

// ============================================================
// Guild Handlers
// ============================================================

void GameHandler::kickGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->kickGuildMember(playerName);
}

void GameHandler::disbandGuild() {
    if (socialHandler_) socialHandler_->disbandGuild();
}

void GameHandler::setGuildLeader(const std::string& name) {
    if (socialHandler_) socialHandler_->setGuildLeader(name);
}

void GameHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildPublicNote(name, note);
}

void GameHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildOfficerNote(name, note);
}

void GameHandler::acceptGuildInvite() {
    if (socialHandler_) socialHandler_->acceptGuildInvite();
}

void GameHandler::declineGuildInvite() {
    if (socialHandler_) socialHandler_->declineGuildInvite();
}

void GameHandler::submitGmTicket(const std::string& text) {
    if (chatHandler_) chatHandler_->submitGmTicket(text);
}

void GameHandler::deleteGmTicket() {
    if (socialHandler_) socialHandler_->deleteGmTicket();
}

void GameHandler::requestGmTicket() {
    if (socialHandler_) socialHandler_->requestGmTicket();
}

void GameHandler::queryGuildInfo(uint32_t guildId) {
    if (socialHandler_) socialHandler_->queryGuildInfo(guildId);
}

static const std::string kEmptyString;

const std::string& GameHandler::lookupGuildName(uint32_t guildId) {
    static const std::string kEmpty;
    if (socialHandler_) return socialHandler_->lookupGuildName(guildId);
    return kEmpty;
}

uint32_t GameHandler::getEntityGuildId(uint64_t guid) const {
    if (socialHandler_) return socialHandler_->getEntityGuildId(guid);
    return 0;
}

void GameHandler::createGuild(const std::string& guildName) {
    if (socialHandler_) socialHandler_->createGuild(guildName);
}

void GameHandler::addGuildRank(const std::string& rankName) {
    if (socialHandler_) socialHandler_->addGuildRank(rankName);
}

void GameHandler::deleteGuildRank() {
    if (socialHandler_) socialHandler_->deleteGuildRank();
}

void GameHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (socialHandler_) socialHandler_->requestPetitionShowlist(npcGuid);
}

void GameHandler::buyPetition(uint64_t npcGuid, const std::string& guildName) {
    if (socialHandler_) socialHandler_->buyPetition(npcGuid, guildName);
}

void GameHandler::signPetition(uint64_t petitionGuid) {
    if (socialHandler_) socialHandler_->signPetition(petitionGuid);
}

void GameHandler::turnInPetition(uint64_t petitionGuid) {
    if (socialHandler_) socialHandler_->turnInPetition(petitionGuid);
}

// ============================================================
// Phase 5: Loot, Gossip, Vendor
// ============================================================

void GameHandler::lootTarget(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->lootTarget(guid);
}

void GameHandler::lootItem(uint8_t slotIndex) {
    if (inventoryHandler_) inventoryHandler_->lootItem(slotIndex);
}

void GameHandler::closeLoot() {
    if (inventoryHandler_) inventoryHandler_->closeLoot();
}

void GameHandler::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->lootMasterGive(lootSlot, targetGuid);
}

void GameHandler::interactWithNpc(uint64_t guid) {
    if (!isInWorld()) return;
    auto packet = GossipHelloPacket::build(guid);
    socket->send(packet);
}

void GameHandler::interactWithGameObject(uint64_t guid) {
    if (guid == 0) return;
    if (!isInWorld()) return;
    // Do not overlap an actual spell cast.
    if (spellHandler_ && spellHandler_->casting_ && spellHandler_->currentCastSpellId_ != 0) return;
    // Always clear melee intent before GO interactions.
    stopAutoAttack();
    // Interact immediately; server drives any real cast/channel feedback.
    pendingGameObjectInteractGuid_ = 0;
    performGameObjectInteractionNow(guid);
}

void GameHandler::performGameObjectInteractionNow(uint64_t guid) {
    if (guid == 0) return;
    if (!isInWorld()) return;
    // Rate-limit to prevent spamming the server
    static uint64_t lastInteractGuid = 0;
    static std::chrono::steady_clock::time_point lastInteractTime{};
    auto now = std::chrono::steady_clock::now();
    // Keep duplicate suppression, but allow quick retry clicks.
    constexpr int64_t minRepeatMs = 150;
    if (guid == lastInteractGuid &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastInteractTime).count() < minRepeatMs) {
        return;
    }
    lastInteractGuid = guid;
    lastInteractTime = now;

    // Ensure GO interaction isn't blocked by stale or active melee state.
    stopAutoAttack();
    auto entity = entityManager.getEntity(guid);
    uint32_t goEntry = 0;
    uint32_t goType = 0;
    std::string goName;

    if (entity) {
        if (entity->getType() == ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<GameObject>(entity);
            goEntry = go->getEntry();
            goName = go->getName();
            if (auto* info = getCachedGameObjectInfo(goEntry)) goType = info->type;
            if (goType == 5 && !goName.empty()) {
                std::string lower = goName;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.rfind("doodad_", 0) != 0) {
                    addSystemChatMessage(goName);
                }
            }
        }
        // Face object and send heartbeat before use so strict servers don't require
        // a nudge movement to accept interaction.
        float dx = entity->getX() - movementInfo.x;
        float dy = entity->getY() - movementInfo.y;
        float dz = entity->getZ() - movementInfo.z;
        float dist3d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist3d > 10.0f) {
            addSystemChatMessage("Too far away.");
            return;
        }
        // Stop movement before interacting — servers may reject GO use or
        // immediately cancel the resulting spell cast if the player is moving.
        const uint32_t moveFlags = movementInfo.flags;
        const bool isMoving = (moveFlags & 0x00000001u) || // FORWARD
                              (moveFlags & 0x00000002u) || // BACKWARD
                              (moveFlags & 0x00000004u) || // STRAFE_LEFT
                              (moveFlags & 0x00000008u);   // STRAFE_RIGHT
        if (isMoving) {
            movementInfo.flags &= ~0x0000000Fu; // clear directional movement flags
            sendMovement(Opcode::MSG_MOVE_STOP);
        }
        if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f) {
            movementInfo.orientation = std::atan2(-dy, dx);
            sendMovement(Opcode::MSG_MOVE_SET_FACING);
        }
        sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    }

    // Determine GO type for interaction strategy
    bool isMailbox = false;
    bool chestLike = false;
    if (entity && entity->getType() == ObjectType::GAMEOBJECT) {
        auto go = std::static_pointer_cast<GameObject>(entity);
        auto* info = getCachedGameObjectInfo(go->getEntry());
        if (info && info->type == 19) {
            isMailbox = true;
        } else if (info && info->type == 3) {
            chestLike = true;
        }
    }
    if (!chestLike && !goName.empty()) {
        std::string lower = goName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        chestLike = (lower.find("chest") != std::string::npos ||
                     lower.find("lockbox") != std::string::npos ||
                     lower.find("strongbox") != std::string::npos ||
                     lower.find("coffer") != std::string::npos ||
                     lower.find("cache") != std::string::npos ||
                     lower.find("bundle") != std::string::npos);
    }

    LOG_INFO("GO interaction: guid=0x", std::hex, guid, std::dec,
             " entry=", goEntry, " type=", goType,
             " name='", goName, "' chestLike=", chestLike, " isMailbox=", isMailbox);

    if (chestLike) {
        // For chest-like GOs: send CMSG_GAMEOBJ_USE (opens the chest) followed
        // immediately by CMSG_LOOT (requests loot contents). Both sent in the
        // same frame so the server processes them sequentially: USE transitions
        // the GO to lootable state, then LOOT reads the contents.
        auto usePacket = GameObjectUsePacket::build(guid);
        socket->send(usePacket);
        lootTarget(guid);
        lastInteractedGoGuid_ = guid;
    } else {
        // Non-chest GOs (doors, buttons, quest givers, etc.): use CMSG_GAMEOBJ_USE
        auto packet = GameObjectUsePacket::build(guid);
        socket->send(packet);
        lastInteractedGoGuid_ = guid;

        if (isMailbox) {
            LOG_INFO("Mailbox interaction: opening mail UI and requesting mail list");
            mailboxGuid_ = guid;
            mailboxOpen_ = true;
            hasNewMail_ = false;
            selectedMailIndex_ = -1;
            showMailCompose_ = false;
            refreshMailList();
        }

        // CMSG_GAMEOBJ_REPORT_USE for GO AI scripts (quest givers, etc.)
        if (!isMailbox) {
            const auto* table = getActiveOpcodeTable();
            if (table && table->hasOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE)) {
                network::Packet reportUse(wireOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE));
                reportUse.writeUInt64(guid);
                socket->send(reportUse);
            }
        }
    }
}

void GameHandler::selectGossipOption(uint32_t optionId) {
    if (questHandler_) questHandler_->selectGossipOption(optionId);
}

void GameHandler::selectGossipQuest(uint32_t questId) {
    if (questHandler_) questHandler_->selectGossipQuest(questId);
}

bool GameHandler::requestQuestQuery(uint32_t questId, bool force) {
    return questHandler_ && questHandler_->requestQuestQuery(questId, force);
}

bool GameHandler::hasQuestInLog(uint32_t questId) const {
    return questHandler_ && questHandler_->hasQuestInLog(questId);
}

Unit* GameHandler::getUnitByGuid(uint64_t guid) {
    auto entity = entityManager.getEntity(guid);
    return entity ? dynamic_cast<Unit*>(entity.get()) : nullptr;
}

std::string GameHandler::guidToUnitId(uint64_t guid) const {
    if (guid == playerGuid)      return "player";
    if (guid == targetGuid)      return "target";
    if (guid == focusGuid)       return "focus";
    if (guid == petGuid_)        return "pet";
    return {};
}

std::string GameHandler::getQuestTitle(uint32_t questId) const {
    for (const auto& q : questLog_)
        if (q.questId == questId && !q.title.empty()) return q.title;
    return {};
}

const GameHandler::QuestLogEntry* GameHandler::findQuestLogEntry(uint32_t questId) const {
    for (const auto& q : questLog_)
        if (q.questId == questId) return &q;
    return nullptr;
}

int GameHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questHandler_) return questHandler_->findQuestLogSlotIndexFromServer(questId);
    return 0;
}

void GameHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questHandler_) questHandler_->addQuestToLocalLogIfMissing(questId, title, objectives);
}

bool GameHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    return questHandler_ && questHandler_->resyncQuestLogFromServerSlots(forceQueryMetadata);
}

// Apply quest completion state from player update fields to already-tracked local quests.
// Called from VALUES update handler so quests that complete mid-session (or that were
// complete on login) get quest.complete=true without waiting for SMSG_QUESTUPDATE_COMPLETE.
void GameHandler::applyQuestStateFromFields(const std::map<uint16_t, uint32_t>& fields) {
    if (questHandler_) questHandler_->applyQuestStateFromFields(fields);
}

// Extract packed 6-bit kill/objective counts from WotLK/TBC/Classic quest-log update fields
// and populate quest.killCounts + quest.itemCounts using the structured objectives obtained
// from a prior SMSG_QUEST_QUERY_RESPONSE.  Silently does nothing if objectives are absent.
void GameHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (questHandler_) questHandler_->applyPackedKillCountsFromFields(quest);
}

void GameHandler::clearPendingQuestAccept(uint32_t questId) {
    if (questHandler_) questHandler_->clearPendingQuestAccept(questId);
}

void GameHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questHandler_) questHandler_->triggerQuestAcceptResync(questId, npcGuid, reason);
}

void GameHandler::acceptQuest() {
    if (questHandler_) questHandler_->acceptQuest();
}

void GameHandler::declineQuest() {
    if (questHandler_) questHandler_->declineQuest();
}

void GameHandler::abandonQuest(uint32_t questId) {
    if (questHandler_) questHandler_->abandonQuest(questId);
}

void GameHandler::shareQuestWithParty(uint32_t questId) {
    if (questHandler_) questHandler_->shareQuestWithParty(questId);
}

void GameHandler::completeQuest() {
    if (questHandler_) questHandler_->completeQuest();
}

void GameHandler::closeQuestRequestItems() {
    if (questHandler_) questHandler_->closeQuestRequestItems();
}

void GameHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (questHandler_) questHandler_->chooseQuestReward(rewardIndex);
}

void GameHandler::closeQuestOfferReward() {
    if (questHandler_) questHandler_->closeQuestOfferReward();
}

void GameHandler::closeGossip() {
    if (questHandler_) questHandler_->closeGossip();
}

void GameHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (questHandler_) questHandler_->offerQuestFromItem(itemGuid, questId);
}

uint64_t GameHandler::getBagItemGuid(int bagIndex, int slotIndex) const {
    if (bagIndex < 0 || bagIndex >= inventory.NUM_BAG_SLOTS) return 0;
    if (slotIndex < 0) return 0;
    uint64_t bagGuid = equipSlotGuids_[19 + bagIndex];
    if (bagGuid == 0) return 0;
    auto it = containerContents_.find(bagGuid);
    if (it == containerContents_.end()) return 0;
    if (slotIndex >= static_cast<int>(it->second.numSlots)) return 0;
    return it->second.slotGuids[slotIndex];
}

void GameHandler::openVendor(uint64_t npcGuid) {
    if (inventoryHandler_) inventoryHandler_->openVendor(npcGuid);
}

void GameHandler::closeVendor() {
    if (inventoryHandler_) inventoryHandler_->closeVendor();
}

void GameHandler::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->buyItem(vendorGuid, itemId, slot, count);
}

void GameHandler::buyBackItem(uint32_t buybackSlot) {
    if (inventoryHandler_) inventoryHandler_->buyBackItem(buybackSlot);
}

void GameHandler::repairItem(uint64_t vendorGuid, uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->repairItem(vendorGuid, itemGuid);
}

void GameHandler::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (inventoryHandler_) inventoryHandler_->repairAll(vendorGuid, useGuildBank);
}

void GameHandler::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->sellItem(vendorGuid, itemGuid, count);
}

void GameHandler::sellItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemBySlot(backpackIndex);
}

void GameHandler::autoEquipItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemBySlot(backpackIndex);
}

void GameHandler::autoEquipItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemInBag(bagIndex, slotIndex);
}

void GameHandler::sellItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemInBag(bagIndex, slotIndex);
}

void GameHandler::unequipToBackpack(EquipSlot equipSlot) {
    if (inventoryHandler_) inventoryHandler_->unequipToBackpack(equipSlot);
}

void GameHandler::swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot) {
    if (inventoryHandler_) inventoryHandler_->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot);
}

void GameHandler::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (inventoryHandler_) inventoryHandler_->swapBagSlots(srcBagIndex, dstBagIndex);
}

void GameHandler::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->destroyItem(bag, slot, count);
}

void GameHandler::splitItem(uint8_t srcBag, uint8_t srcSlot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->splitItem(srcBag, srcSlot, count);
}

void GameHandler::useItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->useItemBySlot(backpackIndex);
}

void GameHandler::useItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->useItemInBag(bagIndex, slotIndex);
}

void GameHandler::openItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemBySlot(backpackIndex);
}

void GameHandler::openItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemInBag(bagIndex, slotIndex);
}

void GameHandler::useItemById(uint32_t itemId) {
    if (inventoryHandler_) inventoryHandler_->useItemById(itemId);
}

void GameHandler::unstuck() {
    if (unstuckCallback_) {
        unstuckCallback_();
        addSystemChatMessage("Unstuck: snapped upward. Use /unstuckgy for full teleport.");
    }
}

void GameHandler::unstuckGy() {
    if (unstuckGyCallback_) {
        unstuckGyCallback_();
        addSystemChatMessage("Unstuck: teleported to safe location.");
    }
}

void GameHandler::unstuckHearth() {
    if (unstuckHearthCallback_) {
        unstuckHearthCallback_();
        addSystemChatMessage("Unstuck: teleported to hearthstone location.");
    } else {
        addSystemChatMessage("No hearthstone bind point set.");
    }
}

// ============================================================
// Trainer
// ============================================================

void GameHandler::trainSpell(uint32_t spellId) {
    if (inventoryHandler_) inventoryHandler_->trainSpell(spellId);
}

void GameHandler::closeTrainer() {
    if (inventoryHandler_) inventoryHandler_->closeTrainer();
}

void GameHandler::preloadDBCCaches() const {
    LOG_INFO("Pre-loading DBC caches during world entry...");
    auto t0 = std::chrono::steady_clock::now();

    loadSpellNameCache();   // Spell.dbc — largest, ~170ms cold
    loadTitleNameCache();   // CharTitles.dbc
    loadFactionNameCache(); // Faction.dbc
    loadAreaNameCache();    // WorldMapArea.dbc
    loadMapNameCache();     // Map.dbc
    loadLfgDungeonDbc();    // LFGDungeons.dbc

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("DBC cache pre-load complete in ", elapsed, " ms");
}

void GameHandler::loadSpellNameCache() const {
    if (spellHandler_) spellHandler_->loadSpellNameCache();
}

void GameHandler::loadSkillLineAbilityDbc() {
    if (spellHandler_) spellHandler_->loadSkillLineAbilityDbc();
}

const std::vector<GameHandler::SpellBookTab>& GameHandler::getSpellBookTabs() {
    static const std::vector<SpellBookTab> kEmpty;
    if (spellHandler_) return spellHandler_->getSpellBookTabs();
    return kEmpty;
}

void GameHandler::categorizeTrainerSpells() {
    if (spellHandler_) spellHandler_->categorizeTrainerSpells();
}

void GameHandler::loadTalentDbc() {
    if (spellHandler_) spellHandler_->loadTalentDbc();
}

static const std::string EMPTY_STRING;

const int32_t* GameHandler::getSpellEffectBasePoints(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellEffectBasePoints(spellId);
    return nullptr;
}

float GameHandler::getSpellDuration(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDuration(spellId);
    return 0.0f;
}

const std::string& GameHandler::getSpellName(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellName(spellId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getSpellRank(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellRank(spellId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getSpellDescription(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDescription(spellId);
    return EMPTY_STRING;
}

std::string GameHandler::getEnchantName(uint32_t enchantId) const {
    if (spellHandler_) return spellHandler_->getEnchantName(enchantId);
    return {};
}

uint8_t GameHandler::getSpellDispelType(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDispelType(spellId);
    return 0;
}

bool GameHandler::isSpellInterruptible(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->isSpellInterruptible(spellId);
    return true;
}

uint32_t GameHandler::getSpellSchoolMask(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellSchoolMask(spellId);
    return 0;
}

const std::string& GameHandler::getSkillLineName(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSkillLineName(spellId);
    return EMPTY_STRING;
}

// ============================================================
// Single-player local combat
// ============================================================

// ============================================================
// XP tracking
// ============================================================

// WotLK 3.3.5a XP-to-next-level table (from player_xp_for_level)
static const uint32_t XP_TABLE[] = {
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
        return transportManager_->getPlayerWorldPosition(playerTransportGuid_, playerTransportOffset_);
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

void GameHandler::auctionSellItem(uint64_t itemGuid, uint32_t stackCount,
                                    uint32_t bid, uint32_t buyout, uint32_t duration) {
    if (inventoryHandler_) inventoryHandler_->auctionSellItem(itemGuid, stackCount, bid, buyout, duration);
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

    auto* am = core::Application::getInstance().getAssetManager();
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

    auto* am = core::Application::getInstance().getAssetManager();
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

    auto* am = core::Application::getInstance().getAssetManager();
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

    auto* am = core::Application::getInstance().getAssetManager();
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("WorldMapArea.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    const auto* layout = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("WorldMapArea") : nullptr;
    const uint32_t areaIdField   = layout ? (*layout)["AreaID"]   : 2;
    const uint32_t areaNameField = layout ? (*layout)["AreaName"] : 3;

    if (dbc->getFieldCount() <= areaNameField) return;

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t areaId = dbc->getUInt32(i, areaIdField);
        if (areaId == 0) continue;
        std::string name = dbc->getString(i, areaNameField);
        if (!name.empty() && !areaNameCache_.count(areaId)) {
            areaNameCache_[areaId] = std::move(name);
        }
    }
    LOG_INFO("WorldMapArea.dbc: loaded ", areaNameCache_.size(), " area names");
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

    auto* am = core::Application::getInstance().getAssetManager();
    if (!am || !am->isInitialized()) return;

    auto dbc = am->loadDBC("Map.dbc");
    if (!dbc || !dbc->isLoaded()) return;

    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        uint32_t id = dbc->getUInt32(i, 0);
        // Field 2 = MapName_enUS (first localized); field 1 = InternalName fallback
        std::string name = dbc->getString(i, 2);
        if (name.empty()) name = dbc->getString(i, 1);
        if (!name.empty() && !mapNameCache_.count(id)) {
            mapNameCache_[id] = std::move(name);
        }
    }
    LOG_INFO("Map.dbc: loaded ", mapNameCache_.size(), " map names");
}

std::string GameHandler::getMapName(uint32_t mapId) const {
    if (mapId == 0) return {};
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

    auto* am = core::Application::getInstance().getAssetManager();
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

} // namespace game
} // namespace wowee
