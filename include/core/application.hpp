#pragma once

#include "core/window.hpp"
#include "core/input.hpp"
#include "core/entity_spawner.hpp"
#include "core/appearance_composer.hpp"
#include "core/world_loader.hpp"
#include "game/character.hpp"
#include "game/game_services.hpp"
#include "pipeline/blp_loader.hpp"
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <optional>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>

namespace wowee {

// Forward declarations
namespace rendering { class Renderer; }
namespace ui { class UIManager; }
namespace auth { class AuthHandler; }
namespace game { class GameHandler; class World; class ExpansionRegistry; }
namespace pipeline { class AssetManager; class DBCLayout; struct M2Model; struct WMOModel; }
namespace audio { enum class VoiceType; class AudioCoordinator; }
namespace addons { class AddonManager; }

namespace core {

// Handler forward declarations
class NPCInteractionCallbackHandler;
class AudioCallbackHandler;
class EntitySpawnCallbackHandler;
class AnimationCallbackHandler;
class TransportCallbackHandler;
class WorldEntryCallbackHandler;
class UIScreenCallbackHandler;

enum class AppState {
    AUTHENTICATION,
    REALM_SELECTION,
    CHARACTER_CREATION,
    CHARACTER_SELECTION,
    IN_GAME,
    DISCONNECTED
};

class Application {
    friend class WorldLoader;

public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize();
    void run();
    void shutdown();

    // State management
    AppState getState() const { return state; }
    void setState(AppState newState);

    // Accessors
    Window* getWindow() { return window.get(); }
    rendering::Renderer* getRenderer() { return renderer.get(); }
    ui::UIManager* getUIManager() { return uiManager.get(); }
    auth::AuthHandler* getAuthHandler() { return authHandler.get(); }
    game::GameHandler* getGameHandler() { return gameHandler.get(); }
    game::World* getWorld() { return world.get(); }
    pipeline::AssetManager* getAssetManager() { return assetManager.get(); }
    addons::AddonManager* getAddonManager() { return addonManager_.get(); }
    game::ExpansionRegistry* getExpansionRegistry() { return expansionRegistry_.get(); }
    pipeline::DBCLayout* getDBCLayout() { return dbcLayout_.get(); }
    void reloadExpansionData(); // Reload DBC layouts, opcodes, etc. after expansion change

    // Singleton access
    static Application& getInstance() { return *instance; }



    // Logout to login screen
    void logoutToLogin();

    // Render bounds lookup (for click targeting / selection) — delegates to EntitySpawner
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Character skin composite state — delegated to AppearanceComposer
    const std::string& getBodySkinPath() const { return appearanceComposer_ ? appearanceComposer_->getBodySkinPath() : emptyString_; }
    const std::vector<std::string>& getUnderwearPaths() const { return appearanceComposer_ ? appearanceComposer_->getUnderwearPaths() : emptyStringVec_; }
    uint32_t getSkinTextureSlotIndex() const { return appearanceComposer_ ? appearanceComposer_->getSkinTextureSlotIndex() : 0; }
    uint32_t getCloakTextureSlotIndex() const { return appearanceComposer_ ? appearanceComposer_->getCloakTextureSlotIndex() : 0; }
    uint32_t getGryphonDisplayId() const { return entitySpawner_ ? entitySpawner_->getGryphonDisplayId() : 0; }
    uint32_t getWyvernDisplayId() const { return entitySpawner_ ? entitySpawner_->getWyvernDisplayId() : 0; }

    // Entity spawner access
    EntitySpawner* getEntitySpawner() { return entitySpawner_.get(); }

    // Appearance composer access
    AppearanceComposer* getAppearanceComposer() { return appearanceComposer_.get(); }

    // World loader access
    WorldLoader* getWorldLoader() { return worldLoader_.get(); }

    // Audio coordinator access
    audio::AudioCoordinator* getAudioCoordinator() { return audioCoordinator_.get(); }

private:
    void update(float deltaTime);
    void render();
    void setupUICallbacks();
    void spawnPlayerCharacter();
    void buildFactionHostilityMap(uint8_t playerRace);
    void setupTestTransport();  // Test transport boat for development

    static Application* instance;

    game::GameServices gameServices_;
    std::unique_ptr<Window> window;
    std::unique_ptr<rendering::Renderer> renderer;
    std::unique_ptr<ui::UIManager> uiManager;
    std::unique_ptr<auth::AuthHandler> authHandler;
    std::unique_ptr<game::GameHandler> gameHandler;
    std::unique_ptr<game::World> world;
    std::unique_ptr<pipeline::AssetManager> assetManager;
    std::unique_ptr<addons::AddonManager> addonManager_;
    bool addonsLoaded_ = false;
    std::unique_ptr<game::ExpansionRegistry> expansionRegistry_;
    std::unique_ptr<pipeline::DBCLayout> dbcLayout_;
    std::unique_ptr<EntitySpawner> entitySpawner_;
    std::unique_ptr<AppearanceComposer> appearanceComposer_;
    std::unique_ptr<WorldLoader> worldLoader_;
    std::unique_ptr<audio::AudioCoordinator> audioCoordinator_;

    // Callback handlers (extracted from setupUICallbacks)
    std::unique_ptr<NPCInteractionCallbackHandler> npcInteractionCallbacks_;
    std::unique_ptr<AudioCallbackHandler> audioCallbacks_;
    std::unique_ptr<EntitySpawnCallbackHandler> entitySpawnCallbacks_;
    std::unique_ptr<AnimationCallbackHandler> animationCallbacks_;
    std::unique_ptr<TransportCallbackHandler> transportCallbacks_;
    std::unique_ptr<WorldEntryCallbackHandler> worldEntryCallbacks_;
    std::unique_ptr<UIScreenCallbackHandler> uiScreenCallbacks_;

    AppState state = AppState::AUTHENTICATION;
    bool running = false;
    bool playerCharacterSpawned = false;
    bool npcsSpawned = false;
    bool spawnSnapToGround = true;
    float lastFrameTime = 0.0f;

    // Player character info (for model spawning)
    game::Race playerRace_ = game::Race::HUMAN;
    game::Gender playerGender_ = game::Gender::MALE;
    game::Class playerClass_ = game::Class::WARRIOR;
    uint64_t spawnedPlayerGuid_ = 0;
    uint32_t spawnedAppearanceBytes_ = 0;
    uint8_t spawnedFacialFeatures_ = 0;

    // Static empty values for null-safe delegation
    static inline const std::string emptyString_;
    static inline const std::vector<std::string> emptyStringVec_;

    float facingSendCooldown_ = 0.0f;        // Rate-limits MSG_MOVE_SET_FACING
    float lastSentCanonicalYaw_ = 1000.0f;   // Sentinel — triggers first send
    float taxiStreamCooldown_ = 0.0f;
    bool idleYawned_ = false;

    bool wasAutoAttacking_ = false;

    // Quest marker billboard sprites (above NPCs)
    void loadQuestMarkerModels();  // Now loads BLP textures
    void updateQuestMarkers();     // Updates billboard positions
};

} // namespace core
} // namespace wowee
