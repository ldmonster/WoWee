#include "addons/lua_engine.hpp"
#include "game/game_handler.hpp"
#include "core/logger.hpp"

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
