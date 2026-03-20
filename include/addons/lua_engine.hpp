#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace wowee::game { class GameHandler; }

namespace wowee::addons {

class LuaEngine {
public:
    LuaEngine();
    ~LuaEngine();

    LuaEngine(const LuaEngine&) = delete;
    LuaEngine& operator=(const LuaEngine&) = delete;

    bool initialize();
    void shutdown();

    bool executeFile(const std::string& path);
    bool executeString(const std::string& code);

    void setGameHandler(game::GameHandler* handler);

    // Fire a WoW event to all registered Lua handlers.
    // Extra string args are pushed as event arguments.
    void fireEvent(const std::string& eventName,
                   const std::vector<std::string>& args = {});

    lua_State* getState() { return L_; }
    bool isInitialized() const { return L_ != nullptr; }

private:
    lua_State* L_ = nullptr;
    game::GameHandler* gameHandler_ = nullptr;

    void registerCoreAPI();
    void registerEventAPI();
};

} // namespace wowee::addons
