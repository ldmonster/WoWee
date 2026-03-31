#pragma once

#include "core/window.hpp"
#include "core/input.hpp"
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
namespace audio { enum class VoiceType; }
namespace addons { class AddonManager; }

namespace core {

enum class AppState {
    AUTHENTICATION,
    REALM_SELECTION,
    CHARACTER_CREATION,
    CHARACTER_SELECTION,
    IN_GAME,
    DISCONNECTED
};

class Application {
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

    // Weapon loading (called at spawn and on equipment change)
    void loadEquippedWeapons();
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    // Logout to login screen
    void logoutToLogin();

    // Render bounds lookup (for click targeting / selection)
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Character skin composite state (saved at spawn for re-compositing on equipment change)
    const std::string& getBodySkinPath() const { return bodySkinPath_; }
    const std::vector<std::string>& getUnderwearPaths() const { return underwearPaths_; }
    uint32_t getSkinTextureSlotIndex() const { return skinTextureSlotIndex_; }
    uint32_t getCloakTextureSlotIndex() const { return cloakTextureSlotIndex_; }
    uint32_t getGryphonDisplayId() const { return gryphonDisplayId_; }
    uint32_t getWyvernDisplayId() const { return wyvernDisplayId_; }

private:
    void update(float deltaTime);
    void render();
    void setupUICallbacks();
    void spawnPlayerCharacter();
    std::string getPlayerModelPath() const;
    static const char* mapIdToName(uint32_t mapId);
    static const char* mapDisplayName(uint32_t mapId);
    void loadOnlineWorldTerrain(uint32_t mapId, float x, float y, float z);
    void buildFactionHostilityMap(uint8_t playerRace);
    pipeline::M2Model loadCreatureM2Sync(const std::string& m2Path);
    void spawnOnlineCreature(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale = 1.0f);
    void despawnOnlineCreature(uint64_t guid);
    bool tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId);
    void spawnOnlinePlayer(uint64_t guid,
                           uint8_t raceId,
                           uint8_t genderId,
                           uint32_t appearanceBytes,
                           uint8_t facialFeatures,
                           float x, float y, float z, float orientation);
    void setOnlinePlayerEquipment(uint64_t guid,
                                  const std::array<uint32_t, 19>& displayInfoIds,
                                  const std::array<uint8_t, 19>& inventoryTypes);
    void despawnOnlinePlayer(uint64_t guid);
    void buildCreatureDisplayLookups();
    std::string getModelPathForDisplayId(uint32_t displayId) const;
    void spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale = 1.0f);
    void despawnOnlineGameObject(uint64_t guid);
    void buildGameObjectDisplayLookups();
    std::string getGameObjectModelPathForDisplayId(uint32_t displayId) const;
    audio::VoiceType detectVoiceTypeFromDisplayId(uint32_t displayId) const;
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

    AppState state = AppState::AUTHENTICATION;
    bool running = false;
    std::string pendingCreatedCharacterName_;  // Auto-select after character creation
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

    // Weapon model ID counter (starting high to avoid collision with character model IDs)
    uint32_t nextWeaponModelId_ = 1000;

    // Saved at spawn for skin re-compositing
    std::string bodySkinPath_;
    std::vector<std::string> underwearPaths_;
    uint32_t skinTextureSlotIndex_ = 0;
    uint32_t cloakTextureSlotIndex_ = 0;

    // Online creature model spawning
    struct CreatureDisplayData {
        uint32_t modelId = 0;
        std::string skin1, skin2, skin3;  // Texture names from CreatureDisplayInfo.dbc
        uint32_t extraDisplayId = 0;      // Link to CreatureDisplayInfoExtra.dbc
    };
    struct HumanoidDisplayExtra {
        uint8_t raceId = 0;
        uint8_t sexId = 0;
        uint8_t skinId = 0;
        uint8_t faceId = 0;
        uint8_t hairStyleId = 0;
        uint8_t hairColorId = 0;
        uint8_t facialHairId = 0;
        std::string bakeName;  // Pre-baked texture path if available
        // Equipment display IDs (from columns 8-18)
        // 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
        uint32_t equipDisplayId[11] = {0};
    };
    std::unordered_map<uint32_t, CreatureDisplayData> displayDataMap_;  // displayId → display data
    std::unordered_map<uint32_t, HumanoidDisplayExtra> humanoidExtraMap_;  // extraDisplayId → humanoid data
    std::unordered_map<uint32_t, std::string> modelIdToPath_;   // modelId → M2 path (from CreatureModelData.dbc)
    // CharHairGeosets.dbc: key = (raceId<<16)|(sexId<<8)|variationId → geosetId (skinSectionId)
    std::unordered_map<uint32_t, uint16_t> hairGeosetMap_;
    // CharFacialHairStyles.dbc: key = (raceId<<16)|(sexId<<8)|variationId → {geoset100, geoset300, geoset200}
    struct FacialHairGeosets { uint16_t geoset100 = 0; uint16_t geoset300 = 0; uint16_t geoset200 = 0; };
    std::unordered_map<uint32_t, FacialHairGeosets> facialHairGeosetMap_;
    std::unordered_map<uint64_t, uint32_t> creatureInstances_;  // guid → render instanceId
    std::unordered_map<uint64_t, uint32_t> creatureModelIds_;   // guid → loaded modelId
    std::unordered_map<uint64_t, glm::vec3> creatureRenderPosCache_; // guid -> last synced render position
    std::unordered_map<uint64_t, bool> creatureWasMoving_;        // guid -> previous-frame movement state
    std::unordered_map<uint64_t, bool> creatureWasSwimming_;     // guid -> previous-frame swim state (for anim transition detection)
    std::unordered_map<uint64_t, bool> creatureWasFlying_;       // guid -> previous-frame flying state (for anim transition detection)
    std::unordered_map<uint64_t, bool> creatureWasWalking_;      // guid -> previous-frame walking state (walk vs run transition detection)
    std::unordered_map<uint64_t, bool> creatureSwimmingState_;   // guid -> currently in swim mode (SWIMMING flag)
    std::unordered_map<uint64_t, bool> creatureWalkingState_;    // guid -> walking (WALKING flag, selects Walk(4) vs Run(5))
    std::unordered_map<uint64_t, bool> creatureFlyingState_;     // guid -> currently flying (FLYING flag)
    std::unordered_set<uint64_t> creatureWeaponsAttached_;       // guid set when NPC virtual weapons attached
    std::unordered_map<uint64_t, uint8_t> creatureWeaponAttachAttempts_; // guid -> attach attempts
    std::unordered_map<uint32_t, bool> modelIdIsWolfLike_;     // modelId → cached wolf/worg check
    static constexpr int MAX_WEAPON_ATTACHES_PER_TICK = 2;     // limit weapon attach work per 1s tick

    // CharSections.dbc lookup cache to avoid O(N) DBC scan per NPC spawn.
    // Key: (race<<24)|(sex<<16)|(section<<12)|(variation<<8)|color → texture path
    std::unordered_map<uint64_t, std::string> charSectionsCache_;
    bool charSectionsCacheBuilt_ = false;
    void buildCharSectionsCache();
    std::string lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                  uint8_t variation, uint8_t color, int texIndex = 0) const;

    // Async creature model loading: file I/O + M2 parsing on background thread,
    // GPU upload + instance creation on main thread.
    struct PreparedCreatureModel {
        uint64_t guid;
        uint32_t displayId;
        uint32_t modelId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::M2Model> model; // parsed on background thread
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures; // decoded on bg thread
        bool valid = false;
        bool permanent_failure = false;
    };
    struct AsyncCreatureLoad {
        std::future<PreparedCreatureModel> future;
    };
    std::vector<AsyncCreatureLoad> asyncCreatureLoads_;
    std::unordered_set<uint32_t> asyncCreatureDisplayLoads_; // displayIds currently loading in background
    void processAsyncCreatureResults(bool unlimited = false);
    static constexpr int MAX_ASYNC_CREATURE_LOADS = 4; // concurrent background loads
    std::unordered_set<uint64_t> deadCreatureGuids_;            // GUIDs that should spawn in corpse/death pose
    std::unordered_map<uint32_t, uint32_t> displayIdModelCache_; // displayId → modelId (model caching)
    std::unordered_set<uint32_t> displayIdTexturesApplied_;    // displayIds with per-model textures applied
    std::unordered_map<uint32_t, std::unordered_map<std::string, pipeline::BLPImage>> displayIdPredecodedTextures_; // displayId → pre-decoded skin textures
    mutable std::unordered_set<uint32_t> warnedMissingDisplayDataIds_; // displayIds already warned
    mutable std::unordered_set<uint32_t> warnedMissingModelPathIds_;   // modelIds/displayIds already warned
    uint32_t nextCreatureModelId_ = 5000;  // Model IDs for online creatures
    uint32_t gryphonDisplayId_ = 0;
    uint32_t wyvernDisplayId_ = 0;
    bool lastTaxiFlight_ = false;
    uint32_t loadedMapId_ = 0xFFFFFFFF;  // Map ID of currently loaded terrain (0xFFFFFFFF = none)
    uint32_t worldLoadGeneration_ = 0;   // Incremented on each world entry to detect re-entrant loads
    bool loadingWorld_ = false;          // True while loadOnlineWorldTerrain is running
    struct PendingWorldEntry {
        uint32_t mapId; float x, y, z;
    };
    std::optional<PendingWorldEntry> pendingWorldEntry_;  // Deferred world entry during loading
    float taxiLandingClampTimer_ = 0.0f;
    float worldEntryMovementGraceTimer_ = 0.0f;

    // Hearth teleport: freeze player until terrain loads at destination
    bool hearthTeleportPending_ = false;
    glm::vec3 hearthTeleportPos_{0.0f};  // render coords
    float hearthTeleportTimer_ = 0.0f;   // timeout safety
    float facingSendCooldown_ = 0.0f;        // Rate-limits MSG_MOVE_SET_FACING
    float lastSentCanonicalYaw_ = 1000.0f;   // Sentinel — triggers first send
    float taxiStreamCooldown_ = 0.0f;
    bool idleYawned_ = false;

    // Charge rush state
    bool chargeActive_ = false;
    float chargeTimer_ = 0.0f;
    float chargeDuration_ = 0.0f;
    glm::vec3 chargeStartPos_{0.0f};  // Render coordinates
    glm::vec3 chargeEndPos_{0.0f};    // Render coordinates
    uint64_t chargeTargetGuid_ = 0;

    // Online gameobject model spawning
    struct GameObjectInstanceInfo {
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        bool isWmo = false;
    };
    std::unordered_map<uint32_t, std::string> gameObjectDisplayIdToPath_;
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdModelCache_; // displayId → M2 modelId
    std::unordered_set<uint32_t> gameObjectDisplayIdFailedCache_;           // displayIds that permanently fail to load
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdWmoCache_;   // displayId → WMO modelId
    std::unordered_map<uint64_t, GameObjectInstanceInfo> gameObjectInstances_; // guid → instance info
    struct PendingTransportMove {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
    };
    struct PendingTransportRegistration {
        uint64_t guid = 0;
        uint32_t entry = 0;
        uint32_t displayId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
    };
    std::unordered_map<uint64_t, PendingTransportMove> pendingTransportMoves_; // guid -> latest pre-registration move
    std::deque<PendingTransportRegistration> pendingTransportRegistrations_;
    uint32_t nextGameObjectModelId_ = 20000;
    uint32_t nextGameObjectWmoModelId_ = 40000;
    bool testTransportSetup_ = false;
    bool gameObjectLookupsBuilt_ = false;

    // Mount model tracking
    uint32_t mountInstanceId_ = 0;
    uint32_t mountModelId_ = 0;
    uint32_t pendingMountDisplayId_ = 0;  // Deferred mount load (0 = none pending)
    bool weaponsSheathed_ = false;
    bool wasAutoAttacking_ = false;
    void processPendingMount();
    bool creatureLookupsBuilt_ = false;
    bool mapNameCacheLoaded_ = false;
    std::unordered_map<uint32_t, std::string> mapNameById_;

    // Deferred creature spawn queue (throttles spawning to avoid hangs)
    struct PendingCreatureSpawn {
        uint64_t guid;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingCreatureSpawn> pendingCreatureSpawns_;
    static constexpr int MAX_SPAWNS_PER_FRAME = 3;
    static constexpr int MAX_NEW_CREATURE_MODELS_PER_FRAME = 1;
    static constexpr uint16_t MAX_CREATURE_SPAWN_RETRIES = 300;
    std::unordered_set<uint64_t> pendingCreatureSpawnGuids_;
    std::unordered_map<uint64_t, uint16_t> creatureSpawnRetryCounts_;
    std::unordered_set<uint32_t> nonRenderableCreatureDisplayIds_;

    // Online player instances (separate from creatures so we can apply per-player skin/hair textures).
    std::unordered_map<uint64_t, uint32_t> playerInstances_;  // guid → render instanceId
    struct OnlinePlayerAppearanceState {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
        uint8_t raceId = 0;
        uint8_t genderId = 0;
        uint32_t appearanceBytes = 0;
        uint8_t facialFeatures = 0;
        std::string bodySkinPath;
        std::vector<std::string> underwearPaths;
    };
    std::unordered_map<uint64_t, OnlinePlayerAppearanceState> onlinePlayerAppearance_;
    std::unordered_map<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>> pendingOnlinePlayerEquipment_;
    // Deferred equipment compositing queue — processes max 1 per frame to avoid stutter
    // deque: consumed from front in a loop; vector::erase(begin) would be O(n²).
    std::deque<std::pair<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>>> deferredEquipmentQueue_;
    void processDeferredEquipmentQueue();
    // Async equipment texture pre-decode: BLP decode on background thread, composite on main thread
    struct PreparedEquipmentUpdate {
        uint64_t guid;
        std::array<uint32_t, 19> displayInfoIds;
        std::array<uint8_t, 19> inventoryTypes;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncEquipmentLoad {
        std::future<PreparedEquipmentUpdate> future;
    };
    std::vector<AsyncEquipmentLoad> asyncEquipmentLoads_;
    void processAsyncEquipmentResults();
    std::vector<std::string> resolveEquipmentTexturePaths(uint64_t guid,
        const std::array<uint32_t, 19>& displayInfoIds,
        const std::array<uint8_t, 19>& inventoryTypes) const;
    // Deferred NPC texture setup — async DBC lookups + BLP pre-decode to avoid main-thread stalls
    struct DeferredNpcComposite {
        uint32_t modelId;
        uint32_t displayId;
        // Skin compositing (type-1 slots)
        std::string basePath;                     // CharSections skin base texture
        std::vector<std::string> overlayPaths;    // face + underwear overlays
        std::vector<std::pair<int, std::string>> regionLayers;  // equipment region overlays
        std::vector<uint32_t> skinTextureSlots;   // model texture slots needing skin composite
        bool hasComposite = false;                // needs compositing (overlays or equipment regions)
        bool hasSimpleSkin = false;               // just base skin, no compositing needed
        // Baked skin (type-1 slots)
        std::string bakedSkinPath;                // baked texture path (if available)
        bool hasBakedSkin = false;                // baked skin resolved successfully
        // Hair (type-6 slots)
        std::vector<uint32_t> hairTextureSlots;   // model texture slots needing hair texture
        std::string hairTexturePath;              // resolved hair texture path
        bool useBakedForHair = false;             // bald NPC: use baked skin for type-6
    };
    struct PreparedNpcComposite {
        DeferredNpcComposite info;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncNpcCompositeLoad {
        std::future<PreparedNpcComposite> future;
    };
    std::vector<AsyncNpcCompositeLoad> asyncNpcCompositeLoads_;
    void processAsyncNpcCompositeResults(bool unlimited = false);
    // Cache base player model geometry by (raceId, genderId)
    std::unordered_map<uint32_t, uint32_t> playerModelCache_; // key=(race<<8)|gender → modelId
    struct PlayerTextureSlots { int skin = -1; int hair = -1; int underwear = -1; };
    std::unordered_map<uint32_t, PlayerTextureSlots> playerTextureSlotsByModelId_;
    uint32_t nextPlayerModelId_ = 60000;
    struct PendingPlayerSpawn {
        uint64_t guid;
        uint8_t raceId;
        uint8_t genderId;
        uint32_t appearanceBytes;
        uint8_t facialFeatures;
        float x, y, z, orientation;
    };
    std::deque<PendingPlayerSpawn> pendingPlayerSpawns_;
    std::unordered_set<uint64_t> pendingPlayerSpawnGuids_;
    void processPlayerSpawnQueue();
    std::unordered_set<uint64_t> creaturePermanentFailureGuids_;
    void processCreatureSpawnQueue(bool unlimited = false);

    struct PendingGameObjectSpawn {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingGameObjectSpawn> pendingGameObjectSpawns_;
    void processGameObjectSpawnQueue();

    // Async WMO loading for game objects (file I/O + parse on background thread)
    struct PreparedGameObjectWMO {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::WMOModel> wmoModel;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures; // decoded on bg thread
        bool valid = false;
        bool isWmo = false;
        std::string modelPath;
    };
    struct AsyncGameObjectLoad {
        std::future<PreparedGameObjectWMO> future;
    };
    std::vector<AsyncGameObjectLoad> asyncGameObjectLoads_;
    void processAsyncGameObjectResults();
    struct PendingTransportDoodadBatch {
        uint64_t guid = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        size_t nextIndex = 0;
        size_t doodadBudget = 0;
        size_t spawnedDoodads = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
    };
    std::vector<PendingTransportDoodadBatch> pendingTransportDoodadBatches_;
    static constexpr size_t MAX_TRANSPORT_DOODADS_PER_FRAME = 4;
    void processPendingTransportRegistrations();
    void processPendingTransportDoodads();

    // Quest marker billboard sprites (above NPCs)
    void loadQuestMarkerModels();  // Now loads BLP textures
    void updateQuestMarkers();     // Updates billboard positions

    // Background world preloader — warms AssetManager file cache for the
    // expected world before the user clicks Enter World.
    struct WorldPreload {
        uint32_t mapId = 0;
        std::string mapName;
        int centerTileX = 0;
        int centerTileY = 0;
        std::atomic<bool> cancel{false};
        std::vector<std::thread> workers;
    };
    std::unique_ptr<WorldPreload> worldPreload_;
    void startWorldPreload(uint32_t mapId, const std::string& mapName, float serverX, float serverY);
    void cancelWorldPreload();
    void saveLastWorldInfo(uint32_t mapId, const std::string& mapName, float serverX, float serverY);
    struct LastWorldInfo { uint32_t mapId = 0; std::string mapName; float x = 0, y = 0; bool valid = false; };
    LastWorldInfo loadLastWorldInfo() const;
};

} // namespace core
} // namespace wowee
