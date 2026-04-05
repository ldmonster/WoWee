#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {

namespace audio { enum class FootstepSurface : uint8_t; }

namespace rendering {

class Renderer;

// ============================================================================
// FootstepDriver — extracted from AnimationController
//
// Owns animation-driven footstep event detection, surface resolution,
// and player/mount footstep tracking state.
// ============================================================================
class FootstepDriver {
public:
    FootstepDriver() = default;

    /// Process footstep events for this frame (called from Renderer::update).
    void update(float deltaTime, Renderer* renderer,
                bool mounted, uint32_t mountInstanceId, bool taxiFlight,
                bool isFootstepState);

    /// Detect if a footstep event should trigger based on animation phase crossing.
    bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs,
                                    float animationDurationMs);

    /// Resolve the surface type under the character for footstep sound selection.
    audio::FootstepSurface resolveFootstepSurface(Renderer* renderer) const;

private:
    // Player footstep event tracking (animation-driven)
    uint32_t footstepLastAnimationId_ = 0;
    float footstepLastNormTime_ = 0.0f;
    bool footstepNormInitialized_ = false;

    // Footstep surface cache (avoid expensive queries every step)
    mutable audio::FootstepSurface cachedFootstepSurface_{};
    mutable glm::vec3 cachedFootstepPosition_{0.0f, 0.0f, 0.0f};
    mutable float cachedFootstepUpdateTimer_{999.0f};

    // Mount footstep tracking (separate from player's)
    uint32_t mountFootstepLastAnimId_ = 0;
    float mountFootstepLastNormTime_ = 0.0f;
    bool mountFootstepNormInitialized_ = false;
};

} // namespace rendering
} // namespace wowee
