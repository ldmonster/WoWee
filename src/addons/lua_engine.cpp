#include "addons/lua_engine.hpp"
#include "addons/toc_parser.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "game/update_field_table.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include <imgui.h>
#include <cstring>
#include <fstream>
#include <filesystem>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace wowee::addons {

// Retrieve GameHandler pointer stored in Lua registry
static game::GameHandler* getGameHandler(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    auto* gh = static_cast<game::GameHandler*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return gh;
}

// WoW-compatible print() — outputs to chat window instead of stdout
static int lua_wow_print(lua_State* L) {
    int nargs = lua_gettop(L);
    std::string result;
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) result += '\t';
        // Lua 5.1: use lua_tostring (luaL_tolstring is 5.3+)
        if (lua_isstring(L, i) || lua_isnumber(L, i)) {
            const char* s = lua_tostring(L, i);
            if (s) result += s;
        } else if (lua_isboolean(L, i)) {
            result += lua_toboolean(L, i) ? "true" : "false";
        } else if (lua_isnil(L, i)) {
            result += "nil";
        } else {
            result += lua_typename(L, lua_type(L, i));
        }
    }

    auto* gh = getGameHandler(L);
    if (gh) {
        game::MessageChatData msg;
        msg.type = game::ChatType::SYSTEM;
        msg.language = game::ChatLanguage::UNIVERSAL;
        msg.message = result;
        gh->addLocalChatMessage(msg);
    }
    LOG_INFO("[Lua] ", result);
    return 0;
}

// WoW-compatible message() — same as print for now
static int lua_wow_message(lua_State* L) {
    return lua_wow_print(L);
}

// Helper: resolve WoW unit IDs to GUID
// Read UNIT_FIELD_TARGET_LO/HI from an entity's update fields to get what it's targeting
static uint64_t getEntityTargetGuid(game::GameHandler* gh, uint64_t guid) {
    if (guid == 0) return 0;
    // If asking for the player's target, use direct accessor
    if (guid == gh->getPlayerGuid()) return gh->getTargetGuid();
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) return 0;
    const auto& fields = entity->getFields();
    auto loIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
    if (loIt == fields.end()) return 0;
    uint64_t targetGuid = loIt->second;
    auto hiIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
    if (hiIt != fields.end())
        targetGuid |= (static_cast<uint64_t>(hiIt->second) << 32);
    return targetGuid;
}

static uint64_t resolveUnitGuid(game::GameHandler* gh, const std::string& uid) {
    if (uid == "player")      return gh->getPlayerGuid();
    if (uid == "target")      return gh->getTargetGuid();
    if (uid == "focus")       return gh->getFocusGuid();
    if (uid == "mouseover")   return gh->getMouseoverGuid();
    if (uid == "pet")         return gh->getPetGuid();
    // Compound unit IDs: targettarget, focustarget, pettarget, mouseovertarget
    if (uid == "targettarget")    return getEntityTargetGuid(gh, gh->getTargetGuid());
    if (uid == "focustarget")     return getEntityTargetGuid(gh, gh->getFocusGuid());
    if (uid == "pettarget")       return getEntityTargetGuid(gh, gh->getPetGuid());
    if (uid == "mouseovertarget") return getEntityTargetGuid(gh, gh->getMouseoverGuid());
    // party1-party4, raid1-raid40
    if (uid.rfind("party", 0) == 0 && uid.size() > 5) {
        int idx = 0;
        try { idx = std::stoi(uid.substr(5)); } catch (...) { return 0; }
        if (idx < 1 || idx > 4) return 0;
        const auto& pd = gh->getPartyData();
        // party members exclude self; index 1-based
        int found = 0;
        for (const auto& m : pd.members) {
            if (m.guid == gh->getPlayerGuid()) continue;
            if (++found == idx) return m.guid;
        }
        return 0;
    }
    if (uid.rfind("raid", 0) == 0 && uid.size() > 4 && uid[4] != 'p') {
        int idx = 0;
        try { idx = std::stoi(uid.substr(4)); } catch (...) { return 0; }
        if (idx < 1 || idx > 40) return 0;
        const auto& pd = gh->getPartyData();
        if (idx <= static_cast<int>(pd.members.size()))
            return pd.members[idx - 1].guid;
        return 0;
    }
    return 0;
}

// Helper: resolve unit IDs (player, target, focus, mouseover, pet, targettarget, focustarget, etc.) to entity
static game::Unit* resolveUnit(lua_State* L, const char* unitId) {
    auto* gh = getGameHandler(L);
    if (!gh || !unitId) return nullptr;
    std::string uid(unitId);
    for (char& c : uid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    uint64_t guid = resolveUnitGuid(gh, uid);
    if (guid == 0) return nullptr;
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) return nullptr;
    return dynamic_cast<game::Unit*>(entity.get());
}

// --- WoW Unit API ---

// Helper: find GroupMember data for a GUID (for party members out of entity range)
static const game::GroupMember* findPartyMember(game::GameHandler* gh, uint64_t guid) {
    if (!gh || guid == 0) return nullptr;
    for (const auto& m : gh->getPartyData().members) {
        if (m.guid == guid && m.hasPartyStats) return &m;
    }
    return nullptr;
}

static int lua_UnitName(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit && !unit->getName().empty()) {
        lua_pushstring(L, unit->getName().c_str());
    } else {
        // Fallback: party member name for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        if (pm && !pm->name.empty()) {
            lua_pushstring(L, pm->name.c_str());
        } else if (gh && guid != 0) {
            // Try player name cache
            const std::string& cached = gh->lookupName(guid);
            lua_pushstring(L, cached.empty() ? "Unknown" : cached.c_str());
        } else {
            lua_pushstring(L, "Unknown");
        }
    }
    return 1;
}


static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getHealth());
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curHealth : 0);
    }
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxHealth());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->maxHealth : 0);
    }
    return 1;
}

static int lua_UnitPower(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getPower());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->curPower : 0);
    }
    return 1;
}

static int lua_UnitPowerMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getMaxPower());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->maxPower : 0);
    }
    return 1;
}

static int lua_UnitLevel(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getLevel());
    } else {
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushnumber(L, pm ? pm->level : 0);
    }
    return 1;
}

static int lua_UnitExists(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, 1);
    } else {
        // Party members in other zones don't have entities but still "exist"
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        lua_pushboolean(L, guid != 0 && findPartyMember(gh, guid) != nullptr);
    }
    return 1;
}

static int lua_UnitIsDead(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushboolean(L, unit->getHealth() == 0);
    } else {
        // Fallback: party member stats for out-of-range members
        auto* gh = getGameHandler(L);
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
        const auto* pm = findPartyMember(gh, guid);
        lua_pushboolean(L, pm ? (pm->curHealth == 0 && pm->maxHealth > 0) : 0);
    }
    return 1;
}

static int lua_UnitClass(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    auto* unit = resolveUnit(L, uid);
    if (unit && gh) {
        static const char* kClasses[] = {"", "Warrior","Paladin","Hunter","Rogue","Priest",
            "Death Knight","Shaman","Mage","Warlock","","Druid"};
        uint8_t classId = 0;
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (uidStr == "player") {
            classId = gh->getPlayerClass();
        } else {
            // Read class from UNIT_FIELD_BYTES_0 (class is byte 1)
            uint64_t guid = resolveUnitGuid(gh, uidStr);
            if (guid != 0) {
                auto entity = gh->getEntityManager().getEntity(guid);
                if (entity) {
                    uint32_t bytes0 = entity->getField(
                        game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                    classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
                }
            }
            // Fallback: check name query class/race cache
            if (classId == 0 && guid != 0) {
                classId = gh->lookupPlayerClass(guid);
            }
        }
        const char* name = (classId > 0 && classId < 12) ? kClasses[classId] : "Unknown";
        lua_pushstring(L, name);
        lua_pushstring(L, name);  // WoW returns localized + English
        lua_pushnumber(L, classId);
        return 3;
    }
    lua_pushstring(L, "Unknown");
    lua_pushstring(L, "Unknown");
    lua_pushnumber(L, 0);
    return 3;
}

// UnitIsGhost(unit) — true if unit is in ghost form
static int lua_UnitIsGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isPlayerGhost());
    } else {
        // Check UNIT_FIELD_FLAGS for UNIT_FLAG_GHOST (0x00000100) — best approximation
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool ghost = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
                ghost = (flags & 0x00000100) != 0; // PLAYER_FLAGS_GHOST
            }
        }
        lua_pushboolean(L, ghost);
    }
    return 1;
}

// UnitIsDeadOrGhost(unit)
static int lua_UnitIsDeadOrGhost(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    auto* gh = getGameHandler(L);
    bool dead = (unit && unit->getHealth() == 0);
    if (!dead && gh) {
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (uidStr == "player") dead = gh->isPlayerGhost() || gh->isPlayerDead();
    }
    lua_pushboolean(L, dead);
    return 1;
}

// UnitIsAFK(unit), UnitIsDND(unit)
static int lua_UnitIsAFK(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // PLAYER_FLAGS at UNIT_FIELD_FLAGS: PLAYER_FLAGS_AFK = 0x01
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
            lua_pushboolean(L, (playerFlags & 0x01) != 0);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int lua_UnitIsDND(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            uint32_t playerFlags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
            lua_pushboolean(L, (playerFlags & 0x02) != 0); // PLAYER_FLAGS_DND
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// UnitPlayerControlled(unit) — true for players and player-controlled pets
static int lua_UnitPlayerControlled(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushboolean(L, 0); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { lua_pushboolean(L, 0); return 1; }
    // Players are always player-controlled; pets check UNIT_FLAG_PLAYER_CONTROLLED (0x01000000)
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushboolean(L, 1);
    } else {
        uint32_t flags = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
        lua_pushboolean(L, (flags & 0x01000000) != 0);
    }
    return 1;
}

// UnitIsTapped(unit) — true if mob is tapped (tagged by any player)
static int lua_UnitIsTapped(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0004) != 0); // UNIT_DYNFLAG_TAPPED_BY_PLAYER
    return 1;
}

// UnitIsTappedByPlayer(unit) — true if tapped by the local player (can loot)
static int lua_UnitIsTappedByPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { lua_pushboolean(L, 0); return 1; }
    uint32_t df = unit->getDynamicFlags();
    // Tapped by player: has TAPPED flag but also LOOTABLE or TAPPED_BY_ALL
    bool tapped = (df & 0x0004) != 0;
    bool lootable = (df & 0x0001) != 0;
    bool sharedTag = (df & 0x0008) != 0;
    lua_pushboolean(L, tapped && (lootable || sharedTag));
    return 1;
}

// UnitIsTappedByAllThreatList(unit) — true if shared-tag mob
static int lua_UnitIsTappedByAllThreatList(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    if (!unit) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, (unit->getDynamicFlags() & 0x0008) != 0);
    return 1;
}

// UnitThreatSituation(unit, mobUnit) → 0=not tanking, 1=not tanking but threat, 2=insecurely tanking, 3=securely tanking
static int lua_UnitThreatSituation(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t playerUnitGuid = resolveUnitGuid(gh, uidStr);
    if (playerUnitGuid == 0) { lua_pushnumber(L, 0); return 1; }
    // If no mob specified, check general combat threat against current target
    uint64_t mobGuid = 0;
    if (mobUid && *mobUid) {
        std::string mStr(mobUid);
        for (char& c : mStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        mobGuid = resolveUnitGuid(gh, mStr);
    }
    // Approximate threat: check if the mob is targeting this unit
    if (mobGuid != 0) {
        auto mobEntity = gh->getEntityManager().getEntity(mobGuid);
        if (mobEntity) {
            const auto& fields = mobEntity->getFields();
            auto loIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
            if (loIt != fields.end()) {
                uint64_t mobTarget = loIt->second;
                auto hiIt = fields.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                if (hiIt != fields.end())
                    mobTarget |= (static_cast<uint64_t>(hiIt->second) << 32);
                if (mobTarget == playerUnitGuid) {
                    lua_pushnumber(L, 3); // securely tanking
                    return 1;
                }
            }
        }
    }
    // Check if player is in combat (basic threat indicator)
    if (playerUnitGuid == gh->getPlayerGuid() && gh->isInCombat()) {
        lua_pushnumber(L, 1); // in combat but not tanking
        return 1;
    }
    lua_pushnumber(L, 0);
    return 1;
}

// UnitDetailedThreatSituation(unit, mobUnit) → isTanking, status, threatPct, rawThreatPct, threatValue
static int lua_UnitDetailedThreatSituation(lua_State* L) {
    // Use UnitThreatSituation logic for the basics
    auto* gh = getGameHandler(L);
    if (!gh) {
        lua_pushboolean(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 5;
    }
    const char* uid = luaL_optstring(L, 1, "player");
    const char* mobUid = luaL_optstring(L, 2, nullptr);
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t unitGuid = resolveUnitGuid(gh, uidStr);
    bool isTanking = false;
    int status = 0;
    if (unitGuid != 0 && mobUid && *mobUid) {
        std::string mStr(mobUid);
        for (char& c : mStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint64_t mobGuid = resolveUnitGuid(gh, mStr);
        if (mobGuid != 0) {
            auto mobEnt = gh->getEntityManager().getEntity(mobGuid);
            if (mobEnt) {
                const auto& f = mobEnt->getFields();
                auto lo = f.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_LO));
                if (lo != f.end()) {
                    uint64_t mt = lo->second;
                    auto hi = f.find(game::fieldIndex(game::UF::UNIT_FIELD_TARGET_HI));
                    if (hi != f.end()) mt |= (static_cast<uint64_t>(hi->second) << 32);
                    if (mt == unitGuid) { isTanking = true; status = 3; }
                }
            }
        }
    }
    lua_pushboolean(L, isTanking);
    lua_pushnumber(L, status);
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // threatPct
    lua_pushnumber(L, isTanking ? 100.0 : 0.0); // rawThreatPct
    lua_pushnumber(L, 0); // threatValue (not available without server threat data)
    return 5;
}

// UnitDistanceSquared(unit) → distSq, canCalculate
static int lua_UnitDistanceSquared(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0 || guid == gh->getPlayerGuid()) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushnumber(L, 0); lua_pushboolean(L, 0); return 2; }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    lua_pushnumber(L, dx*dx + dy*dy + dz*dz);
    lua_pushboolean(L, 1);
    return 2;
}

// CheckInteractDistance(unit, distIndex) → boolean
// distIndex: 1=inspect(28yd), 2=trade(11yd), 3=duel(10yd), 4=follow(28yd)
static int lua_CheckInteractDistance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const char* uid = luaL_checkstring(L, 1);
    int distIdx = static_cast<int>(luaL_optnumber(L, 2, 4));
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushboolean(L, 0); return 1; }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushboolean(L, 0); return 1; }
    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    float maxDist = 28.0f; // default: follow/inspect range
    switch (distIdx) {
        case 1: maxDist = 28.0f; break; // inspect
        case 2: maxDist = 11.11f; break; // trade
        case 3: maxDist = 9.9f; break; // duel
        case 4: maxDist = 28.0f; break; // follow
    }
    lua_pushboolean(L, dist <= maxDist);
    return 1;
}

// IsSpellInRange(spellName, unit) → 0 or 1 (nil if can't determine)
static int lua_IsSpellInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    const char* spellNameOrId = luaL_checkstring(L, 1);
    const char* uid = luaL_optstring(L, 2, "target");

    // Resolve spell ID
    uint32_t spellId = 0;
    if (spellNameOrId[0] >= '0' && spellNameOrId[0] <= '9') {
        spellId = static_cast<uint32_t>(strtoul(spellNameOrId, nullptr, 10));
    } else {
        std::string nameLow(spellNameOrId);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { lua_pushnil(L); return 1; }

    // Get spell max range from DBC
    auto data = gh->getSpellData(spellId);
    if (data.maxRange <= 0.0f) { lua_pushnil(L); return 1; }

    // Resolve target position
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushnil(L); return 1; }
    auto targetEnt = gh->getEntityManager().getEntity(guid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushnil(L); return 1; }

    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    lua_pushnumber(L, dist <= data.maxRange ? 1 : 0);
    return 1;
}

// UnitIsVisible(unit) → boolean (entity exists in the client's entity manager)
static int lua_UnitIsVisible(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "target");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit != nullptr);
    return 1;
}

// UnitGroupRolesAssigned(unit) → "TANK", "HEALER", "DAMAGER", or "NONE"
static int lua_UnitGroupRolesAssigned(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "NONE"); return 1; }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "NONE"); return 1; }
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            // WotLK roles bitmask: 0x02=Tank, 0x04=Healer, 0x08=DPS
            if (m.roles & 0x02) { lua_pushstring(L, "TANK"); return 1; }
            if (m.roles & 0x04) { lua_pushstring(L, "HEALER"); return 1; }
            if (m.roles & 0x08) { lua_pushstring(L, "DAMAGER"); return 1; }
            break;
        }
    }
    lua_pushstring(L, "NONE");
    return 1;
}

// UnitCanAttack(unit, otherUnit) → boolean
static int lua_UnitCanAttack(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    for (char& c : u1) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : u2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == 0 || g2 == 0 || g1 == g2) { lua_pushboolean(L, 0); return 1; }
    // Check if unit2 is hostile to unit1
    auto* unit2 = resolveUnit(L, uid2);
    if (unit2 && unit2->isHostile()) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// UnitCanCooperate(unit, otherUnit) → boolean
static int lua_UnitCanCooperate(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    (void)luaL_checkstring(L, 1); // unit1 (unused — cooperation is based on unit2's hostility)
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, !unit2->isHostile());
    return 1;
}

// UnitCreatureFamily(unit) → familyName or nil
static int lua_UnitCreatureFamily(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushnil(L); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) { lua_pushnil(L); return 1; }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushnil(L); return 1; }
    uint32_t family = gh->getCreatureFamily(unit->getEntry());
    if (family == 0) { lua_pushnil(L); return 1; }
    static const char* kFamilies[] = {
        "", "Wolf", "Cat", "Spider", "Bear", "Boar", "Crocolisk", "Carrion Bird",
        "Crab", "Gorilla", "Raptor", "", "Tallstrider", "", "", "Felhunter",
        "Voidwalker", "Succubus", "", "Doomguard", "Scorpid", "Turtle", "",
        "Imp", "Bat", "Hyena", "Bird of Prey", "Wind Serpent", "", "Dragonhawk",
        "Ravager", "Warp Stalker", "Sporebat", "Nether Ray", "Serpent", "Moth",
        "Chimaera", "Devilsaur", "Ghoul", "Silithid", "Worm", "Rhino", "Wasp",
        "Core Hound", "Spirit Beast"
    };
    lua_pushstring(L, (family < sizeof(kFamilies)/sizeof(kFamilies[0]) && kFamilies[family][0])
        ? kFamilies[family] : "Beast");
    return 1;
}

// UnitOnTaxi(unit) → boolean (true if on a flight path)
static int lua_UnitOnTaxi(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isOnTaxiFlight());
    } else {
        lua_pushboolean(L, 0); // Can't determine for other units
    }
    return 1;
}

// UnitSex(unit) → 1=unknown, 2=male, 3=female
static int lua_UnitSex(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) {
        auto entity = gh->getEntityManager().getEntity(guid);
        if (entity) {
            // Gender is byte 2 of UNIT_FIELD_BYTES_0 (0=male, 1=female)
            uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
            uint8_t gender = static_cast<uint8_t>((bytes0 >> 16) & 0xFF);
            lua_pushnumber(L, gender == 0 ? 2 : (gender == 1 ? 3 : 1)); // WoW: 2=male, 3=female
            return 1;
        }
    }
    lua_pushnumber(L, 1); // unknown
    return 1;
}

// --- Player/Game API ---

static int lua_GetMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getMoneyCopper()) : 0.0);
    return 1;
}

// UnitStat(unit, statIndex) → base, effective, posBuff, negBuff
// statIndex: 1=STR, 2=AGI, 3=STA, 4=INT, 5=SPI (1-indexed per WoW API)
static int lua_UnitStat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 4; }
    int statIdx = static_cast<int>(luaL_checknumber(L, 2)) - 1; // WoW API is 1-indexed
    int32_t val = gh->getPlayerStat(statIdx);
    if (val < 0) val = 0;
    // We only have the effective value from the server; report base=effective, no buffs
    lua_pushnumber(L, val); // base (approximate — server only sends effective)
    lua_pushnumber(L, val); // effective
    lua_pushnumber(L, 0);   // positive buff
    lua_pushnumber(L, 0);   // negative buff
    return 4;
}

// GetDodgeChance() → percent
static int lua_GetDodgeChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getDodgePct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetParryChance() → percent
static int lua_GetParryChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getParryPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetBlockChance() → percent
static int lua_GetBlockChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getBlockPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCritChance() → percent (melee crit)
static int lua_GetCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetRangedCritChance() → percent
static int lua_GetRangedCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    float v = gh ? gh->getRangedCritPct() : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetSpellCritChance(school) → percent  (1=Holy,2=Fire,3=Nature,4=Frost,5=Shadow,6=Arcane)
static int lua_GetSpellCritChance(lua_State* L) {
    auto* gh = getGameHandler(L);
    int school = static_cast<int>(luaL_checknumber(L, 1));
    float v = gh ? gh->getSpellCritPct(school) : 0.0f;
    lua_pushnumber(L, v >= 0 ? v : 0.0);
    return 1;
}

// GetCombatRating(ratingIndex) → value
static int lua_GetCombatRating(lua_State* L) {
    auto* gh = getGameHandler(L);
    int cr = static_cast<int>(luaL_checknumber(L, 1));
    int32_t v = gh ? gh->getCombatRating(cr) : 0;
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetSpellBonusDamage(school) → value  (1-6 magic schools)
static int lua_GetSpellBonusDamage(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    int32_t sp = gh->getSpellPower();
    lua_pushnumber(L, sp >= 0 ? sp : 0);
    return 1;
}

// GetSpellBonusHealing() → value
static int lua_GetSpellBonusHealing(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    int32_t v = gh->getHealingPower();
    lua_pushnumber(L, v >= 0 ? v : 0);
    return 1;
}

// GetMeleeHaste / GetAttackPowerForStat stubs for addon compat
static int lua_GetAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getMeleeAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);  // base
    lua_pushnumber(L, 0);   // posBuff
    lua_pushnumber(L, 0);   // negBuff
    return 3;
}

static int lua_GetRangedAttackPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    int32_t ap = gh ? gh->getRangedAttackPower() : 0;
    if (ap < 0) ap = 0;
    lua_pushnumber(L, ap);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 3;
}

static int lua_IsInGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup());
    return 1;
}

static int lua_IsInRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGroup() && gh->getPartyData().groupType == 1);
    return 1;
}

static int lua_GetPlayerMapPosition(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) {
        const auto& mi = gh->getMovementInfo();
        lua_pushnumber(L, mi.x);
        lua_pushnumber(L, mi.y);
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    return 2;
}

// GetPlayerFacing() → radians (0 = north, increasing counter-clockwise)
static int lua_GetPlayerFacing(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) {
        float facing = gh->getMovementInfo().orientation;
        // Normalize to [0, 2π)
        while (facing < 0) facing += 6.2831853f;
        while (facing >= 6.2831853f) facing -= 6.2831853f;
        lua_pushnumber(L, facing);
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetCVar(name) → value string (stub for most, real for a few)
static int lua_GetCVar(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string n(name);
    // Return sensible defaults for commonly queried CVars
    if (n == "uiScale") lua_pushstring(L, "1");
    else if (n == "useUIScale") lua_pushstring(L, "1");
    else if (n == "screenWidth" || n == "gxResolution") {
        auto* win = core::Application::getInstance().getWindow();
        lua_pushstring(L, std::to_string(win ? win->getWidth() : 1920).c_str());
    } else if (n == "screenHeight" || n == "gxFullscreenResolution") {
        auto* win = core::Application::getInstance().getWindow();
        lua_pushstring(L, std::to_string(win ? win->getHeight() : 1080).c_str());
    } else if (n == "nameplateShowFriends") lua_pushstring(L, "1");
    else if (n == "nameplateShowEnemies") lua_pushstring(L, "1");
    else if (n == "Sound_EnableSFX") lua_pushstring(L, "1");
    else if (n == "Sound_EnableMusic") lua_pushstring(L, "1");
    else if (n == "chatBubbles") lua_pushstring(L, "1");
    else if (n == "autoLootDefault") lua_pushstring(L, "1");
    else lua_pushstring(L, "0");
    return 1;
}

// SetCVar(name, value) — no-op stub (log for debugging)
static int lua_SetCVar(lua_State* L) {
    (void)L;
    return 0;
}

static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); lua_pushstring(L, "Unknown"); lua_pushnumber(L, 0); return 3; }
    std::string uid(luaL_optstring(L, 1, "player"));
    for (char& c : uid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* kRaces[] = {"","Human","Orc","Dwarf","Night Elf","Undead",
        "Tauren","Gnome","Troll","","Blood Elf","Draenei"};
    uint8_t raceId = 0;
    if (uid == "player") {
        raceId = gh->getPlayerRace();
    } else {
        // Read race from UNIT_FIELD_BYTES_0 (race is byte 0)
        uint64_t guid = resolveUnitGuid(gh, uid);
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t bytes0 = entity->getField(
                    game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
                raceId = static_cast<uint8_t>(bytes0 & 0xFF);
            }
            // Fallback: name query class/race cache
            if (raceId == 0) raceId = gh->lookupPlayerRace(guid);
        }
    }
    const char* name = (raceId > 0 && raceId < 12) ? kRaces[raceId] : "Unknown";
    lua_pushstring(L, name);      // 1: localized race
    lua_pushstring(L, name);      // 2: English race
    lua_pushnumber(L, raceId);    // 3: raceId (WoW returns 3 values)
    return 3;
}

static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    static const char* kPowerNames[] = {"MANA","RAGE","FOCUS","ENERGY","HAPPINESS","","RUNIC_POWER"};
    if (unit) {
        uint8_t pt = unit->getPowerType();
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kPowerNames[pt] : "MANA");
        return 2;
    }
    // Fallback: party member stats for out-of-range members
    auto* gh = getGameHandler(L);
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = gh ? resolveUnitGuid(gh, uidStr) : 0;
    const auto* pm = findPartyMember(gh, guid);
    if (pm) {
        uint8_t pt = pm->powerType;
        lua_pushnumber(L, pt);
        lua_pushstring(L, (pt < 7) ? kPowerNames[pt] : "MANA");
        return 2;
    }
    lua_pushnumber(L, 0);
    lua_pushstring(L, "MANA");
    return 2;
}

static int lua_GetNumGroupMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getPartyData().memberCount : 0);
    return 1;
}

static int lua_UnitGUID(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushnil(L); return 1; }
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%016llX", (unsigned long long)guid);
    lua_pushstring(L, buf);
    return 1;
}

static int lua_UnitIsPlayer(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    auto entity = guid ? gh->getEntityManager().getEntity(guid) : nullptr;
    lua_pushboolean(L, entity && entity->getType() == game::ObjectType::PLAYER);
    return 1;
}

static int lua_InCombatLockdown(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInCombat());
    return 1;
}

// --- Addon Info API ---
// These need the AddonManager pointer stored in registry

static int lua_GetNumAddOns(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_count");
    return 1;
}

static int lua_GetAddOnInfo(lua_State* L) {
    // Accept index (1-based) or addon name
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_info");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L); return 1;
    }

    int idx = 0;
    if (lua_isnumber(L, 1)) {
        idx = static_cast<int>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        // Search by name
        const char* name = lua_tostring(L, 1);
        int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "name");
            const char* aName = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (aName && strcmp(aName, name) == 0) { idx = i; lua_pop(L, 1); break; }
            lua_pop(L, 1);
        }
    }

    if (idx < 1) { lua_pop(L, 1); lua_pushnil(L); return 1; }

    lua_rawgeti(L, -1, idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return 1; }

    lua_getfield(L, -1, "name");
    lua_getfield(L, -2, "title");
    lua_getfield(L, -3, "notes");
    lua_pushboolean(L, 1); // loadable (always true for now)
    lua_pushstring(L, "INSECURE"); // security
    lua_pop(L, 1); // pop addon info entry (keep others)
    // Return: name, title, notes, loadable, reason, security
    return 5;
}

// GetAddOnMetadata(addonNameOrIndex, key) → value
static int lua_GetAddOnMetadata(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_info");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushnil(L); return 1; }

    int idx = 0;
    if (lua_isnumber(L, 1)) {
        idx = static_cast<int>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "name");
            const char* aName = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (aName && strcmp(aName, name) == 0) { idx = i; lua_pop(L, 1); break; }
            lua_pop(L, 1);
        }
    }
    if (idx < 1) { lua_pop(L, 1); lua_pushnil(L); return 1; }

    const char* key = luaL_checkstring(L, 2);
    lua_rawgeti(L, -1, idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return 1; }
    lua_getfield(L, -1, "metadata");
    if (!lua_istable(L, -1)) { lua_pop(L, 3); lua_pushnil(L); return 1; }
    lua_getfield(L, -1, key);
    return 1;
}

// UnitBuff(unitId, index) / UnitDebuff(unitId, index)
// Returns: name, rank, icon, count, debuffType, duration, expirationTime, caster, isStealable, shouldConsolidate, spellId
static int lua_UnitAura(lua_State* L, bool wantBuff) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    const char* uid = luaL_optstring(L, 1, "player");
    int index = static_cast<int>(luaL_optnumber(L, 2, 1));
    if (index < 1) { lua_pushnil(L); return 1; }

    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const std::vector<game::AuraSlot>* auras = nullptr;
    if (uidStr == "player")      auras = &gh->getPlayerAuras();
    else if (uidStr == "target") auras = &gh->getTargetAuras();
    else {
        // Try party/raid/focus via GUID lookup in unitAurasCache
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid != 0) auras = gh->getUnitAuras(guid);
    }
    if (!auras) { lua_pushnil(L); return 1; }

    // Filter to buffs or debuffs and find the Nth one
    int found = 0;
    for (const auto& aura : *auras) {
        if (aura.isEmpty() || aura.spellId == 0) continue;
        bool isDebuff = (aura.flags & 0x80) != 0;
        if (wantBuff ? isDebuff : !isDebuff) continue;
        found++;
        if (found == index) {
            // Return: name, rank, icon, count, debuffType, duration, expirationTime, ...spellId
            std::string name = gh->getSpellName(aura.spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
            lua_pushstring(L, "");           // rank
            std::string iconPath = gh->getSpellIconPath(aura.spellId);
            if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
            else lua_pushnil(L);             // icon texture path
            lua_pushnumber(L, aura.charges); // count
            // debuffType: resolve from Spell.dbc dispel type
            {
                uint8_t dt = gh->getSpellDispelType(aura.spellId);
                switch (dt) {
                    case 1:  lua_pushstring(L, "Magic");  break;
                    case 2:  lua_pushstring(L, "Curse");  break;
                    case 3:  lua_pushstring(L, "Disease"); break;
                    case 4:  lua_pushstring(L, "Poison"); break;
                    default: lua_pushnil(L);              break;
                }
            }
            lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
            // expirationTime: GetTime() + remaining seconds (so addons can compute countdown)
            if (aura.durationMs > 0) {
                uint64_t auraNowMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                int32_t remMs = aura.getRemainingMs(auraNowMs);
                // GetTime epoch = steady_clock relative to engine start
                static auto sStart = std::chrono::steady_clock::now();
                double nowSec = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - sStart).count();
                lua_pushnumber(L, nowSec + remMs / 1000.0);
            } else {
                lua_pushnumber(L, 0);  // permanent aura
            }
            // caster: return unit ID string if caster is known
            if (aura.casterGuid != 0) {
                if (aura.casterGuid == gh->getPlayerGuid())
                    lua_pushstring(L, "player");
                else if (aura.casterGuid == gh->getTargetGuid())
                    lua_pushstring(L, "target");
                else if (aura.casterGuid == gh->getFocusGuid())
                    lua_pushstring(L, "focus");
                else if (aura.casterGuid == gh->getPetGuid())
                    lua_pushstring(L, "pet");
                else {
                    char cBuf[32];
                    snprintf(cBuf, sizeof(cBuf), "0x%016llX", (unsigned long long)aura.casterGuid);
                    lua_pushstring(L, cBuf);
                }
            } else {
                lua_pushnil(L);
            }
            lua_pushboolean(L, 0);           // isStealable
            lua_pushboolean(L, 0);           // shouldConsolidate
            lua_pushnumber(L, aura.spellId); // spellId
            return 11;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int lua_UnitBuff(lua_State* L) { return lua_UnitAura(L, true); }
static int lua_UnitDebuff(lua_State* L) { return lua_UnitAura(L, false); }

// UnitAura(unit, index, filter) — generic aura query with filter string
// filter: "HELPFUL" = buffs, "HARMFUL" = debuffs, "PLAYER" = cast by player,
//         "HELPFUL|PLAYER" = buffs cast by player, etc.
static int lua_UnitAuraGeneric(lua_State* L) {
    const char* filter = luaL_optstring(L, 3, "HELPFUL");
    std::string f(filter ? filter : "HELPFUL");
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    bool wantBuff = (f.find("HARMFUL") == std::string::npos);
    return lua_UnitAura(L, wantBuff);
}

// ---------- UnitCastingInfo / UnitChannelInfo ----------
// Internal helper: pushes cast/channel info for a unit.
// Returns number of Lua return values (0 if not casting/channeling the requested type).
static int lua_UnitCastInfo(lua_State* L, bool wantChannel) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }

    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid ? uid : "player");

    // GetTime epoch for consistent time values
    static auto sStart = std::chrono::steady_clock::now();
    double nowSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sStart).count();

    // Resolve cast state for the unit
    bool isCasting = false;
    bool isChannel = false;
    uint32_t spellId = 0;
    float timeTotal = 0.0f;
    float timeRemaining = 0.0f;
    bool interruptible = true;

    if (uidStr == "player") {
        isCasting = gh->isCasting();
        isChannel = gh->isChanneling();
        spellId = gh->getCurrentCastSpellId();
        timeTotal = gh->getCastTimeTotal();
        timeRemaining = gh->getCastTimeRemaining();
        // Player interruptibility: always true for own casts (server controls actual interrupt)
        interruptible = true;
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { lua_pushnil(L); return 1; }
        const auto* state = gh->getUnitCastState(guid);
        if (!state) { lua_pushnil(L); return 1; }
        isCasting = state->casting;
        isChannel = state->isChannel;
        spellId = state->spellId;
        timeTotal = state->timeTotal;
        timeRemaining = state->timeRemaining;
        interruptible = state->interruptible;
    }

    if (!isCasting) { lua_pushnil(L); return 1; }

    // UnitCastingInfo: only returns for non-channel casts
    // UnitChannelInfo: only returns for channels
    if (wantChannel != isChannel) { lua_pushnil(L); return 1; }

    // Spell name + icon
    const std::string& name = gh->getSpellName(spellId);
    std::string iconPath = gh->getSpellIconPath(spellId);

    // Time values in milliseconds (WoW API convention)
    double startTimeMs = (nowSec - (timeTotal - timeRemaining)) * 1000.0;
    double endTimeMs   = (nowSec + timeRemaining) * 1000.0;

    // Return values match WoW API:
    // UnitCastingInfo: name, text, texture, startTime, endTime, isTradeSkill, castID, notInterruptible
    // UnitChannelInfo: name, text, texture, startTime, endTime, isTradeSkill, notInterruptible
    lua_pushstring(L, name.empty() ? "Unknown" : name.c_str()); // name
    lua_pushstring(L, "");                                       // text (sub-text, usually empty)
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushstring(L, "Interface\\Icons\\INV_Misc_QuestionMark");  // texture
    lua_pushnumber(L, startTimeMs);                              // startTime (ms)
    lua_pushnumber(L, endTimeMs);                                // endTime (ms)
    lua_pushboolean(L, gh->isProfessionSpell(spellId) ? 1 : 0); // isTradeSkill
    if (!wantChannel) {
        lua_pushnumber(L, spellId);                              // castID (UnitCastingInfo only)
    }
    lua_pushboolean(L, interruptible ? 0 : 1);                  // notInterruptible
    return wantChannel ? 7 : 8;
}

static int lua_UnitCastingInfo(lua_State* L) { return lua_UnitCastInfo(L, false); }
static int lua_UnitChannelInfo(lua_State* L) { return lua_UnitCastInfo(L, true); }

// --- Action API ---

static int lua_SendChatMessage(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* msg = luaL_checkstring(L, 1);
    const char* chatType = luaL_optstring(L, 2, "SAY");
    // language arg (3) ignored — server determines language
    const char* target = luaL_optstring(L, 4, "");

    std::string typeStr(chatType);
    for (char& c : typeStr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    game::ChatType ct = game::ChatType::SAY;
    if (typeStr == "SAY")            ct = game::ChatType::SAY;
    else if (typeStr == "YELL")      ct = game::ChatType::YELL;
    else if (typeStr == "PARTY")     ct = game::ChatType::PARTY;
    else if (typeStr == "GUILD")     ct = game::ChatType::GUILD;
    else if (typeStr == "OFFICER")   ct = game::ChatType::OFFICER;
    else if (typeStr == "RAID")      ct = game::ChatType::RAID;
    else if (typeStr == "WHISPER")   ct = game::ChatType::WHISPER;
    else if (typeStr == "BATTLEGROUND") ct = game::ChatType::BATTLEGROUND;

    std::string targetStr(target && *target ? target : "");
    gh->sendChatMessage(ct, msg, targetStr);
    return 0;
}

static int lua_CastSpellByName(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* name = luaL_checkstring(L, 1);
    if (!name || !*name) return 0;

    // Find highest rank of spell by name (same logic as /cast)
    std::string nameLow(name);
    for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    uint32_t bestId = 0;
    int bestRank = -1;
    for (uint32_t sid : gh->getKnownSpells()) {
        std::string sn = gh->getSpellName(sid);
        for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (sn != nameLow) continue;
        int rank = 0;
        const std::string& rk = gh->getSpellRank(sid);
        if (!rk.empty()) {
            std::string rkl = rk;
            for (char& c : rkl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (rkl.rfind("rank ", 0) == 0) {
                try { rank = std::stoi(rkl.substr(5)); } catch (...) {}
            }
        }
        if (rank > bestRank) { bestRank = rank; bestId = sid; }
    }
    if (bestId != 0) {
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        gh->castSpell(bestId, target);
    }
    return 0;
}

// SendAddonMessage(prefix, text, chatType, target) — send addon message
static int lua_SendAddonMessage(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* prefix = luaL_checkstring(L, 1);
    const char* text = luaL_checkstring(L, 2);
    const char* chatType = luaL_optstring(L, 3, "PARTY");
    const char* target = luaL_optstring(L, 4, "");

    // Build addon message: prefix + TAB + text, send via the appropriate channel
    std::string typeStr(chatType);
    for (char& c : typeStr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    game::ChatType ct = game::ChatType::PARTY;
    if (typeStr == "PARTY")           ct = game::ChatType::PARTY;
    else if (typeStr == "RAID")       ct = game::ChatType::RAID;
    else if (typeStr == "GUILD")      ct = game::ChatType::GUILD;
    else if (typeStr == "OFFICER")    ct = game::ChatType::OFFICER;
    else if (typeStr == "BATTLEGROUND") ct = game::ChatType::BATTLEGROUND;
    else if (typeStr == "WHISPER")    ct = game::ChatType::WHISPER;

    // Encode as prefix\ttext (WoW addon message format)
    std::string encoded = std::string(prefix) + "\t" + text;
    std::string targetStr(target && *target ? target : "");
    gh->sendChatMessage(ct, encoded, targetStr);
    return 0;
}

// RegisterAddonMessagePrefix(prefix) — register prefix for receiving addon messages
static int lua_RegisterAddonMessagePrefix(lua_State* L) {
    const char* prefix = luaL_checkstring(L, 1);
    // Store in a global Lua table for filtering
    lua_getglobal(L, "__WoweeAddonPrefixes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeAddonPrefixes");
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, prefix);
    lua_pop(L, 1);
    lua_pushboolean(L, 1); // success
    return 1;
}

// IsAddonMessagePrefixRegistered(prefix) → boolean
static int lua_IsAddonMessagePrefixRegistered(lua_State* L) {
    const char* prefix = luaL_checkstring(L, 1);
    lua_getglobal(L, "__WoweeAddonPrefixes");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, prefix);
        lua_pushboolean(L, lua_toboolean(L, -1));
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
    return 1;
}

// --- Spell Book Tab API ---

// GetNumSpellTabs() → count
static int lua_GetNumSpellTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getSpellBookTabs().size());
    return 1;
}

// GetSpellTabInfo(tabIndex) → name, texture, offset, numSpells
// tabIndex is 1-based; offset is 1-based global spell book slot
static int lua_GetSpellTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIdx = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || tabIdx < 1) {
        lua_pushnil(L); return 1;
    }
    const auto& tabs = gh->getSpellBookTabs();
    if (tabIdx > static_cast<int>(tabs.size())) {
        lua_pushnil(L); return 1;
    }
    // Compute offset: sum of spells in all preceding tabs (1-based)
    int offset = 0;
    for (int i = 0; i < tabIdx - 1; ++i)
        offset += static_cast<int>(tabs[i].spellIds.size());
    const auto& tab = tabs[tabIdx - 1];
    lua_pushstring(L, tab.name.c_str());           // name
    lua_pushstring(L, tab.texture.c_str());        // texture
    lua_pushnumber(L, offset);                     // offset (0-based for WoW compat)
    lua_pushnumber(L, tab.spellIds.size());        // numSpells
    return 4;
}

// GetSpellBookItemInfo(slot, bookType) → "SPELL", spellId
// slot is 1-based global spell book index
static int lua_GetSpellBookItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) {
        lua_pushstring(L, "SPELL");
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot; // 1-based
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            lua_pushstring(L, "SPELL");
            lua_pushnumber(L, tab.spellIds[idx - 1]);
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushstring(L, "SPELL");
    lua_pushnumber(L, 0);
    return 2;
}

// GetSpellBookItemName(slot, bookType) → name, subName
static int lua_GetSpellBookItemName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || slot < 1) { lua_pushnil(L); return 1; }
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            uint32_t spellId = tab.spellIds[idx - 1];
            const std::string& name = gh->getSpellName(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, ""); // subName/rank
            return 2;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    lua_pushnil(L);
    return 1;
}

static int lua_GetSpellCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    // Accept spell name or ID
    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else {
        const char* name = luaL_checkstring(L, 1);
        std::string nameLow(name);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    float cd = gh->getSpellCooldown(spellId);
    // Also check GCD — if spell has no individual cooldown but GCD is active,
    // return the GCD timing (this is how WoW handles it)
    float gcdRem = gh->getGCDRemaining();
    float gcdTotal = gh->getGCDTotal();

    // WoW returns (start, duration, enabled) where remaining = start + duration - GetTime()
    static auto sStart = std::chrono::steady_clock::now();
    double nowSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sStart).count();

    if (cd > 0.01f) {
        // Spell-specific cooldown (longer than GCD)
        double start = nowSec - 0.01; // approximate start as "just now" minus epsilon
        lua_pushnumber(L, start);
        lua_pushnumber(L, cd);
    } else if (gcdRem > 0.01f) {
        // GCD is active — return GCD timing
        double elapsed = gcdTotal - gcdRem;
        double start = nowSec - elapsed;
        lua_pushnumber(L, start);
        lua_pushnumber(L, gcdTotal);
    } else {
        lua_pushnumber(L, 0);       // not on cooldown
        lua_pushnumber(L, 0);
    }
    lua_pushnumber(L, 1);           // enabled
    return 3;
}

static int lua_HasTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasTarget());
    return 1;
}

// TargetUnit(unitId) — set current target
static int lua_TargetUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_checkstring(L, 1);
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setTarget(guid);
    return 0;
}

// ClearTarget() — clear current target
static int lua_ClearTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearTarget();
    return 0;
}

// FocusUnit(unitId) — set focus target
static int lua_FocusUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, nullptr);
    if (!uid || !*uid) return 0;
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid != 0) gh->setFocus(guid);
    return 0;
}

// ClearFocus() — clear focus target
static int lua_ClearFocus(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->clearFocus();
    return 0;
}

// AssistUnit(unitId) — target whatever the given unit is targeting
static int lua_AssistUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    uint64_t theirTarget = getEntityTargetGuid(gh, guid);
    if (theirTarget != 0) gh->setTarget(theirTarget);
    return 0;
}

// TargetLastTarget() — re-target previous target
static int lua_TargetLastTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetLastTarget();
    return 0;
}

// TargetNearestEnemy() — tab-target nearest enemy
static int lua_TargetNearestEnemy(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetEnemy(false);
    return 0;
}

// TargetNearestFriend() — target nearest friendly unit
static int lua_TargetNearestFriend(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->targetFriend(false);
    return 0;
}

// GetRaidTargetIndex(unit) → icon index (1-8) or nil
static int lua_GetRaidTargetIndex(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushnil(L); return 1; }
    uint8_t mark = gh->getEntityRaidMark(guid);
    if (mark == 0xFF) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, mark + 1); // WoW uses 1-indexed (1=Star, 2=Circle, ... 8=Skull)
    return 1;
}

// SetRaidTarget(unit, index) — set raid marker (1-8, or 0 to clear)
static int lua_SetRaidTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "target");
    int index = static_cast<int>(luaL_checknumber(L, 2));
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) return 0;
    if (index >= 1 && index <= 8)
        gh->setRaidMark(guid, static_cast<uint8_t>(index - 1));
    else if (index == 0)
        gh->setRaidMark(guid, 0xFF); // clear
    return 0;
}

// GetSpellPowerCost(spellId) → {{ type=powerType, cost=manaCost, name=powerName }}
static int lua_GetSpellPowerCost(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_newtable(L); return 1; }
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    auto data = gh->getSpellData(spellId);
    lua_newtable(L); // outer table (array of cost entries)
    if (data.manaCost > 0) {
        lua_newtable(L); // cost entry
        lua_pushnumber(L, data.powerType);
        lua_setfield(L, -2, "type");
        lua_pushnumber(L, data.manaCost);
        lua_setfield(L, -2, "cost");
        static const char* kPowerNames[] = {"MANA","RAGE","FOCUS","ENERGY","HAPPINESS","","RUNIC_POWER"};
        lua_pushstring(L, data.powerType < 7 ? kPowerNames[data.powerType] : "MANA");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, 1); // outer[1] = entry
    }
    return 1;
}

// --- GetSpellInfo / GetSpellTexture ---
// GetSpellInfo(spellIdOrName) -> name, rank, icon, castTime, minRange, maxRange, spellId
static int lua_GetSpellInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushnil(L); return 1; }
        std::string nameLow(name);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        int bestRank = -1;
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn != nameLow) continue;
            int rank = 0;
            const std::string& rk = gh->getSpellRank(sid);
            if (!rk.empty()) {
                std::string rkl = rk;
                for (char& c : rkl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (rkl.rfind("rank ", 0) == 0) {
                    try { rank = std::stoi(rkl.substr(5)); } catch (...) {}
                }
            }
            if (rank > bestRank) { bestRank = rank; spellId = sid; }
        }
    }

    if (spellId == 0) { lua_pushnil(L); return 1; }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { lua_pushnil(L); return 1; }

    lua_pushstring(L, name.c_str());                        // 1: name
    const std::string& rank = gh->getSpellRank(spellId);
    lua_pushstring(L, rank.c_str());                        // 2: rank
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);                                     // 3: icon texture path
    // Resolve cast time and range from Spell.dbc → SpellCastTimes.dbc / SpellRange.dbc
    auto spellData = gh->getSpellData(spellId);
    lua_pushnumber(L, spellData.castTimeMs);                 // 4: castTime (ms)
    lua_pushnumber(L, spellData.minRange);                   // 5: minRange (yards)
    lua_pushnumber(L, spellData.maxRange);                   // 6: maxRange (yards)
    lua_pushnumber(L, spellId);                              // 7: spellId
    return 7;
}

// GetSpellTexture(spellIdOrName) -> icon texture path string
static int lua_GetSpellTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushnil(L); return 1; }
        std::string nameLow(name);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { lua_pushnil(L); return 1; }
    std::string iconPath = gh->getSpellIconPath(spellId);
    if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
    else lua_pushnil(L);
    return 1;
}

// GetItemInfo(itemId) -> name, link, quality, iLevel, reqLevel, class, subclass, maxStack, equipSlot, texture, vendorPrice
static int lua_GetItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }

    uint32_t itemId = 0;
    if (lua_isnumber(L, 1)) {
        itemId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        // Try to parse "item:12345" link format
        const char* s = lua_tostring(L, 1);
        std::string str(s ? s : "");
        auto pos = str.find("item:");
        if (pos != std::string::npos) {
            try { itemId = static_cast<uint32_t>(std::stoul(str.substr(pos + 5))); } catch (...) {}
        }
    }
    if (itemId == 0) { lua_pushnil(L); return 1; }

    const auto* info = gh->getItemInfo(itemId);
    if (!info) { lua_pushnil(L); return 1; }

    lua_pushstring(L, info->name.c_str());          // 1: name
    // Build item link with quality-colored text
    static const char* kQualityHex[] = {
        "ff9d9d9d", // 0 Poor (gray)
        "ffffffff", // 1 Common (white)
        "ff1eff00", // 2 Uncommon (green)
        "ff0070dd", // 3 Rare (blue)
        "ffa335ee", // 4 Epic (purple)
        "ffff8000", // 5 Legendary (orange)
        "ffe6cc80", // 6 Artifact (gold)
        "ff00ccff", // 7 Heirloom (cyan)
    };
    const char* colorHex = (info->quality < 8) ? kQualityHex[info->quality] : "ffffffff";
    char link[256];
    snprintf(link, sizeof(link), "|c%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             colorHex, itemId, info->name.c_str());
    lua_pushstring(L, link);                         // 2: link
    lua_pushnumber(L, info->quality);                // 3: quality
    lua_pushnumber(L, info->itemLevel);              // 4: iLevel
    lua_pushnumber(L, info->requiredLevel);          // 5: requiredLevel
    lua_pushstring(L, "");                           // 6: class (type string)
    lua_pushstring(L, "");                           // 7: subclass
    lua_pushnumber(L, info->maxStack > 0 ? info->maxStack : 1); // 8: maxStack
    lua_pushstring(L, "");                           // 9: equipSlot
    // 10: texture (icon path from ItemDisplayInfo.dbc)
    if (info->displayInfoId != 0) {
        std::string iconPath = gh->getItemIconPath(info->displayInfoId);
        if (!iconPath.empty()) lua_pushstring(L, iconPath.c_str());
        else lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    lua_pushnumber(L, info->sellPrice);              // 11: vendorPrice
    return 11;
}

// GetItemQualityColor(quality) → r, g, b, hex
// Quality: 0=Poor(gray), 1=Common(white), 2=Uncommon(green), 3=Rare(blue),
//          4=Epic(purple), 5=Legendary(orange), 6=Artifact(gold), 7=Heirloom(gold)
static int lua_GetItemQualityColor(lua_State* L) {
    int q = static_cast<int>(luaL_checknumber(L, 1));
    struct QC { float r, g, b; const char* hex; };
    static const QC colors[] = {
        {0.62f, 0.62f, 0.62f, "ff9d9d9d"}, // 0 Poor
        {1.00f, 1.00f, 1.00f, "ffffffff"}, // 1 Common
        {0.12f, 1.00f, 0.00f, "ff1eff00"}, // 2 Uncommon
        {0.00f, 0.44f, 0.87f, "ff0070dd"}, // 3 Rare
        {0.64f, 0.21f, 0.93f, "ffa335ee"}, // 4 Epic
        {1.00f, 0.50f, 0.00f, "ffff8000"}, // 5 Legendary
        {0.90f, 0.80f, 0.50f, "ffe6cc80"}, // 6 Artifact
        {0.00f, 0.80f, 1.00f, "ff00ccff"}, // 7 Heirloom
    };
    if (q < 0 || q > 7) q = 1;
    lua_pushnumber(L, colors[q].r);
    lua_pushnumber(L, colors[q].g);
    lua_pushnumber(L, colors[q].b);
    lua_pushstring(L, colors[q].hex);
    return 4;
}

// GetItemCount(itemId [, includeBank]) → count
static int lua_GetItemCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    uint32_t itemId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    const auto& inv = gh->getInventory();
    uint32_t count = 0;
    // Backpack
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        const auto& s = inv.getBackpackSlot(i);
        if (!s.empty() && s.item.itemId == itemId)
            count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
    }
    // Bags 1-4
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
        int sz = inv.getBagSize(b);
        for (int i = 0; i < sz; ++i) {
            const auto& s = inv.getBagSlot(b, i);
            if (!s.empty() && s.item.itemId == itemId)
                count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
        }
    }
    lua_pushnumber(L, count);
    return 1;
}

// UseContainerItem(bag, slot) — use/equip an item from a bag
static int lua_UseContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;
    if (bag == 0 && slot >= 1 && slot <= inv.getBackpackSize())
        itemSlot = &inv.getBackpackSlot(slot - 1);
    else if (bag >= 1 && bag <= 4) {
        int sz = inv.getBagSize(bag - 1);
        if (slot >= 1 && slot <= sz)
            itemSlot = &inv.getBagSlot(bag - 1, slot - 1);
    }
    if (itemSlot && !itemSlot->empty())
        gh->useItemById(itemSlot->item.itemId);
    return 0;
}

// --- Locale/Build/Realm info ---

static int lua_GetLocale(lua_State* L) {
    lua_pushstring(L, "enUS");
    return 1;
}

static int lua_GetBuildInfo(lua_State* L) {
    // Return WotLK defaults; expansion-specific version detection would need
    // access to the expansion registry which isn't available here.
    lua_pushstring(L, "3.3.5a");    // 1: version
    lua_pushnumber(L, 12340);       // 2: buildNumber
    lua_pushstring(L, "Jan 1 2025");// 3: date
    lua_pushnumber(L, 30300);       // 4: tocVersion
    return 4;
}

static int lua_GetCurrentMapAreaID(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getCurrentMapId() : 0);
    return 1;
}

// GetZoneText() / GetRealZoneText() → current zone name
static int lua_GetZoneText(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    uint32_t zoneId = gh->getWorldStateZoneId();
    if (zoneId != 0) {
        std::string name = gh->getWhoAreaName(zoneId);
        if (!name.empty()) { lua_pushstring(L, name.c_str()); return 1; }
    }
    lua_pushstring(L, "");
    return 1;
}

// GetSubZoneText() → subzone name (same as zone for now — server doesn't always send subzone)
static int lua_GetSubZoneText(lua_State* L) {
    return lua_GetZoneText(L);  // Best-effort: zone and subzone often overlap
}

// GetMinimapZoneText() → zone name displayed near minimap
static int lua_GetMinimapZoneText(lua_State* L) {
    return lua_GetZoneText(L);
}

// --- World Map Navigation API ---

// Map ID → continent mapping
static int mapIdToContinent(uint32_t mapId) {
    switch (mapId) {
        case 0:   return 2; // Eastern Kingdoms
        case 1:   return 1; // Kalimdor
        case 530: return 3; // Outland
        case 571: return 4; // Northrend
        default:  return 0; // Instance or unknown
    }
}

// Internal tracked map state (which continent/zone the map UI is viewing)
static int s_mapContinent = 0;
static int s_mapZone = 0;

// SetMapToCurrentZone() — sets map view to the player's current zone
static int lua_SetMapToCurrentZone(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) {
        s_mapContinent = mapIdToContinent(gh->getCurrentMapId());
        s_mapZone = static_cast<int>(gh->getWorldStateZoneId());
    }
    return 0;
}

// GetCurrentMapContinent() → continentId (1=Kalimdor, 2=EK, 3=Outland, 4=Northrend)
static int lua_GetCurrentMapContinent(lua_State* L) {
    if (s_mapContinent == 0) {
        auto* gh = getGameHandler(L);
        if (gh) s_mapContinent = mapIdToContinent(gh->getCurrentMapId());
    }
    lua_pushnumber(L, s_mapContinent);
    return 1;
}

// GetCurrentMapZone() → zoneId
static int lua_GetCurrentMapZone(lua_State* L) {
    if (s_mapZone == 0) {
        auto* gh = getGameHandler(L);
        if (gh) s_mapZone = static_cast<int>(gh->getWorldStateZoneId());
    }
    lua_pushnumber(L, s_mapZone);
    return 1;
}

// SetMapZoom(continent [, zone]) — sets map view to continent/zone
static int lua_SetMapZoom(lua_State* L) {
    s_mapContinent = static_cast<int>(luaL_checknumber(L, 1));
    s_mapZone = static_cast<int>(luaL_optnumber(L, 2, 0));
    return 0;
}

// GetMapContinents() → "Kalimdor", "Eastern Kingdoms", ...
static int lua_GetMapContinents(lua_State* L) {
    lua_pushstring(L, "Kalimdor");
    lua_pushstring(L, "Eastern Kingdoms");
    lua_pushstring(L, "Outland");
    lua_pushstring(L, "Northrend");
    return 4;
}

// GetMapZones(continent) → zone names for that continent
// Returns a basic list; addons mainly need this to not error
static int lua_GetMapZones(lua_State* L) {
    int cont = static_cast<int>(luaL_checknumber(L, 1));
    // Return a minimal representative set per continent
    switch (cont) {
        case 1: // Kalimdor
            lua_pushstring(L, "Durotar"); lua_pushstring(L, "Mulgore");
            lua_pushstring(L, "The Barrens"); lua_pushstring(L, "Teldrassil");
            return 4;
        case 2: // Eastern Kingdoms
            lua_pushstring(L, "Elwynn Forest"); lua_pushstring(L, "Westfall");
            lua_pushstring(L, "Dun Morogh"); lua_pushstring(L, "Tirisfal Glades");
            return 4;
        case 3: // Outland
            lua_pushstring(L, "Hellfire Peninsula"); lua_pushstring(L, "Zangarmarsh");
            return 2;
        case 4: // Northrend
            lua_pushstring(L, "Borean Tundra"); lua_pushstring(L, "Howling Fjord");
            return 2;
        default:
            return 0;
    }
}

// GetNumMapLandmarks() → 0 (no landmark data exposed yet)
static int lua_GetNumMapLandmarks(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// --- Player State API ---
// These replace the hardcoded "return false" Lua stubs with real game state.

static int lua_IsMounted(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isMounted());
    return 1;
}

static int lua_IsFlying(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerFlying());
    return 1;
}

static int lua_IsSwimming(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isSwimming());
    return 1;
}

static int lua_IsResting(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isPlayerResting());
    return 1;
}

static int lua_IsFalling(lua_State* L) {
    auto* gh = getGameHandler(L);
    // Check FALLING movement flag
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const auto& mi = gh->getMovementInfo();
    lua_pushboolean(L, (mi.flags & 0x2000) != 0); // MOVEFLAG_FALLING = 0x2000
    return 1;
}

static int lua_IsStealthed(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    // Check for stealth auras (aura flags bit 0x40 = is harmful, stealth is a buff)
    // WoW detects stealth via unit flags: UNIT_FLAG_IMMUNE (0x02) or specific aura IDs
    // Simplified: check player auras for known stealth spell IDs
    bool stealthed = false;
    for (const auto& a : gh->getPlayerAuras()) {
        if (a.isEmpty() || a.spellId == 0) continue;
        // Common stealth IDs: 1784 (Stealth), 5215 (Prowl), 66 (Invisibility)
        if (a.spellId == 1784 || a.spellId == 5215 || a.spellId == 66 ||
            a.spellId == 1785 || a.spellId == 1786 || a.spellId == 1787 ||
            a.spellId == 11305 || a.spellId == 11306) {
            stealthed = true;
            break;
        }
    }
    lua_pushboolean(L, stealthed);
    return 1;
}

static int lua_GetUnitSpeed(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (!gh || std::string(uid) != "player") {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, gh->getServerRunSpeed());
    return 1;
}

// --- Container/Bag API ---
// WoW bags: container 0 = backpack (16 slots), containers 1-4 = equipped bags

static int lua_GetContainerNumSlots(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    const auto& inv = gh->getInventory();
    if (container == 0) {
        lua_pushnumber(L, inv.getBackpackSize());
    } else if (container >= 1 && container <= 4) {
        lua_pushnumber(L, inv.getBagSize(container - 1));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetContainerItemInfo(container, slot) → texture, count, locked, quality, readable, lootable, link
static int lua_GetContainerItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh) { lua_pushnil(L); return 1; }

    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;

    if (container == 0 && slot >= 1 && slot <= inv.getBackpackSize()) {
        itemSlot = &inv.getBackpackSlot(slot - 1);  // WoW uses 1-based
    } else if (container >= 1 && container <= 4) {
        int bagIdx = container - 1;
        int bagSize = inv.getBagSize(bagIdx);
        if (slot >= 1 && slot <= bagSize)
            itemSlot = &inv.getBagSlot(bagIdx, slot - 1);
    }

    if (!itemSlot || itemSlot->empty()) { lua_pushnil(L); return 1; }

    // Get item info for quality/icon
    const auto* info = gh->getItemInfo(itemSlot->item.itemId);

    lua_pushnil(L);  // texture (icon path — would need ItemDisplayInfo icon resolver)
    lua_pushnumber(L, itemSlot->item.stackCount);  // count
    lua_pushboolean(L, 0);  // locked
    lua_pushnumber(L, info ? info->quality : 0);  // quality
    lua_pushboolean(L, 0);  // readable
    lua_pushboolean(L, 0);  // lootable
    // Build item link with quality color
    std::string name = info ? info->name : ("Item #" + std::to_string(itemSlot->item.itemId));
    uint32_t q = info ? info->quality : 0;
    static const char* kQH[] = {"9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","e6cc80"};
    uint32_t qi = q < 8 ? q : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQH[qi], itemSlot->item.itemId, name.c_str());
    lua_pushstring(L, link);  // link
    return 7;
}

// GetContainerItemLink(container, slot) → item link string
static int lua_GetContainerItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh) { lua_pushnil(L); return 1; }

    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;

    if (container == 0 && slot >= 1 && slot <= inv.getBackpackSize()) {
        itemSlot = &inv.getBackpackSlot(slot - 1);
    } else if (container >= 1 && container <= 4) {
        int bagIdx = container - 1;
        int bagSize = inv.getBagSize(bagIdx);
        if (slot >= 1 && slot <= bagSize)
            itemSlot = &inv.getBagSlot(bagIdx, slot - 1);
    }

    if (!itemSlot || itemSlot->empty()) { lua_pushnil(L); return 1; }
    const auto* info = gh->getItemInfo(itemSlot->item.itemId);
    std::string name = info ? info->name : ("Item #" + std::to_string(itemSlot->item.itemId));
    uint32_t q = info ? info->quality : 0;
    char link[256];
    static const char* kQH[] = {"9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","e6cc80"};
    uint32_t qi = q < 8 ? q : 1u;
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQH[qi], itemSlot->item.itemId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// GetContainerNumFreeSlots(container) → numFreeSlots, bagType
static int lua_GetContainerNumFreeSlots(lua_State* L) {
    auto* gh = getGameHandler(L);
    int container = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }

    const auto& inv = gh->getInventory();
    int freeSlots = 0;
    int totalSlots = 0;

    if (container == 0) {
        totalSlots = inv.getBackpackSize();
        for (int i = 0; i < totalSlots; ++i)
            if (inv.getBackpackSlot(i).empty()) ++freeSlots;
    } else if (container >= 1 && container <= 4) {
        totalSlots = inv.getBagSize(container - 1);
        for (int i = 0; i < totalSlots; ++i)
            if (inv.getBagSlot(container - 1, i).empty()) ++freeSlots;
    }

    lua_pushnumber(L, freeSlots);
    lua_pushnumber(L, 0);  // bagType (0 = normal)
    return 2;
}

// --- Equipment Slot API ---
// WoW inventory slot IDs: 1=Head,2=Neck,3=Shoulders,4=Shirt,5=Chest,
// 6=Waist,7=Legs,8=Feet,9=Wrists,10=Hands,11=Ring1,12=Ring2,
// 13=Trinket1,14=Trinket2,15=Back,16=MainHand,17=OffHand,18=Ranged,19=Tabard

// GetInventorySlotInfo("slotName") → slotId, textureName, checkRelic
// Maps WoW slot names (e.g. "HeadSlot", "HEADSLOT") to inventory slot IDs
static int lua_GetInventorySlotInfo(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string slot(name);
    // Normalize: uppercase, strip trailing "SLOT" if present
    for (char& c : slot) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (slot.size() > 4 && slot.substr(slot.size() - 4) == "SLOT")
        slot = slot.substr(0, slot.size() - 4);

    // WoW inventory slots are 1-indexed
    struct SlotMap { const char* name; int id; const char* texture; };
    static const SlotMap mapping[] = {
        {"HEAD",          1,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Head"},
        {"NECK",          2,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Neck"},
        {"SHOULDER",      3,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Shoulder"},
        {"SHIRT",         4,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Shirt"},
        {"CHEST",         5,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Chest"},
        {"WAIST",         6,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Waist"},
        {"LEGS",          7,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Legs"},
        {"FEET",          8,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Feet"},
        {"WRIST",         9,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Wrists"},
        {"HANDS",        10,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Hands"},
        {"FINGER0",      11,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Finger"},
        {"FINGER1",      12,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Finger"},
        {"TRINKET0",     13,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Trinket"},
        {"TRINKET1",     14,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Trinket"},
        {"BACK",         15,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Chest"},
        {"MAINHAND",     16,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-MainHand"},
        {"SECONDARYHAND",17,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-SecondaryHand"},
        {"RANGED",       18,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Ranged"},
        {"TABARD",       19,  "Interface\\PaperDoll\\UI-PaperDoll-Slot-Tabard"},
    };
    for (const auto& m : mapping) {
        if (slot == m.name) {
            lua_pushnumber(L, m.id);
            lua_pushstring(L, m.texture);
            lua_pushboolean(L, m.id == 18 ? 1 : 0); // checkRelic: only ranged slot
            return 3;
        }
    }
    luaL_error(L, "Unknown inventory slot: %s", name);
    return 0;
}

static int lua_GetInventoryItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { lua_pushnil(L); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr != "player") { lua_pushnil(L); return 1; }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { lua_pushnil(L); return 1; }

    const auto* info = gh->getItemInfo(slot.item.itemId);
    std::string name = info ? info->name : slot.item.name;
    uint32_t q = info ? info->quality : static_cast<uint32_t>(slot.item.quality);
    static const char* kQH[] = {"9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","e6cc80"};
    uint32_t qi = q < 8 ? q : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQH[qi], slot.item.itemId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

static int lua_GetInventoryItemID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { lua_pushnil(L); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr != "player") { lua_pushnil(L); return 1; }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, slot.item.itemId);
    return 1;
}

static int lua_GetInventoryItemTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* uid = luaL_optstring(L, 1, "player");
    int slotId = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || slotId < 1 || slotId > 19) { lua_pushnil(L); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr != "player") { lua_pushnil(L); return 1; }

    const auto& inv = gh->getInventory();
    const auto& slot = inv.getEquipSlot(static_cast<game::EquipSlot>(slotId - 1));
    if (slot.empty()) { lua_pushnil(L); return 1; }
    // Return spell icon path for the item's on-use spell, or nil
    lua_pushnil(L);
    return 1;
}

// --- Time & XP API ---

static int lua_GetGameTime(lua_State* L) {
    // Returns server game time as hours, minutes
    auto* gh = getGameHandler(L);
    if (gh) {
        float gt = gh->getGameTime();
        int hours = static_cast<int>(gt) % 24;
        int mins = static_cast<int>((gt - static_cast<int>(gt)) * 60.0f);
        lua_pushnumber(L, hours);
        lua_pushnumber(L, mins);
    } else {
        lua_pushnumber(L, 12);
        lua_pushnumber(L, 0);
    }
    return 2;
}

static int lua_GetServerTime(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(std::time(nullptr)));
    return 1;
}

static int lua_UnitXP(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    std::string u(uid);
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (u == "player") lua_pushnumber(L, gh->getPlayerXp());
    else lua_pushnumber(L, 0);
    return 1;
}

static int lua_UnitXPMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 1); return 1; }
    std::string u(uid);
    for (char& c : u) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (u == "player") {
        uint32_t nlxp = gh->getPlayerNextLevelXp();
        lua_pushnumber(L, nlxp > 0 ? nlxp : 1);
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

// GetXPExhaustion() → rested XP pool remaining (nil if none)
static int lua_GetXPExhaustion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    uint32_t rested = gh->getPlayerRestedXp();
    if (rested > 0) lua_pushnumber(L, rested);
    else lua_pushnil(L);
    return 1;
}

// GetRestState() → 1 = normal, 2 = rested
static int lua_GetRestState(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, (gh && gh->isPlayerResting()) ? 2 : 1);
    return 1;
}

// --- Quest Log API ---

static int lua_GetNumQuestLogEntries(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    const auto& ql = gh->getQuestLog();
    lua_pushnumber(L, ql.size());  // numEntries
    lua_pushnumber(L, 0);          // numQuests (headers not tracked)
    return 2;
}

// GetQuestLogTitle(index) → title, level, suggestedGroup, isHeader, isCollapsed, isComplete, frequency, questID
static int lua_GetQuestLogTitle(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    const auto& ql = gh->getQuestLog();
    if (index > static_cast<int>(ql.size())) { lua_pushnil(L); return 1; }
    const auto& q = ql[index - 1];  // 1-based
    lua_pushstring(L, q.title.c_str());  // title
    lua_pushnumber(L, 0);                // level (not tracked)
    lua_pushnumber(L, 0);                // suggestedGroup
    lua_pushboolean(L, 0);               // isHeader
    lua_pushboolean(L, 0);               // isCollapsed
    lua_pushboolean(L, q.complete);      // isComplete
    lua_pushnumber(L, 0);                // frequency
    lua_pushnumber(L, q.questId);        // questID
    return 8;
}

// GetQuestLogQuestText(index) → description, objectives
static int lua_GetQuestLogQuestText(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    const auto& ql = gh->getQuestLog();
    if (index > static_cast<int>(ql.size())) { lua_pushnil(L); return 1; }
    const auto& q = ql[index - 1];
    lua_pushstring(L, "");                    // description (not stored)
    lua_pushstring(L, q.objectives.c_str());  // objectives
    return 2;
}

// IsQuestComplete(questID) → boolean
static int lua_IsQuestComplete(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t questId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    for (const auto& q : gh->getQuestLog()) {
        if (q.questId == questId) {
            lua_pushboolean(L, q.complete);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// SelectQuestLogEntry(index) — select a quest in the quest log
static int lua_SelectQuestLogEntry(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (gh) gh->setSelectedQuestLogIndex(index);
    return 0;
}

// GetQuestLogSelection() → index
static int lua_GetQuestLogSelection(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getSelectedQuestLogIndex() : 0);
    return 1;
}

// GetNumQuestWatches() → count
static int lua_GetNumQuestWatches(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getTrackedQuestIds().size() : 0);
    return 1;
}

// GetQuestIndexForWatch(watchIndex) → questLogIndex
// Maps the Nth watched quest to its quest log index (1-based)
static int lua_GetQuestIndexForWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int watchIdx = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || watchIdx < 1) { lua_pushnil(L); return 1; }
    const auto& ql = gh->getQuestLog();
    const auto& tracked = gh->getTrackedQuestIds();
    int found = 0;
    for (size_t i = 0; i < ql.size(); ++i) {
        if (tracked.count(ql[i].questId)) {
            found++;
            if (found == watchIdx) {
                lua_pushnumber(L, static_cast<int>(i) + 1); // 1-based
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// AddQuestWatch(questLogIndex) — add a quest to the watch list
static int lua_AddQuestWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) return 0;
    const auto& ql = gh->getQuestLog();
    if (index <= static_cast<int>(ql.size())) {
        gh->setQuestTracked(ql[index - 1].questId, true);
    }
    return 0;
}

// RemoveQuestWatch(questLogIndex) — remove a quest from the watch list
static int lua_RemoveQuestWatch(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) return 0;
    const auto& ql = gh->getQuestLog();
    if (index <= static_cast<int>(ql.size())) {
        gh->setQuestTracked(ql[index - 1].questId, false);
    }
    return 0;
}

// IsQuestWatched(questLogIndex) → boolean
static int lua_IsQuestWatched(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushboolean(L, 0); return 1; }
    const auto& ql = gh->getQuestLog();
    if (index <= static_cast<int>(ql.size())) {
        lua_pushboolean(L, gh->isQuestTracked(ql[index - 1].questId) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// GetQuestLink(questLogIndex) → "|cff...|Hquest:id:level|h[title]|h|r"
static int lua_GetQuestLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    const auto& ql = gh->getQuestLog();
    if (index > static_cast<int>(ql.size())) { lua_pushnil(L); return 1; }
    const auto& q = ql[index - 1];
    // Yellow quest link format matching WoW
    std::string link = "|cff808000|Hquest:" + std::to_string(q.questId) +
                       ":0|h[" + q.title + "]|h|r";
    lua_pushstring(L, link.c_str());
    return 1;
}

// GetNumQuestLeaderBoards(questLogIndex) → count of objectives
static int lua_GetNumQuestLeaderBoards(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnumber(L, 0); return 1; }
    const auto& ql = gh->getQuestLog();
    if (index > static_cast<int>(ql.size())) { lua_pushnumber(L, 0); return 1; }
    const auto& q = ql[index - 1];
    int count = 0;
    for (const auto& ko : q.killObjectives) {
        if (ko.npcOrGoId != 0 || ko.required > 0) ++count;
    }
    for (const auto& io : q.itemObjectives) {
        if (io.itemId != 0 || io.required > 0) ++count;
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetQuestLogLeaderBoard(objIndex, questLogIndex) → text, type, finished
// objIndex is 1-based within the quest's objectives
static int lua_GetQuestLogLeaderBoard(lua_State* L) {
    auto* gh = getGameHandler(L);
    int objIdx = static_cast<int>(luaL_checknumber(L, 1));
    int questIdx = static_cast<int>(luaL_optnumber(L, 2,
        gh ? gh->getSelectedQuestLogIndex() : 0));
    if (!gh || questIdx < 1 || objIdx < 1) { lua_pushnil(L); return 1; }
    const auto& ql = gh->getQuestLog();
    if (questIdx > static_cast<int>(ql.size())) { lua_pushnil(L); return 1; }
    const auto& q = ql[questIdx - 1];

    // Build ordered list: kill objectives first, then item objectives
    int cur = 0;
    for (int i = 0; i < 4; ++i) {
        if (q.killObjectives[i].npcOrGoId == 0 && q.killObjectives[i].required == 0) continue;
        ++cur;
        if (cur == objIdx) {
            // Get current count from killCounts map (keyed by abs(npcOrGoId))
            uint32_t key = static_cast<uint32_t>(std::abs(q.killObjectives[i].npcOrGoId));
            uint32_t current = 0;
            auto it = q.killCounts.find(key);
            if (it != q.killCounts.end()) current = it->second.first;
            uint32_t required = q.killObjectives[i].required;
            bool finished = (current >= required);
            // Build display text like "Kobold Vermin slain: 3/8"
            std::string text = (q.killObjectives[i].npcOrGoId < 0 ? "Object" : "Creature")
                + std::string(" slain: ") + std::to_string(current) + "/" + std::to_string(required);
            lua_pushstring(L, text.c_str());
            lua_pushstring(L, q.killObjectives[i].npcOrGoId < 0 ? "object" : "monster");
            lua_pushboolean(L, finished ? 1 : 0);
            return 3;
        }
    }
    for (int i = 0; i < 6; ++i) {
        if (q.itemObjectives[i].itemId == 0 && q.itemObjectives[i].required == 0) continue;
        ++cur;
        if (cur == objIdx) {
            uint32_t current = 0;
            auto it = q.itemCounts.find(q.itemObjectives[i].itemId);
            if (it != q.itemCounts.end()) current = it->second;
            uint32_t required = q.itemObjectives[i].required;
            bool finished = (current >= required);
            // Get item name if available
            std::string itemName;
            const auto* info = gh->getItemInfo(q.itemObjectives[i].itemId);
            if (info && !info->name.empty()) itemName = info->name;
            else itemName = "Item #" + std::to_string(q.itemObjectives[i].itemId);
            std::string text = itemName + ": " + std::to_string(current) + "/" + std::to_string(required);
            lua_pushstring(L, text.c_str());
            lua_pushstring(L, "item");
            lua_pushboolean(L, finished ? 1 : 0);
            return 3;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ExpandQuestHeader / CollapseQuestHeader — no-ops (flat quest list, no headers)
static int lua_ExpandQuestHeader(lua_State* L) { (void)L; return 0; }
static int lua_CollapseQuestHeader(lua_State* L) { (void)L; return 0; }

// GetQuestLogSpecialItemInfo(questLogIndex) — returns nil (no special items)
static int lua_GetQuestLogSpecialItemInfo(lua_State* L) { (void)L; lua_pushnil(L); return 1; }

// --- Skill Line API ---

// GetNumSkillLines() → count
static int lua_GetNumSkillLines(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getPlayerSkills().size());
    return 1;
}

// GetSkillLineInfo(index) → skillName, isHeader, isExpanded, skillRank, numTempPoints, skillModifier, skillMaxRank, isAbandonable, stepCost, rankCost, minLevel, skillCostType
static int lua_GetSkillLineInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) {
        lua_pushnil(L);
        return 1;
    }
    const auto& skills = gh->getPlayerSkills();
    if (index > static_cast<int>(skills.size())) {
        lua_pushnil(L);
        return 1;
    }
    // Skills are in a map — iterate to the Nth entry
    auto it = skills.begin();
    std::advance(it, index - 1);
    const auto& skill = it->second;
    std::string name = gh->getSkillName(skill.skillId);
    if (name.empty()) name = "Skill " + std::to_string(skill.skillId);

    lua_pushstring(L, name.c_str());                    // 1: skillName
    lua_pushboolean(L, 0);                              // 2: isHeader (false — flat list)
    lua_pushboolean(L, 1);                              // 3: isExpanded
    lua_pushnumber(L, skill.effectiveValue());           // 4: skillRank
    lua_pushnumber(L, skill.bonusTemp);                  // 5: numTempPoints
    lua_pushnumber(L, skill.bonusPerm);                  // 6: skillModifier
    lua_pushnumber(L, skill.maxValue);                   // 7: skillMaxRank
    lua_pushboolean(L, 0);                              // 8: isAbandonable
    lua_pushnumber(L, 0);                               // 9: stepCost
    lua_pushnumber(L, 0);                               // 10: rankCost
    lua_pushnumber(L, 0);                               // 11: minLevel
    lua_pushnumber(L, 0);                               // 12: skillCostType
    return 12;
}

// --- Friends/Ignore API ---

// GetNumFriends() → count
static int lua_GetNumFriends(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    int count = 0;
    for (const auto& c : gh->getContacts())
        if (c.isFriend()) count++;
    lua_pushnumber(L, count);
    return 1;
}

// GetFriendInfo(index) → name, level, class, area, connected, status, note
static int lua_GetFriendInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) {
        lua_pushnil(L); return 1;
    }
    int found = 0;
    for (const auto& c : gh->getContacts()) {
        if (!c.isFriend()) continue;
        if (++found == index) {
            lua_pushstring(L, c.name.c_str());      // 1: name
            lua_pushnumber(L, c.level);              // 2: level
            static const char* kClasses[] = {"","Warrior","Paladin","Hunter","Rogue","Priest",
                "Death Knight","Shaman","Mage","Warlock","","Druid"};
            lua_pushstring(L, c.classId < 12 ? kClasses[c.classId] : "Unknown"); // 3: class
            std::string area;
            if (c.areaId != 0) area = gh->getWhoAreaName(c.areaId);
            lua_pushstring(L, area.c_str());         // 4: area
            lua_pushboolean(L, c.isOnline());        // 5: connected
            lua_pushstring(L, c.status == 2 ? "<AFK>" : (c.status == 3 ? "<DND>" : "")); // 6: status
            lua_pushstring(L, c.note.c_str());       // 7: note
            return 7;
        }
    }
    lua_pushnil(L);
    return 1;
}

// --- Guild API ---

// IsInGuild() → boolean
static int lua_IsInGuild(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isInGuild());
    return 1;
}

// GetGuildInfo("player") → guildName, guildRankName, guildRankIndex
static int lua_GetGuildInfoFunc(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGuild()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, gh->getGuildName().c_str());
    // Get rank name for the player
    const auto& roster = gh->getGuildRoster();
    std::string rankName;
    uint32_t rankIndex = 0;
    for (const auto& m : roster.members) {
        if (m.guid == gh->getPlayerGuid()) {
            rankIndex = m.rankIndex;
            const auto& rankNames = gh->getGuildRankNames();
            if (rankIndex < rankNames.size()) rankName = rankNames[rankIndex];
            break;
        }
    }
    lua_pushstring(L, rankName.c_str());
    lua_pushnumber(L, rankIndex);
    return 3;
}

// GetNumGuildMembers() → totalMembers, onlineMembers
static int lua_GetNumGuildMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    const auto& roster = gh->getGuildRoster();
    int online = 0;
    for (const auto& m : roster.members)
        if (m.online) online++;
    lua_pushnumber(L, roster.members.size());
    lua_pushnumber(L, online);
    return 2;
}

// GetGuildRosterInfo(index) → name, rank, rankIndex, level, class, zone, note, officerNote, online, status, classId
static int lua_GetGuildRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    const auto& roster = gh->getGuildRoster();
    if (index > static_cast<int>(roster.members.size())) { lua_pushnil(L); return 1; }
    const auto& m = roster.members[index - 1];

    lua_pushstring(L, m.name.c_str());                      // 1: name
    const auto& rankNames = gh->getGuildRankNames();
    lua_pushstring(L, m.rankIndex < rankNames.size()
        ? rankNames[m.rankIndex].c_str() : "");              // 2: rank name
    lua_pushnumber(L, m.rankIndex);                          // 3: rankIndex
    lua_pushnumber(L, m.level);                              // 4: level
    static const char* kCls[] = {"","Warrior","Paladin","Hunter","Rogue","Priest",
        "Death Knight","Shaman","Mage","Warlock","","Druid"};
    lua_pushstring(L, m.classId < 12 ? kCls[m.classId] : "Unknown"); // 5: class
    std::string zone;
    if (m.zoneId != 0 && m.online) zone = gh->getWhoAreaName(m.zoneId);
    lua_pushstring(L, zone.c_str());                         // 6: zone
    lua_pushstring(L, m.publicNote.c_str());                 // 7: note
    lua_pushstring(L, m.officerNote.c_str());                // 8: officerNote
    lua_pushboolean(L, m.online);                            // 9: online
    lua_pushnumber(L, 0);                                    // 10: status (0=online, 1=AFK, 2=DND)
    lua_pushnumber(L, m.classId);                            // 11: classId (numeric)
    return 11;
}

// GetGuildRosterMOTD() → motd
static int lua_GetGuildRosterMOTD(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, ""); return 1; }
    lua_pushstring(L, gh->getGuildRoster().motd.c_str());
    return 1;
}

// GetNumIgnores() → count
static int lua_GetNumIgnores(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    int count = 0;
    for (const auto& c : gh->getContacts())
        if (c.isIgnored()) count++;
    lua_pushnumber(L, count);
    return 1;
}

// GetIgnoreName(index) → name
static int lua_GetIgnoreName(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    int found = 0;
    for (const auto& c : gh->getContacts()) {
        if (!c.isIgnored()) continue;
        if (++found == index) {
            lua_pushstring(L, c.name.c_str());
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// --- Talent API ---

// GetNumTalentTabs() → count (usually 3)
static int lua_GetNumTalentTabs(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    // Count tabs matching the player's class
    uint8_t classId = gh->getPlayerClass();
    uint32_t classMask = (classId > 0) ? (1u << (classId - 1)) : 0;
    int count = 0;
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) count++;
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetTalentTabInfo(tabIndex) → name, iconTexture, pointsSpent, background
static int lua_GetTalentTabInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIndex = static_cast<int>(luaL_checknumber(L, 1)); // 1-indexed
    if (!gh || tabIndex < 1) {
        lua_pushnil(L); return 1;
    }
    uint8_t classId = gh->getPlayerClass();
    uint32_t classMask = (classId > 0) ? (1u << (classId - 1)) : 0;
    // Find the Nth tab for this class (sorted by orderIndex)
    std::vector<const game::GameHandler::TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) classTabs.push_back(&tab);
    }
    std::sort(classTabs.begin(), classTabs.end(),
        [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (tabIndex > static_cast<int>(classTabs.size())) {
        lua_pushnil(L); return 1;
    }
    const auto* tab = classTabs[tabIndex - 1];
    // Count points spent in this tab
    int pointsSpent = 0;
    const auto& learned = gh->getLearnedTalents();
    for (const auto& [talentId, rank] : learned) {
        const auto* entry = gh->getTalentEntry(talentId);
        if (entry && entry->tabId == tab->tabId) pointsSpent += rank;
    }
    lua_pushstring(L, tab->name.c_str());              // 1: name
    lua_pushnil(L);                                     // 2: iconTexture (not resolved)
    lua_pushnumber(L, pointsSpent);                     // 3: pointsSpent
    lua_pushstring(L, tab->backgroundFile.c_str());     // 4: background
    return 4;
}

// GetNumTalents(tabIndex) → count
static int lua_GetNumTalents(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIndex = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || tabIndex < 1) { lua_pushnumber(L, 0); return 1; }
    uint8_t classId = gh->getPlayerClass();
    uint32_t classMask = (classId > 0) ? (1u << (classId - 1)) : 0;
    std::vector<const game::GameHandler::TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) classTabs.push_back(&tab);
    }
    std::sort(classTabs.begin(), classTabs.end(),
        [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (tabIndex > static_cast<int>(classTabs.size())) {
        lua_pushnumber(L, 0); return 1;
    }
    uint32_t targetTabId = classTabs[tabIndex - 1]->tabId;
    int count = 0;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (entry.tabId == targetTabId) count++;
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetTalentInfo(tabIndex, talentIndex) → name, iconTexture, tier, column, rank, maxRank, isExceptional, available
static int lua_GetTalentInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int tabIndex = static_cast<int>(luaL_checknumber(L, 1));
    int talentIndex = static_cast<int>(luaL_checknumber(L, 2));
    if (!gh || tabIndex < 1 || talentIndex < 1) {
        for (int i = 0; i < 8; i++) lua_pushnil(L);
        return 8;
    }
    uint8_t classId = gh->getPlayerClass();
    uint32_t classMask = (classId > 0) ? (1u << (classId - 1)) : 0;
    std::vector<const game::GameHandler::TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : gh->getAllTalentTabs()) {
        if (tab.classMask & classMask) classTabs.push_back(&tab);
    }
    std::sort(classTabs.begin(), classTabs.end(),
        [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (tabIndex > static_cast<int>(classTabs.size())) {
        for (int i = 0; i < 8; i++) lua_pushnil(L);
        return 8;
    }
    uint32_t targetTabId = classTabs[tabIndex - 1]->tabId;
    // Collect talents for this tab, sorted by row then column
    std::vector<const game::GameHandler::TalentEntry*> tabTalents;
    for (const auto& [talentId, entry] : gh->getAllTalents()) {
        if (entry.tabId == targetTabId) tabTalents.push_back(&entry);
    }
    std::sort(tabTalents.begin(), tabTalents.end(),
        [](const auto* a, const auto* b) {
            return (a->row != b->row) ? a->row < b->row : a->column < b->column;
        });
    if (talentIndex > static_cast<int>(tabTalents.size())) {
        for (int i = 0; i < 8; i++) lua_pushnil(L);
        return 8;
    }
    const auto* talent = tabTalents[talentIndex - 1];
    uint8_t rank = gh->getTalentRank(talent->talentId);
    // Get spell name for rank 1 spell
    std::string name = gh->getSpellName(talent->rankSpells[0]);
    if (name.empty()) name = "Talent " + std::to_string(talent->talentId);

    lua_pushstring(L, name.c_str());          // 1: name
    lua_pushnil(L);                            // 2: iconTexture
    lua_pushnumber(L, talent->row + 1);        // 3: tier (1-indexed)
    lua_pushnumber(L, talent->column + 1);     // 4: column (1-indexed)
    lua_pushnumber(L, rank);                   // 5: rank
    lua_pushnumber(L, talent->maxRank);        // 6: maxRank
    lua_pushboolean(L, 0);                     // 7: isExceptional
    lua_pushboolean(L, 1);                     // 8: available
    return 8;
}

// GetActiveTalentGroup() → 1 or 2
static int lua_GetActiveTalentGroup(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? (gh->getActiveTalentSpec() + 1) : 1);
    return 1;
}

// --- Loot API ---

// GetNumLootItems() → count
static int lua_GetNumLootItems(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isLootWindowOpen()) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, gh->getCurrentLoot().items.size());
    return 1;
}

// GetLootSlotInfo(slot) → texture, name, quantity, quality, locked
static int lua_GetLootSlotInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1)); // 1-indexed
    if (!gh || !gh->isLootWindowOpen()) {
        lua_pushnil(L); return 1;
    }
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) {
        lua_pushnil(L); return 1;
    }
    const auto& item = loot.items[slot - 1];
    const auto* info = gh->getItemInfo(item.itemId);

    // texture (icon path from ItemDisplayInfo.dbc)
    std::string icon;
    if (info && info->displayInfoId != 0) {
        icon = gh->getItemIconPath(info->displayInfoId);
    }
    if (!icon.empty()) lua_pushstring(L, icon.c_str());
    else lua_pushnil(L);

    // name
    if (info && !info->name.empty()) lua_pushstring(L, info->name.c_str());
    else lua_pushstring(L, ("Item #" + std::to_string(item.itemId)).c_str());

    lua_pushnumber(L, item.count);                           // quantity
    lua_pushnumber(L, info ? info->quality : 1);             // quality
    lua_pushboolean(L, 0);                                   // locked (not tracked)
    return 5;
}

// GetLootSlotLink(slot) → itemLink
static int lua_GetLootSlotLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || !gh->isLootWindowOpen()) { lua_pushnil(L); return 1; }
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) {
        lua_pushnil(L); return 1;
    }
    const auto& item = loot.items[slot - 1];
    const auto* info = gh->getItemInfo(item.itemId);
    if (!info || info->name.empty()) { lua_pushnil(L); return 1; }
    static const char* kQH[] = {"9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","e6cc80"};
    uint32_t qi = info->quality < 8 ? info->quality : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQH[qi], item.itemId, info->name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// LootSlot(slot) — take item from loot
static int lua_LootSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || !gh->isLootWindowOpen()) return 0;
    const auto& loot = gh->getCurrentLoot();
    if (slot < 1 || slot > static_cast<int>(loot.items.size())) return 0;
    gh->lootItem(loot.items[slot - 1].slotIndex);
    return 0;
}

// CloseLoot() — close loot window
static int lua_CloseLoot(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->closeLoot();
    return 0;
}

// GetLootMethod() → "freeforall"|"roundrobin"|"master"|"group"|"needbeforegreed", partyLoot, raidLoot
static int lua_GetLootMethod(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "freeforall"); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    const auto& pd = gh->getPartyData();
    const char* method = "freeforall";
    switch (pd.lootMethod) {
        case 0: method = "freeforall"; break;
        case 1: method = "roundrobin"; break;
        case 2: method = "master"; break;
        case 3: method = "group"; break;
        case 4: method = "needbeforegreed"; break;
    }
    lua_pushstring(L, method);
    lua_pushnumber(L, 0); // partyLootMaster (index)
    lua_pushnumber(L, 0); // raidLootMaster (index)
    return 3;
}

// --- Additional WoW API ---

static int lua_UnitAffectingCombat(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInCombat());
    } else {
        // Check UNIT_FLAG_IN_COMBAT (0x00080000) in UNIT_FIELD_FLAGS
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        bool inCombat = false;
        if (guid != 0) {
            auto entity = gh->getEntityManager().getEntity(guid);
            if (entity) {
                uint32_t flags = entity->getField(
                    game::fieldIndex(game::UF::UNIT_FIELD_FLAGS));
                inCombat = (flags & 0x00080000) != 0; // UNIT_FLAG_IN_COMBAT
            }
        }
        lua_pushboolean(L, inCombat);
    }
    return 1;
}

static int lua_GetNumRaidMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { lua_pushnumber(L, 0); return 1; }
    const auto& pd = gh->getPartyData();
    lua_pushnumber(L, (pd.groupType == 1) ? pd.memberCount : 0);
    return 1;
}

static int lua_GetNumPartyMembers(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || !gh->isInGroup()) { lua_pushnumber(L, 0); return 1; }
    const auto& pd = gh->getPartyData();
    // In party (not raid), count excludes self
    int count = (pd.groupType == 0) ? static_cast<int>(pd.memberCount) : 0;
    // memberCount includes self on some servers, subtract 1 if needed
    if (count > 0) count = std::max(0, count - 1);
    lua_pushnumber(L, count);
    return 1;
}

static int lua_UnitInParty(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr == "player") {
        lua_pushboolean(L, gh->isInGroup());
    } else {
        uint64_t guid = resolveUnitGuid(gh, uidStr);
        if (guid == 0) { lua_pushboolean(L, 0); return 1; }
        const auto& pd = gh->getPartyData();
        bool found = false;
        for (const auto& m : pd.members) {
            if (m.guid == guid) { found = true; break; }
        }
        lua_pushboolean(L, found);
    }
    return 1;
}

static int lua_UnitInRaid(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto& pd = gh->getPartyData();
    if (pd.groupType != 1) { lua_pushboolean(L, 0); return 1; }
    if (uidStr == "player") {
        lua_pushboolean(L, 1);
        return 1;
    }
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    bool found = false;
    for (const auto& m : pd.members) {
        if (m.guid == guid) { found = true; break; }
    }
    lua_pushboolean(L, found);
    return 1;
}

// GetRaidRosterInfo(index) → name, rank, subgroup, level, class, fileName, zone, online, isDead, role, isML
static int lua_GetRaidRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh || index < 1) { lua_pushnil(L); return 1; }
    const auto& pd = gh->getPartyData();
    if (index > static_cast<int>(pd.members.size())) { lua_pushnil(L); return 1; }
    const auto& m = pd.members[index - 1];
    lua_pushstring(L, m.name.c_str());       // name
    lua_pushnumber(L, m.guid == pd.leaderGuid ? 2 : (m.flags & 0x01 ? 1 : 0)); // rank (0=member, 1=assist, 2=leader)
    lua_pushnumber(L, m.subGroup + 1);       // subgroup (1-indexed)
    lua_pushnumber(L, m.level);              // level
    // Class: resolve from entity if available
    std::string className = "Unknown";
    auto entity = gh->getEntityManager().getEntity(m.guid);
    if (entity) {
        uint32_t bytes0 = entity->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        uint8_t classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
        static const char* kClasses[] = {"","Warrior","Paladin","Hunter","Rogue","Priest",
            "Death Knight","Shaman","Mage","Warlock","","Druid"};
        if (classId > 0 && classId < 12) className = kClasses[classId];
    }
    lua_pushstring(L, className.c_str());    // class (localized)
    lua_pushstring(L, className.c_str());    // fileName
    lua_pushstring(L, "");                   // zone
    lua_pushboolean(L, m.isOnline);          // online
    lua_pushboolean(L, m.curHealth == 0);    // isDead
    lua_pushstring(L, "NONE");               // role
    lua_pushboolean(L, pd.looterGuid == m.guid ? 1 : 0); // isML
    return 11;
}

// GetThreatStatusColor(statusIndex) → r, g, b
static int lua_GetThreatStatusColor(lua_State* L) {
    int status = static_cast<int>(luaL_optnumber(L, 1, 0));
    switch (status) {
        case 0: lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); lua_pushnumber(L, 0.69f); break; // gray (no threat)
        case 1: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.47f); break; // yellow (threat)
        case 2: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.6f);  lua_pushnumber(L, 0.0f);  break; // orange (high threat)
        case 3: lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 0.0f);  lua_pushnumber(L, 0.0f);  break; // red (tanking)
        default: lua_pushnumber(L, 1.0f); lua_pushnumber(L, 1.0f);  lua_pushnumber(L, 1.0f);  break;
    }
    return 3;
}

// GetReadyCheckStatus(unit) → status string
static int lua_GetReadyCheckStatus(lua_State* L) {
    (void)L;
    lua_pushnil(L); // No ready check in progress
    return 1;
}

// RegisterUnitWatch / UnregisterUnitWatch — secure unit frame stubs
static int lua_RegisterUnitWatch(lua_State* L) { (void)L; return 0; }
static int lua_UnregisterUnitWatch(lua_State* L) { (void)L; return 0; }

static int lua_UnitIsUnit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    std::string u1(uid1), u2(uid2);
    for (char& c : u1) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : u2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    lua_pushboolean(L, g1 != 0 && g1 == g2);
    return 1;
}

static int lua_UnitIsFriend(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit && !unit->isHostile());
    return 1;
}

static int lua_UnitIsEnemy(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit && unit->isHostile());
    return 1;
}

static int lua_UnitCreatureType(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "Unknown"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity) { lua_pushstring(L, "Unknown"); return 1; }
    // Player units are always "Humanoid"
    if (entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "Humanoid");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "Unknown"); return 1; }
    uint32_t ct = gh->getCreatureType(unit->getEntry());
    static const char* kTypes[] = {
        "Unknown", "Beast", "Dragonkin", "Demon", "Elemental",
        "Giant", "Undead", "Humanoid", "Critter", "Mechanical",
        "Not specified", "Totem", "Non-combat Pet", "Gas Cloud"
    };
    lua_pushstring(L, (ct < 14) ? kTypes[ct] : "Unknown");
    return 1;
}

// GetPlayerInfoByGUID(guid) → localizedClass, englishClass, localizedRace, englishRace, sex, name, realm
static int lua_GetPlayerInfoByGUID(lua_State* L) {
    auto* gh = getGameHandler(L);
    const char* guidStr = luaL_checkstring(L, 1);
    if (!gh || !guidStr) {
        for (int i = 0; i < 7; i++) lua_pushnil(L);
        return 7;
    }
    // Parse hex GUID string "0x0000000000000001"
    uint64_t guid = 0;
    if (guidStr[0] == '0' && (guidStr[1] == 'x' || guidStr[1] == 'X'))
        guid = strtoull(guidStr + 2, nullptr, 16);
    else
        guid = strtoull(guidStr, nullptr, 16);

    if (guid == 0) { for (int i = 0; i < 7; i++) lua_pushnil(L); return 7; }

    // Look up entity name
    std::string name = gh->lookupName(guid);
    if (name.empty() && guid == gh->getPlayerGuid()) {
        const auto& chars = gh->getCharacters();
        for (const auto& c : chars)
            if (c.guid == guid) { name = c.name; break; }
    }

    // For player GUID, return class/race if it's the local player
    const char* className = "Unknown";
    const char* raceName = "Unknown";
    if (guid == gh->getPlayerGuid()) {
        static const char* kClasses[] = {"","Warrior","Paladin","Hunter","Rogue","Priest",
            "Death Knight","Shaman","Mage","Warlock","","Druid"};
        static const char* kRaces[] = {"","Human","Orc","Dwarf","Night Elf","Undead",
            "Tauren","Gnome","Troll","","Blood Elf","Draenei"};
        uint8_t cid = gh->getPlayerClass();
        uint8_t rid = gh->getPlayerRace();
        if (cid < 12) className = kClasses[cid];
        if (rid < 12) raceName = kRaces[rid];
    }

    lua_pushstring(L, className);  // 1: localizedClass
    lua_pushstring(L, className);  // 2: englishClass
    lua_pushstring(L, raceName);   // 3: localizedRace
    lua_pushstring(L, raceName);   // 4: englishRace
    lua_pushnumber(L, 0);          // 5: sex (0=unknown)
    lua_pushstring(L, name.c_str()); // 6: name
    lua_pushstring(L, "");         // 7: realm
    return 7;
}

// GetItemLink(itemId) → "|cFFxxxxxx|Hitem:ID:...|h[Name]|h|r"
static int lua_GetItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    uint32_t itemId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (itemId == 0) { lua_pushnil(L); return 1; }
    const auto* info = gh->getItemInfo(itemId);
    if (!info || info->name.empty()) { lua_pushnil(L); return 1; }
    static const char* kQH[] = {"9d9d9d","ffffff","1eff00","0070dd","a335ee","ff8000","e6cc80","e6cc80"};
    uint32_t qi = info->quality < 8 ? info->quality : 1u;
    char link[256];
    snprintf(link, sizeof(link), "|cff%s|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             kQH[qi], itemId, info->name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// GetSpellLink(spellIdOrName) → "|cFFxxxxxx|Hspell:ID|h[Name]|h|r"
static int lua_GetSpellLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushnil(L); return 1; }
        std::string nameLow(name);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn == nameLow) { spellId = sid; break; }
        }
    }
    if (spellId == 0) { lua_pushnil(L); return 1; }
    std::string name = gh->getSpellName(spellId);
    if (name.empty()) { lua_pushnil(L); return 1; }
    char link[256];
    snprintf(link, sizeof(link), "|cff71d5ff|Hspell:%u|h[%s]|h|r", spellId, name.c_str());
    lua_pushstring(L, link);
    return 1;
}

// IsUsableSpell(spellIdOrName) → usable, noMana
static int lua_IsUsableSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }

    uint32_t spellId = 0;
    if (lua_isnumber(L, 1)) {
        spellId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char* name = lua_tostring(L, 1);
        if (!name || !*name) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
        std::string nameLow(name);
        for (char& c : nameLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (uint32_t sid : gh->getKnownSpells()) {
            std::string sn = gh->getSpellName(sid);
            for (char& c : sn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (sn == nameLow) { spellId = sid; break; }
        }
    }

    if (spellId == 0 || !gh->getKnownSpells().count(spellId)) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }

    // Check if on cooldown
    float cd = gh->getSpellCooldown(spellId);
    bool onCooldown = (cd > 0.1f);

    // Check mana/power cost
    bool noMana = false;
    if (!onCooldown) {
        auto spellData = gh->getSpellData(spellId);
        if (spellData.manaCost > 0) {
            auto playerEntity = gh->getEntityManager().getEntity(gh->getPlayerGuid());
            if (playerEntity) {
                auto* unit = dynamic_cast<game::Unit*>(playerEntity.get());
                if (unit && unit->getPower() < spellData.manaCost) {
                    noMana = true;
                }
            }
        }
    }
    lua_pushboolean(L, (onCooldown || noMana) ? 0 : 1);  // usable
    lua_pushboolean(L, noMana ? 1 : 0);                    // notEnoughMana
    return 2;
}

// IsInInstance() → isInstance, instanceType
static int lua_IsInInstance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushstring(L, "none"); return 2; }
    bool inInstance = gh->isInInstance();
    lua_pushboolean(L, inInstance);
    lua_pushstring(L, inInstance ? "party" : "none");  // simplified: "none", "party", "raid", "pvp", "arena"
    return 2;
}

// GetInstanceInfo() → name, type, difficultyIndex, difficultyName, maxPlayers, ...
static int lua_GetInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) {
        lua_pushstring(L, ""); lua_pushstring(L, "none"); lua_pushnumber(L, 0);
        lua_pushstring(L, "Normal"); lua_pushnumber(L, 0);
        return 5;
    }
    std::string mapName = gh->getMapName(gh->getCurrentMapId());
    lua_pushstring(L, mapName.c_str());                    // 1: name
    lua_pushstring(L, gh->isInInstance() ? "party" : "none"); // 2: instanceType
    lua_pushnumber(L, gh->getInstanceDifficulty());        // 3: difficultyIndex
    static const char* kDiff[] = {"Normal", "Heroic", "25 Normal", "25 Heroic"};
    uint32_t diff = gh->getInstanceDifficulty();
    lua_pushstring(L, (diff < 4) ? kDiff[diff] : "Normal"); // 4: difficultyName
    lua_pushnumber(L, 5);                                   // 5: maxPlayers (default 5-man)
    return 5;
}

// GetInstanceDifficulty() → difficulty (1=normal, 2=heroic, 3=25normal, 4=25heroic)
static int lua_GetInstanceDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? (gh->getInstanceDifficulty() + 1) : 1);  // WoW returns 1-based
    return 1;
}

// UnitClassification(unit) → "normal", "elite", "rareelite", "worldboss", "rare"
static int lua_UnitClassification(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "normal"); return 1; }
    const char* uid = luaL_optstring(L, 1, "target");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushstring(L, "normal"); return 1; }
    auto entity = gh->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() == game::ObjectType::PLAYER) {
        lua_pushstring(L, "normal");
        return 1;
    }
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    if (!unit) { lua_pushstring(L, "normal"); return 1; }
    int rank = gh->getCreatureRank(unit->getEntry());
    switch (rank) {
        case 1:  lua_pushstring(L, "elite"); break;
        case 2:  lua_pushstring(L, "rareelite"); break;
        case 3:  lua_pushstring(L, "worldboss"); break;
        case 4:  lua_pushstring(L, "rare"); break;
        default: lua_pushstring(L, "normal"); break;
    }
    return 1;
}

// GetComboPoints("player"|"vehicle", "target") → number
static int lua_GetComboPoints(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getComboPoints() : 0);
    return 1;
}

// UnitReaction(unit, otherUnit) → 1-8 (hostile to exalted)
// Simplified: hostile=2, neutral=4, friendly=5
static int lua_UnitReaction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    const char* uid1 = luaL_checkstring(L, 1);
    const char* uid2 = luaL_checkstring(L, 2);
    auto* unit2 = resolveUnit(L, uid2);
    if (!unit2) { lua_pushnil(L); return 1; }
    // If unit2 is the player, always friendly to self
    std::string u1(uid1);
    for (char& c : u1) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string u2(uid2);
    for (char& c : u2) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t g1 = resolveUnitGuid(gh, u1);
    uint64_t g2 = resolveUnitGuid(gh, u2);
    if (g1 == g2) { lua_pushnumber(L, 5); return 1; } // same unit = friendly
    if (unit2->isHostile()) {
        lua_pushnumber(L, 2); // hostile
    } else {
        lua_pushnumber(L, 5); // friendly
    }
    return 1;
}

// UnitIsConnected(unit) → boolean
static int lua_UnitIsConnected(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint64_t guid = resolveUnitGuid(gh, uidStr);
    if (guid == 0) { lua_pushboolean(L, 0); return 1; }
    // Player is always connected
    if (guid == gh->getPlayerGuid()) { lua_pushboolean(L, 1); return 1; }
    // Check party/raid member online status
    const auto& pd = gh->getPartyData();
    for (const auto& m : pd.members) {
        if (m.guid == guid) {
            lua_pushboolean(L, m.isOnline ? 1 : 0);
            return 1;
        }
    }
    // Non-party entities that exist are considered connected
    auto entity = gh->getEntityManager().getEntity(guid);
    lua_pushboolean(L, entity ? 1 : 0);
    return 1;
}

// HasAction(slot) → boolean (1-indexed slot)
static int lua_HasAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); return 1; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1; // WoW uses 1-indexed slots
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, !bar[slot].isEmpty());
    return 1;
}

// GetActionTexture(slot) → texturePath or nil
static int lua_GetActionTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::SPELL) {
        std::string icon = gh->getSpellIconPath(action.id);
        if (!icon.empty()) {
            lua_pushstring(L, icon.c_str());
            return 1;
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        const auto* info = gh->getItemInfo(action.id);
        if (info && info->displayInfoId != 0) {
            std::string icon = gh->getItemIconPath(info->displayInfoId);
            if (!icon.empty()) {
                lua_pushstring(L, icon.c_str());
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// IsCurrentAction(slot) → boolean
static int lua_IsCurrentAction(lua_State* L) {
    // Currently no "active action" tracking; return false
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

// IsUsableAction(slot) → usable, notEnoughMana
static int lua_IsUsableAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushboolean(L, 0); return 2; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushboolean(L, 0);
        lua_pushboolean(L, 0);
        return 2;
    }
    const auto& action = bar[slot];
    bool usable = action.isReady();
    bool noMana = false;
    if (action.type == game::ActionBarSlot::SPELL) {
        usable = usable && gh->getKnownSpells().count(action.id);
        // Check power cost
        if (usable && action.id != 0) {
            auto spellData = gh->getSpellData(action.id);
            if (spellData.manaCost > 0) {
                auto pe = gh->getEntityManager().getEntity(gh->getPlayerGuid());
                if (pe) {
                    auto* unit = dynamic_cast<game::Unit*>(pe.get());
                    if (unit && unit->getPower() < spellData.manaCost) {
                        noMana = true;
                        usable = false;
                    }
                }
            }
        }
    }
    lua_pushboolean(L, usable ? 1 : 0);
    lua_pushboolean(L, noMana ? 1 : 0);
    return 2;
}

// IsActionInRange(slot) → 1 if in range, 0 if out, nil if no range check applicable
static int lua_IsActionInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); return 1; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    uint32_t spellId = 0;
    if (action.type == game::ActionBarSlot::SPELL) {
        spellId = action.id;
    } else {
        // Items/macros: no range check for now
        lua_pushnil(L);
        return 1;
    }
    if (spellId == 0) { lua_pushnil(L); return 1; }

    auto data = gh->getSpellData(spellId);
    if (data.maxRange <= 0.0f) {
        // Melee or self-cast spells: no range indicator
        lua_pushnil(L);
        return 1;
    }

    // Need a target to check range against
    uint64_t targetGuid = gh->getTargetGuid();
    if (targetGuid == 0) { lua_pushnil(L); return 1; }
    auto targetEnt = gh->getEntityManager().getEntity(targetGuid);
    auto playerEnt = gh->getEntityManager().getEntity(gh->getPlayerGuid());
    if (!targetEnt || !playerEnt) { lua_pushnil(L); return 1; }

    float dx = playerEnt->getX() - targetEnt->getX();
    float dy = playerEnt->getY() - targetEnt->getY();
    float dz = playerEnt->getZ() - targetEnt->getZ();
    float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    lua_pushnumber(L, dist <= data.maxRange ? 1 : 0);
    return 1;
}

// GetActionInfo(slot) → actionType, id, subType
static int lua_GetActionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        return 0;
    }
    const auto& action = bar[slot];
    switch (action.type) {
        case game::ActionBarSlot::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "spell");
            return 3;
        case game::ActionBarSlot::ITEM:
            lua_pushstring(L, "item");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "item");
            return 3;
        case game::ActionBarSlot::MACRO:
            lua_pushstring(L, "macro");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "macro");
            return 3;
        default:
            return 0;
    }
}

// GetActionCount(slot) → count (item stack count or 0)
static int lua_GetActionCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        // Count items across backpack + bags
        uint32_t count = 0;
        const auto& inv = gh->getInventory();
        for (int i = 0; i < inv.getBackpackSize(); ++i) {
            const auto& s = inv.getBackpackSlot(i);
            if (!s.empty() && s.item.itemId == action.id)
                count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
        }
        for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
            int bagSize = inv.getBagSize(b);
            for (int i = 0; i < bagSize; ++i) {
                const auto& s = inv.getBagSlot(b, i);
                if (!s.empty() && s.item.itemId == action.id)
                    count += (s.item.stackCount > 0 ? s.item.stackCount : 1);
            }
        }
        lua_pushnumber(L, count);
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetActionCooldown(slot) → start, duration, enable
static int lua_GetActionCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
        return 3;
    }
    const auto& action = bar[slot];
    if (action.cooldownRemaining > 0.0f) {
        // WoW returns GetTime()-based start time; approximate
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) {
            lua_call(L, 0, 1);
            now = lua_tonumber(L, -1);
            lua_pop(L, 1);
        } else {
            lua_pop(L, 1);
        }
        double start = now - (action.cooldownTotal - action.cooldownRemaining);
        lua_pushnumber(L, start);
        lua_pushnumber(L, action.cooldownTotal);
        lua_pushnumber(L, 1);
    } else if (action.type == game::ActionBarSlot::SPELL && gh->isGCDActive()) {
        // No individual cooldown but GCD is active — show GCD sweep
        float gcdRem = gh->getGCDRemaining();
        float gcdTotal = gh->getGCDTotal();
        double now = 0;
        lua_getglobal(L, "GetTime");
        if (lua_isfunction(L, -1)) { lua_call(L, 0, 1); now = lua_tonumber(L, -1); lua_pop(L, 1); }
        else lua_pop(L, 1);
        double elapsed = gcdTotal - gcdRem;
        lua_pushnumber(L, now - elapsed);
        lua_pushnumber(L, gcdTotal);
        lua_pushnumber(L, 1);
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
    }
    return 3;
}

// UseAction(slot, checkCursor, onSelf) — activate action bar slot (1-indexed)
static int lua_UseAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) return 0;
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::SPELL && action.isReady()) {
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        gh->castSpell(action.id, target);
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        gh->useItemById(action.id);
    }
    // Macro execution requires GameScreen context; not available from pure Lua API
    return 0;
}

// CancelUnitBuff(unit, index) — cancel a buff by index (1-indexed)
static int lua_CancelUnitBuff(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const char* uid = luaL_optstring(L, 1, "player");
    std::string uidStr(uid);
    for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uidStr != "player") return 0; // Can only cancel own buffs
    int index = static_cast<int>(luaL_checknumber(L, 2));
    const auto& auras = gh->getPlayerAuras();
    // Find the Nth buff (non-debuff)
    int buffCount = 0;
    for (const auto& a : auras) {
        if (a.isEmpty()) continue;
        if ((a.flags & 0x80) != 0) continue; // skip debuffs
        if (++buffCount == index) {
            gh->cancelAura(a.spellId);
            break;
        }
    }
    return 0;
}

// CastSpellByID(spellId) — cast spell by numeric ID
static int lua_CastSpellByID(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    if (spellId == 0) return 0;
    uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
    gh->castSpell(spellId, target);
    return 0;
}

// --- Cursor / Drag-Drop System ---
// Tracks what the player is "holding" on the cursor (spell, item, action).

enum class CursorType { NONE, SPELL, ITEM, ACTION };
static CursorType s_cursorType = CursorType::NONE;
static uint32_t   s_cursorId   = 0;    // spellId, itemId, or action slot
static int        s_cursorSlot = 0;    // source slot for placement
static int        s_cursorBag  = -1;   // source bag for container items

static int lua_ClearCursor(lua_State* L) {
    (void)L;
    s_cursorType = CursorType::NONE;
    s_cursorId = 0;
    s_cursorSlot = 0;
    s_cursorBag = -1;
    return 0;
}

static int lua_GetCursorInfo(lua_State* L) {
    switch (s_cursorType) {
        case CursorType::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, 0);          // bookSlotIndex
            lua_pushstring(L, "spell");    // bookType
            lua_pushnumber(L, s_cursorId); // spellId
            return 4;
        case CursorType::ITEM:
            lua_pushstring(L, "item");
            lua_pushnumber(L, s_cursorId);
            return 2;
        case CursorType::ACTION:
            lua_pushstring(L, "action");
            lua_pushnumber(L, s_cursorSlot);
            return 2;
        default:
            return 0;
    }
}

static int lua_CursorHasItem(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::ITEM ? 1 : 0);
    return 1;
}

static int lua_CursorHasSpell(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::SPELL ? 1 : 0);
    return 1;
}

// PickupAction(slot) — picks up an action from the action bar
static int lua_PickupAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto& bar = gh->getActionBar();
    if (slot < 1 || slot > static_cast<int>(bar.size())) return 0;
    const auto& action = bar[slot - 1];
    if (action.isEmpty()) {
        // Empty slot — if cursor has something, place it
        if (s_cursorType == CursorType::SPELL && s_cursorId != 0) {
            gh->setActionBarSlot(slot - 1, game::ActionBarSlot::SPELL, s_cursorId);
            s_cursorType = CursorType::NONE;
            s_cursorId = 0;
        }
    } else {
        // Pick up existing action
        s_cursorType = (action.type == game::ActionBarSlot::SPELL) ? CursorType::SPELL :
                       (action.type == game::ActionBarSlot::ITEM)  ? CursorType::ITEM :
                       CursorType::ACTION;
        s_cursorId = action.id;
        s_cursorSlot = slot;
    }
    return 0;
}

// PlaceAction(slot) — places cursor content into an action bar slot
static int lua_PlaceAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (slot < 1 || slot > static_cast<int>(gh->getActionBar().size())) return 0;
    if (s_cursorType == CursorType::SPELL && s_cursorId != 0) {
        gh->setActionBarSlot(slot - 1, game::ActionBarSlot::SPELL, s_cursorId);
    } else if (s_cursorType == CursorType::ITEM && s_cursorId != 0) {
        gh->setActionBarSlot(slot - 1, game::ActionBarSlot::ITEM, s_cursorId);
    }
    s_cursorType = CursorType::NONE;
    s_cursorId = 0;
    return 0;
}

// PickupSpell(bookSlot, bookType) — picks up a spell from the spellbook
static int lua_PickupSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    const auto& tabs = gh->getSpellBookTabs();
    int idx = slot;
    for (const auto& tab : tabs) {
        if (idx <= static_cast<int>(tab.spellIds.size())) {
            s_cursorType = CursorType::SPELL;
            s_cursorId = tab.spellIds[idx - 1];
            return 0;
        }
        idx -= static_cast<int>(tab.spellIds.size());
    }
    return 0;
}

// PickupSpellBookItem(bookSlot, bookType) — alias for PickupSpell
static int lua_PickupSpellBookItem(lua_State* L) {
    return lua_PickupSpell(L);
}

// PickupContainerItem(bag, slot) — picks up an item from a bag
static int lua_PickupContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));
    const auto& inv = gh->getInventory();
    const game::ItemSlot* itemSlot = nullptr;
    if (bag == 0 && slot >= 1 && slot <= inv.getBackpackSize()) {
        itemSlot = &inv.getBackpackSlot(slot - 1);
    } else if (bag >= 1 && bag <= 4) {
        int bagSize = inv.getBagSize(bag - 1);
        if (slot >= 1 && slot <= bagSize) {
            itemSlot = &inv.getBagSlot(bag - 1, slot - 1);
        }
    }
    if (itemSlot && !itemSlot->empty()) {
        s_cursorType = CursorType::ITEM;
        s_cursorId = itemSlot->item.itemId;
        s_cursorBag = bag;
        s_cursorSlot = slot;
    }
    return 0;
}

// PickupInventoryItem(slot) — picks up an equipped item
static int lua_PickupInventoryItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    if (slot < 1 || slot > 19) return 0;
    const auto& inv = gh->getInventory();
    const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
    if (!eq.empty()) {
        s_cursorType = CursorType::ITEM;
        s_cursorId = eq.item.itemId;
        s_cursorSlot = slot;
        s_cursorBag = -1;
    }
    return 0;
}

// DeleteCursorItem() — destroys the item on cursor
static int lua_DeleteCursorItem(lua_State* L) {
    (void)L;
    s_cursorType = CursorType::NONE;
    s_cursorId = 0;
    return 0;
}

// AutoEquipCursorItem() — equip item from cursor
static int lua_AutoEquipCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && s_cursorType == CursorType::ITEM && s_cursorId != 0) {
        gh->useItemById(s_cursorId);
    }
    s_cursorType = CursorType::NONE;
    s_cursorId = 0;
    return 0;
}

// --- Frame System ---
// Minimal WoW-compatible frame objects with RegisterEvent/SetScript/GetScript.
// Frames are Lua tables with a metatable that provides methods.

// Frame method: frame:RegisterEvent("EVENT")
static int lua_Frame_RegisterEvent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self
    const char* eventName = luaL_checkstring(L, 2);

    // Get frame's registered events table (create if needed)
    lua_getfield(L, 1, "__events");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, 1, "__events");
    }
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, eventName);
    lua_pop(L, 1);

    // Also register in global __WoweeFrameEvents for dispatch
    lua_getglobal(L, "__WoweeFrameEvents");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeFrameEvents");
    }
    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, eventName);
    }
    // Append frame reference
    int len = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, 1);  // push frame
    lua_rawseti(L, -2, len + 1);
    lua_pop(L, 2);  // pop list + __WoweeFrameEvents
    return 0;
}

// Frame method: frame:UnregisterEvent("EVENT")
static int lua_Frame_UnregisterEvent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* eventName = luaL_checkstring(L, 2);

    // Remove from frame's own events
    lua_getfield(L, 1, "__events");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, eventName);
    }
    lua_pop(L, 1);
    return 0;
}

// Frame method: frame:SetScript("handler", func)
static int lua_Frame_SetScript(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* scriptType = luaL_checkstring(L, 2);
    // arg 3 can be function or nil
    lua_getfield(L, 1, "__scripts");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, 1, "__scripts");
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, scriptType);
    lua_pop(L, 1);

    // Track frames with OnUpdate in __WoweeOnUpdateFrames
    if (strcmp(scriptType, "OnUpdate") == 0) {
        lua_getglobal(L, "__WoweeOnUpdateFrames");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        if (lua_isfunction(L, 3)) {
            // Add frame to the list
            int len = static_cast<int>(lua_objlen(L, -1));
            lua_pushvalue(L, 1);
            lua_rawseti(L, -2, len + 1);
        }
        lua_pop(L, 1);
    }
    return 0;
}

// Frame method: frame:GetScript("handler")
static int lua_Frame_GetScript(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* scriptType = luaL_checkstring(L, 2);
    lua_getfield(L, 1, "__scripts");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, scriptType);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// Frame method: frame:GetName()
static int lua_Frame_GetName(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__name");
    return 1;
}

// Frame method: frame:Show() / frame:Hide() / frame:IsShown() / frame:IsVisible()
static int lua_Frame_Show(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    lua_setfield(L, 1, "__visible");
    return 0;
}
static int lua_Frame_Hide(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 0);
    lua_setfield(L, 1, "__visible");
    return 0;
}
static int lua_Frame_IsShown(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__visible");
    lua_pushboolean(L, lua_toboolean(L, -1));
    return 1;
}

// Frame method: frame:CreateTexture(name, layer) → texture stub
static int lua_Frame_CreateTexture(lua_State* L) {
    lua_newtable(L);
    // Add noop methods for common texture operations
    luaL_dostring(L,
        "return function(t) "
        "function t:SetTexture() end "
        "function t:SetTexCoord() end "
        "function t:SetVertexColor() end "
        "function t:SetAllPoints() end "
        "function t:SetPoint() end "
        "function t:SetSize() end "
        "function t:SetWidth() end "
        "function t:SetHeight() end "
        "function t:Show() end "
        "function t:Hide() end "
        "function t:SetAlpha() end "
        "function t:GetTexture() return '' end "
        "function t:SetDesaturated() end "
        "function t:SetBlendMode() end "
        "function t:SetDrawLayer() end "
        "end");
    lua_pushvalue(L, -2); // push the table
    lua_call(L, 1, 0);    // call the function with the table
    return 1;
}

// Frame method: frame:CreateFontString(name, layer, template) → fontstring stub
static int lua_Frame_CreateFontString(lua_State* L) {
    lua_newtable(L);
    luaL_dostring(L,
        "return function(fs) "
        "fs._text = '' "
        "function fs:SetText(t) self._text = t or '' end "
        "function fs:GetText() return self._text end "
        "function fs:SetFont() end "
        "function fs:SetFontObject() end "
        "function fs:SetTextColor() end "
        "function fs:SetJustifyH() end "
        "function fs:SetJustifyV() end "
        "function fs:SetPoint() end "
        "function fs:SetAllPoints() end "
        "function fs:Show() end "
        "function fs:Hide() end "
        "function fs:SetAlpha() end "
        "function fs:GetStringWidth() return 0 end "
        "function fs:GetStringHeight() return 0 end "
        "function fs:SetWordWrap() end "
        "function fs:SetNonSpaceWrap() end "
        "function fs:SetMaxLines() end "
        "function fs:SetShadowOffset() end "
        "function fs:SetShadowColor() end "
        "function fs:SetWidth() end "
        "function fs:SetHeight() end "
        "end");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 0);
    return 1;
}

// GetFramerate() → fps
static int lua_GetFramerate(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(ImGui::GetIO().Framerate));
    return 1;
}

// GetCursorPosition() → x, y (screen coordinates, origin top-left)
static int lua_GetCursorPosition(lua_State* L) {
    const auto& io = ImGui::GetIO();
    lua_pushnumber(L, io.MousePos.x);
    lua_pushnumber(L, io.MousePos.y);
    return 2;
}

// GetScreenWidth() → width
static int lua_GetScreenWidth(lua_State* L) {
    auto* window = core::Application::getInstance().getWindow();
    lua_pushnumber(L, window ? window->getWidth() : 1920);
    return 1;
}

// GetScreenHeight() → height
static int lua_GetScreenHeight(lua_State* L) {
    auto* window = core::Application::getInstance().getWindow();
    lua_pushnumber(L, window ? window->getHeight() : 1080);
    return 1;
}

// Modifier key state queries using ImGui IO
static int lua_IsShiftKeyDown(lua_State* L) {
    lua_pushboolean(L, ImGui::GetIO().KeyShift ? 1 : 0);
    return 1;
}
static int lua_IsControlKeyDown(lua_State* L) {
    lua_pushboolean(L, ImGui::GetIO().KeyCtrl ? 1 : 0);
    return 1;
}
static int lua_IsAltKeyDown(lua_State* L) {
    lua_pushboolean(L, ImGui::GetIO().KeyAlt ? 1 : 0);
    return 1;
}

// IsModifiedClick(action) → boolean
// Checks if a modifier key combo matches a named click action.
// Common actions: "CHATLINK" (shift-click), "DRESSUP" (ctrl-click),
//                 "SPLITSTACK" (shift-click), "SELFCAST" (alt-click)
static int lua_IsModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto& io = ImGui::GetIO();
    bool result = false;
    if (act == "CHATLINK" || act == "SPLITSTACK")
        result = io.KeyShift;
    else if (act == "DRESSUP" || act == "COMPAREITEMS")
        result = io.KeyCtrl;
    else if (act == "SELFCAST" || act == "FOCUSCAST")
        result = io.KeyAlt;
    else if (act == "STICKYCAMERA")
        result = io.KeyCtrl;
    else
        result = io.KeyShift; // Default: shift for unknown actions
    lua_pushboolean(L, result ? 1 : 0);
    return 1;
}

// GetModifiedClick(action) → key name ("SHIFT", "CTRL", "ALT", "NONE")
static int lua_GetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (act == "CHATLINK" || act == "SPLITSTACK")
        lua_pushstring(L, "SHIFT");
    else if (act == "DRESSUP" || act == "COMPAREITEMS")
        lua_pushstring(L, "CTRL");
    else if (act == "SELFCAST" || act == "FOCUSCAST")
        lua_pushstring(L, "ALT");
    else
        lua_pushstring(L, "SHIFT");
    return 1;
}
static int lua_SetModifiedClick(lua_State* L) { (void)L; return 0; }

// Frame methods: SetPoint, SetSize, SetWidth, SetHeight, GetWidth, GetHeight, GetCenter, SetAlpha, GetAlpha
static int lua_Frame_SetPoint(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* point = luaL_optstring(L, 2, "CENTER");
    // Store point info in frame table
    lua_pushstring(L, point);
    lua_setfield(L, 1, "__point");
    // Optional x/y offsets (args 4,5 if relativeTo is given, or 3,4 if not)
    double xOfs = 0, yOfs = 0;
    if (lua_isnumber(L, 4)) { xOfs = lua_tonumber(L, 4); yOfs = lua_tonumber(L, 5); }
    else if (lua_isnumber(L, 3)) { xOfs = lua_tonumber(L, 3); yOfs = lua_tonumber(L, 4); }
    lua_pushnumber(L, xOfs);
    lua_setfield(L, 1, "__xOfs");
    lua_pushnumber(L, yOfs);
    lua_setfield(L, 1, "__yOfs");
    return 0;
}

static int lua_Frame_SetSize(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    double w = luaL_optnumber(L, 2, 0);
    double h = luaL_optnumber(L, 3, 0);
    lua_pushnumber(L, w);
    lua_setfield(L, 1, "__width");
    lua_pushnumber(L, h);
    lua_setfield(L, 1, "__height");
    return 0;
}

static int lua_Frame_SetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnumber(L, luaL_checknumber(L, 2));
    lua_setfield(L, 1, "__width");
    return 0;
}

static int lua_Frame_SetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnumber(L, luaL_checknumber(L, 2));
    lua_setfield(L, 1, "__height");
    return 0;
}

static int lua_Frame_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__width");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 0); }
    return 1;
}

static int lua_Frame_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__height");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 0); }
    return 1;
}

static int lua_Frame_GetCenter(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__xOfs");
    double x = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "__yOfs");
    double y = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0;
    lua_pop(L, 1);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

static int lua_Frame_SetAlpha(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnumber(L, luaL_checknumber(L, 2));
    lua_setfield(L, 1, "__alpha");
    return 0;
}

static int lua_Frame_GetAlpha(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__alpha");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, 1.0); }
    return 1;
}

static int lua_Frame_SetParent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_istable(L, 2) || lua_isnil(L, 2)) {
        lua_pushvalue(L, 2);
        lua_setfield(L, 1, "__parent");
    }
    return 0;
}

static int lua_Frame_GetParent(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__parent");
    return 1;
}

// CreateFrame(frameType, name, parent, template)
static int lua_CreateFrame(lua_State* L) {
    const char* frameType = luaL_optstring(L, 1, "Frame");
    const char* name = luaL_optstring(L, 2, nullptr);
    (void)frameType; // All frame types use the same table structure for now

    // Create the frame table
    lua_newtable(L);

    // Set frame name
    if (name && *name) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "__name");
        // Also set as a global so other addons can find it by name
        lua_pushvalue(L, -1);
        lua_setglobal(L, name);
    }

    // Set initial visibility
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__visible");

    // Apply frame metatable with methods
    lua_getglobal(L, "__WoweeFrameMT");
    lua_setmetatable(L, -2);

    return 1;
}

// --- WoW Utility Functions ---

// strsplit(delimiter, str) — WoW's string split
static int lua_strsplit(lua_State* L) {
    const char* delim = luaL_checkstring(L, 1);
    const char* str = luaL_checkstring(L, 2);
    if (!delim[0]) { lua_pushstring(L, str); return 1; }
    int count = 0;
    std::string s(str);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t found = s.find(delim[0], pos);
        if (found == std::string::npos) {
            lua_pushstring(L, s.substr(pos).c_str());
            count++;
            break;
        }
        lua_pushstring(L, s.substr(pos, found - pos).c_str());
        count++;
        pos = found + 1;
    }
    return count;
}

// strtrim(str) — remove leading/trailing whitespace
static int lua_strtrim(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    std::string s(str);
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    lua_pushstring(L, (start == std::string::npos) ? "" : s.substr(start, end - start + 1).c_str());
    return 1;
}

// wipe(table) — clear all entries from a table
static int lua_wipe(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Remove all integer keys
    int len = static_cast<int>(lua_objlen(L, 1));
    for (int i = len; i >= 1; i--) {
        lua_pushnil(L);
        lua_rawseti(L, 1, i);
    }
    // Remove all string keys
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);       // pop value
        lua_pushvalue(L, -1); // copy key
        lua_pushnil(L);
        lua_rawset(L, 1);    // table[key] = nil
    }
    lua_pushvalue(L, 1);
    return 1;
}

// date(format) — safe date function (os.date was removed)
static int lua_wow_date(lua_State* L) {
    const char* fmt = luaL_optstring(L, 1, "%c");
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, tm);
    lua_pushstring(L, buf);
    return 1;
}

// time() — current unix timestamp
static int lua_wow_time(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(time(nullptr)));
    return 1;
}

// Stub for GetTime() — returns elapsed seconds
static int lua_wow_gettime(lua_State* L) {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start).count();
    lua_pushnumber(L, elapsed);
    return 1;
}

LuaEngine::LuaEngine() = default;

LuaEngine::~LuaEngine() {
    shutdown();
}

bool LuaEngine::initialize() {
    if (L_) return true;

    L_ = luaL_newstate();
    if (!L_) {
        LOG_ERROR("LuaEngine: failed to create Lua state");
        return false;
    }

    // Open safe standard libraries (no io, os, debug, package)
    luaopen_base(L_);
    luaopen_table(L_);
    luaopen_string(L_);
    luaopen_math(L_);

    // Remove unsafe globals from base library
    const char* unsafeGlobals[] = {
        "dofile", "loadfile", "load", "collectgarbage", "newproxy", nullptr
    };
    for (const char** g = unsafeGlobals; *g; ++g) {
        lua_pushnil(L_);
        lua_setglobal(L_, *g);
    }

    registerCoreAPI();
    registerEventAPI();

    LOG_INFO("LuaEngine: initialized (Lua 5.1)");
    return true;
}

void LuaEngine::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
        LOG_INFO("LuaEngine: shut down");
    }
}

void LuaEngine::setGameHandler(game::GameHandler* handler) {
    gameHandler_ = handler;
    if (L_) {
        lua_pushlightuserdata(L_, handler);
        lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_game_handler");
    }
}

void LuaEngine::registerCoreAPI() {
    // Override print() to go to chat
    lua_pushcfunction(L_, lua_wow_print);
    lua_setglobal(L_, "print");

    // WoW API stubs
    lua_pushcfunction(L_, lua_wow_message);
    lua_setglobal(L_, "message");

    lua_pushcfunction(L_, lua_wow_gettime);
    lua_setglobal(L_, "GetTime");

    // Unit API
    static const struct { const char* name; lua_CFunction func; } unitAPI[] = {
        {"UnitName",      lua_UnitName},
        {"UnitHealth",    lua_UnitHealth},
        {"UnitHealthMax", lua_UnitHealthMax},
        {"UnitPower",     lua_UnitPower},
        {"UnitPowerMax",  lua_UnitPowerMax},
        {"UnitMana",      lua_UnitPower},
        {"UnitManaMax",   lua_UnitPowerMax},
        {"UnitLevel",     lua_UnitLevel},
        {"UnitExists",    lua_UnitExists},
        {"UnitIsDead",    lua_UnitIsDead},
        {"UnitIsGhost",   lua_UnitIsGhost},
        {"UnitIsDeadOrGhost", lua_UnitIsDeadOrGhost},
        {"UnitIsAFK",     lua_UnitIsAFK},
        {"UnitIsDND",     lua_UnitIsDND},
        {"UnitPlayerControlled", lua_UnitPlayerControlled},
        {"UnitIsTapped",        lua_UnitIsTapped},
        {"UnitIsTappedByPlayer", lua_UnitIsTappedByPlayer},
        {"UnitIsTappedByAllThreatList", lua_UnitIsTappedByAllThreatList},
        {"UnitIsVisible",       lua_UnitIsVisible},
        {"UnitGroupRolesAssigned", lua_UnitGroupRolesAssigned},
        {"UnitCanAttack",       lua_UnitCanAttack},
        {"UnitCanCooperate",    lua_UnitCanCooperate},
        {"UnitCreatureFamily",  lua_UnitCreatureFamily},
        {"UnitOnTaxi",          lua_UnitOnTaxi},
        {"UnitThreatSituation", lua_UnitThreatSituation},
        {"UnitDetailedThreatSituation", lua_UnitDetailedThreatSituation},
        {"UnitSex",       lua_UnitSex},
        {"UnitClass",     lua_UnitClass},
        {"GetMoney",      lua_GetMoney},
        {"UnitStat",      lua_UnitStat},
        {"GetDodgeChance",    lua_GetDodgeChance},
        {"GetParryChance",    lua_GetParryChance},
        {"GetBlockChance",    lua_GetBlockChance},
        {"GetCritChance",     lua_GetCritChance},
        {"GetRangedCritChance", lua_GetRangedCritChance},
        {"GetSpellCritChance",  lua_GetSpellCritChance},
        {"GetCombatRating",     lua_GetCombatRating},
        {"GetSpellBonusDamage", lua_GetSpellBonusDamage},
        {"GetSpellBonusHealing", lua_GetSpellBonusHealing},
        {"GetAttackPowerForStat", lua_GetAttackPower},
        {"GetRangedAttackPower",  lua_GetRangedAttackPower},
        {"IsInGroup",     lua_IsInGroup},
        {"IsInRaid",      lua_IsInRaid},
        {"GetPlayerMapPosition", lua_GetPlayerMapPosition},
        {"GetPlayerFacing",     lua_GetPlayerFacing},
        {"GetCVar",             lua_GetCVar},
        {"SetCVar",             lua_SetCVar},
        {"IsShiftKeyDown",      lua_IsShiftKeyDown},
        {"IsControlKeyDown",    lua_IsControlKeyDown},
        {"IsAltKeyDown",        lua_IsAltKeyDown},
        {"IsModifiedClick",     lua_IsModifiedClick},
        {"GetModifiedClick",    lua_GetModifiedClick},
        {"SetModifiedClick",    lua_SetModifiedClick},
        {"SendChatMessage",   lua_SendChatMessage},
        {"SendAddonMessage",  lua_SendAddonMessage},
        {"RegisterAddonMessagePrefix", lua_RegisterAddonMessagePrefix},
        {"IsAddonMessagePrefixRegistered", lua_IsAddonMessagePrefixRegistered},
        {"CastSpellByName",   lua_CastSpellByName},
        {"IsSpellKnown",      lua_IsSpellKnown},
        {"GetNumSpellTabs",   lua_GetNumSpellTabs},
        {"GetSpellTabInfo",   lua_GetSpellTabInfo},
        {"GetSpellBookItemInfo", lua_GetSpellBookItemInfo},
        {"GetSpellBookItemName", lua_GetSpellBookItemName},
        {"GetSpellCooldown",  lua_GetSpellCooldown},
        {"GetSpellPowerCost", lua_GetSpellPowerCost},
        {"IsSpellInRange",    lua_IsSpellInRange},
        {"UnitDistanceSquared", lua_UnitDistanceSquared},
        {"CheckInteractDistance", lua_CheckInteractDistance},
        {"HasTarget",         lua_HasTarget},
        {"TargetUnit",        lua_TargetUnit},
        {"ClearTarget",       lua_ClearTarget},
        {"FocusUnit",         lua_FocusUnit},
        {"ClearFocus",        lua_ClearFocus},
        {"AssistUnit",        lua_AssistUnit},
        {"TargetLastTarget",  lua_TargetLastTarget},
        {"TargetNearestEnemy",  lua_TargetNearestEnemy},
        {"TargetNearestFriend", lua_TargetNearestFriend},
        {"GetRaidTargetIndex",  lua_GetRaidTargetIndex},
        {"SetRaidTarget",       lua_SetRaidTarget},
        {"UnitRace",          lua_UnitRace},
        {"UnitPowerType",     lua_UnitPowerType},
        {"GetNumGroupMembers", lua_GetNumGroupMembers},
        {"UnitGUID",          lua_UnitGUID},
        {"UnitIsPlayer",      lua_UnitIsPlayer},
        {"InCombatLockdown",  lua_InCombatLockdown},
        {"UnitBuff",          lua_UnitBuff},
        {"UnitDebuff",        lua_UnitDebuff},
        {"UnitAura",          lua_UnitAuraGeneric},
        {"UnitCastingInfo",   lua_UnitCastingInfo},
        {"UnitChannelInfo",   lua_UnitChannelInfo},
        {"GetNumAddOns",      lua_GetNumAddOns},
        {"GetAddOnInfo",      lua_GetAddOnInfo},
        {"GetAddOnMetadata",  lua_GetAddOnMetadata},
        {"GetSpellInfo",      lua_GetSpellInfo},
        {"GetSpellTexture",   lua_GetSpellTexture},
        {"GetItemInfo",       lua_GetItemInfo},
        {"GetItemQualityColor", lua_GetItemQualityColor},
        {"GetItemCount",      lua_GetItemCount},
        {"UseContainerItem",  lua_UseContainerItem},
        {"GetLocale",         lua_GetLocale},
        {"GetBuildInfo",      lua_GetBuildInfo},
        {"GetCurrentMapAreaID", lua_GetCurrentMapAreaID},
        {"SetMapToCurrentZone", lua_SetMapToCurrentZone},
        {"GetCurrentMapContinent", lua_GetCurrentMapContinent},
        {"GetCurrentMapZone",   lua_GetCurrentMapZone},
        {"SetMapZoom",          lua_SetMapZoom},
        {"GetMapContinents",    lua_GetMapContinents},
        {"GetMapZones",         lua_GetMapZones},
        {"GetNumMapLandmarks",  lua_GetNumMapLandmarks},
        {"GetZoneText",          lua_GetZoneText},
        {"GetRealZoneText",      lua_GetZoneText},
        {"GetSubZoneText",       lua_GetSubZoneText},
        {"GetMinimapZoneText",   lua_GetMinimapZoneText},
        // Player state (replaces hardcoded stubs)
        {"IsMounted",         lua_IsMounted},
        {"IsFlying",          lua_IsFlying},
        {"IsSwimming",        lua_IsSwimming},
        {"IsResting",         lua_IsResting},
        {"IsFalling",         lua_IsFalling},
        {"IsStealthed",       lua_IsStealthed},
        {"GetUnitSpeed",      lua_GetUnitSpeed},
        // Combat/group queries
        {"UnitAffectingCombat", lua_UnitAffectingCombat},
        {"GetNumRaidMembers",   lua_GetNumRaidMembers},
        {"GetNumPartyMembers",  lua_GetNumPartyMembers},
        {"UnitInParty",         lua_UnitInParty},
        {"UnitInRaid",          lua_UnitInRaid},
        {"GetRaidRosterInfo",   lua_GetRaidRosterInfo},
        {"GetThreatStatusColor", lua_GetThreatStatusColor},
        {"GetReadyCheckStatus", lua_GetReadyCheckStatus},
        {"RegisterUnitWatch",   lua_RegisterUnitWatch},
        {"UnregisterUnitWatch", lua_UnregisterUnitWatch},
        {"UnitIsUnit",          lua_UnitIsUnit},
        {"UnitIsFriend",        lua_UnitIsFriend},
        {"UnitIsEnemy",         lua_UnitIsEnemy},
        {"UnitCreatureType",    lua_UnitCreatureType},
        {"UnitClassification",  lua_UnitClassification},
        {"GetPlayerInfoByGUID",  lua_GetPlayerInfoByGUID},
        {"GetItemLink",          lua_GetItemLink},
        {"GetSpellLink",         lua_GetSpellLink},
        {"IsUsableSpell",        lua_IsUsableSpell},
        {"IsInInstance",         lua_IsInInstance},
        {"GetInstanceInfo",      lua_GetInstanceInfo},
        {"GetInstanceDifficulty", lua_GetInstanceDifficulty},
        // Container/bag API
        {"GetContainerNumSlots",    lua_GetContainerNumSlots},
        {"GetContainerItemInfo",    lua_GetContainerItemInfo},
        {"GetContainerItemLink",    lua_GetContainerItemLink},
        {"GetContainerNumFreeSlots", lua_GetContainerNumFreeSlots},
        // Equipment slot API
        {"GetInventorySlotInfo",    lua_GetInventorySlotInfo},
        {"GetInventoryItemLink",    lua_GetInventoryItemLink},
        {"GetInventoryItemID",      lua_GetInventoryItemID},
        {"GetInventoryItemTexture", lua_GetInventoryItemTexture},
        // Time/XP API
        {"GetGameTime",             lua_GetGameTime},
        {"GetServerTime",           lua_GetServerTime},
        {"UnitXP",                  lua_UnitXP},
        {"UnitXPMax",               lua_UnitXPMax},
        {"GetXPExhaustion",         lua_GetXPExhaustion},
        {"GetRestState",            lua_GetRestState},
        // Quest log API
        {"GetNumQuestLogEntries",   lua_GetNumQuestLogEntries},
        {"GetQuestLogTitle",        lua_GetQuestLogTitle},
        {"GetQuestLogQuestText",    lua_GetQuestLogQuestText},
        {"IsQuestComplete",         lua_IsQuestComplete},
        {"SelectQuestLogEntry",     lua_SelectQuestLogEntry},
        {"GetQuestLogSelection",    lua_GetQuestLogSelection},
        {"GetNumQuestWatches",      lua_GetNumQuestWatches},
        {"GetQuestIndexForWatch",   lua_GetQuestIndexForWatch},
        {"AddQuestWatch",           lua_AddQuestWatch},
        {"RemoveQuestWatch",        lua_RemoveQuestWatch},
        {"IsQuestWatched",          lua_IsQuestWatched},
        {"GetQuestLink",            lua_GetQuestLink},
        {"GetNumQuestLeaderBoards", lua_GetNumQuestLeaderBoards},
        {"GetQuestLogLeaderBoard",  lua_GetQuestLogLeaderBoard},
        {"ExpandQuestHeader",       lua_ExpandQuestHeader},
        {"CollapseQuestHeader",     lua_CollapseQuestHeader},
        {"GetQuestLogSpecialItemInfo", lua_GetQuestLogSpecialItemInfo},
        // Skill line API
        {"GetNumSkillLines",        lua_GetNumSkillLines},
        {"GetSkillLineInfo",        lua_GetSkillLineInfo},
        // Talent API
        {"GetNumTalentTabs",        lua_GetNumTalentTabs},
        {"GetTalentTabInfo",        lua_GetTalentTabInfo},
        {"GetNumTalents",           lua_GetNumTalents},
        {"GetTalentInfo",           lua_GetTalentInfo},
        {"GetActiveTalentGroup",    lua_GetActiveTalentGroup},
        // Friends/ignore API
        // Guild API
        {"IsInGuild",               lua_IsInGuild},
        {"GetGuildInfo",            lua_GetGuildInfoFunc},
        {"GetNumGuildMembers",      lua_GetNumGuildMembers},
        {"GetGuildRosterInfo",      lua_GetGuildRosterInfo},
        {"GetGuildRosterMOTD",      lua_GetGuildRosterMOTD},
        {"GetNumFriends",           lua_GetNumFriends},
        {"GetFriendInfo",           lua_GetFriendInfo},
        {"GetNumIgnores",           lua_GetNumIgnores},
        {"GetIgnoreName",           lua_GetIgnoreName},
        // Reaction/connection queries
        {"UnitReaction",        lua_UnitReaction},
        {"UnitIsConnected",     lua_UnitIsConnected},
        {"GetComboPoints",      lua_GetComboPoints},
        // Action bar API
        {"HasAction",           lua_HasAction},
        {"GetActionTexture",    lua_GetActionTexture},
        {"IsCurrentAction",     lua_IsCurrentAction},
        {"IsUsableAction",      lua_IsUsableAction},
        {"IsActionInRange",     lua_IsActionInRange},
        {"GetActionInfo",       lua_GetActionInfo},
        {"GetActionCount",      lua_GetActionCount},
        {"GetActionCooldown",   lua_GetActionCooldown},
        {"UseAction",           lua_UseAction},
        {"PickupAction",        lua_PickupAction},
        {"PlaceAction",         lua_PlaceAction},
        {"PickupSpell",         lua_PickupSpell},
        {"PickupSpellBookItem", lua_PickupSpellBookItem},
        {"PickupContainerItem", lua_PickupContainerItem},
        {"PickupInventoryItem", lua_PickupInventoryItem},
        {"ClearCursor",         lua_ClearCursor},
        {"GetCursorInfo",       lua_GetCursorInfo},
        {"CursorHasItem",       lua_CursorHasItem},
        {"CursorHasSpell",      lua_CursorHasSpell},
        {"DeleteCursorItem",    lua_DeleteCursorItem},
        {"AutoEquipCursorItem", lua_AutoEquipCursorItem},
        {"CancelUnitBuff",      lua_CancelUnitBuff},
        {"CastSpellByID",       lua_CastSpellByID},
        // Loot API
        {"GetNumLootItems",     lua_GetNumLootItems},
        {"GetLootSlotInfo",     lua_GetLootSlotInfo},
        {"GetLootSlotLink",     lua_GetLootSlotLink},
        {"LootSlot",            lua_LootSlot},
        {"CloseLoot",           lua_CloseLoot},
        {"GetLootMethod",       lua_GetLootMethod},
        // Utilities
        {"strsplit",          lua_strsplit},
        {"strtrim",           lua_strtrim},
        {"wipe",              lua_wipe},
        {"date",              lua_wow_date},
        {"time",              lua_wow_time},
    };
    for (const auto& [name, func] : unitAPI) {
        lua_pushcfunction(L_, func);
        lua_setglobal(L_, name);
    }

    // WoW aliases
    lua_getglobal(L_, "string");
    lua_getfield(L_, -1, "format");
    lua_setglobal(L_, "format");
    lua_pop(L_, 1);  // pop string table

    // tinsert/tremove aliases
    lua_getglobal(L_, "table");
    lua_getfield(L_, -1, "insert");
    lua_setglobal(L_, "tinsert");
    lua_getfield(L_, -1, "remove");
    lua_setglobal(L_, "tremove");
    lua_pop(L_, 1);  // pop table

    // SlashCmdList table — addons register slash commands here
    lua_newtable(L_);
    lua_setglobal(L_, "SlashCmdList");

    // Frame metatable with methods
    lua_newtable(L_);  // metatable
    lua_pushvalue(L_, -1);
    lua_setfield(L_, -2, "__index"); // metatable.__index = metatable

    static const struct luaL_Reg frameMethods[] = {
        {"RegisterEvent",   lua_Frame_RegisterEvent},
        {"UnregisterEvent", lua_Frame_UnregisterEvent},
        {"SetScript",       lua_Frame_SetScript},
        {"GetScript",       lua_Frame_GetScript},
        {"GetName",         lua_Frame_GetName},
        {"Show",            lua_Frame_Show},
        {"Hide",            lua_Frame_Hide},
        {"IsShown",         lua_Frame_IsShown},
        {"IsVisible",       lua_Frame_IsShown}, // alias
        {"SetPoint",        lua_Frame_SetPoint},
        {"SetSize",         lua_Frame_SetSize},
        {"SetWidth",        lua_Frame_SetWidth},
        {"SetHeight",       lua_Frame_SetHeight},
        {"GetWidth",        lua_Frame_GetWidth},
        {"GetHeight",       lua_Frame_GetHeight},
        {"GetCenter",       lua_Frame_GetCenter},
        {"SetAlpha",        lua_Frame_SetAlpha},
        {"GetAlpha",        lua_Frame_GetAlpha},
        {"SetParent",       lua_Frame_SetParent},
        {"GetParent",       lua_Frame_GetParent},
        {"CreateTexture",   lua_Frame_CreateTexture},
        {"CreateFontString", lua_Frame_CreateFontString},
        {nullptr, nullptr}
    };
    for (const luaL_Reg* r = frameMethods; r->name; r++) {
        lua_pushcfunction(L_, r->func);
        lua_setfield(L_, -2, r->name);
    }
    lua_setglobal(L_, "__WoweeFrameMT");

    // Add commonly called no-op frame methods to prevent addon errors
    luaL_dostring(L_,
        "local mt = __WoweeFrameMT\n"
        "function mt:SetFrameLevel(level) self.__frameLevel = level end\n"
        "function mt:GetFrameLevel() return self.__frameLevel or 1 end\n"
        "function mt:SetFrameStrata(strata) self.__strata = strata end\n"
        "function mt:GetFrameStrata() return self.__strata or 'MEDIUM' end\n"
        "function mt:EnableMouse(enable) end\n"
        "function mt:EnableMouseWheel(enable) end\n"
        "function mt:SetMovable(movable) end\n"
        "function mt:SetResizable(resizable) end\n"
        "function mt:RegisterForDrag(...) end\n"
        "function mt:SetClampedToScreen(clamped) end\n"
        "function mt:SetBackdrop(backdrop) end\n"
        "function mt:SetBackdropColor(...) end\n"
        "function mt:SetBackdropBorderColor(...) end\n"
        "function mt:ClearAllPoints() end\n"
        "function mt:SetID(id) self.__id = id end\n"
        "function mt:GetID() return self.__id or 0 end\n"
        "function mt:SetScale(scale) self.__scale = scale end\n"
        "function mt:GetScale() return self.__scale or 1.0 end\n"
        "function mt:GetEffectiveScale() return self.__scale or 1.0 end\n"
        "function mt:SetToplevel(top) end\n"
        "function mt:Raise() end\n"
        "function mt:Lower() end\n"
        "function mt:GetLeft() return 0 end\n"
        "function mt:GetRight() return 0 end\n"
        "function mt:GetTop() return 0 end\n"
        "function mt:GetBottom() return 0 end\n"
        "function mt:GetNumPoints() return 0 end\n"
        "function mt:GetPoint(n) return 'CENTER', nil, 'CENTER', 0, 0 end\n"
        "function mt:SetHitRectInsets(...) end\n"
        "function mt:RegisterForClicks(...) end\n"
        "function mt:SetAttribute(name, value) self['attr_'..name] = value end\n"
        "function mt:GetAttribute(name) return self['attr_'..name] end\n"
        "function mt:HookScript(scriptType, fn)\n"
        "    local orig = self.__scripts and self.__scripts[scriptType]\n"
        "    if orig then\n"
        "        self:SetScript(scriptType, function(...) orig(...); fn(...) end)\n"
        "    else\n"
        "        self:SetScript(scriptType, fn)\n"
        "    end\n"
        "end\n"
        "function mt:SetMinResize(...) end\n"
        "function mt:SetMaxResize(...) end\n"
        "function mt:StartMoving() end\n"
        "function mt:StopMovingOrSizing() end\n"
        "function mt:IsMouseOver() return false end\n"
        "function mt:GetObjectType() return 'Frame' end\n"
    );

    // CreateFrame function
    lua_pushcfunction(L_, lua_CreateFrame);
    lua_setglobal(L_, "CreateFrame");

    // Cursor/screen/FPS functions
    lua_pushcfunction(L_, lua_GetCursorPosition);
    lua_setglobal(L_, "GetCursorPosition");
    lua_pushcfunction(L_, lua_GetScreenWidth);
    lua_setglobal(L_, "GetScreenWidth");
    lua_pushcfunction(L_, lua_GetScreenHeight);
    lua_setglobal(L_, "GetScreenHeight");
    lua_pushcfunction(L_, lua_GetFramerate);
    lua_setglobal(L_, "GetFramerate");

    // Frame event dispatch table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeFrameEvents");

    // OnUpdate frame tracking table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeOnUpdateFrames");

    // C_Timer implementation via Lua (uses OnUpdate internally)
    luaL_dostring(L_,
        "C_Timer = {}\n"
        "local timers = {}\n"
        "local timerFrame = CreateFrame('Frame', '__WoweeTimerFrame')\n"
        "timerFrame:SetScript('OnUpdate', function(self, elapsed)\n"
        "    local i = 1\n"
        "    while i <= #timers do\n"
        "        timers[i].remaining = timers[i].remaining - elapsed\n"
        "        if timers[i].remaining <= 0 then\n"
        "            local cb = timers[i].callback\n"
        "            table.remove(timers, i)\n"
        "            cb()\n"
        "        else\n"
        "            i = i + 1\n"
        "        end\n"
        "    end\n"
        "    if #timers == 0 then self:Hide() end\n"
        "end)\n"
        "timerFrame:Hide()\n"
        "function C_Timer.After(seconds, callback)\n"
        "    tinsert(timers, {remaining = seconds, callback = callback})\n"
        "    timerFrame:Show()\n"
        "end\n"
        "function C_Timer.NewTicker(seconds, callback, iterations)\n"
        "    local count = 0\n"
        "    local maxIter = iterations or -1\n"
        "    local ticker = {cancelled = false}\n"
        "    local function tick()\n"
        "        if ticker.cancelled then return end\n"
        "        count = count + 1\n"
        "        callback(ticker)\n"
        "        if maxIter > 0 and count >= maxIter then return end\n"
        "        C_Timer.After(seconds, tick)\n"
        "    end\n"
        "    C_Timer.After(seconds, tick)\n"
        "    function ticker:Cancel() self.cancelled = true end\n"
        "    return ticker\n"
        "end\n"
    );

    // DEFAULT_CHAT_FRAME with AddMessage method (used by many addons)
    luaL_dostring(L_,
        "DEFAULT_CHAT_FRAME = {}\n"
        "function DEFAULT_CHAT_FRAME:AddMessage(text, r, g, b)\n"
        "    if r and g and b then\n"
        "        local hex = format('|cff%02x%02x%02x', "
        "            math.floor(r*255), math.floor(g*255), math.floor(b*255))\n"
        "        print(hex .. tostring(text) .. '|r')\n"
        "    else\n"
        "        print(tostring(text))\n"
        "    end\n"
        "end\n"
        "ChatFrame1 = DEFAULT_CHAT_FRAME\n"
    );

    // hooksecurefunc — hook a function to run additional code after it
    luaL_dostring(L_,
        "function hooksecurefunc(tblOrName, nameOrFunc, funcOrNil)\n"
        "    local tbl, name, hook\n"
        "    if type(tblOrName) == 'table' then\n"
        "        tbl, name, hook = tblOrName, nameOrFunc, funcOrNil\n"
        "    else\n"
        "        tbl, name, hook = _G, tblOrName, nameOrFunc\n"
        "    end\n"
        "    local orig = tbl[name]\n"
        "    if type(orig) ~= 'function' then return end\n"
        "    tbl[name] = function(...)\n"
        "        local r = {orig(...)}\n"
        "        hook(...)\n"
        "        return unpack(r)\n"
        "    end\n"
        "end\n"
    );

    // LibStub — universal library version management used by Ace3 and virtually all addon libs.
    // This is the standard WoW LibStub implementation that addons embed/expect globally.
    luaL_dostring(L_,
        "local LibStub = LibStub or {}\n"
        "LibStub.libs = LibStub.libs or {}\n"
        "LibStub.minors = LibStub.minors or {}\n"
        "function LibStub:NewLibrary(major, minor)\n"
        "    assert(type(major) == 'string', 'LibStub:NewLibrary: bad argument #1 (string expected)')\n"
        "    minor = assert(tonumber(minor or (type(minor) == 'string' and minor:match('(%d+)'))), 'LibStub:NewLibrary: bad argument #2 (number expected)')\n"
        "    local oldMinor = self.minors[major]\n"
        "    if oldMinor and oldMinor >= minor then return nil end\n"
        "    local lib = self.libs[major] or {}\n"
        "    self.libs[major] = lib\n"
        "    self.minors[major] = minor\n"
        "    return lib, oldMinor\n"
        "end\n"
        "function LibStub:GetLibrary(major, silent)\n"
        "    if not self.libs[major] and not silent then\n"
        "        error('Cannot find a library instance of \"' .. tostring(major) .. '\".')\n"
        "    end\n"
        "    return self.libs[major], self.minors[major]\n"
        "end\n"
        "function LibStub:IterateLibraries() return pairs(self.libs) end\n"
        "setmetatable(LibStub, { __call = LibStub.GetLibrary })\n"
        "_G['LibStub'] = LibStub\n"
    );

    // CallbackHandler-1.0 — minimal implementation for Ace3-based addons
    luaL_dostring(L_,
        "if LibStub then\n"
        "  local CBH = LibStub:NewLibrary('CallbackHandler-1.0', 7)\n"
        "  if CBH then\n"
        "    CBH.mixins = { 'RegisterCallback', 'UnregisterCallback', 'UnregisterAllCallbacks', 'Fire' }\n"
        "    function CBH:New(target, regName, unregName, unregAllName, onUsed)\n"
        "      local registry = setmetatable({}, { __index = CBH })\n"
        "      registry.callbacks = {}\n"
        "      target = target or {}\n"
        "      target[regName or 'RegisterCallback'] = function(self, event, method, ...)\n"
        "        if not registry.callbacks[event] then registry.callbacks[event] = {} end\n"
        "        local handler = type(method) == 'function' and method or self[method]\n"
        "        registry.callbacks[event][self] = handler\n"
        "      end\n"
        "      target[unregName or 'UnregisterCallback'] = function(self, event)\n"
        "        if registry.callbacks[event] then registry.callbacks[event][self] = nil end\n"
        "      end\n"
        "      target[unregAllName or 'UnregisterAllCallbacks'] = function(self)\n"
        "        for event, handlers in pairs(registry.callbacks) do handlers[self] = nil end\n"
        "      end\n"
        "      registry.Fire = function(self, event, ...)\n"
        "        if not self.callbacks[event] then return end\n"
        "        for obj, handler in pairs(self.callbacks[event]) do\n"
        "          handler(obj, event, ...)\n"
        "        end\n"
        "      end\n"
        "      return registry\n"
        "    end\n"
        "  end\n"
        "end\n"
    );

    // Noop stubs for commonly called functions that don't need implementation
    luaL_dostring(L_,
        "function SetDesaturation() end\n"
        "function SetPortraitTexture() end\n"
        "function PlaySound() end\n"
        "function PlaySoundFile() end\n"
        "function StopSound() end\n"
        "function UIParent_OnEvent() end\n"
        "UIParent = CreateFrame('Frame', 'UIParent')\n"
        "UIPanelWindows = {}\n"
        "WorldFrame = CreateFrame('Frame', 'WorldFrame')\n"
        // GameTooltip: global tooltip frame used by virtually all addons
        "GameTooltip = CreateFrame('Frame', 'GameTooltip')\n"
        "GameTooltip.__lines = {}\n"
        "function GameTooltip:SetOwner(owner, anchor) self.__owner = owner; self.__anchor = anchor end\n"
        "function GameTooltip:ClearLines() self.__lines = {} end\n"
        "function GameTooltip:AddLine(text, r, g, b, wrap) table.insert(self.__lines, {text=text or '',r=r,g=g,b=b}) end\n"
        "function GameTooltip:AddDoubleLine(l, r, lr, lg, lb, rr, rg, rb) table.insert(self.__lines, {text=(l or '')..'  '..(r or '')}) end\n"
        "function GameTooltip:SetText(text, r, g, b) self.__lines = {{text=text or '',r=r,g=g,b=b}} end\n"
        "function GameTooltip:GetItem()\n"
        "    if self.__itemId and self.__itemId > 0 then\n"
        "        local name = GetItemInfo(self.__itemId)\n"
        "        local _, itemLink = GetItemInfo(self.__itemId)\n"
        "        return name, itemLink or ('|cffffffff|Hitem:'..self.__itemId..':0|h['..tostring(name)..']|h|r')\n"
        "    end\n"
        "    return nil\n"
        "end\n"
        "function GameTooltip:GetSpell()\n"
        "    if self.__spellId and self.__spellId > 0 then\n"
        "        local name = GetSpellInfo(self.__spellId)\n"
        "        return name, nil, self.__spellId\n"
        "    end\n"
        "    return nil\n"
        "end\n"
        "function GameTooltip:GetUnit() return nil end\n"
        "function GameTooltip:NumLines() return #self.__lines end\n"
        "function GameTooltip:GetText() return self.__lines[1] and self.__lines[1].text or '' end\n"
        "function GameTooltip:SetUnitBuff(unit, index, filter)\n"
        "    self:ClearLines()\n"
        "    local name, rank, icon, count, debuffType, duration, expTime, caster, steal, consolidate, spellId = UnitBuff(unit, index, filter)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 1, 1)\n"
        "        if duration and duration > 0 then\n"
        "            self:AddLine(string.format('%.0f sec remaining', expTime - GetTime()), 1, 1, 1)\n"
        "        end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        "function GameTooltip:SetUnitDebuff(unit, index, filter)\n"
        "    self:ClearLines()\n"
        "    local name, rank, icon, count, debuffType, duration, expTime, caster, steal, consolidate, spellId = UnitDebuff(unit, index, filter)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 0, 0)\n"
        "        if debuffType then self:AddLine(debuffType, 0.5, 0.5, 0.5) end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        "function GameTooltip:SetHyperlink(link)\n"
        "    self:ClearLines()\n"
        "    if not link then return end\n"
        "    local id = link:match('item:(%d+)')\n"
        "    if id then\n"
        "        local name, _, quality = GetItemInfo(tonumber(id))\n"
        "        if name then self:SetText(name, 1, 1, 1) end\n"
        "        return\n"
        "    end\n"
        "    id = link:match('spell:(%d+)')\n"
        "    if id then\n"
        "        local name = GetSpellInfo(tonumber(id))\n"
        "        if name then self:SetText(name, 1, 1, 1) end\n"
        "    end\n"
        "end\n"
        "function GameTooltip:SetInventoryItem(unit, slot)\n"
        "    self:ClearLines()\n"
        "    if unit ~= 'player' then return false, false, 0 end\n"
        "    local link = GetInventoryItemLink(unit, slot)\n"
        "    if not link then return false, false, 0 end\n"
        "    local id = link:match('item:(%d+)')\n"
        "    if not id then return false, false, 0 end\n"
        "    local name, itemLink, quality, iLevel, reqLevel, class, subclass = GetItemInfo(tonumber(id))\n"
        "    if name then\n"
        "        local colors = {[0]={0.62,0.62,0.62},[1]={1,1,1},[2]={0.12,1,0},[3]={0,0.44,0.87},[4]={0.64,0.21,0.93},[5]={1,0.5,0},[6]={0.9,0.8,0.5}}\n"
        "        local c = colors[quality or 1] or {1,1,1}\n"
        "        self:SetText(name, c[1], c[2], c[3])\n"
        "        if class and class ~= '' then self:AddLine(class, 1, 1, 1) end\n"
        "        self.__itemId = tonumber(id)\n"
        "    end\n"
        "    return true, false, 0\n"
        "end\n"
        "function GameTooltip:SetBagItem(bag, slot)\n"
        "    self:ClearLines()\n"
        "    local tex, count, locked, quality, readable, lootable, link = GetContainerItemInfo(bag, slot)\n"
        "    if not link then return end\n"
        "    local id = link:match('item:(%d+)')\n"
        "    if not id then return end\n"
        "    local name, itemLink, q = GetItemInfo(tonumber(id))\n"
        "    if name then\n"
        "        local colors = {[0]={0.62,0.62,0.62},[1]={1,1,1},[2]={0.12,1,0},[3]={0,0.44,0.87},[4]={0.64,0.21,0.93},[5]={1,0.5,0}}\n"
        "        local c = colors[q or 1] or {1,1,1}\n"
        "        self:SetText(name, c[1], c[2], c[3])\n"
        "        if count and count > 1 then self:AddLine('Count: '..count, 1, 1, 1) end\n"
        "        self.__itemId = tonumber(id)\n"
        "    end\n"
        "end\n"
        "function GameTooltip:SetSpellByID(spellId)\n"
        "    self:ClearLines()\n"
        "    if not spellId or spellId == 0 then return end\n"
        "    local name, rank, icon = GetSpellInfo(spellId)\n"
        "    if name then\n"
        "        self:SetText(name, 1, 1, 1)\n"
        "        if rank and rank ~= '' then self:AddLine(rank, 0.5, 0.5, 0.5) end\n"
        "        self.__spellId = spellId\n"
        "    end\n"
        "end\n"
        "function GameTooltip:SetAction(slot)\n"
        "    self:ClearLines()\n"
        "    if not slot then return end\n"
        "    local actionType, id = GetActionInfo(slot)\n"
        "    if actionType == 'spell' and id and id > 0 then\n"
        "        self:SetSpellByID(id)\n"
        "    elseif actionType == 'item' and id and id > 0 then\n"
        "        local name, _, quality = GetItemInfo(id)\n"
        "        if name then self:SetText(name, 1, 1, 1) end\n"
        "    end\n"
        "end\n"
        "function GameTooltip:FadeOut() end\n"
        "function GameTooltip:SetFrameStrata(...) end\n"
        "function GameTooltip:SetClampedToScreen(...) end\n"
        "function GameTooltip:IsOwned(f) return self.__owner == f end\n"
        // ShoppingTooltip: used by comparison tooltips
        "ShoppingTooltip1 = CreateFrame('Frame', 'ShoppingTooltip1')\n"
        "ShoppingTooltip2 = CreateFrame('Frame', 'ShoppingTooltip2')\n"
        // Error handling stubs (used by many addons)
        "local _errorHandler = function(err) return err end\n"
        "function geterrorhandler() return _errorHandler end\n"
        "function seterrorhandler(fn) if type(fn)=='function' then _errorHandler=fn end end\n"
        "function debugstack(start, count1, count2) return '' end\n"
        "function securecall(fn, ...) if type(fn)=='function' then return fn(...) end end\n"
        "function issecurevariable(...) return false end\n"
        "function issecure() return false end\n"
        // GetCVarBool wraps C-side GetCVar (registered in table) for boolean queries
        "function GetCVarBool(name) return GetCVar(name) == '1' end\n"
        // Misc compatibility stubs
        // GetScreenWidth, GetScreenHeight, GetNumLootItems are now C functions
        // GetFramerate is now a C function
        "function GetNetStats() return 0, 0, 0, 0 end\n"
        "function IsLoggedIn() return true end\n"
        "function StaticPopup_Show() end\n"
        "function StaticPopup_Hide() end\n"
        // UI Panel management — Show/Hide standard WoW panels
        "UIPanelWindows = {}\n"
        "function ShowUIPanel(frame, force)\n"
        "    if frame and frame.Show then frame:Show() end\n"
        "end\n"
        "function HideUIPanel(frame)\n"
        "    if frame and frame.Hide then frame:Hide() end\n"
        "end\n"
        "function ToggleFrame(frame)\n"
        "    if frame then\n"
        "        if frame:IsShown() then frame:Hide() else frame:Show() end\n"
        "    end\n"
        "end\n"
        "function GetUIPanel(which) return nil end\n"
        "function CloseWindows(ignoreCenter) return false end\n"
        // TEXT localization stub — returns input string unchanged
        "function TEXT(text) return text end\n"
        // Faux scroll frame helpers (used by many list UIs)
        "function FauxScrollFrame_GetOffset(frame)\n"
        "    return frame and frame.offset or 0\n"
        "end\n"
        "function FauxScrollFrame_Update(frame, numItems, numVisible, valueStep, button, smallWidth, bigWidth, highlightFrame, smallHighlightWidth, bigHighlightWidth)\n"
        "    if not frame then return false end\n"
        "    frame.offset = frame.offset or 0\n"
        "    local showScrollBar = numItems > numVisible\n"
        "    return showScrollBar\n"
        "end\n"
        "function FauxScrollFrame_SetOffset(frame, offset)\n"
        "    if frame then frame.offset = offset or 0 end\n"
        "end\n"
        "function FauxScrollFrame_OnVerticalScroll(frame, value, itemHeight, updateFunction)\n"
        "    if not frame then return end\n"
        "    frame.offset = math.floor(value / (itemHeight or 1) + 0.5)\n"
        "    if updateFunction then updateFunction() end\n"
        "end\n"
        // SecureCmdOptionParse — parses conditional macros like [target=focus]
        "function SecureCmdOptionParse(options)\n"
        "    if not options then return nil end\n"
        "    -- Simple: return the unconditional fallback (text after last semicolon or the whole string)\n"
        "    local result = options:match(';%s*(.-)$') or options:match('^%[.*%]%s*(.-)$') or options\n"
        "    return result\n"
        "end\n"
        // ChatFrame message group stubs
        "function ChatFrame_AddMessageGroup(frame, group) end\n"
        "function ChatFrame_RemoveMessageGroup(frame, group) end\n"
        "function ChatFrame_AddChannel(frame, channel) end\n"
        "function ChatFrame_RemoveChannel(frame, channel) end\n"
        // CreateTexture/CreateFontString are now C frame methods in the metatable
        "do\n"
        "  local function cc(r,g,b)\n"
        "    local t = {r=r, g=g, b=b}\n"
        "    t.colorStr = string.format('%02x%02x%02x', math.floor(r*255), math.floor(g*255), math.floor(b*255))\n"
        "    function t:GenerateHexColor() return '|cff' .. self.colorStr end\n"
        "    function t:GenerateHexColorMarkup() return '|cff' .. self.colorStr end\n"
        "    return t\n"
        "  end\n"
        "  RAID_CLASS_COLORS = {\n"
        "    WARRIOR=cc(0.78,0.61,0.43), PALADIN=cc(0.96,0.55,0.73),\n"
        "    HUNTER=cc(0.67,0.83,0.45), ROGUE=cc(1.0,0.96,0.41),\n"
        "    PRIEST=cc(1.0,1.0,1.0), DEATHKNIGHT=cc(0.77,0.12,0.23),\n"
        "    SHAMAN=cc(0.0,0.44,0.87), MAGE=cc(0.41,0.80,0.94),\n"
        "    WARLOCK=cc(0.58,0.51,0.79), DRUID=cc(1.0,0.49,0.04),\n"
        "  }\n"
        "end\n"
        // Money formatting utility
        "function GetCoinTextureString(copper)\n"
        "    if not copper or copper == 0 then return '0c' end\n"
        "    copper = math.floor(copper)\n"
        "    local g = math.floor(copper / 10000)\n"
        "    local s = math.floor(math.fmod(copper, 10000) / 100)\n"
        "    local c = math.fmod(copper, 100)\n"
        "    local r = ''\n"
        "    if g > 0 then r = r .. g .. 'g ' end\n"
        "    if s > 0 then r = r .. s .. 's ' end\n"
        "    if c > 0 or r == '' then r = r .. c .. 'c' end\n"
        "    return r\n"
        "end\n"
        "GetCoinText = GetCoinTextureString\n"
    );

    // UIDropDownMenu framework — minimal compat for addons using dropdown menus
    luaL_dostring(L_,
        "UIDROPDOWNMENU_MENU_LEVEL = 1\n"
        "UIDROPDOWNMENU_MENU_VALUE = nil\n"
        "UIDROPDOWNMENU_OPEN_MENU = nil\n"
        "local _ddMenuList = {}\n"
        "function UIDropDownMenu_Initialize(frame, initFunc, displayMode, level, menuList)\n"
        "    if frame then frame.__initFunc = initFunc end\n"
        "end\n"
        "function UIDropDownMenu_CreateInfo() return {} end\n"
        "function UIDropDownMenu_AddButton(info, level) table.insert(_ddMenuList, info) end\n"
        "function UIDropDownMenu_SetWidth(frame, width) end\n"
        "function UIDropDownMenu_SetButtonWidth(frame, width) end\n"
        "function UIDropDownMenu_SetText(frame, text)\n"
        "    if frame then frame.__text = text end\n"
        "end\n"
        "function UIDropDownMenu_GetText(frame)\n"
        "    return frame and frame.__text or ''\n"
        "end\n"
        "function UIDropDownMenu_SetSelectedID(frame, id) end\n"
        "function UIDropDownMenu_SetSelectedValue(frame, value) end\n"
        "function UIDropDownMenu_GetSelectedID(frame) return 1 end\n"
        "function UIDropDownMenu_GetSelectedValue(frame) return nil end\n"
        "function UIDropDownMenu_JustifyText(frame, justify) end\n"
        "function UIDropDownMenu_EnableDropDown(frame) end\n"
        "function UIDropDownMenu_DisableDropDown(frame) end\n"
        "function CloseDropDownMenus() end\n"
        "function ToggleDropDownMenu(level, value, frame, anchor, xOfs, yOfs) end\n"
    );

    // UISpecialFrames: frames in this list close on Escape key
    luaL_dostring(L_,
        "UISpecialFrames = {}\n"
        // Font object stubs — addons reference these for CreateFontString templates
        "GameFontNormal = {}\n"
        "GameFontNormalSmall = {}\n"
        "GameFontNormalLarge = {}\n"
        "GameFontHighlight = {}\n"
        "GameFontHighlightSmall = {}\n"
        "GameFontHighlightLarge = {}\n"
        "GameFontDisable = {}\n"
        "GameFontDisableSmall = {}\n"
        "GameFontWhite = {}\n"
        "GameFontRed = {}\n"
        "GameFontGreen = {}\n"
        "NumberFontNormal = {}\n"
        "ChatFontNormal = {}\n"
        "SystemFont = {}\n"
        // InterfaceOptionsFrame: addons register settings panels here
        "InterfaceOptionsFrame = CreateFrame('Frame', 'InterfaceOptionsFrame')\n"
        "InterfaceOptionsFramePanelContainer = CreateFrame('Frame', 'InterfaceOptionsFramePanelContainer')\n"
        "function InterfaceOptions_AddCategory(panel) end\n"
        "function InterfaceOptionsFrame_OpenToCategory(panel) end\n"
        // Commonly expected global tables
        "SLASH_RELOAD1 = '/reload'\n"
        "SLASH_RELOADUI1 = '/reloadui'\n"
        "GRAY_FONT_COLOR = {r=0.5,g=0.5,b=0.5}\n"
        "NORMAL_FONT_COLOR = {r=1.0,g=0.82,b=0.0}\n"
        "HIGHLIGHT_FONT_COLOR = {r=1.0,g=1.0,b=1.0}\n"
        "GREEN_FONT_COLOR = {r=0.1,g=1.0,b=0.1}\n"
        "RED_FONT_COLOR = {r=1.0,g=0.1,b=0.1}\n"
        // C_ChatInfo — addon message prefix API used by some addons
        "C_ChatInfo = C_ChatInfo or {}\n"
        "C_ChatInfo.RegisterAddonMessagePrefix = RegisterAddonMessagePrefix\n"
        "C_ChatInfo.IsAddonMessagePrefixRegistered = IsAddonMessagePrefixRegistered\n"
        "C_ChatInfo.SendAddonMessage = SendAddonMessage\n"
    );

    // Action bar constants and functions used by action bar addons
    luaL_dostring(L_,
        "NUM_ACTIONBAR_BUTTONS = 12\n"
        "NUM_ACTIONBAR_PAGES = 6\n"
        "ACTION_BUTTON_SHOW_GRID_REASON_CVAR = 1\n"
        "ACTION_BUTTON_SHOW_GRID_REASON_EVENT = 2\n"
        // Action bar page tracking
        "local _actionBarPage = 1\n"
        "function GetActionBarPage() return _actionBarPage end\n"
        "function ChangeActionBarPage(page) _actionBarPage = page end\n"
        "function GetBonusBarOffset() return 0 end\n"
        // Action type query
        "function GetActionText(slot) return nil end\n"
        "function GetActionCount(slot) return 0 end\n"
        // Binding functions
        "function GetBindingKey(action) return nil end\n"
        "function GetBindingAction(key) return nil end\n"
        "function SetBinding(key, action) end\n"
        "function SaveBindings(which) end\n"
        "function GetCurrentBindingSet() return 1 end\n"
        // Macro functions
        "function GetNumMacros() return 0, 0 end\n"
        "function GetMacroInfo(id) return nil end\n"
        "function GetMacroBody(id) return nil end\n"
        "function GetMacroIndexByName(name) return 0 end\n"
        // Stance bar
        "function GetNumShapeshiftForms() return 0 end\n"
        "function GetShapeshiftFormInfo(index) return nil, nil, nil, nil end\n"
        // Pet action bar
        "NUM_PET_ACTION_SLOTS = 10\n"
        // Common WoW constants used by many addons
        "MAX_TALENT_TABS = 3\n"
        "MAX_NUM_TALENTS = 100\n"
        "BOOKTYPE_SPELL = 0\n"
        "BOOKTYPE_PET = 1\n"
        "MAX_PARTY_MEMBERS = 4\n"
        "MAX_RAID_MEMBERS = 40\n"
        "MAX_ARENA_TEAMS = 3\n"
        "INVSLOT_FIRST_EQUIPPED = 1\n"
        "INVSLOT_LAST_EQUIPPED = 19\n"
        "NUM_BAG_SLOTS = 4\n"
        "NUM_BANKBAGSLOTS = 7\n"
        "CONTAINER_BAG_OFFSET = 0\n"
        "MAX_SKILLLINE_TABS = 8\n"
        "TRADE_ENCHANT_SLOT = 7\n"
        "function GetPetActionInfo(slot) return nil end\n"
        "function GetPetActionsUsable() return false end\n"
    );

    // WoW table/string utility functions used by many addons
    luaL_dostring(L_,
        // Table utilities
        "function tContains(tbl, item)\n"
        "    for _, v in pairs(tbl) do if v == item then return true end end\n"
        "    return false\n"
        "end\n"
        "function tInvert(tbl)\n"
        "    local inv = {}\n"
        "    for k, v in pairs(tbl) do inv[v] = k end\n"
        "    return inv\n"
        "end\n"
        "function CopyTable(src)\n"
        "    if type(src) ~= 'table' then return src end\n"
        "    local copy = {}\n"
        "    for k, v in pairs(src) do copy[k] = CopyTable(v) end\n"
        "    return setmetatable(copy, getmetatable(src))\n"
        "end\n"
        "function tDeleteItem(tbl, item)\n"
        "    for i = #tbl, 1, -1 do if tbl[i] == item then table.remove(tbl, i) end end\n"
        "end\n"
        // String utilities (WoW globals that alias Lua string functions)
        "strupper = string.upper\n"
        "strlower = string.lower\n"
        "strfind = string.find\n"
        "strsub = string.sub\n"
        "strlen = string.len\n"
        "strrep = string.rep\n"
        "strbyte = string.byte\n"
        "strchar = string.char\n"
        "strrev = string.reverse\n"
        "gsub = string.gsub\n"
        "gmatch = string.gmatch\n"
        "strjoin = function(delim, ...)\n"
        "    return table.concat({...}, delim)\n"
        "end\n"
        // Math utilities
        "function Clamp(val, lo, hi) return math.min(math.max(val, lo), hi) end\n"
        "function Round(val) return math.floor(val + 0.5) end\n"
        // Bit operations (WoW provides these; Lua 5.1 doesn't have native bit ops)
        "bit = bit or {}\n"
        "bit.band = bit.band or function(a, b) local r,m=0,1 for i=0,31 do if a%2==1 and b%2==1 then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bor = bit.bor or function(a, b) local r,m=0,1 for i=0,31 do if a%2==1 or b%2==1 then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bxor = bit.bxor or function(a, b) local r,m=0,1 for i=0,31 do if (a%2==1)~=(b%2==1) then r=r+m end a=math.floor(a/2) b=math.floor(b/2) m=m*2 end return r end\n"
        "bit.bnot = bit.bnot or function(a) return 4294967295 - a end\n"
        "bit.lshift = bit.lshift or function(a, n) return a * (2^n) end\n"
        "bit.rshift = bit.rshift or function(a, n) return math.floor(a / (2^n)) end\n"
    );
}

// ---- Event System ----
// Lua-side: WoweeEvents table holds { ["EVENT_NAME"] = { handler1, handler2, ... } }
// RegisterEvent("EVENT", handler) adds a handler function
// UnregisterEvent("EVENT", handler) removes it

static int lua_RegisterEvent(lua_State* L) {
    const char* eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Get or create the WoweeEvents table
    lua_getglobal(L, "__WoweeEvents");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "__WoweeEvents");
    }

    // Get or create the handler list for this event
    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, eventName);
    }

    // Append the handler function to the list
    int len = static_cast<int>(lua_objlen(L, -1));
    lua_pushvalue(L, 2);  // push the handler function
    lua_rawseti(L, -2, len + 1);

    lua_pop(L, 2);  // pop handler list + WoweeEvents
    return 0;
}

static int lua_UnregisterEvent(lua_State* L) {
    const char* eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_getglobal(L, "__WoweeEvents");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 0; }

    lua_getfield(L, -1, eventName);
    if (lua_isnil(L, -1)) { lua_pop(L, 2); return 0; }

    // Remove matching handler from the list
    int len = static_cast<int>(lua_objlen(L, -1));
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_rawequal(L, -1, 2)) {
            lua_pop(L, 1);
            // Shift remaining elements down
            for (int j = i; j < len; j++) {
                lua_rawgeti(L, -1, j + 1);
                lua_rawseti(L, -2, j);
            }
            lua_pushnil(L);
            lua_rawseti(L, -2, len);
            break;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    return 0;
}

void LuaEngine::registerEventAPI() {
    lua_pushcfunction(L_, lua_RegisterEvent);
    lua_setglobal(L_, "RegisterEvent");

    lua_pushcfunction(L_, lua_UnregisterEvent);
    lua_setglobal(L_, "UnregisterEvent");

    // Create the events table
    lua_newtable(L_);
    lua_setglobal(L_, "__WoweeEvents");
}

void LuaEngine::fireEvent(const std::string& eventName,
                           const std::vector<std::string>& args) {
    if (!L_) return;

    lua_getglobal(L_, "__WoweeEvents");
    if (lua_isnil(L_, -1)) { lua_pop(L_, 1); return; }

    lua_getfield(L_, -1, eventName.c_str());
    if (lua_isnil(L_, -1)) { lua_pop(L_, 2); return; }

    int handlerCount = static_cast<int>(lua_objlen(L_, -1));
    for (int i = 1; i <= handlerCount; i++) {
        lua_rawgeti(L_, -1, i);
        if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); continue; }

        // Push arguments: event name first, then extra args
        lua_pushstring(L_, eventName.c_str());
        for (const auto& arg : args) {
            lua_pushstring(L_, arg.c_str());
        }

        int nargs = 1 + static_cast<int>(args.size());
        if (lua_pcall(L_, nargs, 0, 0) != 0) {
            const char* err = lua_tostring(L_, -1);
            std::string errStr = err ? err : "(unknown)";
            LOG_ERROR("LuaEngine: event '", eventName, "' handler error: ", errStr);
            if (luaErrorCallback_) luaErrorCallback_(errStr);
            lua_pop(L_, 1);
        }
    }
    lua_pop(L_, 2);  // pop handler list + WoweeEvents

    // Also dispatch to frames that registered for this event via frame:RegisterEvent()
    lua_getglobal(L_, "__WoweeFrameEvents");
    if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, eventName.c_str());
        if (lua_istable(L_, -1)) {
            int frameCount = static_cast<int>(lua_objlen(L_, -1));
            for (int i = 1; i <= frameCount; i++) {
                lua_rawgeti(L_, -1, i);
                if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }

                // Get the frame's OnEvent script
                lua_getfield(L_, -1, "__scripts");
                if (lua_istable(L_, -1)) {
                    lua_getfield(L_, -1, "OnEvent");
                    if (lua_isfunction(L_, -1)) {
                        lua_pushvalue(L_, -3);  // self (frame)
                        lua_pushstring(L_, eventName.c_str());
                        for (const auto& arg : args) lua_pushstring(L_, arg.c_str());
                        int nargs = 2 + static_cast<int>(args.size());
                        if (lua_pcall(L_, nargs, 0, 0) != 0) {
                            const char* ferr = lua_tostring(L_, -1);
                            std::string ferrStr = ferr ? ferr : "(unknown)";
                            LOG_ERROR("LuaEngine: frame OnEvent error: ", ferrStr);
                            if (luaErrorCallback_) luaErrorCallback_(ferrStr);
                            lua_pop(L_, 1);
                        }
                    } else {
                        lua_pop(L_, 1); // pop non-function
                    }
                }
                lua_pop(L_, 2); // pop __scripts + frame
            }
        }
        lua_pop(L_, 1); // pop event frame list
    }
    lua_pop(L_, 1); // pop __WoweeFrameEvents
}

void LuaEngine::dispatchOnUpdate(float elapsed) {
    if (!L_) return;

    lua_getglobal(L_, "__WoweeOnUpdateFrames");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    int count = static_cast<int>(lua_objlen(L_, -1));
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L_, -1, i);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }

        // Check if frame is visible
        lua_getfield(L_, -1, "__visible");
        bool visible = lua_toboolean(L_, -1);
        lua_pop(L_, 1);
        if (!visible) { lua_pop(L_, 1); continue; }

        // Get OnUpdate script
        lua_getfield(L_, -1, "__scripts");
        if (lua_istable(L_, -1)) {
            lua_getfield(L_, -1, "OnUpdate");
            if (lua_isfunction(L_, -1)) {
                lua_pushvalue(L_, -3);  // self (frame)
                lua_pushnumber(L_, static_cast<double>(elapsed));
                if (lua_pcall(L_, 2, 0, 0) != 0) {
                    const char* uerr = lua_tostring(L_, -1);
                    std::string uerrStr = uerr ? uerr : "(unknown)";
                    LOG_ERROR("LuaEngine: OnUpdate error: ", uerrStr);
                    if (luaErrorCallback_) luaErrorCallback_(uerrStr);
                    lua_pop(L_, 1);
                }
            } else {
                lua_pop(L_, 1);
            }
        }
        lua_pop(L_, 2); // pop __scripts + frame
    }
    lua_pop(L_, 1); // pop __WoweeOnUpdateFrames
}

bool LuaEngine::dispatchSlashCommand(const std::string& command, const std::string& args) {
    if (!L_) return false;

    // Check each SlashCmdList entry: for key NAME, check SLASH_NAME1, SLASH_NAME2, etc.
    lua_getglobal(L_, "SlashCmdList");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }

    std::string cmdLower = command;
    for (char& c : cmdLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    lua_pushnil(L_);
    while (lua_next(L_, -2) != 0) {
        // Stack: SlashCmdList, key, handler
        if (!lua_isfunction(L_, -1) || !lua_isstring(L_, -2)) {
            lua_pop(L_, 1);
            continue;
        }
        const char* name = lua_tostring(L_, -2);

        // Check SLASH_<NAME>1 through SLASH_<NAME>9
        for (int i = 1; i <= 9; i++) {
            std::string globalName = "SLASH_" + std::string(name) + std::to_string(i);
            lua_getglobal(L_, globalName.c_str());
            if (lua_isstring(L_, -1)) {
                std::string slashStr = lua_tostring(L_, -1);
                for (char& c : slashStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (slashStr == cmdLower) {
                    lua_pop(L_, 1); // pop global
                    // Call the handler with args
                    lua_pushvalue(L_, -1); // copy handler
                    lua_pushstring(L_, args.c_str());
                    if (lua_pcall(L_, 1, 0, 0) != 0) {
                        LOG_ERROR("LuaEngine: SlashCmdList['", name, "'] error: ",
                                  lua_tostring(L_, -1));
                        lua_pop(L_, 1);
                    }
                    lua_pop(L_, 3); // pop handler, key, SlashCmdList
                    return true;
                }
            }
            lua_pop(L_, 1); // pop global
        }
        lua_pop(L_, 1); // pop handler, keep key for next iteration
    }
    lua_pop(L_, 1); // pop SlashCmdList
    return false;
}

// ---- SavedVariables serialization ----

static void serializeLuaValue(lua_State* L, int idx, std::string& out, int indent);

static void serializeLuaTable(lua_State* L, int idx, std::string& out, int indent) {
    out += "{\n";
    std::string pad(indent + 2, ' ');
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        out += pad;
        // Key
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char* k = lua_tostring(L, -2);
            out += "[\"";
            for (const char* p = k; *p; ++p) {
                if (*p == '"' || *p == '\\') out += '\\';
                out += *p;
            }
            out += "\"] = ";
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
            out += "[" + std::to_string(static_cast<long long>(lua_tonumber(L, -2))) + "] = ";
        } else {
            lua_pop(L, 1);
            continue;
        }
        // Value
        serializeLuaValue(L, lua_gettop(L), out, indent + 2);
        out += ",\n";
        lua_pop(L, 1);
    }
    out += std::string(indent, ' ') + "}";
}

static void serializeLuaValue(lua_State* L, int idx, std::string& out, int indent) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     out += "nil"; break;
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; break;
        case LUA_TNUMBER: {
            double v = lua_tonumber(L, idx);
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", v);
            out += buf;
            break;
        }
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, idx);
            out += "\"";
            for (const char* p = s; *p; ++p) {
                if (*p == '"' || *p == '\\') out += '\\';
                else if (*p == '\n') { out += "\\n"; continue; }
                else if (*p == '\r') continue;
                out += *p;
            }
            out += "\"";
            break;
        }
        case LUA_TTABLE:
            serializeLuaTable(L, idx, out, indent);
            break;
        default:
            out += "nil"; // Functions, userdata, etc. can't be serialized
            break;
    }
}

void LuaEngine::setAddonList(const std::vector<TocFile>& addons) {
    if (!L_) return;
    lua_pushnumber(L_, static_cast<double>(addons.size()));
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_addon_count");

    lua_newtable(L_);
    for (size_t i = 0; i < addons.size(); i++) {
        lua_newtable(L_);
        lua_pushstring(L_, addons[i].addonName.c_str());
        lua_setfield(L_, -2, "name");
        lua_pushstring(L_, addons[i].getTitle().c_str());
        lua_setfield(L_, -2, "title");
        auto notesIt = addons[i].directives.find("Notes");
        lua_pushstring(L_, notesIt != addons[i].directives.end() ? notesIt->second.c_str() : "");
        lua_setfield(L_, -2, "notes");
        // Store all TOC directives for GetAddOnMetadata
        lua_newtable(L_);
        for (const auto& [key, val] : addons[i].directives) {
            lua_pushstring(L_, val.c_str());
            lua_setfield(L_, -2, key.c_str());
        }
        lua_setfield(L_, -2, "metadata");
        lua_rawseti(L_, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L_, LUA_REGISTRYINDEX, "wowee_addon_info");
}

bool LuaEngine::loadSavedVariables(const std::string& path) {
    if (!L_) return false;
    std::ifstream f(path);
    if (!f.is_open()) return false; // No saved data yet — not an error
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return true;
    int err = luaL_dostring(L_, content.c_str());
    if (err != 0) {
        LOG_WARNING("LuaEngine: error loading saved variables from '", path, "': ",
                    lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEngine::saveSavedVariables(const std::string& path, const std::vector<std::string>& varNames) {
    if (!L_ || varNames.empty()) return false;
    std::string output;
    for (const auto& name : varNames) {
        lua_getglobal(L_, name.c_str());
        if (!lua_isnil(L_, -1)) {
            output += name + " = ";
            serializeLuaValue(L_, lua_gettop(L_), output, 0);
            output += "\n";
        }
        lua_pop(L_, 1);
    }
    if (output.empty()) return true;

    // Ensure directory exists
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, lastSlash), ec);
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        LOG_WARNING("LuaEngine: cannot write saved variables to '", path, "'");
        return false;
    }
    f << output;
    LOG_INFO("LuaEngine: saved variables to '", path, "' (", output.size(), " bytes)");
    return true;
}

bool LuaEngine::executeFile(const std::string& path) {
    if (!L_) return false;

    int err = luaL_dofile(L_, path.c_str());
    if (err != 0) {
        const char* errMsg = lua_tostring(L_, -1);
        std::string msg = errMsg ? errMsg : "(unknown error)";
        LOG_ERROR("LuaEngine: error loading '", path, "': ", msg);
        if (luaErrorCallback_) luaErrorCallback_(msg);
        if (gameHandler_) {
            game::MessageChatData errChat;
            errChat.type = game::ChatType::SYSTEM;
            errChat.language = game::ChatLanguage::UNIVERSAL;
            errChat.message = "|cffff4040[Lua Error] " + msg + "|r";
            gameHandler_->addLocalChatMessage(errChat);
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaEngine::executeString(const std::string& code) {
    if (!L_) return false;

    int err = luaL_dostring(L_, code.c_str());
    if (err != 0) {
        const char* errMsg = lua_tostring(L_, -1);
        std::string msg = errMsg ? errMsg : "(unknown error)";
        LOG_ERROR("LuaEngine: script error: ", msg);
        if (luaErrorCallback_) luaErrorCallback_(msg);
        if (gameHandler_) {
            game::MessageChatData errChat;
            errChat.type = game::ChatType::SYSTEM;
            errChat.language = game::ChatLanguage::UNIVERSAL;
            errChat.message = "|cffff4040[Lua Error] " + msg + "|r";
            gameHandler_->addLocalChatMessage(errChat);
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace wowee::addons
