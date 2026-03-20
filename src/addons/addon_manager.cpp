#include "addons/addon_manager.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace wowee::addons {

AddonManager::AddonManager() = default;
AddonManager::~AddonManager() { shutdown(); }

bool AddonManager::initialize(game::GameHandler* gameHandler) {
    if (!luaEngine_.initialize()) return false;
    luaEngine_.setGameHandler(gameHandler);
    return true;
}

void AddonManager::scanAddons(const std::string& addonsPath) {
    addons_.clear();

    std::error_code ec;
    if (!fs::is_directory(addonsPath, ec)) {
        LOG_INFO("AddonManager: no AddOns directory at ", addonsPath);
        return;
    }

    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(addonsPath, ec)) {
        if (entry.is_directory()) dirs.push_back(entry.path());
    }
    // Sort alphabetically for deterministic load order
    std::sort(dirs.begin(), dirs.end());

    for (const auto& dir : dirs) {
        std::string dirName = dir.filename().string();
        std::string tocPath = (dir / (dirName + ".toc")).string();
        auto toc = parseTocFile(tocPath);
        if (!toc) continue;

        if (toc->isLoadOnDemand()) {
            LOG_DEBUG("AddonManager: skipping LoadOnDemand addon: ", dirName);
            continue;
        }

        LOG_INFO("AddonManager: registered addon '", toc->getTitle(),
                 "' (", toc->files.size(), " files)");
        addons_.push_back(std::move(*toc));
    }

    LOG_INFO("AddonManager: scanned ", addons_.size(), " addons");
}

void AddonManager::loadAllAddons() {
    int loaded = 0, failed = 0;
    for (const auto& addon : addons_) {
        if (loadAddon(addon)) loaded++;
        else failed++;
    }
    LOG_INFO("AddonManager: loaded ", loaded, " addons",
             (failed > 0 ? (", " + std::to_string(failed) + " failed") : ""));
}

bool AddonManager::loadAddon(const TocFile& addon) {
    bool success = true;
    for (const auto& filename : addon.files) {
        // For Step 1: only load .lua files, skip .xml (frame system not yet implemented)
        std::string lower = filename;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".lua") {
            std::string fullPath = addon.basePath + "/" + filename;
            if (!luaEngine_.executeFile(fullPath)) {
                success = false;
            }
        } else if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".xml") {
            LOG_DEBUG("AddonManager: skipping XML file '", filename,
                      "' in addon '", addon.addonName, "' (XML frames not yet implemented)");
        }
    }
    return success;
}

bool AddonManager::runScript(const std::string& code) {
    return luaEngine_.executeString(code);
}

void AddonManager::shutdown() {
    addons_.clear();
    luaEngine_.shutdown();
}

} // namespace wowee::addons
