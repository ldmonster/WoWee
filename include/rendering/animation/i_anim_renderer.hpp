#pragma once

#include "rendering/animation/anim_capability_set.hpp"
#include "rendering/animation/anim_event.hpp"
#include <cstdint>
#include <vector>

namespace wowee {
namespace pipeline { struct M2Sequence; }

namespace rendering {

// ============================================================================
// IAnimRenderer
//
// Abstraction for renderer animation operations. Sub-FSMs and animators
// talk to this interface, not to CharacterRenderer directly.
// ============================================================================
class IAnimRenderer {
public:
    virtual void playAnimation(uint32_t instanceId, uint32_t animId, bool loop) = 0;
    virtual bool hasAnimation(uint32_t instanceId, uint32_t animId) const = 0;
    virtual bool getAnimationState(uint32_t instanceId, uint32_t& outAnimId,
                                   float& outTimeMs, float& outDurMs) const = 0;
    virtual bool getAnimationSequences(uint32_t instanceId,
                                       std::vector<pipeline::M2Sequence>& out) const = 0;
    virtual ~IAnimRenderer() = default;
};

} // namespace rendering
} // namespace wowee
