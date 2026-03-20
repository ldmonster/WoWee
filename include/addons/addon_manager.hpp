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
    void shutdown();

    const std::vector<TocFile>& getAddons() const { return addons_; }
    LuaEngine* getLuaEngine() { return &luaEngine_; }
    bool isInitialized() const { return luaEngine_.isInitialized(); }

private:
    LuaEngine luaEngine_;
    std::vector<TocFile> addons_;

    bool loadAddon(const TocFile& addon);
};

} // namespace wowee::addons
