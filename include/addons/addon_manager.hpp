#pragma once

#include "addons/lua_engine.hpp"
#include "addons/toc_parser.hpp"
#include <memory>
#include <string>
#include <vector>

namespace wowee::addons {

class AddonManager {
public:
    AddonManager();
    ~AddonManager();

    bool initialize(game::GameHandler* gameHandler);
    void scanAddons(const std::string& addonsPath);
    void loadAllAddons();
    bool runScript(const std::string& code);
    void fireEvent(const std::string& event, const std::vector<std::string>& args = {});
    void update(float deltaTime);
    void shutdown();

    const std::vector<TocFile>& getAddons() const { return addons_; }
    LuaEngine* getLuaEngine() { return &luaEngine_; }
    bool isInitialized() const { return luaEngine_.isInitialized(); }

    void saveAllSavedVariables();

    /// Re-initialize the Lua VM and reload all addons (used by /reload).
    bool reload();

private:
    LuaEngine luaEngine_;
    std::vector<TocFile> addons_;
    game::GameHandler* gameHandler_ = nullptr;
    std::string addonsPath_;

    bool loadAddon(const TocFile& addon);
    std::string getSavedVariablesPath(const TocFile& addon) const;
};

} // namespace wowee::addons
