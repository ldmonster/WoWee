#pragma once

#include <memory>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace audio {

class MusicManager;
class FootstepManager;
class ActivitySoundManager;
class MountSoundManager;
class NpcVoiceManager;
class AmbientSoundManager;
class UiSoundManager;
class CombatSoundManager;
class SpellSoundManager;
class MovementSoundManager;

/// Coordinates all audio subsystems.
/// Extracted from Renderer to separate audio lifecycle from rendering.
/// Owned by Application; Renderer and UI components access through Application.
class AudioCoordinator {
public:
    AudioCoordinator();
    ~AudioCoordinator();

    /// Initialize the audio engine and all managers.
    /// @return true if audio is available (engine initialized successfully)
    bool initialize();

    /// Initialize managers that need AssetManager (music lookups, sound banks).
    void initializeWithAssets(pipeline::AssetManager* assetManager);

    /// Shutdown all audio managers and engine.
    void shutdown();

    // Accessors for all audio managers (same interface as Renderer had)
    MusicManager* getMusicManager() { return musicManager_.get(); }
    FootstepManager* getFootstepManager() { return footstepManager_.get(); }
    ActivitySoundManager* getActivitySoundManager() { return activitySoundManager_.get(); }
    MountSoundManager* getMountSoundManager() { return mountSoundManager_.get(); }
    NpcVoiceManager* getNpcVoiceManager() { return npcVoiceManager_.get(); }
    AmbientSoundManager* getAmbientSoundManager() { return ambientSoundManager_.get(); }
    UiSoundManager* getUiSoundManager() { return uiSoundManager_.get(); }
    CombatSoundManager* getCombatSoundManager() { return combatSoundManager_.get(); }
    SpellSoundManager* getSpellSoundManager() { return spellSoundManager_.get(); }
    MovementSoundManager* getMovementSoundManager() { return movementSoundManager_.get(); }

private:
    std::unique_ptr<MusicManager> musicManager_;
    std::unique_ptr<FootstepManager> footstepManager_;
    std::unique_ptr<ActivitySoundManager> activitySoundManager_;
    std::unique_ptr<MountSoundManager> mountSoundManager_;
    std::unique_ptr<NpcVoiceManager> npcVoiceManager_;
    std::unique_ptr<AmbientSoundManager> ambientSoundManager_;
    std::unique_ptr<UiSoundManager> uiSoundManager_;
    std::unique_ptr<CombatSoundManager> combatSoundManager_;
    std::unique_ptr<SpellSoundManager> spellSoundManager_;
    std::unique_ptr<MovementSoundManager> movementSoundManager_;

    bool audioAvailable_ = false;
};

} // namespace audio
} // namespace wowee
