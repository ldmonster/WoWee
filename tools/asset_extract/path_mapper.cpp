#include "path_mapper.hpp"
#include <algorithm>
#include <cctype>

namespace wowee {
namespace tools {

std::string PathMapper::toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string PathMapper::toForwardSlash(const std::string& str) {
    std::string result = str;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool PathMapper::startsWithCI(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::string PathMapper::extractAfterPrefix(const std::string& path, size_t prefixLen) {
    if (prefixLen >= path.size()) return {};
    return path.substr(prefixLen);
}

std::string PathMapper::mapPath(const std::string& wowPath) {
    // Lowercase entire output path — WoW archives contain mixed-case variants
    // of the same path which create duplicate directories on case-sensitive filesystems.
    return toLower(mapPathImpl(wowPath));
}

std::string PathMapper::mapPathImpl(const std::string& wowPath) {
    std::string rest;

    // DBFilesClient\ → db/
    if (startsWithCI(wowPath, "DBFilesClient\\")) {
        rest = extractAfterPrefix(wowPath, 14);
        return "db/" + toForwardSlash(rest);
    }

    // Character\{Race}\{Gender}\ → character/{race}/{gender}/
    if (startsWithCI(wowPath, "Character\\")) {
        rest = extractAfterPrefix(wowPath, 10);
        std::string lowered = toLower(rest);
        return "character/" + toForwardSlash(lowered);
    }

    // Creature\{Name}\ → creature/{name}/
    if (startsWithCI(wowPath, "Creature\\")) {
        rest = extractAfterPrefix(wowPath, 9);
        // Keep first component lowercase for directory, preserve filename case
        std::string fwd = toForwardSlash(rest);
        auto slash = fwd.find('/');
        if (slash != std::string::npos) {
            return "creature/" + toLower(fwd.substr(0, slash)) + "/" + fwd.substr(slash + 1);
        }
        return "creature/" + fwd;
    }

    // Item\ObjectComponents\ → item/objectcomponents/
    if (startsWithCI(wowPath, "Item\\ObjectComponents\\")) {
        rest = extractAfterPrefix(wowPath, 22);
        return "item/objectcomponents/" + toForwardSlash(rest);
    }

    // Item\TextureComponents\ → item/texturecomponents/
    if (startsWithCI(wowPath, "Item\\TextureComponents\\")) {
        rest = extractAfterPrefix(wowPath, 23);
        return "item/texturecomponents/" + toForwardSlash(rest);
    }

    // Interface\Icons\ → interface/icons/
    if (startsWithCI(wowPath, "Interface\\Icons\\")) {
        rest = extractAfterPrefix(wowPath, 16);
        return "interface/icons/" + toForwardSlash(rest);
    }

    // Interface\GossipFrame\ → interface/gossip/
    if (startsWithCI(wowPath, "Interface\\GossipFrame\\")) {
        rest = extractAfterPrefix(wowPath, 21);
        return "interface/gossip/" + toForwardSlash(rest);
    }

    // Interface\{rest} → interface/{rest}/
    if (startsWithCI(wowPath, "Interface\\")) {
        rest = extractAfterPrefix(wowPath, 10);
        return "interface/" + toForwardSlash(rest);
    }

    // Textures\Minimap\ → terrain/minimap/
    if (startsWithCI(wowPath, "Textures\\Minimap\\")) {
        rest = extractAfterPrefix(wowPath, 17);
        return "terrain/minimap/" + toForwardSlash(rest);
    }

    // Textures\BakedNpcTextures\ → creature/baked/
    if (startsWithCI(wowPath, "Textures\\BakedNpcTextures\\")) {
        rest = extractAfterPrefix(wowPath, 25);
        return "creature/baked/" + toForwardSlash(rest);
    }

    // Textures\{rest} → terrain/textures/{rest}
    if (startsWithCI(wowPath, "Textures\\")) {
        rest = extractAfterPrefix(wowPath, 9);
        return "terrain/textures/" + toForwardSlash(rest);
    }

    // World\Maps\{Map}\ → terrain/maps/{map}/
    if (startsWithCI(wowPath, "World\\Maps\\")) {
        rest = extractAfterPrefix(wowPath, 11);
        std::string fwd = toForwardSlash(rest);
        auto slash = fwd.find('/');
        if (slash != std::string::npos) {
            return "terrain/maps/" + toLower(fwd.substr(0, slash)) + "/" + fwd.substr(slash + 1);
        }
        return "terrain/maps/" + fwd;
    }

    // World\wmo\ → world/wmo/ (preserve subpath)
    if (startsWithCI(wowPath, "World\\wmo\\")) {
        rest = extractAfterPrefix(wowPath, 10);
        return "world/wmo/" + toForwardSlash(rest);
    }

    // World\Doodads\ → world/doodads/
    if (startsWithCI(wowPath, "World\\Doodads\\")) {
        rest = extractAfterPrefix(wowPath, 14);
        return "world/doodads/" + toForwardSlash(rest);
    }

    // World\{rest} → world/{rest}/
    if (startsWithCI(wowPath, "World\\")) {
        rest = extractAfterPrefix(wowPath, 6);
        return "world/" + toForwardSlash(rest);
    }

    // Environments\ → environment/
    if (startsWithCI(wowPath, "Environments\\")) {
        rest = extractAfterPrefix(wowPath, 13);
        return "environment/" + toForwardSlash(rest);
    }

    // Sound\Ambience\ → sound/ambient/
    if (startsWithCI(wowPath, "Sound\\Ambience\\")) {
        rest = extractAfterPrefix(wowPath, 15);
        return "sound/ambient/" + toForwardSlash(rest);
    }

    // Sound\Character\ → sound/character/
    if (startsWithCI(wowPath, "Sound\\Character\\")) {
        rest = extractAfterPrefix(wowPath, 16);
        return "sound/character/" + toForwardSlash(rest);
    }

    // Sound\Doodad\ → sound/doodad/
    if (startsWithCI(wowPath, "Sound\\Doodad\\")) {
        rest = extractAfterPrefix(wowPath, 13);
        return "sound/doodad/" + toForwardSlash(rest);
    }

    // Sound\Creature\ → sound/creature/
    if (startsWithCI(wowPath, "Sound\\Creature\\")) {
        rest = extractAfterPrefix(wowPath, 15);
        return "sound/creature/" + toForwardSlash(rest);
    }

    // Sound\Spells\ → sound/spell/
    if (startsWithCI(wowPath, "Sound\\Spells\\")) {
        rest = extractAfterPrefix(wowPath, 13);
        return "sound/spell/" + toForwardSlash(rest);
    }

    // Sound\Music\ → sound/music/
    if (startsWithCI(wowPath, "Sound\\Music\\")) {
        rest = extractAfterPrefix(wowPath, 12);
        return "sound/music/" + toForwardSlash(rest);
    }

    // Sound\{rest} → sound/{rest}/
    if (startsWithCI(wowPath, "Sound\\")) {
        rest = extractAfterPrefix(wowPath, 6);
        return "sound/" + toForwardSlash(rest);
    }

    // Spells\ → spell/
    if (startsWithCI(wowPath, "Spells\\")) {
        rest = extractAfterPrefix(wowPath, 7);
        return "spell/" + toForwardSlash(rest);
    }

    // Everything else → misc/{original_path}
    return "misc/" + toForwardSlash(wowPath);
}

} // namespace tools
} // namespace wowee
