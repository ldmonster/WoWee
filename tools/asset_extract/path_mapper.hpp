#pragma once

#include <string>

namespace wowee {
namespace tools {

/**
 * Maps WoW virtual paths to reorganized filesystem categories.
 *
 * Input:  WoW virtual path (e.g., "Creature\\Bear\\BearSkin.blp")
 * Output: Category-based relative path (e.g., "creature/bear/BearSkin.blp")
 */
class PathMapper {
public:
    /**
     * Map a WoW virtual path to a reorganized filesystem path.
     * @param wowPath Original WoW virtual path (backslash-separated)
     * @return Reorganized relative path (forward-slash separated, fully lowercased)
     */
    static std::string mapPath(const std::string& wowPath);

private:
    static std::string mapPathImpl(const std::string& wowPath);
    // Helpers for prefix matching (case-insensitive)
    static bool startsWithCI(const std::string& str, const std::string& prefix);
    static std::string toLower(const std::string& str);
    static std::string toForwardSlash(const std::string& str);
    static std::string extractAfterPrefix(const std::string& path, size_t prefixLen);
};

} // namespace tools
} // namespace wowee
