#include "addons/lua_engine.hpp"
#include "addons/toc_parser.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "core/logger.hpp"
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
static uint64_t resolveUnitGuid(game::GameHandler* gh, const std::string& uid) {
    if (uid == "player")      return gh->getPlayerGuid();
    if (uid == "target")      return gh->getTargetGuid();
    if (uid == "focus")       return gh->getFocusGuid();
    if (uid == "pet")         return gh->getPetGuid();
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

// Helper: resolve "player", "target", "focus", "pet", "partyN", "raidN" unit IDs to entity
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

static int lua_UnitName(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit && !unit->getName().empty()) {
        lua_pushstring(L, unit->getName().c_str());
    } else {
        lua_pushstring(L, "Unknown");
    }
    return 1;
}

static int lua_UnitHealth(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushnumber(L, unit ? unit->getHealth() : 0);
    return 1;
}

static int lua_UnitHealthMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushnumber(L, unit ? unit->getMaxHealth() : 0);
    return 1;
}

static int lua_UnitPower(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushnumber(L, unit ? unit->getPower() : 0);
    return 1;
}

static int lua_UnitPowerMax(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushnumber(L, unit ? unit->getMaxPower() : 0);
    return 1;
}

static int lua_UnitLevel(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushnumber(L, unit ? unit->getLevel() : 0);
    return 1;
}

static int lua_UnitExists(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit != nullptr);
    return 1;
}

static int lua_UnitIsDead(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    lua_pushboolean(L, unit && unit->getHealth() == 0);
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
        // For player, use character data; for others, use UNIT_FIELD_BYTES_0
        std::string uidStr(uid);
        for (char& c : uidStr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (uidStr == "player") classId = gh->getPlayerClass();
        const char* name = (classId < 12) ? kClasses[classId] : "Unknown";
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

// --- Player/Game API ---

static int lua_GetMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getMoneyCopper()) : 0.0);
    return 1;
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

static int lua_UnitRace(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
    std::string uid(luaL_optstring(L, 1, "player"));
    for (char& c : uid) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (uid == "player") {
        uint8_t race = gh->getPlayerRace();
        static const char* kRaces[] = {"","Human","Orc","Dwarf","Night Elf","Undead",
            "Tauren","Gnome","Troll","","Blood Elf","Draenei"};
        lua_pushstring(L, (race < 12) ? kRaces[race] : "Unknown");
        return 1;
    }
    lua_pushstring(L, "Unknown");
    return 1;
}

static int lua_UnitPowerType(lua_State* L) {
    const char* uid = luaL_optstring(L, 1, "player");
    auto* unit = resolveUnit(L, uid);
    if (unit) {
        lua_pushnumber(L, unit->getPowerType());
        static const char* kPowerNames[] = {"MANA","RAGE","FOCUS","ENERGY","HAPPINESS","","RUNIC_POWER"};
        uint8_t pt = unit->getPowerType();
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
            lua_pushnil(L);                  // debuffType
            lua_pushnumber(L, aura.maxDurationMs > 0 ? aura.maxDurationMs / 1000.0 : 0); // duration
            lua_pushnumber(L, 0);            // expirationTime (would need absolute time)
            lua_pushnil(L);                  // caster
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

static int lua_IsSpellKnown(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint32_t spellId = static_cast<uint32_t>(luaL_checknumber(L, 1));
    lua_pushboolean(L, gh && gh->getKnownSpells().count(spellId));
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
    lua_pushnumber(L, 0);   // start time (not tracked precisely, return 0)
    lua_pushnumber(L, cd);  // duration remaining
    return 2;
}

static int lua_HasTarget(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->hasTarget());
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
    lua_pushnumber(L, 0);                                    // 4: castTime (ms) — not tracked
    lua_pushnumber(L, 0);                                    // 5: minRange
    lua_pushnumber(L, 0);                                    // 6: maxRange
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
    // Build item link string: |cFFFFFFFF|Hitem:ID:0:0:0:0:0:0:0|h[Name]|h|r
    char link[256];
    snprintf(link, sizeof(link), "|cFFFFFFFF|Hitem:%u:0:0:0:0:0:0:0|h[%s]|h|r",
             itemId, info->name.c_str());
    lua_pushstring(L, link);                         // 2: link
    lua_pushnumber(L, info->quality);                // 3: quality
    lua_pushnumber(L, info->itemLevel);              // 4: iLevel
    lua_pushnumber(L, info->requiredLevel);          // 5: requiredLevel
    lua_pushstring(L, "");                           // 6: class (type string)
    lua_pushstring(L, "");                           // 7: subclass
    lua_pushnumber(L, info->maxStack > 0 ? info->maxStack : 1); // 8: maxStack
    lua_pushstring(L, "");                           // 9: equipSlot
    lua_pushnil(L);                                  // 10: texture (icon path — no ItemDisplayInfo icon resolver yet)
    lua_pushnumber(L, info->sellPrice);              // 11: vendorPrice
    return 11;
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
        lua_pushboolean(L, 0);
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
        {"UnitLevel",     lua_UnitLevel},
        {"UnitExists",    lua_UnitExists},
        {"UnitIsDead",    lua_UnitIsDead},
        {"UnitClass",     lua_UnitClass},
        {"GetMoney",      lua_GetMoney},
        {"IsInGroup",     lua_IsInGroup},
        {"IsInRaid",      lua_IsInRaid},
        {"GetPlayerMapPosition", lua_GetPlayerMapPosition},
        {"SendChatMessage",   lua_SendChatMessage},
        {"CastSpellByName",   lua_CastSpellByName},
        {"IsSpellKnown",      lua_IsSpellKnown},
        {"GetSpellCooldown",  lua_GetSpellCooldown},
        {"HasTarget",         lua_HasTarget},
        {"UnitRace",          lua_UnitRace},
        {"UnitPowerType",     lua_UnitPowerType},
        {"GetNumGroupMembers", lua_GetNumGroupMembers},
        {"UnitGUID",          lua_UnitGUID},
        {"UnitIsPlayer",      lua_UnitIsPlayer},
        {"InCombatLockdown",  lua_InCombatLockdown},
        {"UnitBuff",          lua_UnitBuff},
        {"UnitDebuff",        lua_UnitDebuff},
        {"UnitAura",          lua_UnitAuraGeneric},
        {"GetNumAddOns",      lua_GetNumAddOns},
        {"GetAddOnInfo",      lua_GetAddOnInfo},
        {"GetSpellInfo",      lua_GetSpellInfo},
        {"GetSpellTexture",   lua_GetSpellTexture},
        {"GetItemInfo",       lua_GetItemInfo},
        {"GetLocale",         lua_GetLocale},
        {"GetBuildInfo",      lua_GetBuildInfo},
        {"GetCurrentMapAreaID", lua_GetCurrentMapAreaID},
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
        {"UnitIsUnit",          lua_UnitIsUnit},
        {"UnitIsFriend",        lua_UnitIsFriend},
        {"UnitIsEnemy",         lua_UnitIsEnemy},
        {"UnitCreatureType",    lua_UnitCreatureType},
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
        {nullptr, nullptr}
    };
    for (const luaL_Reg* r = frameMethods; r->name; r++) {
        lua_pushcfunction(L_, r->func);
        lua_setfield(L_, -2, r->name);
    }
    lua_setglobal(L_, "__WoweeFrameMT");

    // CreateFrame function
    lua_pushcfunction(L_, lua_CreateFrame);
    lua_setglobal(L_, "CreateFrame");

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

    // Noop stubs for commonly called functions that don't need implementation
    luaL_dostring(L_,
        "function SetDesaturation() end\n"
        "function SetPortraitTexture() end\n"
        "function PlaySound() end\n"
        "function PlaySoundFile() end\n"
        "function StopSound() end\n"
        "function UIParent_OnEvent() end\n"
        "UIParent = CreateFrame('Frame', 'UIParent')\n"
        "WorldFrame = CreateFrame('Frame', 'WorldFrame')\n"
        // Error handling stubs (used by many addons)
        "local _errorHandler = function(err) return err end\n"
        "function geterrorhandler() return _errorHandler end\n"
        "function seterrorhandler(fn) if type(fn)=='function' then _errorHandler=fn end end\n"
        "function debugstack(start, count1, count2) return '' end\n"
        "function securecall(fn, ...) if type(fn)=='function' then return fn(...) end end\n"
        "function issecurevariable(...) return false end\n"
        "function issecure() return false end\n"
        // CVar stubs (many addons check settings)
        "local _cvars = {}\n"
        "function GetCVar(name) return _cvars[name] or '0' end\n"
        "function GetCVarBool(name) return _cvars[name] == '1' end\n"
        "function SetCVar(name, value) _cvars[name] = tostring(value) end\n"
        // Misc compatibility stubs
        "function GetScreenWidth() return 1920 end\n"
        "function GetScreenHeight() return 1080 end\n"
        "function GetFramerate() return 60 end\n"
        "function GetNetStats() return 0, 0, 0, 0 end\n"
        "function IsLoggedIn() return true end\n"
        // IsMounted, IsFlying, IsSwimming, IsResting, IsFalling, IsStealthed
        // are now C functions registered in registerCoreAPI() with real game state
        "function GetNumLootItems() return 0 end\n"
        "function StaticPopup_Show() end\n"
        "function StaticPopup_Hide() end\n"
        "RAID_CLASS_COLORS = {\n"
        "    WARRIOR={r=0.78,g=0.61,b=0.43}, PALADIN={r=0.96,g=0.55,b=0.73},\n"
        "    HUNTER={r=0.67,g=0.83,b=0.45}, ROGUE={r=1.0,g=0.96,b=0.41},\n"
        "    PRIEST={r=1.0,g=1.0,b=1.0}, DEATHKNIGHT={r=0.77,g=0.12,b=0.23},\n"
        "    SHAMAN={r=0.0,g=0.44,b=0.87}, MAGE={r=0.41,g=0.80,b=0.94},\n"
        "    WARLOCK={r=0.58,g=0.51,b=0.79}, DRUID={r=1.0,g=0.49,b=0.04},\n"
        "}\n"
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
            LOG_ERROR("LuaEngine: event '", eventName, "' handler error: ",
                      err ? err : "(unknown)");
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
                            LOG_ERROR("LuaEngine: frame OnEvent error: ", lua_tostring(L_, -1));
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
                    LOG_ERROR("LuaEngine: OnUpdate error: ", lua_tostring(L_, -1));
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
