#include "rendering/animation/footstep_driver.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/swim_effects.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/movement_sound_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wowee {
namespace rendering {

// ── Footstep event detection (moved from AnimationController) ────────────────

bool FootstepDriver::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs) {
    if (animationDurationMs <= 1.0f) {
        footstepNormInitialized_ = false;
        return false;
    }

    float wrappedTime = animationTimeMs;
    while (wrappedTime >= animationDurationMs) {
        wrappedTime -= animationDurationMs;
    }
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    float norm = wrappedTime / animationDurationMs;

    if (animationId != footstepLastAnimationId_) {
        footstepLastAnimationId_ = animationId;
        footstepLastNormTime_ = norm;
        footstepNormInitialized_ = true;
        return false;
    }

    if (!footstepNormInitialized_) {
        footstepNormInitialized_ = true;
        footstepLastNormTime_ = norm;
        return false;
    }

    auto crossed = [&](float eventNorm) {
        if (footstepLastNormTime_ <= norm) {
            return footstepLastNormTime_ < eventNorm && eventNorm <= norm;
        }
        return footstepLastNormTime_ < eventNorm || eventNorm <= norm;
    };

    bool trigger = crossed(0.22f) || crossed(0.72f);
    footstepLastNormTime_ = norm;
    return trigger;
}

audio::FootstepSurface FootstepDriver::resolveFootstepSurface(Renderer* renderer) const {
    auto* cameraController = renderer->getCameraController();
    if (!cameraController || !cameraController->isThirdPerson()) {
        return audio::FootstepSurface::STONE;
    }

    const glm::vec3& p = renderer->getCharacterPosition();

    float distSq = glm::dot(p - cachedFootstepPosition_, p - cachedFootstepPosition_);
    if (distSq < 2.25f && cachedFootstepUpdateTimer_ < 0.5f) {
        return cachedFootstepSurface_;
    }

    cachedFootstepPosition_ = p;
    cachedFootstepUpdateTimer_ = 0.0f;

    if (cameraController->isSwimming()) {
        cachedFootstepSurface_ = audio::FootstepSurface::WATER;
        return audio::FootstepSurface::WATER;
    }

    auto* waterRenderer = renderer->getWaterRenderer();
    if (waterRenderer) {
        auto waterH = waterRenderer->getWaterHeightAt(p.x, p.y);
        if (waterH && p.z < (*waterH + 0.25f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::WATER;
            return audio::FootstepSurface::WATER;
        }
    }

    auto* wmoRenderer = renderer->getWMORenderer();
    auto* terrainManager = renderer->getTerrainManager();
    if (wmoRenderer) {
        auto wmoFloor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
        auto terrainFloor = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
        if (wmoFloor && (!terrainFloor || *wmoFloor >= *terrainFloor - 0.1f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::STONE;
            return audio::FootstepSurface::STONE;
        }
    }

    audio::FootstepSurface surface = audio::FootstepSurface::STONE;

    if (terrainManager) {
        auto texture = terrainManager->getDominantTextureAt(p.x, p.y);
        if (texture) {
            std::string t = *texture;
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t.find("snow") != std::string::npos || t.find("ice") != std::string::npos) surface = audio::FootstepSurface::SNOW;
            else if (t.find("grass") != std::string::npos || t.find("moss") != std::string::npos || t.find("leaf") != std::string::npos) surface = audio::FootstepSurface::GRASS;
            else if (t.find("sand") != std::string::npos || t.find("dirt") != std::string::npos || t.find("mud") != std::string::npos) surface = audio::FootstepSurface::DIRT;
            else if (t.find("wood") != std::string::npos || t.find("timber") != std::string::npos) surface = audio::FootstepSurface::WOOD;
            else if (t.find("metal") != std::string::npos || t.find("iron") != std::string::npos) surface = audio::FootstepSurface::METAL;
            else if (t.find("stone") != std::string::npos || t.find("rock") != std::string::npos || t.find("cobble") != std::string::npos || t.find("brick") != std::string::npos) surface = audio::FootstepSurface::STONE;
        }
    }

    cachedFootstepSurface_ = surface;
    return surface;
}

// ── Footstep update (moved from AnimationController::updateFootsteps) ────────

void FootstepDriver::update(float deltaTime, Renderer* renderer,
                             bool mounted, uint32_t mountInstanceId, bool taxiFlight,
                             bool isFootstepState) {
    auto* footstepManager = renderer->getAudioCoordinator()->getFootstepManager();
    if (!footstepManager) return;

    auto* characterRenderer = renderer->getCharacterRenderer();
    auto* cameraController = renderer->getCameraController();
    uint32_t characterInstanceId = renderer->getCharacterInstanceId();

    footstepManager->update(deltaTime);
    cachedFootstepUpdateTimer_ += deltaTime;

    bool canPlayFootsteps = characterRenderer && characterInstanceId > 0 &&
        cameraController && cameraController->isThirdPerson() &&
        cameraController->isGrounded() && !cameraController->isSwimming();

    if (canPlayFootsteps && mounted && mountInstanceId > 0 && !taxiFlight) {
        // Mount footsteps: use mount's animation for timing
        uint32_t animId = 0;
        float animTimeMs = 0.0f, animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(mountInstanceId, animId, animTimeMs, animDurationMs) &&
            animDurationMs > 1.0f && cameraController->isMoving()) {
            float wrappedTime = animTimeMs;
            while (wrappedTime >= animDurationMs) {
                wrappedTime -= animDurationMs;
            }
            if (wrappedTime < 0.0f) wrappedTime += animDurationMs;
            float norm = wrappedTime / animDurationMs;

            if (animId != mountFootstepLastAnimId_) {
                mountFootstepLastAnimId_ = animId;
                mountFootstepLastNormTime_ = norm;
                mountFootstepNormInitialized_ = true;
            } else if (!mountFootstepNormInitialized_) {
                mountFootstepNormInitialized_ = true;
                mountFootstepLastNormTime_ = norm;
            } else {
                auto crossed = [&](float eventNorm) {
                    if (mountFootstepLastNormTime_ <= norm) {
                        return mountFootstepLastNormTime_ < eventNorm && eventNorm <= norm;
                    }
                    return mountFootstepLastNormTime_ < eventNorm || eventNorm <= norm;
                };
                if (crossed(0.25f) || crossed(0.75f)) {
                    footstepManager->playFootstep(resolveFootstepSurface(renderer), true);
                }
                mountFootstepLastNormTime_ = norm;
            }
        } else {
            mountFootstepNormInitialized_ = false;
        }
        footstepNormInitialized_ = false;
    } else if (canPlayFootsteps && isFootstepState) {
        uint32_t animId = 0;
        float animTimeMs = 0.0f;
        float animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(characterInstanceId, animId, animTimeMs, animDurationMs) &&
            shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs)) {
            auto surface = resolveFootstepSurface(renderer);
            footstepManager->playFootstep(surface, cameraController->isSprinting());
            if (surface == audio::FootstepSurface::WATER) {
                if (renderer->getAudioCoordinator()->getMovementSoundManager()) {
                    renderer->getAudioCoordinator()->getMovementSoundManager()->playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
                }
                auto* swimEffects = renderer->getSwimEffects();
                auto* waterRenderer = renderer->getWaterRenderer();
                if (swimEffects && waterRenderer) {
                    const glm::vec3& characterPosition = renderer->getCharacterPosition();
                    auto wh = waterRenderer->getWaterHeightAt(characterPosition.x, characterPosition.y);
                    if (wh) {
                        swimEffects->spawnFootSplash(characterPosition, *wh);
                    }
                }
            }
        }
        mountFootstepNormInitialized_ = false;
    } else {
        footstepNormInitialized_ = false;
        mountFootstepNormInitialized_ = false;
    }
}

} // namespace rendering
} // namespace wowee
