#pragma once

#include "game/expansion_profile.hpp"
#include "core/application.hpp"

namespace wowee {
namespace game {

inline bool isActiveExpansion(const char* expansionId) {
    auto& app = core::Application::getInstance();
    auto* registry = app.getExpansionRegistry();
    if (!registry) return false;
    auto* profile = registry->getActive();
    if (!profile) return false;
    return profile->id == expansionId;
}

inline bool isClassicLikeExpansion() {
    return isActiveExpansion("classic") || isActiveExpansion("turtle");
}

inline bool isPreWotlk() {
    return isClassicLikeExpansion() || isActiveExpansion("tbc");
}

} // namespace game
} // namespace wowee
