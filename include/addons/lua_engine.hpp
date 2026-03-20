#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace wowee::game { class GameHandler; }

namespace wowee::addons {

struct TocFile;  // forward declaration

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
    void fireEvent(const std::string& eventName,
                   const std::vector<std::string>& args = {});

    // Try to dispatch a slash command via SlashCmdList. Returns true if handled.
    bool dispatchSlashCommand(const std::string& command, const std::string& args);

    // Call OnUpdate scripts on all frames that have one.
    void dispatchOnUpdate(float elapsed);

    // SavedVariables: load globals from file, save globals to file
    bool loadSavedVariables(const std::string& path);
    bool saveSavedVariables(const std::string& path, const std::vector<std::string>& varNames);

    // Store addon info in registry for GetAddOnInfo/GetNumAddOns
    void setAddonList(const std::vector<TocFile>& addons);

    lua_State* getState() { return L_; }
    bool isInitialized() const { return L_ != nullptr; }

private:
    lua_State* L_ = nullptr;
    game::GameHandler* gameHandler_ = nullptr;

    void registerCoreAPI();
    void registerEventAPI();
};

} // namespace wowee::addons
