#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "audio/npc_voice_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "rendering/animation_ids.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/game_handler.hpp"
#include "game/game_services.hpp"
#include "game/transport_manager.hpp"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cstring>

namespace wowee {
namespace core {

namespace {
// Default (bare) geoset IDs per equipment group.
// Each group's base is groupNumber * 100; variant 01 is typically bare/default.
constexpr uint16_t kGeosetDefaultConnector = 101;   // Group  1: default hair connector
constexpr uint16_t kGeosetBareForearms     = 401;   // Group  4: no gloves
constexpr uint16_t kGeosetBareShins        = 503;   // Group  5: no boots
constexpr uint16_t kGeosetDefaultEars      = 702;   // Group  7: ears
constexpr uint16_t kGeosetBareSleeves      = 801;   // Group  8: no chest armor sleeves
constexpr uint16_t kGeosetDefaultKneepads  = 902;   // Group  9: kneepads
constexpr uint16_t kGeosetDefaultTabard    = 1201;  // Group 12: tabard base
constexpr uint16_t kGeosetBarePants        = 1301;  // Group 13: no leggings
constexpr uint16_t kGeosetNoCape           = 1501;  // Group 15: no cape
constexpr uint16_t kGeosetWithCape         = 1502;  // Group 15: with cape
constexpr uint16_t kGeosetBareFeet         = 2002;  // Group 20: bare feet
} // namespace

// --- Constructor / Destructor ---

EntitySpawner::EntitySpawner(rendering::Renderer* renderer,
                             pipeline::AssetManager* assetManager,
                             game::GameHandler* gameHandler,
                             pipeline::DBCLayout* dbcLayout,
                             game::GameServices* gameServices)
    : renderer_(renderer)
    , assetManager_(assetManager)
    , gameHandler_(gameHandler)
    , dbcLayout_(dbcLayout)
    , gameServices_(gameServices)
{
}

EntitySpawner::~EntitySpawner() = default;

// --- Lifecycle ---

void EntitySpawner::initialize() {
    buildCharSectionsCache();
    buildCreatureDisplayLookups();
    buildGameObjectDisplayLookups();
}

void EntitySpawner::update() {
    processPlayerSpawnQueue();
    processCreatureSpawnQueue();
    processAsyncNpcCompositeResults();
    processDeferredEquipmentQueue();
    processGameObjectSpawnQueue();
    processPendingTransportRegistrations();
    processPendingTransportDoodads();
    processPendingMount();
}

void EntitySpawner::shutdown() {
    clearAllQueues();
    // Clear all instances
    creatureInstances_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureWeaponsAttached_.clear();
    creatureWeaponAttachAttempts_.clear();
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    gameObjectInstances_.clear();
}

void EntitySpawner::resetAllState() {
    // Wait for in-flight async loads before clearing state
    for (auto& load : asyncCreatureLoads_) {
        if (load.future.valid()) load.future.wait();
    }

    // Despawn all entities (renderer cleanup)
    despawnAllCreatures();
    despawnAllPlayers();
    despawnAllGameObjects();
    clearMountState();

    // Clear all queues and async loads
    clearAllQueues();

    // Clear all instance tracking
    creatureInstances_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    playerInstances_.clear();
    onlinePlayerAppearance_.clear();
    gameObjectInstances_.clear();

    // Clear animation state maps
    creatureWasMoving_.clear();
    creatureWasSwimming_.clear();
    creatureWasFlying_.clear();
    creatureWasWalking_.clear();
    creatureSwimmingState_.clear();
    creatureWalkingState_.clear();
    creatureFlyingState_.clear();
    creatureWeaponsAttached_.clear();
    creatureWeaponAttachAttempts_.clear();
    modelIdIsWolfLike_.clear();

    // Clear display/spawn caches
    nonRenderableCreatureDisplayIds_.clear();
    displayIdTexturesApplied_.clear();
    charSectionsCache_.clear();
    charSectionsCacheBuilt_ = false;

    // Clear GO display caches
    gameObjectDisplayIdModelCache_.clear();
    gameObjectDisplayIdWmoCache_.clear();
    gameObjectDisplayIdFailedCache_.clear();
}

void EntitySpawner::rebuildLookups() {
    creatureLookupsBuilt_ = false;
    displayDataMap_.clear();
    humanoidExtraMap_.clear();
    creatureModelIds_.clear();
    creatureRenderPosCache_.clear();
    nonRenderableCreatureDisplayIds_.clear();
    initialize();
}

bool EntitySpawner::hasWorkPending() const {
    return !pendingCreatureSpawns_.empty() || !asyncCreatureLoads_.empty() ||
           !asyncNpcCompositeLoads_.empty() || !pendingPlayerSpawns_.empty() ||
           !asyncEquipmentLoads_.empty() || !deferredEquipmentQueue_.empty() ||
           !pendingGameObjectSpawns_.empty() || !asyncGameObjectLoads_.empty();
}

void EntitySpawner::clearMountState() {
    if (mountInstanceId_ != 0 && renderer_) {
        if (auto* charRenderer = renderer_->getCharacterRenderer()) {
            charRenderer->removeInstance(mountInstanceId_);
        }
    }
    mountInstanceId_ = 0;
    mountModelId_ = 0;
    pendingMountDisplayId_ = 0;
}

void EntitySpawner::queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                                float x, float y, float z, float orientation) {
    pendingTransportRegistrations_.push_back({guid, entry, displayId, x, y, z, orientation});
}

void EntitySpawner::setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation) {
    pendingTransportMoves_[guid] = {x, y, z, orientation};
}

bool EntitySpawner::hasTransportRegistrationPending(uint64_t guid) const {
    return std::any_of(pendingTransportRegistrations_.begin(), pendingTransportRegistrations_.end(),
                       [guid](const PendingTransportRegistration& reg) { return reg.guid == guid; });
}

void EntitySpawner::updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                                 float x, float y, float z, float orientation) {
    for (auto& reg : pendingTransportRegistrations_) {
        if (reg.guid == guid) {
            reg.displayId = displayId;
            reg.x = x; reg.y = y; reg.z = z; reg.orientation = orientation;
            return;
        }
    }
}

// --- Queue API ---

void EntitySpawner::queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                                        float x, float y, float z, float orientation, float scale) {
    if (creatureInstances_.count(guid)) return;
    if (pendingCreatureSpawnGuids_.count(guid)) return;
    pendingCreatureSpawns_.push_back({guid, displayId, x, y, z, orientation, scale});
    pendingCreatureSpawnGuids_.insert(guid);
}

void EntitySpawner::queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                      uint32_t appearanceBytes, uint8_t facialFeatures,
                                      float x, float y, float z, float orientation) {
    if (playerInstances_.count(guid)) return;
    if (pendingPlayerSpawnGuids_.count(guid)) return;
    pendingPlayerSpawns_.push_back({guid, raceId, genderId, appearanceBytes, facialFeatures, x, y, z, orientation});
    pendingPlayerSpawnGuids_.insert(guid);
}

void EntitySpawner::queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                                          float x, float y, float z, float orientation, float scale) {
    pendingGameObjectSpawns_.push_back({guid, entry, displayId, x, y, z, orientation, scale});
}

void EntitySpawner::queuePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    deferredEquipmentQueue_.push_back({guid, {displayInfoIds, inventoryTypes}});
}

// --- Immediate despawn wrappers ---

void EntitySpawner::clearAllQueues() {
    pendingCreatureSpawns_.clear();
    pendingCreatureSpawnGuids_.clear();
    creatureSpawnRetryCounts_.clear();
    creaturePermanentFailureGuids_.clear();
    deadCreatureGuids_.clear();
    pendingPlayerSpawns_.clear();
    pendingPlayerSpawnGuids_.clear();
    pendingOnlinePlayerEquipment_.clear();
    deferredEquipmentQueue_.clear();
    pendingGameObjectSpawns_.clear();
    pendingTransportRegistrations_.clear();
    pendingTransportMoves_.clear();
    pendingTransportDoodadBatches_.clear();
    asyncCreatureLoads_.clear();
    asyncCreatureDisplayLoads_.clear();
    asyncEquipmentLoads_.clear();
    asyncNpcCompositeLoads_.clear();
    asyncGameObjectLoads_.clear();
}

void EntitySpawner::despawnAllCreatures() {
    std::vector<uint64_t> guids;
    guids.reserve(creatureInstances_.size());
    for (const auto& [g, _] : creatureInstances_) guids.push_back(g);
    for (auto g : guids) despawnCreature(g);
}

void EntitySpawner::despawnAllPlayers() {
    std::vector<uint64_t> guids;
    guids.reserve(playerInstances_.size());
    for (const auto& [g, _] : playerInstances_) guids.push_back(g);
    for (auto g : guids) despawnPlayer(g);
}

void EntitySpawner::despawnAllGameObjects() {
    std::vector<uint64_t> guids;
    guids.reserve(gameObjectInstances_.size());
    for (const auto& [g, _] : gameObjectInstances_) guids.push_back(g);
    for (auto g : guids) despawnGameObject(g);
}

// --- Methods extracted from Application (with comments preserved) ---

bool EntitySpawner::tryAttachCreatureVirtualWeapons(uint64_t guid, uint32_t instanceId) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !gameHandler_) return false;
    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return false;

    auto entity = gameHandler_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != game::ObjectType::UNIT) return false;
    auto unit = std::static_pointer_cast<game::Unit>(entity);
    if (!unit) return false;

    // Virtual weapons are only appropriate for humanoid-style displays.
    // Non-humanoids (wolves/boars/etc.) can expose non-zero virtual item fields
    // and otherwise end up with comedic floating weapons.
    uint32_t displayId = unit->getDisplayId();
    auto dIt = displayDataMap_.find(displayId);
    if (dIt == displayDataMap_.end()) return false;
    uint32_t extraDisplayId = dIt->second.extraDisplayId;
    if (extraDisplayId == 0 || humanoidExtraMap_.find(extraDisplayId) == humanoidExtraMap_.end()) {
        return false;
    }

    auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!itemDisplayDbc) return false;
    // Item.dbc is not distributed to clients in Vanilla 1.12; on those expansions
    // item display IDs are resolved via the server-sent item cache instead.
    auto itemDbc = assetManager_->loadDBCOptional("Item.dbc");
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
    const auto* itemL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("Item") : nullptr;

    auto resolveDisplayInfoId = [&](uint32_t rawId) -> uint32_t {
        if (rawId == 0) return 0;
        // Primary path: AzerothCore uses item entries in UNIT_VIRTUAL_ITEM_SLOT_ID.
        // Resolve strictly through Item.dbc entry -> DisplayID to avoid
        // accidental ItemDisplayInfo ID collisions (staff/hilt mismatches).
        if (itemDbc) {
            int32_t itemRec = itemDbc->findRecordById(rawId); // treat as item entry
            if (itemRec >= 0) {
                const uint32_t dispFieldPrimary = itemL ? (*itemL)["DisplayID"] : 5u;
                uint32_t displayIdA = itemDbc->getUInt32(static_cast<uint32_t>(itemRec), dispFieldPrimary);
                if (displayIdA != 0 && itemDisplayDbc->findRecordById(displayIdA) >= 0) {
                    return displayIdA;
                }
            }
        }
        // Fallback: Vanilla 1.12 does not distribute Item.dbc to clients.
        // Items arrive via SMSG_ITEM_QUERY_SINGLE_RESPONSE and are cached in
        // itemInfoCache_. Use the server-sent displayInfoId when available.
        if (!itemDbc && gameHandler_) {
            if (const auto* info = gameHandler_->getItemInfo(rawId)) {
                uint32_t displayIdB = info->displayInfoId;
                if (displayIdB != 0 && itemDisplayDbc->findRecordById(displayIdB) >= 0) {
                    return displayIdB;
                }
            }
        }
        return 0;
    };

    auto attachNpcWeaponDisplay = [&](uint32_t itemDisplayId, uint32_t attachmentId) -> bool {
        uint32_t resolvedDisplayId = resolveDisplayInfoId(itemDisplayId);
        if (resolvedDisplayId == 0) return false;
        int32_t recIdx = itemDisplayDbc->findRecordById(resolvedDisplayId);
        if (recIdx < 0) return false;

        const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
        const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
        const uint32_t texFieldL = idiL ? (*idiL)["LeftModelTexture"] : 3u;
        const uint32_t texFieldR = idiL ? (*idiL)["RightModelTexture"] : 4u;
        // Prefer LeftModel (stock player equipment path uses LeftModel and avoids
        // the "hilt-only" variants seen when forcing RightModel).
        std::string modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        std::string textureName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), texFieldL);
        if (modelName.empty()) {
            modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
            textureName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), texFieldR);
        }
        if (modelName.empty()) return false;

        std::string modelFile = modelName;
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
        modelFile += ".m2";

        // Main-hand NPC weapon path: only use actual weapon models.
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) return false;

        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) texturePath.clear();
        }

        uint32_t weaponModelId = nextWeaponModelId_++;
        return charRenderer->attachWeapon(instanceId, attachmentId, weaponModel, weaponModelId, texturePath);
    };

    auto hasResolvableWeaponModel = [&](uint32_t itemDisplayId) -> bool {
        uint32_t resolvedDisplayId = resolveDisplayInfoId(itemDisplayId);
        if (resolvedDisplayId == 0) return false;
        int32_t recIdx = itemDisplayDbc->findRecordById(resolvedDisplayId);
        if (recIdx < 0) return false;
        const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
        const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
        std::string modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        if (modelName.empty()) {
            modelName = itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
        }
        if (modelName.empty()) return false;
        std::string modelFile = modelName;
        size_t dotPos = modelFile.rfind('.');
        if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
        modelFile += ".m2";
        return assetManager_->fileExists("Item\\ObjectComponents\\Weapon\\" + modelFile);
    };

    bool attachedMain = false;
    bool hadWeaponCandidate = false;

    const uint16_t candidateBases[] = {56, 57, 58, 70, 148, 149, 150, 151, 152};
    for (uint16_t base : candidateBases) {
        uint32_t v0 = entity->getField(static_cast<uint16_t>(base + 0));
        if (v0 != 0) hadWeaponCandidate = true;
        if (!attachedMain && v0 != 0) attachedMain = attachNpcWeaponDisplay(v0, 1);
        if (attachedMain) break;
    }

    uint16_t unitEnd = game::fieldIndex(game::UF::UNIT_END);
    uint16_t scanLo = 60;
    uint16_t scanHi = (unitEnd != 0xFFFF) ? static_cast<uint16_t>(unitEnd + 96) : 320;
    std::map<uint16_t, uint32_t> candidateByIndex;
    for (const auto& [idx, val] : entity->getFields()) {
        if (idx < scanLo || idx > scanHi) continue;
        if (val == 0) continue;
        if (hasResolvableWeaponModel(val)) {
            candidateByIndex[idx] = val;
            hadWeaponCandidate = true;
        }
    }
    for (const auto& [idx, val] : candidateByIndex) {
        if (!attachedMain) attachedMain = attachNpcWeaponDisplay(val, 1);
        if (attachedMain) break;
    }

    // Force off-hand clear in NPC path to avoid incorrect shields/placeholder hilts.
    charRenderer->detachWeapon(instanceId, 2);
    // Success if main-hand attached when there was at least one candidate.
    return hadWeaponCandidate && attachedMain;
}


void EntitySpawner::buildCharSectionsCache() {
    if (charSectionsCacheBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;
    auto dbc = assetManager_->loadDBC("CharSections.dbc");
    if (!dbc) return;
    const auto* csL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
    auto csF = pipeline::detectCharSectionsFields(dbc.get(), csL);
    for (uint32_t r = 0; r < dbc->getRecordCount(); r++) {
        uint32_t race = dbc->getUInt32(r, csF.raceId);
        uint32_t sex = dbc->getUInt32(r, csF.sexId);
        uint32_t section = dbc->getUInt32(r, csF.baseSection);
        uint32_t variation = dbc->getUInt32(r, csF.variationIndex);
        uint32_t color = dbc->getUInt32(r, csF.colorIndex);
        // We only cache sections 0 (skin), 1 (face), 3 (hair), 4 (underwear)
        if (section != 0 && section != 1 && section != 3 && section != 4) continue;
        for (int ti = 0; ti < 3; ti++) {
            std::string tex = dbc->getString(r, csF.texture1 + ti);
            if (tex.empty()) continue;
            // Key: race(8)|sex(4)|section(4)|variation(8)|color(8)|texIndex(2) packed into 64 bits
            uint64_t key = (static_cast<uint64_t>(race) << 26) |
                           (static_cast<uint64_t>(sex & 0xF) << 22) |
                           (static_cast<uint64_t>(section & 0xF) << 18) |
                           (static_cast<uint64_t>(variation & 0xFF) << 10) |
                           (static_cast<uint64_t>(color & 0xFF) << 2) |
                           static_cast<uint64_t>(ti);
            charSectionsCache_.emplace(key, tex);
        }
    }
    charSectionsCacheBuilt_ = true;
    LOG_INFO("CharSections cache built: ", charSectionsCache_.size(), " entries");
}

std::string EntitySpawner::lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                           uint8_t variation, uint8_t color, int texIndex) const {
    uint64_t key = (static_cast<uint64_t>(race) << 26) |
                   (static_cast<uint64_t>(sex & 0xF) << 22) |
                   (static_cast<uint64_t>(section & 0xF) << 18) |
                   (static_cast<uint64_t>(variation & 0xFF) << 10) |
                   (static_cast<uint64_t>(color & 0xFF) << 2) |
                   static_cast<uint64_t>(texIndex);
    auto it = charSectionsCache_.find(key);
    return (it != charSectionsCache_.end()) ? it->second : std::string();
}

void EntitySpawner::buildCreatureDisplayLookups() {
    if (creatureLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building creature display lookups from DBC files");

    // CreatureDisplayInfo.dbc structure (3.3.5a):
    // Col 0: displayId
    // Col 1: modelId
    // Col 3: extendedDisplayInfoID (link to CreatureDisplayInfoExtra.dbc)
    // Col 6: Skin1 (texture name)
    // Col 7: Skin2
    // Col 8: Skin3
    if (auto cdi = assetManager_->loadDBC("CreatureDisplayInfo.dbc"); cdi && cdi->isLoaded()) {
        const auto* cdiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < cdi->getRecordCount(); i++) {
            CreatureDisplayData data;
            data.modelId = cdi->getUInt32(i, cdiL ? (*cdiL)["ModelID"] : 1);
            data.extraDisplayId = cdi->getUInt32(i, cdiL ? (*cdiL)["ExtraDisplayId"] : 3);
            data.skin1 = cdi->getString(i, cdiL ? (*cdiL)["Skin1"] : 6);
            data.skin2 = cdi->getString(i, cdiL ? (*cdiL)["Skin2"] : 7);
            data.skin3 = cdi->getString(i, cdiL ? (*cdiL)["Skin3"] : 8);
            displayDataMap_[cdi->getUInt32(i, cdiL ? (*cdiL)["ID"] : 0)] = data;
        }
        LOG_INFO("Loaded ", displayDataMap_.size(), " display→model mappings");
    }

    // CreatureDisplayInfoExtra.dbc structure (3.3.5a):
    // Col 0: ID
    // Col 1: DisplayRaceID
    // Col 2: DisplaySexID
    // Col 3: SkinID
    // Col 4: FaceID
    // Col 5: HairStyleID
    // Col 6: HairColorID
    // Col 7: FacialHairID
    // CreatureDisplayInfoExtra.dbc field layout depends on actual field count:
    //   19 fields: 10 equip slots (8-17), BakeName=18 (no Flags field)
    //   21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
    if (auto cdie = assetManager_->loadDBC("CreatureDisplayInfoExtra.dbc"); cdie && cdie->isLoaded()) {
        const auto* cdieL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureDisplayInfoExtra") : nullptr;
        const uint32_t cdieEquip0 = cdieL ? (*cdieL)["EquipDisplay0"] : 8;
        // Detect actual field count to determine equip slot count and BakeName position
        const uint32_t dbcFieldCount = cdie->getFieldCount();
        int numEquipSlots;
        uint32_t bakeField;
        if (dbcFieldCount <= 19) {
            // 19 fields: 10 equip slots (8-17), BakeName at 18
            numEquipSlots = 10;
            bakeField = 18;
        } else {
            // 21 fields: 11 equip slots (8-18), Flags=19, BakeName=20
            numEquipSlots = 11;
            bakeField = cdieL ? (*cdieL)["BakeName"] : 20;
        }
        uint32_t withBakeName = 0;
        for (uint32_t i = 0; i < cdie->getRecordCount(); i++) {
            HumanoidDisplayExtra extra;
            extra.raceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["RaceID"] : 1));
            extra.sexId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SexID"] : 2));
            extra.skinId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["SkinID"] : 3));
            extra.faceId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FaceID"] : 4));
            extra.hairStyleId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairStyleID"] : 5));
            extra.hairColorId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["HairColorID"] : 6));
            extra.facialHairId = static_cast<uint8_t>(cdie->getUInt32(i, cdieL ? (*cdieL)["FacialHairID"] : 7));
            for (int eq = 0; eq < numEquipSlots; eq++) {
                extra.equipDisplayId[eq] = cdie->getUInt32(i, cdieEquip0 + eq);
            }
            extra.bakeName = cdie->getString(i, bakeField);
            if (!extra.bakeName.empty()) withBakeName++;
            humanoidExtraMap_[cdie->getUInt32(i, cdieL ? (*cdieL)["ID"] : 0)] = extra;
        }
        LOG_WARNING("Loaded ", humanoidExtraMap_.size(), " humanoid display extra entries (",
                 withBakeName, " with baked textures, ", numEquipSlots, " equip slots, ",
                 dbcFieldCount, " DBC fields, bakeField=", bakeField, ")");
    }

    // CreatureModelData.dbc: modelId (col 0) → modelPath (col 2, .mdx → .m2)
    if (auto cmd = assetManager_->loadDBC("CreatureModelData.dbc"); cmd && cmd->isLoaded()) {
        const auto* cmdL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CreatureModelData") : nullptr;
        for (uint32_t i = 0; i < cmd->getRecordCount(); i++) {
            std::string mdx = cmd->getString(i, cmdL ? (*cmdL)["ModelPath"] : 2);
            if (mdx.empty()) continue;
            if (mdx.size() >= 4) {
                mdx = mdx.substr(0, mdx.size() - 4) + ".m2";
            }
            modelIdToPath_[cmd->getUInt32(i, cmdL ? (*cmdL)["ID"] : 0)] = mdx;
        }
        LOG_INFO("Loaded ", modelIdToPath_.size(), " model→path mappings");
    }

    // Resolve gryphon/wyvern display IDs by exact model path so taxi mounts have textures.
    auto toLower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    auto normalizePath = [&](const std::string& p) {
        std::string s = p;
        for (char& c : s) if (c == '/') c = '\\';
        return toLower(s);
    };
    auto resolveDisplayIdForExactPath = [&](const std::string& exactPath) -> uint32_t {
        const std::string target = normalizePath(exactPath);
        // Collect ALL model IDs that map to this path (multiple model IDs can
        // share the same .m2 file, e.g. modelId 147 and 792 both → Gryphon.m2)
        std::vector<uint32_t> modelIds;
        for (const auto& [mid, path] : modelIdToPath_) {
            if (normalizePath(path) == target) {
                modelIds.push_back(mid);
            }
        }
        if (modelIds.empty()) return 0;
        uint32_t bestDisplayId = 0;
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            bool matches = false;
            for (uint32_t mid : modelIds) {
                if (data.modelId == mid) { matches = true; break; }
            }
            if (!matches) continue;
            int score = 0;
            if (!data.skin1.empty()) score += 3;
            if (!data.skin2.empty()) score += 2;
            if (!data.skin3.empty()) score += 1;
            if (score > bestScore) {
                bestScore = score;
                bestDisplayId = dispId;
            }
        }
        return bestDisplayId;
    };

    gryphonDisplayId_ = resolveDisplayIdForExactPath("Creature\\Gryphon\\Gryphon.m2");
    wyvernDisplayId_  = resolveDisplayIdForExactPath("Creature\\Wyvern\\Wyvern.m2");
    gameServices_->gryphonDisplayId = gryphonDisplayId_;
    gameServices_->wyvernDisplayId  = wyvernDisplayId_;
    LOG_INFO("Taxi mount displayIds: gryphon=", gryphonDisplayId_, " wyvern=", wyvernDisplayId_);

    // CharHairGeosets.dbc: maps (race, sex, hairStyleId) → skinSectionId for hair mesh
    // Col 0: ID, Col 1: RaceID, Col 2: SexID, Col 3: VariationID, Col 4: GeosetID, Col 5: Showscalp
    if (auto chg = assetManager_->loadDBC("CharHairGeosets.dbc"); chg && chg->isLoaded()) {
        const auto* chgL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharHairGeosets") : nullptr;
        for (uint32_t i = 0; i < chg->getRecordCount(); i++) {
            uint32_t raceId = chg->getUInt32(i, chgL ? (*chgL)["RaceID"] : 1);
            uint32_t sexId = chg->getUInt32(i, chgL ? (*chgL)["SexID"] : 2);
            uint32_t variation = chg->getUInt32(i, chgL ? (*chgL)["Variation"] : 3);
            uint32_t geosetId = chg->getUInt32(i, chgL ? (*chgL)["GeosetID"] : 4);
            uint32_t key = (raceId << 16) | (sexId << 8) | variation;
            hairGeosetMap_[key] = static_cast<uint16_t>(geosetId);
        }
        LOG_INFO("Loaded ", hairGeosetMap_.size(), " hair geoset mappings from CharHairGeosets.dbc");
    }

    // CharacterFacialHairStyles.dbc: maps (race, sex, facialHairId) → geoset IDs
    // No ID column: Col 0: RaceID, Col 1: SexID, Col 2: VariationID
    // Col 3: Geoset100, Col 4: Geoset300, Col 5: Geoset200
    if (auto cfh = assetManager_->loadDBC("CharacterFacialHairStyles.dbc"); cfh && cfh->isLoaded()) {
        const auto* cfhL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharacterFacialHairStyles") : nullptr;
        for (uint32_t i = 0; i < cfh->getRecordCount(); i++) {
            uint32_t raceId = cfh->getUInt32(i, cfhL ? (*cfhL)["RaceID"] : 0);
            uint32_t sexId = cfh->getUInt32(i, cfhL ? (*cfhL)["SexID"] : 1);
            uint32_t variation = cfh->getUInt32(i, cfhL ? (*cfhL)["Variation"] : 2);
            uint32_t key = (raceId << 16) | (sexId << 8) | variation;
            FacialHairGeosets fhg;
            fhg.geoset100 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset100"] : 3));
            fhg.geoset300 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset300"] : 4));
            fhg.geoset200 = static_cast<uint16_t>(cfh->getUInt32(i, cfhL ? (*cfhL)["Geoset200"] : 5));
            facialHairGeosetMap_[key] = fhg;
        }
        LOG_INFO("Loaded ", facialHairGeosetMap_.size(), " facial hair geoset mappings from CharacterFacialHairStyles.dbc");
    }

    creatureLookupsBuilt_ = true;
}

std::string EntitySpawner::getModelPathForDisplayId(uint32_t displayId) const {
    if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
    if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";

    // WotLK servers can send display IDs that do not exist in older/local
    // CreatureDisplayInfo datasets. Keep those creatures visible by falling
    // back to a close base model instead of dropping spawn entirely.
    switch (displayId) {
        case 31048: // Diseased Young Wolf variants (AzerothCore WotLK)
        case 31049: // Diseased Wolf variants (AzerothCore WotLK)
            return "Creature\\Wolf\\Wolf.m2";
        default:
            break;
    }

    auto itData = displayDataMap_.find(displayId);
    if (itData == displayDataMap_.end()) {
        // Some sources (e.g., taxi nodes) may provide a modelId directly.
        auto itPath = modelIdToPath_.find(displayId);
        if (itPath != modelIdToPath_.end()) {
            return itPath->second;
        }
        if (displayId == 30412) return "Creature\\Gryphon\\Gryphon.m2";
        if (displayId == 30413) return "Creature\\Wyvern\\Wyvern.m2";
        if (warnedMissingDisplayDataIds_.insert(displayId).second) {
            LOG_WARNING("No display data for displayId ", displayId,
                        " (displayDataMap_ has ", displayDataMap_.size(), " entries)");
        }
        return "";
    }

    auto itPath = modelIdToPath_.find(itData->second.modelId);
    if (itPath == modelIdToPath_.end()) {
        if (warnedMissingModelPathIds_.insert(displayId).second) {
            LOG_WARNING("No model path for modelId ", itData->second.modelId,
                        " from displayId ", displayId,
                        " (modelIdToPath_ has ", modelIdToPath_.size(), " entries)");
        }
        return "";
    }

    return itPath->second;
}

audio::VoiceType EntitySpawner::detectVoiceTypeFromDisplayId(uint32_t displayId) const {
    // Look up display data
    auto itDisplay = displayDataMap_.find(displayId);
    if (itDisplay == displayDataMap_.end() || itDisplay->second.extraDisplayId == 0) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no display data)");
        return audio::VoiceType::GENERIC;  // Not a humanoid or no extra data
    }

    // Look up humanoid extra data (race/sex info)
    auto itExtra = humanoidExtraMap_.find(itDisplay->second.extraDisplayId);
    if (itExtra == humanoidExtraMap_.end()) {
        LOG_INFO("Voice detection: displayId ", displayId, " -> GENERIC (no humanoid extra data)");
        return audio::VoiceType::GENERIC;
    }

    uint8_t raceId = itExtra->second.raceId;
    uint8_t sexId = itExtra->second.sexId;

    const char* raceName = "Unknown";
    const char* sexName = (sexId == 0) ? "Male" : "Female";

    // Map (raceId, sexId) to VoiceType
    // Race IDs: 1=Human, 2=Orc, 3=Dwarf, 4=NightElf, 5=Undead, 6=Tauren, 7=Gnome, 8=Troll
    // Sex IDs: 0=Male, 1=Female
    audio::VoiceType result;
    switch (raceId) {
        case 1: raceName = "Human"; result = (sexId == 0) ? audio::VoiceType::HUMAN_MALE : audio::VoiceType::HUMAN_FEMALE; break;
        case 2: raceName = "Orc"; result = (sexId == 0) ? audio::VoiceType::ORC_MALE : audio::VoiceType::ORC_FEMALE; break;
        case 3: raceName = "Dwarf"; result = (sexId == 0) ? audio::VoiceType::DWARF_MALE : audio::VoiceType::DWARF_FEMALE; break;
        case 4: raceName = "NightElf"; result = (sexId == 0) ? audio::VoiceType::NIGHTELF_MALE : audio::VoiceType::NIGHTELF_FEMALE; break;
        case 5: raceName = "Undead"; result = (sexId == 0) ? audio::VoiceType::UNDEAD_MALE : audio::VoiceType::UNDEAD_FEMALE; break;
        case 6: raceName = "Tauren"; result = (sexId == 0) ? audio::VoiceType::TAUREN_MALE : audio::VoiceType::TAUREN_FEMALE; break;
        case 7: raceName = "Gnome"; result = (sexId == 0) ? audio::VoiceType::GNOME_MALE : audio::VoiceType::GNOME_FEMALE; break;
        case 8: raceName = "Troll"; result = (sexId == 0) ? audio::VoiceType::TROLL_MALE : audio::VoiceType::TROLL_FEMALE; break;
        case 10: raceName = "BloodElf"; result = (sexId == 0) ? audio::VoiceType::BLOODELF_MALE : audio::VoiceType::BLOODELF_FEMALE; break;
        case 11: raceName = "Draenei"; result = (sexId == 0) ? audio::VoiceType::DRAENEI_MALE : audio::VoiceType::DRAENEI_FEMALE; break;
        default: result = audio::VoiceType::GENERIC; break;
    }

    LOG_INFO("Voice detection: displayId ", displayId, " -> ", raceName, " ", sexName, " (race=", static_cast<int>(raceId), ", sex=", static_cast<int>(sexId), ")");
    return result;
}

void EntitySpawner::buildGameObjectDisplayLookups() {
    if (gameObjectLookupsBuilt_ || !assetManager_ || !assetManager_->isInitialized()) return;

    LOG_INFO("Building gameobject display lookups from DBC files");

    // GameObjectDisplayInfo.dbc structure (3.3.5a):
    // Col 0: ID (displayId)
    // Col 1: ModelName
    if (auto godi = assetManager_->loadDBC("GameObjectDisplayInfo.dbc"); godi && godi->isLoaded()) {
        const auto* godiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("GameObjectDisplayInfo") : nullptr;
        for (uint32_t i = 0; i < godi->getRecordCount(); i++) {
            uint32_t displayId = godi->getUInt32(i, godiL ? (*godiL)["ID"] : 0);
            std::string modelName = godi->getString(i, godiL ? (*godiL)["ModelName"] : 1);
            if (modelName.empty()) continue;
            if (modelName.size() >= 4) {
                std::string ext = modelName.substr(modelName.size() - 4);
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".mdx") {
                    modelName = modelName.substr(0, modelName.size() - 4) + ".m2";
                }
            }
            gameObjectDisplayIdToPath_[displayId] = modelName;
        }
        LOG_INFO("Loaded ", gameObjectDisplayIdToPath_.size(), " gameobject display mappings");
    }

    gameObjectLookupsBuilt_ = true;
}

std::string EntitySpawner::getGameObjectModelPathForDisplayId(uint32_t displayId) const {
    auto it = gameObjectDisplayIdToPath_.find(displayId);
    if (it == gameObjectDisplayIdToPath_.end()) return "";
    return it->second;
}


bool EntitySpawner::getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) {
        instanceId = renderer_->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstanceBounds(instanceId, outCenter, outRadius);
}

bool EntitySpawner::getRenderFootZForGuid(uint64_t guid, float& outFootZ) const {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) {
        instanceId = renderer_->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstanceFootZ(instanceId, outFootZ);
}

bool EntitySpawner::getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return false;
    uint32_t instanceId = 0;

    if (gameHandler_ && guid == gameHandler_->getPlayerGuid()) {
        instanceId = renderer_->getCharacterInstanceId();
    }
    if (instanceId == 0) {
        auto pit = playerInstances_.find(guid);
        if (pit != playerInstances_.end()) instanceId = pit->second;
    }
    if (instanceId == 0) {
        auto it = creatureInstances_.find(guid);
        if (it != creatureInstances_.end()) instanceId = it->second;
    }
    if (instanceId == 0) return false;

    return renderer_->getCharacterRenderer()->getInstancePosition(instanceId, outPos);
}


pipeline::M2Model EntitySpawner::loadCreatureM2Sync(const std::string& m2Path) {
    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) {
        LOG_WARNING("Failed to read creature M2: ", m2Path);
        return {};
    }

    pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
    if (model.vertices.empty()) {
        LOG_WARNING("Failed to parse creature M2: ", m2Path);
        return {};
    }

    // Load skin file (only for WotLK M2s - vanilla has embedded skin)
    if (model.version >= 264) {
        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
        auto skinData = assetManager_->readFile(skinPath);
        if (!skinData.empty()) {
            pipeline::M2Loader::loadSkin(skinData, model);
        } else {
            LOG_WARNING("Missing skin file for WotLK creature M2: ", skinPath);
        }
    }

    // Load external .anim files for sequences without flag 0x20
    std::string basePath = m2Path.substr(0, m2Path.size() - 3);
    for (uint32_t si = 0; si < model.sequences.size(); si++) {
        if (!(model.sequences[si].flags & 0x20)) {
            char animFileName[256];
            snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                basePath.c_str(), model.sequences[si].id, model.sequences[si].variationIndex);
            auto animData = assetManager_->readFileOptional(animFileName);
            if (!animData.empty()) {
                pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
            }
        }
    }

    return model;
}

void EntitySpawner::spawnOnlineCreature(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_) return;

    // Skip if lookups not yet built (asset manager not ready)
    if (!creatureLookupsBuilt_) return;

    // Skip if already spawned
    if (creatureInstances_.count(guid)) return;
    if (nonRenderableCreatureDisplayIds_.count(displayId)) {
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }

    // Get model path from displayId
    std::string m2Path = getModelPathForDisplayId(displayId);
    if (m2Path.empty()) {
        nonRenderableCreatureDisplayIds_.insert(displayId);
        creaturePermanentFailureGuids_.insert(guid);
        return;
    }
    {
        // Intentionally invisible helper creatures should not consume retry budget.
        std::string lowerPath = m2Path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerPath.find("invisiblestalker") != std::string::npos ||
            lowerPath.find("invisible_stalker") != std::string::npos) {
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }
    }

    auto* charRenderer = renderer_->getCharacterRenderer();

    // Check model cache - reuse if same displayId was already loaded
    uint32_t modelId = 0;
    auto cacheIt = displayIdModelCache_.find(displayId);
    if (cacheIt != displayIdModelCache_.end()) {
        modelId = cacheIt->second;
    } else {
        // Load model from disk (only once per displayId)
        modelId = nextCreatureModelId_++;

        pipeline::M2Model model = loadCreatureM2Sync(m2Path);
        if (!model.isValid()) {
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }

        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("Failed to load creature model: ", m2Path);
            nonRenderableCreatureDisplayIds_.insert(displayId);
            creaturePermanentFailureGuids_.insert(guid);
            return;
        }

        displayIdModelCache_[displayId] = modelId;
    }

    // Apply skin textures from CreatureDisplayInfo.dbc (only once per displayId model).
    // Track separately from model cache because async loading may upload the model
    // before textures are applied.
    auto itDisplayData = displayDataMap_.find(displayId);
    bool needsTextures = (displayIdTexturesApplied_.find(displayId) == displayIdTexturesApplied_.end());
    if (needsTextures && itDisplayData != displayDataMap_.end()) {
        auto texStart = std::chrono::steady_clock::now();
        displayIdTexturesApplied_.insert(displayId);
        const auto& dispData = itDisplayData->second;

        // Use pre-decoded textures from async creature load (if available)
        auto itPreDec = displayIdPredecodedTextures_.find(displayId);
        bool hasPreDec = (itPreDec != displayIdPredecodedTextures_.end());
        if (hasPreDec) {
            charRenderer->setPredecodedBLPCache(&itPreDec->second);
        }

        // Get model directory for texture path construction
        std::string modelDir;
        size_t lastSlash = m2Path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            modelDir = m2Path.substr(0, lastSlash + 1);
        }

        LOG_DEBUG("DisplayId ", displayId, " skins: '", dispData.skin1, "', '", dispData.skin2, "', '", dispData.skin3,
                  "' extraDisplayId=", dispData.extraDisplayId);

        // Get model data from CharacterRenderer for texture iteration
        const auto* modelData = charRenderer->getModelData(modelId);
        if (!modelData) {
            LOG_WARNING("Model data not found for modelId ", modelId);
        }

        // Log texture types in the model
        if (modelData) {
        for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
            LOG_DEBUG("  Model texture ", ti, ": type=", modelData->textures[ti].type, " filename='", modelData->textures[ti].filename, "'");
        }
        }

        // Check if this is a humanoid NPC with extra display info
        bool hasHumanoidTexture = false;
        if (dispData.extraDisplayId != 0) {
            auto itExtra = humanoidExtraMap_.find(dispData.extraDisplayId);
            if (itExtra != humanoidExtraMap_.end()) {
                const auto& extra = itExtra->second;
                LOG_DEBUG("  Found humanoid extra: raceId=", static_cast<int>(extra.raceId), " sexId=", static_cast<int>(extra.sexId),
                          " hairStyle=", static_cast<int>(extra.hairStyleId), " hairColor=", static_cast<int>(extra.hairColorId),
                          " bakeName='", extra.bakeName, "'");

                // Collect model texture slot info (type 1 = skin, type 6 = hair)
                std::vector<uint32_t> skinSlots, hairSlots;
                if (modelData) {
                    for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                        uint32_t texType = modelData->textures[ti].type;
                        if (texType == 1 || texType == 11 || texType == 12 || texType == 13)
                            skinSlots.push_back(static_cast<uint32_t>(ti));
                        if (texType == 6)
                            hairSlots.push_back(static_cast<uint32_t>(ti));
                    }
                }

                // Copy extra data for the async task (avoid dangling reference)
                HumanoidDisplayExtra extraCopy = extra;

                // Launch async task: ALL DBC lookups, path resolution, and BLP pre-decode
                // happen on a background thread. Only GPU texture upload runs on main thread
                // (in processAsyncNpcCompositeResults).
                auto* am = assetManager_;
                AsyncNpcCompositeLoad load;
                load.future = std::async(std::launch::async,
                    [am, extraCopy, skinSlots = std::move(skinSlots),
                     hairSlots = std::move(hairSlots), modelId, displayId]() mutable -> PreparedNpcComposite {
                        PreparedNpcComposite result;
                        DeferredNpcComposite& def = result.info;
                        def.modelId = modelId;
                        def.displayId = displayId;
                        def.skinTextureSlots = std::move(skinSlots);
                        def.hairTextureSlots = std::move(hairSlots);

                        std::vector<std::string> allPaths;  // paths to pre-decode

                        // --- Baked skin texture ---
                        if (!extraCopy.bakeName.empty()) {
                            def.bakedSkinPath = "Textures\\BakedNpcTextures\\" + extraCopy.bakeName;
                            def.hasBakedSkin = true;
                            allPaths.push_back(def.bakedSkinPath);
                        }

                        // --- CharSections fallback (skin/face/underwear) ---
                        if (!def.hasBakedSkin) {
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t npcRace = static_cast<uint32_t>(extraCopy.raceId);
                                uint32_t npcSex = static_cast<uint32_t>(extraCopy.sexId);
                                uint32_t npcSkin = static_cast<uint32_t>(extraCopy.skinId);
                                uint32_t npcFace = static_cast<uint32_t>(extraCopy.faceId);
                                std::string npcFaceLower, npcFaceUpper;
                                std::vector<std::string> npcUnderwear;

                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t rId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sId = csDbc->getUInt32(r, csF.sexId);
                                    if (rId != npcRace || sId != npcSex) continue;

                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t color = csDbc->getUInt32(r, csF.colorIndex);

                                    if (section == 0 && def.basePath.empty() && color == npcSkin) {
                                        def.basePath = csDbc->getString(r, csF.texture1);
                                    } else if (section == 1 && npcFaceLower.empty() &&
                                               variation == npcFace && color == npcSkin) {
                                        npcFaceLower = csDbc->getString(r, csF.texture1);
                                        npcFaceUpper = csDbc->getString(r, csF.texture2);
                                    } else if (section == 4 && npcUnderwear.empty() && color == npcSkin) {
                                        for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                                            std::string tex = csDbc->getString(r, f);
                                            if (!tex.empty()) npcUnderwear.push_back(tex);
                                        }
                                    }
                                }

                                if (!def.basePath.empty()) {
                                    allPaths.push_back(def.basePath);
                                    if (!npcFaceLower.empty()) { def.overlayPaths.push_back(npcFaceLower); allPaths.push_back(npcFaceLower); }
                                    if (!npcFaceUpper.empty()) { def.overlayPaths.push_back(npcFaceUpper); allPaths.push_back(npcFaceUpper); }
                                    for (const auto& uw : npcUnderwear) { def.overlayPaths.push_back(uw); allPaths.push_back(uw); }
                                }
                            }
                        }

                        // --- Equipment region layers (ItemDisplayInfo DBC) ---
                        auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                        if (idiDbc) {
                            static constexpr const char* componentDirs[] = {
                                "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
                                "TorsoUpperTexture", "TorsoLowerTexture",
                                "LegUpperTexture", "LegLowerTexture", "FootTexture",
                            };
                            const auto* idiL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                            uint32_t texRegionFields[8];
                            pipeline::getItemDisplayInfoTextureFields(*idiDbc, idiL, texRegionFields);
                            const bool npcIsFemale = (extraCopy.sexId == 1);
                            const bool npcHasArmArmor = (extraCopy.equipDisplayId[7] != 0 || extraCopy.equipDisplayId[8] != 0);

                            auto regionAllowedForNpcSlot = [](int eqSlot, int region) -> bool {
                                switch (eqSlot) {
                                    case 2: case 3: return region <= 4;
                                    case 4: return false;
                                    case 5: return region == 5 || region == 6;
                                    case 6: return region == 7;
                                    case 7: return false;
                                    case 8: return region == 2;
                                    case 9: return region == 3 || region == 4;
                                    default: return false;
                                }
                            };

                            for (int eqSlot = 0; eqSlot < 11; eqSlot++) {
                                uint32_t did = extraCopy.equipDisplayId[eqSlot];
                                if (did == 0) continue;
                                int32_t recIdx = idiDbc->findRecordById(did);
                                if (recIdx < 0) continue;

                                for (int region = 0; region < 8; region++) {
                                    if (!regionAllowedForNpcSlot(eqSlot, region)) continue;
                                    if (eqSlot == 2 && !npcHasArmArmor && !(region == 3 || region == 4)) continue;
                                    std::string texName = idiDbc->getString(
                                        static_cast<uint32_t>(recIdx), texRegionFields[region]);
                                    if (texName.empty()) continue;

                                    std::string base = "Item\\TextureComponents\\" +
                                        std::string(componentDirs[region]) + "\\" + texName;
                                    std::string genderPath = base + (npcIsFemale ? "_F.blp" : "_M.blp");
                                    std::string unisexPath = base + "_U.blp";
                                    std::string basePath = base + ".blp";
                                    std::string fullPath;
                                    if (am->fileExists(genderPath)) fullPath = genderPath;
                                    else if (am->fileExists(unisexPath)) fullPath = unisexPath;
                                    else if (am->fileExists(basePath)) fullPath = basePath;
                                    else continue;

                                    def.regionLayers.emplace_back(region, fullPath);
                                    allPaths.push_back(fullPath);
                                }
                            }
                        }

                        // Determine compositing mode
                        if (!def.basePath.empty()) {
                            bool needsComposite = !def.overlayPaths.empty() || !def.regionLayers.empty();
                            if (needsComposite && !def.skinTextureSlots.empty()) {
                                def.hasComposite = true;
                            } else if (!def.skinTextureSlots.empty()) {
                                def.hasSimpleSkin = true;
                            }
                        }

                        // --- Hair texture from CharSections (section 3) ---
                        {
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t targetRace = static_cast<uint32_t>(extraCopy.raceId);
                                uint32_t targetSex = static_cast<uint32_t>(extraCopy.sexId);

                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t raceId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sexId = csDbc->getUInt32(r, csF.sexId);
                                    if (raceId != targetRace || sexId != targetSex) continue;
                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    if (section != 3) continue;
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t colorIdx = csDbc->getUInt32(r, csF.colorIndex);
                                    if (variation != static_cast<uint32_t>(extraCopy.hairStyleId)) continue;
                                    if (colorIdx != static_cast<uint32_t>(extraCopy.hairColorId)) continue;
                                    def.hairTexturePath = csDbc->getString(r, csF.texture1);
                                    break;
                                }

                                if (!def.hairTexturePath.empty()) {
                                    allPaths.push_back(def.hairTexturePath);
                                } else if (def.hasBakedSkin && !def.hairTextureSlots.empty()) {
                                    def.useBakedForHair = true;
                                    // bakedSkinPath already in allPaths
                                }
                            }
                        }

                        // --- Pre-decode all BLP textures on this background thread ---
                        for (const auto& path : allPaths) {
                            std::string key = path;
                            std::replace(key.begin(), key.end(), '/', '\\');
                            std::transform(key.begin(), key.end(), key.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (result.predecodedTextures.count(key)) continue;
                            auto blp = am->loadTexture(key);
                            if (blp.isValid()) {
                                result.predecodedTextures[key] = std::move(blp);
                            }
                        }

                        return result;
                    });
                asyncNpcCompositeLoads_.push_back(std::move(load));
                hasHumanoidTexture = true;  // skip non-humanoid skin block
            } else {
                LOG_WARNING("  extraDisplayId ", dispData.extraDisplayId, " not found in humanoidExtraMap");
            }
        }

        // Apply creature skin textures (for non-humanoid creatures)
        if (!hasHumanoidTexture && modelData) {
            auto resolveCreatureSkinPath = [&](const std::string& skinField) -> std::string {
                if (skinField.empty()) return "";

                std::string raw = skinField;
                std::replace(raw.begin(), raw.end(), '/', '\\');
                auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
                raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [&](unsigned char c) { return !isSpace(c); }));
                raw.erase(std::find_if(raw.rbegin(), raw.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), raw.end());
                if (raw.empty()) return "";

                auto hasBlpExt = [](const std::string& p) {
                    if (p.size() < 4) return false;
                    std::string ext = p.substr(p.size() - 4);
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return ext == ".blp";
                };
                auto addCandidate = [](std::vector<std::string>& out, const std::string& p) {
                    if (p.empty()) return;
                    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
                };

                std::vector<std::string> candidates;
                const bool hasDir = (raw.find('\\') != std::string::npos || raw.find('/') != std::string::npos);
                const bool hasExt = hasBlpExt(raw);

                if (hasDir) {
                    addCandidate(candidates, raw);
                    if (!hasExt) addCandidate(candidates, raw + ".blp");
                } else {
                    addCandidate(candidates, modelDir + raw);
                    if (!hasExt) addCandidate(candidates, modelDir + raw + ".blp");
                    addCandidate(candidates, raw);
                    if (!hasExt) addCandidate(candidates, raw + ".blp");
                }

                for (const auto& c : candidates) {
                    if (assetManager_->fileExists(c)) return c;
                }
                return "";
            };

            for (size_t ti = 0; ti < modelData->textures.size(); ti++) {
                const auto& tex = modelData->textures[ti];
                std::string skinPath;

                // Creature skin types: 11 = skin1, 12 = skin2, 13 = skin3
                if (tex.type == 11 && !dispData.skin1.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin1);
                } else if (tex.type == 12 && !dispData.skin2.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin2);
                } else if (tex.type == 13 && !dispData.skin3.empty()) {
                    skinPath = resolveCreatureSkinPath(dispData.skin3);
                }

                if (!skinPath.empty()) {
                    rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                    if (skinTex) {
                        charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                        LOG_DEBUG("Applied creature skin texture: ", skinPath, " to slot ", ti);
                    }
                } else if ((tex.type == 11 && !dispData.skin1.empty()) ||
                           (tex.type == 12 && !dispData.skin2.empty()) ||
                           (tex.type == 13 && !dispData.skin3.empty())) {
                    LOG_WARNING("Creature skin texture not found for displayId ", displayId,
                                " slot ", ti, " type ", tex.type,
                                " (skin fields: '", dispData.skin1, "', '",
                                dispData.skin2, "', '", dispData.skin3, "')");
                }
            }
        }

        // Clear pre-decoded cache after applying all display textures
        charRenderer->setPredecodedBLPCache(nullptr);
        displayIdPredecodedTextures_.erase(displayId);
        {
            auto texEnd = std::chrono::steady_clock::now();
            float texMs = std::chrono::duration<float, std::milli>(texEnd - texStart).count();
            if (texMs > 50.0f) {
                LOG_WARNING("spawnCreature texture setup took ", texMs, "ms displayId=", displayId,
                            " hasPreDec=", hasPreDec, " extra=", dispData.extraDisplayId);
            }
        }
    }

    // Use the entity's latest server-authoritative position rather than the stale spawn
    // position. Movement packets (SMSG_MONSTER_MOVE) can arrive while a creature is still
    // queued in pendingCreatureSpawns_ and get silently dropped. getLatestX/Y/Z returns
    // the movement destination if the entity is mid-move, which is always up-to-date
    // regardless of distance culling (unlike getX/Y/Z which requires updateMovement).
    if (gameHandler_) {
        if (auto entity = gameHandler_->getEntityManager().getEntity(guid)) {
            x = entity->getLatestX();
            y = entity->getLatestY();
            z = entity->getLatestZ();
            orientation = entity->getOrientation();
        }
    }

    // Convert canonical → render coordinates
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));

    // Keep authoritative server Z for online creature spawns.
    // Terrain-based lifting can incorrectly move elevated NPCs (e.g. flight masters on
    // Stormwind ramparts) to bad heights relative to WMO geometry.

    // Convert canonical WoW orientation (0=north) -> render yaw (0=west)
    float renderYaw = orientation + glm::radians(90.0f);

    // Create instance (apply server-provided scale from OBJECT_FIELD_SCALE_X)
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos,
        glm::vec3(0.0f, 0.0f, renderYaw), scale);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create creature instance for guid 0x", std::hex, guid, std::dec);
        return;
    }

    // Per-instance hair/skin texture overrides — runs for ALL NPCs (including cached models)
    // so that each NPC gets its own hair/skin color regardless of model sharing.
    // Uses pre-built CharSections cache (O(1) lookup instead of O(N) DBC scan).
    {
        if (!charSectionsCacheBuilt_) buildCharSectionsCache();
        auto itDD = displayDataMap_.find(displayId);
        if (itDD != displayDataMap_.end() && itDD->second.extraDisplayId != 0) {
            auto itExtra2 = humanoidExtraMap_.find(itDD->second.extraDisplayId);
            if (itExtra2 != humanoidExtraMap_.end()) {
                const auto& extra = itExtra2->second;
                const auto* md = charRenderer->getModelData(modelId);
                if (md) {
                        // Look up hair texture (section 3) via cache
                        rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                        std::string hairPath = lookupCharSection(
                            extra.raceId, extra.sexId, 3, extra.hairStyleId, extra.hairColorId, 0);
                        if (!hairPath.empty()) {
                            rendering::VkTexture* hairTex = charRenderer->loadTexture(hairPath);
                            if (hairTex && hairTex != whiteTex) {
                                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                    if (md->textures[ti].type == 6) {
                                        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), hairTex);
                                    }
                                }
                            }
                        }

                        // Look up skin texture (section 0) for per-instance skin color.
                        // Skip when the NPC has a baked texture or composited equipment —
                        // those already encode armor over skin and must not be replaced.
                        bool hasEquipOrBake = !extra.bakeName.empty();
                        if (!hasEquipOrBake) {
                            for (int s = 0; s < 11 && !hasEquipOrBake; s++)
                                if (extra.equipDisplayId[s] != 0) hasEquipOrBake = true;
                        }
                        if (!hasEquipOrBake) {
                            std::string skinPath = lookupCharSection(
                                extra.raceId, extra.sexId, 0, 0, extra.skinId, 0);
                            if (!skinPath.empty()) {
                                rendering::VkTexture* skinTex = charRenderer->loadTexture(skinPath);
                                if (skinTex) {
                                    for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                        uint32_t tt = md->textures[ti].type;
                                        if (tt == 1 || tt == 11) {
                                            charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), skinTex);
                                        }
                                    }
                                }
                            }
                        }
                }
            }
        }
    }

    // Optional humanoid NPC geoset mask. Disabled by default because forcing geosets
    // causes long-standing visual artifacts on some models (missing waist, phantom
    // bracers, flickering apron overlays). Prefer model defaults.
    static constexpr bool kEnableNpcSafeGeosetMask = false;
    if (kEnableNpcSafeGeosetMask &&
        itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            std::unordered_set<uint16_t> safeGeosets;
            std::unordered_set<uint16_t> modelGeosets;
            std::unordered_map<uint16_t, uint16_t> firstGeosetByGroup;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (const auto& b : md->batches) {
                    const uint16_t sid = b.submeshId;
                    modelGeosets.insert(sid);
                    const uint16_t group = static_cast<uint16_t>(sid / 100);
                    auto it = firstGeosetByGroup.find(group);
                    if (it == firstGeosetByGroup.end() || sid < it->second) {
                        firstGeosetByGroup[group] = sid;
                    }
                }
            }
            auto addSafeGeoset = [&](uint16_t preferredId) {
                if (preferredId < 100 || modelGeosets.empty()) {
                    safeGeosets.insert(preferredId);
                    return;
                }
                if (modelGeosets.count(preferredId) > 0) {
                    safeGeosets.insert(preferredId);
                    return;
                }
                const uint16_t group = static_cast<uint16_t>(preferredId / 100);
                auto it = firstGeosetByGroup.find(group);
                if (it != firstGeosetByGroup.end()) {
                    safeGeosets.insert(it->second);
                }
            };
            uint16_t hairGeoset = 1;
            uint32_t hairKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                               (static_cast<uint32_t>(extra.sexId) << 8) |
                               static_cast<uint32_t>(extra.hairStyleId);
            auto itHairGeo = hairGeosetMap_.find(hairKey);
            if (itHairGeo != hairGeosetMap_.end() && itHairGeo->second > 0) {
                hairGeoset = itHairGeo->second;
            }
            const uint16_t selectedHairScalp = (hairGeoset > 0 ? hairGeoset : 1);
            std::unordered_set<uint16_t> hairScalpGeosetsForRaceSex;
            for (const auto& [k, v] : hairGeosetMap_) {
                uint8_t race = static_cast<uint8_t>((k >> 16) & 0xFF);
                uint8_t sex = static_cast<uint8_t>((k >> 8) & 0xFF);
                if (race == extra.raceId && sex == extra.sexId && v > 0 && v < 100) {
                    hairScalpGeosetsForRaceSex.insert(v);
                }
            }
            // Group 0 contains both base body parts and race/sex hair scalp variants.
            // Keep all non-hair body submeshes, but only the selected hair scalp.
            for (uint16_t sid : modelGeosets) {
                if (sid >= 100) continue;
                if (hairScalpGeosetsForRaceSex.count(sid) > 0 && sid != selectedHairScalp) continue;
                safeGeosets.insert(sid);
            }
            safeGeosets.insert(selectedHairScalp);
            addSafeGeoset(static_cast<uint16_t>(100 + std::max<uint16_t>(hairGeoset, 1)));

            uint32_t facialKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                                 (static_cast<uint32_t>(extra.sexId) << 8) |
                                 static_cast<uint32_t>(extra.facialHairId);
            auto itFacial = facialHairGeosetMap_.find(facialKey);
            if (itFacial != facialHairGeosetMap_.end()) {
                const auto& fhg = itFacial->second;
                addSafeGeoset(static_cast<uint16_t>(200 + std::max<uint16_t>(fhg.geoset200, 1)));
                addSafeGeoset(static_cast<uint16_t>(300 + std::max<uint16_t>(fhg.geoset300, 1)));
            } else {
                addSafeGeoset(201);
                addSafeGeoset(301);
            }

            // Force pants (1301) and avoid robe skirt variants unless we re-enable full slot-accurate geosets.
            addSafeGeoset(301);
            addSafeGeoset(kGeosetBareForearms);
            addSafeGeoset(402);
            addSafeGeoset(501);
            addSafeGeoset(701);
            addSafeGeoset(kGeosetBareSleeves);
            addSafeGeoset(901);
            addSafeGeoset(kGeosetDefaultTabard);
            addSafeGeoset(kGeosetBarePants);
            addSafeGeoset(kGeosetBareFeet);

            charRenderer->setActiveGeosets(instanceId, safeGeosets);
        }
    }

    // NOTE: Custom humanoid NPC geoset/equipment overrides are currently too
    // aggressive and can make NPCs invisible (targetable but not rendered).
    // Keep default model geosets for online creatures until this path is made
    // data-accurate per display model.
    static constexpr bool kEnableNpcHumanoidOverrides = false;

    // Set geosets for humanoid NPCs based on CreatureDisplayInfoExtra
    if (kEnableNpcHumanoidOverrides &&
        itDisplayData != displayDataMap_.end() &&
        itDisplayData->second.extraDisplayId != 0) {
        auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
        if (itExtra != humanoidExtraMap_.end()) {
            const auto& extra = itExtra->second;
            std::unordered_set<uint16_t> activeGeosets;

            // Group 0: body base (id=0 always) + hair scalp mesh from CharHairGeosets.dbc
            activeGeosets.insert(0);  // Body base mesh

            // Hair: CharHairGeosets.dbc maps (race, sex, hairStyleId) → group 0 scalp submeshId
            uint32_t hairKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                               (static_cast<uint32_t>(extra.sexId) << 8) |
                               static_cast<uint32_t>(extra.hairStyleId);
            auto itHairGeo = hairGeosetMap_.find(hairKey);
            uint16_t hairScalpId = (itHairGeo != hairGeosetMap_.end()) ? itHairGeo->second : 0;
            if (hairScalpId > 0) {
                activeGeosets.insert(hairScalpId);                        // Group 0 scalp/hair mesh
                activeGeosets.insert(static_cast<uint16_t>(100 + hairScalpId)); // Group 1 connector (if exists)
            } else {
                // Bald (geosetId=0): body base has a hole at the crown, so include
                // submeshId=1 (bald scalp cap with body skin texture) to cover it.
                activeGeosets.insert(1);    // Group 0 bald scalp mesh
                activeGeosets.insert(kGeosetDefaultConnector);  // Group 1 connector
            }
            uint16_t hairGeoset = (hairScalpId > 0) ? hairScalpId : 1;

            // Facial hair geosets from CharFacialHairStyles.dbc lookup
            uint32_t facialKey = (static_cast<uint32_t>(extra.raceId) << 16) |
                                 (static_cast<uint32_t>(extra.sexId) << 8) |
                                 static_cast<uint32_t>(extra.facialHairId);
            auto itFacial = facialHairGeosetMap_.find(facialKey);
            if (itFacial != facialHairGeosetMap_.end()) {
                const auto& fhg = itFacial->second;
                // DBC values are variation indices within each group; add group base
                activeGeosets.insert(static_cast<uint16_t>(100 + std::max(fhg.geoset100, static_cast<uint16_t>(1))));
                activeGeosets.insert(static_cast<uint16_t>(300 + std::max(fhg.geoset300, static_cast<uint16_t>(1))));
                activeGeosets.insert(static_cast<uint16_t>(200 + std::max(fhg.geoset200, static_cast<uint16_t>(1))));
            } else {
                activeGeosets.insert(kGeosetDefaultConnector); // Default group 1: no extra
                activeGeosets.insert(201); // Default group 2: no facial hair
                activeGeosets.insert(301); // Default group 3: no facial hair
            }

            // Default equipment geosets (bare/no armor)
            // CharGeosets: group 4=gloves(forearm), 5=boots(shin), 8=sleeves, 12=tabard, 13=pants
            std::unordered_set<uint16_t> modelGeosets;
            std::unordered_map<uint16_t, uint16_t> firstByGroup;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (const auto& b : md->batches) {
                    const uint16_t sid = b.submeshId;
                    modelGeosets.insert(sid);
                    const uint16_t group = static_cast<uint16_t>(sid / 100);
                    auto it = firstByGroup.find(group);
                    if (it == firstByGroup.end() || sid < it->second) {
                        firstByGroup[group] = sid;
                    }
                }
            }
            auto pickGeoset = [&](uint16_t preferred, uint16_t group) -> uint16_t {
                if (preferred != 0 && modelGeosets.count(preferred) > 0) return preferred;
                auto it = firstByGroup.find(group);
                if (it != firstByGroup.end()) return it->second;
                return preferred;
            };

            uint16_t geosetGloves = pickGeoset(kGeosetBareForearms, 4);
            uint16_t geosetBoots = pickGeoset(kGeosetBareShins, 5);
            uint16_t geosetSleeves = pickGeoset(kGeosetBareSleeves, 8);
            uint16_t geosetPants = pickGeoset(kGeosetBarePants, 13);
            uint16_t geosetCape = 0;       // Group 15 disabled unless cape is equipped
            uint16_t geosetTabard = pickGeoset(kGeosetDefaultTabard, 12);
            uint16_t geosetBelt = 0;       // Group 18 disabled unless belt is equipped
            rendering::VkTexture* npcCapeTextureId = nullptr;

            // Load equipment geosets from ItemDisplayInfo.dbc
            // DBC columns: 7=GeosetGroup[0], 8=GeosetGroup[1], 9=GeosetGroup[2]
            auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
            const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
            if (itemDisplayDbc) {
                // Equipment slots: 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
                const uint32_t fGG1 = idiL ? (*idiL)["GeosetGroup1"] : 7;

                auto readGeosetGroup = [&](int slot, const char* slotName) -> uint32_t {
                    uint32_t did = extra.equipDisplayId[slot];
                    if (did == 0) return 0;
                    int32_t idx = itemDisplayDbc->findRecordById(did);
                    if (idx < 0) {
                        LOG_DEBUG("NPC equip slot ", slotName, " displayId=", did, " NOT FOUND in ItemDisplayInfo.dbc");
                        return 0;
                    }
                    uint32_t gg = itemDisplayDbc->getUInt32(static_cast<uint32_t>(idx), fGG1);
                    LOG_DEBUG("NPC equip slot ", slotName, " displayId=", did, " GeosetGroup1=", gg);
                    return gg;
                };

                // Chest (slot 3) → group 8 (sleeves/wristbands)
                {
                    uint32_t gg = readGeosetGroup(3, "chest");
                    if (gg > 0) geosetSleeves = pickGeoset(static_cast<uint16_t>(kGeosetBareSleeves + gg), 8);
                }

                // Legs (slot 5) → group 13 (trousers)
                {
                    uint32_t gg = readGeosetGroup(5, "legs");
                    if (gg > 0) geosetPants = pickGeoset(static_cast<uint16_t>(kGeosetBarePants + gg), 13);
                }

                // Feet (slot 6) → group 5 (boots/shins)
                {
                    uint32_t gg = readGeosetGroup(6, "feet");
                    if (gg > 0) geosetBoots = pickGeoset(static_cast<uint16_t>(501 + gg), 5);
                }

                // Hands (slot 8) → group 4 (gloves/forearms)
                {
                    uint32_t gg = readGeosetGroup(8, "hands");
                    if (gg > 0) geosetGloves = pickGeoset(static_cast<uint16_t>(kGeosetBareForearms + gg), 4);
                }

                // Wrists (slot 7) → group 8 (sleeves, only if chest didn't set it)
                {
                    uint32_t gg = readGeosetGroup(7, "wrist");
                    if (gg > 0 && geosetSleeves == pickGeoset(kGeosetBareSleeves, 8))
                        geosetSleeves = pickGeoset(static_cast<uint16_t>(kGeosetBareSleeves + gg), 8);
                }

                // Belt (slot 4) → group 18 (buckle)
                {
                    uint32_t gg = readGeosetGroup(4, "belt");
                    if (gg > 0) geosetBelt = static_cast<uint16_t>(1801 + gg);
                }

                // Tabard (slot 9) → group 12 (tabard/robe mesh)
                {
                    uint32_t gg = readGeosetGroup(9, "tabard");
                    if (gg > 0) geosetTabard = pickGeoset(static_cast<uint16_t>(1200 + gg), 12);
                }

                // Cape (slot 10) → group 15
                if (extra.equipDisplayId[10] != 0) {
                    int32_t idx = itemDisplayDbc->findRecordById(extra.equipDisplayId[10]);
                    if (idx >= 0) {
                        geosetCape = kGeosetWithCape;
                        const bool npcIsFemale = (extra.sexId == 1);
                        const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                        std::vector<std::string> capeNames;
                        auto addName = [&](const std::string& n) {
                            if (!n.empty() && std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                                capeNames.push_back(n);
                            }
                        };
                        std::string leftName = itemDisplayDbc->getString(static_cast<uint32_t>(idx), leftTexField);
                        addName(leftName);

                        auto hasBlpExt = [](const std::string& p) {
                            if (p.size() < 4) return false;
                            std::string ext = p.substr(p.size() - 4);
                            std::transform(ext.begin(), ext.end(), ext.begin(),
                                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            return ext == ".blp";
                        };
                        std::vector<std::string> capeCandidates;
                        auto addCapeCandidate = [&](const std::string& p) {
                            if (p.empty()) return;
                            if (std::find(capeCandidates.begin(), capeCandidates.end(), p) == capeCandidates.end()) {
                                capeCandidates.push_back(p);
                            }
                        };
                        for (const auto& nameRaw : capeNames) {
                            std::string name = nameRaw;
                            std::replace(name.begin(), name.end(), '/', '\\');
                            const bool hasDir = (name.find('\\') != std::string::npos);
                            const bool hasExt = hasBlpExt(name);
                            if (hasDir) {
                                addCapeCandidate(name);
                                if (!hasExt) addCapeCandidate(name + ".blp");
                            } else {
                                std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name;
                                std::string baseTex = "Item\\TextureComponents\\Cape\\" + name;
                                addCapeCandidate(baseObj);
                                addCapeCandidate(baseTex);
                                if (!hasExt) {
                                    addCapeCandidate(baseObj + ".blp");
                                    addCapeCandidate(baseTex + ".blp");
                                }
                                addCapeCandidate(baseObj + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                addCapeCandidate(baseObj + "_U.blp");
                                addCapeCandidate(baseTex + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                addCapeCandidate(baseTex + "_U.blp");
                            }
                        }
                        const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                        for (const auto& candidate : capeCandidates) {
                            rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                            if (tex && tex != whiteTex) {
                                npcCapeTextureId = tex;
                                break;
                            }
                        }
                    }
                }
            }

            // Apply equipment geosets
            activeGeosets.insert(geosetGloves);
            activeGeosets.insert(geosetBoots);
            activeGeosets.insert(geosetSleeves);
            activeGeosets.insert(geosetPants);
            if (geosetCape != 0) {
                activeGeosets.insert(geosetCape);
            }
            if (geosetTabard != 0) {
                activeGeosets.insert(geosetTabard);
            }
            if (geosetBelt != 0) {
                activeGeosets.insert(geosetBelt);
            }
            activeGeosets.insert(pickGeoset(kGeosetDefaultEars, 7));
            activeGeosets.insert(pickGeoset(kGeosetDefaultKneepads, 9));
            activeGeosets.insert(pickGeoset(kGeosetBareFeet, 20));
            // Keep all model-present torso variants active to avoid missing male
            // abdomen/waist sections when a single 5xx pick is wrong.
            for (uint16_t sid : modelGeosets) {
                if ((sid / 100) == 5) activeGeosets.insert(sid);
            }
            // Keep all model-present pelvis variants active to avoid missing waist/belt
            // sections on some humanoid males when a single 9xx variant is wrong.
            for (uint16_t sid : modelGeosets) {
                if ((sid / 100) == 9) activeGeosets.insert(sid);
            }

            // Hide hair under helmets: replace style-specific scalp with bald scalp
            if (extra.equipDisplayId[0] != 0 && hairGeoset > 1) {
                activeGeosets.erase(hairGeoset);                              // Remove style scalp
                activeGeosets.erase(static_cast<uint16_t>(100 + hairGeoset)); // Remove style group 1
                activeGeosets.insert(1);    // Bald scalp cap (group 0)
                activeGeosets.insert(kGeosetDefaultConnector);  // Default group 1 connector
            }

            charRenderer->setActiveGeosets(instanceId, activeGeosets);
            if (geosetCape != 0 && npcCapeTextureId) {
                charRenderer->setGroupTextureOverride(instanceId, 15, npcCapeTextureId);
                if (const auto* md = charRenderer->getModelData(modelId)) {
                    for (size_t ti = 0; ti < md->textures.size(); ti++) {
                        if (md->textures[ti].type == 2) {
                            charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(ti), npcCapeTextureId);
                        }
                    }
                }
            }
            LOG_DEBUG("Set humanoid geosets: hair=", static_cast<int>(hairGeoset),
                      " sleeves=", geosetSleeves, " pants=", geosetPants,
                      " boots=", geosetBoots, " gloves=", geosetGloves);

            // NOTE: NPC helmet attachment with fallback logic to use bone 0 if attachment
            // point 11 is missing. This improves compatibility with models that don't have
            // attachment 11 explicitly defined.
            static constexpr bool kEnableNpcHelmetAttachmentsMainPath = true;
            // Load and attach helmet model if equipped
            if (kEnableNpcHelmetAttachmentsMainPath && extra.equipDisplayId[0] != 0 && itemDisplayDbc) {
                int32_t helmIdx = itemDisplayDbc->findRecordById(extra.equipDisplayId[0]);
                if (helmIdx >= 0) {
                    // Get helmet model name from ItemDisplayInfo.dbc (LeftModel)
                    std::string helmModelName = itemDisplayDbc->getString(static_cast<uint32_t>(helmIdx), idiL ? (*idiL)["LeftModel"] : 1);
                    if (!helmModelName.empty()) {
                        // Convert .mdx to .m2
                        size_t dotPos = helmModelName.rfind('.');
                        if (dotPos != std::string::npos) {
                            helmModelName = helmModelName.substr(0, dotPos);
                        }

                        // WoW helmet M2 files have per-race/gender variants with a suffix
                        // e.g. Helm_Plate_B_01Stormwind_HuM.M2 for Human Male
                        // ChrRaces.dbc ClientPrefix values (raceId → prefix):
                        static const std::unordered_map<uint8_t, std::string> racePrefix = {
                            {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                            {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                        };
                        std::string genderSuffix = (extra.sexId == 0) ? "M" : "F";
                        std::string raceSuffix;
                        auto itRace = racePrefix.find(extra.raceId);
                        if (itRace != racePrefix.end()) {
                            raceSuffix = "_" + itRace->second + genderSuffix;
                        }

                        // Try race/gender-specific variant first, then base name
                        std::string helmPath;
                        std::vector<uint8_t> helmData;
                        if (!raceSuffix.empty()) {
                            helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + raceSuffix + ".m2";
                            helmData = assetManager_->readFile(helmPath);
                        }
                        if (helmData.empty()) {
                            helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + ".m2";
                            helmData = assetManager_->readFile(helmPath);
                        }

                        if (!helmData.empty()) {
                            auto helmModel = pipeline::M2Loader::load(helmData);
                            // Load skin (only for WotLK M2s)
                            std::string skinPath = helmPath.substr(0, helmPath.size() - 3) + "00.skin";
                            auto skinData = assetManager_->readFile(skinPath);
                            if (!skinData.empty() && helmModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, helmModel);
                            }

                            if (helmModel.isValid()) {
                                // Attachment point 11 = Head
                                uint32_t helmModelId = nextCreatureModelId_++;
                                // Get texture from ItemDisplayInfo (LeftModelTexture)
                                std::string helmTexName = itemDisplayDbc->getString(static_cast<uint32_t>(helmIdx), idiL ? (*idiL)["LeftModelTexture"] : 3);
                                std::string helmTexPath;
                                if (!helmTexName.empty()) {
                                    // Try race/gender suffixed texture first
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Head\\" + helmTexName + raceSuffix + ".blp";
                                        if (assetManager_->fileExists(suffixedTex)) {
                                            helmTexPath = suffixedTex;
                                        }
                                    }
                                    if (helmTexPath.empty()) {
                                        helmTexPath = "Item\\ObjectComponents\\Head\\" + helmTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 0, helmModel, helmModelId, helmTexPath);
                                if (!attached) {
                                    attached = charRenderer->attachWeapon(instanceId, 11, helmModel, helmModelId, helmTexPath);
                                }
                                if (attached) {
                                    LOG_DEBUG("Attached helmet model: ", helmPath, " tex: ", helmTexPath);
                                }
                            }
                        }
                    }
                }
            }

            // NPC shoulder attachment: slot 1 = shoulder in the NPC equipment array.
            // Shoulders have TWO M2 models (left + right) at attachment points 5 and 6.
            if (extra.equipDisplayId[1] != 0) {
                int32_t shoulderIdx = itemDisplayDbc->findRecordById(extra.equipDisplayId[1]);
                if (shoulderIdx >= 0) {
                    const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
                    const uint32_t rightModelField = idiL ? (*idiL)["RightModel"] : 2u;
                    const uint32_t leftTexFieldS = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                    const uint32_t rightTexFieldS = idiL ? (*idiL)["RightModelTexture"] : 4u;

                    static const std::unordered_map<uint8_t, std::string> shoulderRacePrefix = {
                        {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                        {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                    };
                    std::string genderSuffix = (extra.sexId == 0) ? "M" : "F";
                    std::string raceSuffix;
                    {
                        auto itRace = shoulderRacePrefix.find(extra.raceId);
                        if (itRace != shoulderRacePrefix.end()) {
                            raceSuffix = "_" + itRace->second + genderSuffix;
                        }
                    }

                    // Left shoulder (attachment point 5) using LeftModel
                    std::string leftModelName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), leftModelField);
                    if (!leftModelName.empty()) {
                        size_t dotPos = leftModelName.rfind('.');
                        if (dotPos != std::string::npos) leftModelName = leftModelName.substr(0, dotPos);

                        std::string leftPath;
                        std::vector<uint8_t> leftData;
                        if (!raceSuffix.empty()) {
                            leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + raceSuffix + ".m2";
                            leftData = assetManager_->readFile(leftPath);
                        }
                        if (leftData.empty()) {
                            leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + ".m2";
                            leftData = assetManager_->readFile(leftPath);
                        }
                        if (!leftData.empty()) {
                            auto leftModel = pipeline::M2Loader::load(leftData);
                            std::string skinPath = leftPath.substr(0, leftPath.size() - 3) + "00.skin";
                            auto skinData = assetManager_->readFile(skinPath);
                            if (!skinData.empty() && leftModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, leftModel);
                            }
                            if (leftModel.isValid()) {
                                uint32_t leftModelId = nextCreatureModelId_++;
                                std::string leftTexName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), leftTexFieldS);
                                std::string leftTexPath;
                                if (!leftTexName.empty()) {
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + raceSuffix + ".blp";
                                        if (assetManager_->fileExists(suffixedTex)) leftTexPath = suffixedTex;
                                    }
                                    if (leftTexPath.empty()) {
                                        leftTexPath = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 5, leftModel, leftModelId, leftTexPath);
                                if (attached) {
                                    LOG_DEBUG("NPC attached left shoulder: ", leftPath, " tex: ", leftTexPath);
                                }
                            }
                        }
                    }

                    // Right shoulder (attachment point 6) using RightModel
                    std::string rightModelName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), rightModelField);
                    if (!rightModelName.empty()) {
                        size_t dotPos = rightModelName.rfind('.');
                        if (dotPos != std::string::npos) rightModelName = rightModelName.substr(0, dotPos);

                        std::string rightPath;
                        std::vector<uint8_t> rightData;
                        if (!raceSuffix.empty()) {
                            rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + raceSuffix + ".m2";
                            rightData = assetManager_->readFile(rightPath);
                        }
                        if (rightData.empty()) {
                            rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + ".m2";
                            rightData = assetManager_->readFile(rightPath);
                        }
                        if (!rightData.empty()) {
                            auto rightModel = pipeline::M2Loader::load(rightData);
                            std::string skinPath = rightPath.substr(0, rightPath.size() - 3) + "00.skin";
                            auto skinData = assetManager_->readFile(skinPath);
                            if (!skinData.empty() && rightModel.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, rightModel);
                            }
                            if (rightModel.isValid()) {
                                uint32_t rightModelId = nextCreatureModelId_++;
                                std::string rightTexName = itemDisplayDbc->getString(static_cast<uint32_t>(shoulderIdx), rightTexFieldS);
                                std::string rightTexPath;
                                if (!rightTexName.empty()) {
                                    if (!raceSuffix.empty()) {
                                        std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + raceSuffix + ".blp";
                                        if (assetManager_->fileExists(suffixedTex)) rightTexPath = suffixedTex;
                                    }
                                    if (rightTexPath.empty()) {
                                        rightTexPath = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + ".blp";
                                    }
                                }
                                bool attached = charRenderer->attachWeapon(instanceId, 6, rightModel, rightModelId, rightTexPath);
                                if (attached) {
                                    LOG_DEBUG("NPC attached right shoulder: ", rightPath, " tex: ", rightTexPath);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // With full humanoid overrides disabled, some character-style NPC models still render
    // conflicting clothing geosets at once (global capes, robe skirts over trousers).
    // Normalize only clothing groups while leaving all other model batches untouched.
    if (const auto* md = charRenderer->getModelData(modelId)) {
        std::unordered_set<uint16_t> allGeosets;
        std::unordered_map<uint16_t, uint16_t> firstByGroup;
        bool hasGroup3 = false;  // glove/forearm variants
        bool hasGroup4 = false;  // glove/forearm variants (some models)
        bool hasGroup8 = false;  // sleeve/wrist variants
        bool hasGroup12 = false; // tabard variants
        bool hasGroup13 = false; // trousers/robe skirt variants
        bool hasGroup15 = false; // cloak variants
        for (const auto& b : md->batches) {
            const uint16_t sid = b.submeshId;
            const uint16_t group = static_cast<uint16_t>(sid / 100);
            allGeosets.insert(sid);
            auto itFirst = firstByGroup.find(group);
            if (itFirst == firstByGroup.end() || sid < itFirst->second) {
                firstByGroup[group] = sid;
            }
            if (group == 3) hasGroup3 = true;
            if (group == 4) hasGroup4 = true;
            if (group == 8) hasGroup8 = true;
            if (group == 12) hasGroup12 = true;
            if (group == 13) hasGroup13 = true;
            if (group == 15) hasGroup15 = true;
        }

        // Only apply to humanoid-like clothing models.
        if (hasGroup3 || hasGroup4 || hasGroup8 || hasGroup12 || hasGroup13 || hasGroup15) {
            bool hasRenderableCape = false;
            bool hasEquippedTabard = false;
            bool hasHumanoidExtra = false;
            uint8_t extraRaceId = 0;
            uint8_t extraSexId = 0;
            uint16_t selectedHairScalp = 1;
            uint16_t selectedFacial200 = 200;
            uint16_t selectedFacial300 = 300;
            uint16_t selectedFacial300Alt = 300;
            bool wantsFacialHair = false;
            uint32_t equipChestGG = 0, equipLegsGG = 0, equipFeetGG = 0;
            std::unordered_set<uint16_t> hairScalpGeosetsForRaceSex;
            if (itDisplayData != displayDataMap_.end() &&
                itDisplayData->second.extraDisplayId != 0) {
                auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
                if (itExtra != humanoidExtraMap_.end()) {
                    hasHumanoidExtra = true;
                    extraRaceId = itExtra->second.raceId;
                    extraSexId = itExtra->second.sexId;
                    hasEquippedTabard = (itExtra->second.equipDisplayId[9] != 0);
                    uint32_t hairKey = (static_cast<uint32_t>(extraRaceId) << 16) |
                                       (static_cast<uint32_t>(extraSexId) << 8) |
                                       static_cast<uint32_t>(itExtra->second.hairStyleId);
                    auto itHairGeo = hairGeosetMap_.find(hairKey);
                    if (itHairGeo != hairGeosetMap_.end() && itHairGeo->second > 0) {
                        selectedHairScalp = itHairGeo->second;
                    }
                    uint32_t facialKey = (static_cast<uint32_t>(extraRaceId) << 16) |
                                         (static_cast<uint32_t>(extraSexId) << 8) |
                                         static_cast<uint32_t>(itExtra->second.facialHairId);
                    wantsFacialHair = (itExtra->second.facialHairId != 0);
                    auto itFacial = facialHairGeosetMap_.find(facialKey);
                    if (itFacial != facialHairGeosetMap_.end()) {
                        selectedFacial200 = static_cast<uint16_t>(200 + itFacial->second.geoset200);
                        selectedFacial300 = static_cast<uint16_t>(300 + itFacial->second.geoset300);
                        selectedFacial300Alt = static_cast<uint16_t>(300 + itFacial->second.geoset200);
                    }
                    for (const auto& [k, v] : hairGeosetMap_) {
                        uint8_t race = static_cast<uint8_t>((k >> 16) & 0xFF);
                        uint8_t sex = static_cast<uint8_t>((k >> 8) & 0xFF);
                        if (race == extraRaceId && sex == extraSexId && v > 0 && v < 100) {
                            hairScalpGeosetsForRaceSex.insert(v);
                        }
                    }
                    auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
                    const auto* idiL = pipeline::getActiveDBCLayout()
                        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

                    uint32_t capeDisplayId = itExtra->second.equipDisplayId[10];
                    if (capeDisplayId != 0 && itemDisplayDbc) {
                            int32_t recIdx = itemDisplayDbc->findRecordById(capeDisplayId);
                            if (recIdx >= 0) {
                                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                                const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;
                                std::vector<std::string> capeNames;
                                auto addName = [&](const std::string& n) {
                                    if (!n.empty() &&
                                        std::find(capeNames.begin(), capeNames.end(), n) == capeNames.end()) {
                                        capeNames.push_back(n);
                                    }
                                };
                                addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), leftTexField));
                                addName(itemDisplayDbc->getString(static_cast<uint32_t>(recIdx), rightTexField));

                                auto hasBlpExt = [](const std::string& p) {
                                    if (p.size() < 4) return false;
                                    std::string ext = p.substr(p.size() - 4);
                                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                    return ext == ".blp";
                                };

                                const bool npcIsFemale = (itExtra->second.sexId == 1);
                                std::vector<std::string> candidates;
                                auto addCandidate = [&](const std::string& p) {
                                    if (p.empty()) return;
                                    if (std::find(candidates.begin(), candidates.end(), p) == candidates.end()) {
                                        candidates.push_back(p);
                                    }
                                };

                                for (const auto& raw : capeNames) {
                                    std::string name = raw;
                                    std::replace(name.begin(), name.end(), '/', '\\');
                                    const bool hasDir = (name.find('\\') != std::string::npos);
                                    const bool hasExt = hasBlpExt(name);
                                    if (hasDir) {
                                        addCandidate(name);
                                        if (!hasExt) addCandidate(name + ".blp");
                                    } else {
                                        std::string baseObj = "Item\\ObjectComponents\\Cape\\" + name;
                                        std::string baseTex = "Item\\TextureComponents\\Cape\\" + name;
                                        addCandidate(baseObj);
                                        addCandidate(baseTex);
                                        if (!hasExt) {
                                            addCandidate(baseObj + ".blp");
                                            addCandidate(baseTex + ".blp");
                                        }
                                        addCandidate(baseObj + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                        addCandidate(baseObj + "_U.blp");
                                        addCandidate(baseTex + (npcIsFemale ? "_F.blp" : "_M.blp"));
                                        addCandidate(baseTex + "_U.blp");
                                    }
                                }

                                for (const auto& p : candidates) {
                                    if (assetManager_->fileExists(p)) {
                                        hasRenderableCape = true;
                                        break;
                                    }
                                }
                            }
                    }

                    // Read GeosetGroup1 from equipment to drive clothed mesh selection
                    if (itemDisplayDbc) {
                        const uint32_t fGG1 = idiL ? (*idiL)["GeosetGroup1"] : 7;
                        auto readGG = [&](uint32_t did) -> uint32_t {
                            if (did == 0) return 0;
                            int32_t idx = itemDisplayDbc->findRecordById(did);
                            return (idx >= 0) ? itemDisplayDbc->getUInt32(static_cast<uint32_t>(idx), fGG1) : 0;
                        };
                        equipChestGG = readGG(itExtra->second.equipDisplayId[3]);
                        if (equipChestGG == 0) equipChestGG = readGG(itExtra->second.equipDisplayId[2]); // shirt fallback
                        equipLegsGG = readGG(itExtra->second.equipDisplayId[5]);
                        equipFeetGG = readGG(itExtra->second.equipDisplayId[6]);
                    }
                }
            }

            std::unordered_set<uint16_t> normalizedGeosets;
            for (uint16_t sid : allGeosets) {
                const uint16_t group = static_cast<uint16_t>(sid / 100);
                if (group == 3 || group == 4 || group == 8 || group == 12 || group == 13 || group == 15) continue;
                // Some humanoid models carry cloak cloth in group 16. Strip this too
                // when no cape is equipped to avoid "everyone has a cape".
                if (!hasRenderableCape && group == 16) continue;
                // Group 0 can contain multiple scalp/hair meshes. Keep only the selected
                // race/sex/style scalp to avoid overlapping broken hair.
                if (hasHumanoidExtra && sid < 100 && hairScalpGeosetsForRaceSex.count(sid) > 0 && sid != selectedHairScalp) {
                    continue;
                }
                // Group 1 contains connector variants that mirror scalp style.
                if (hasHumanoidExtra && group == 1) {
                    const uint16_t selectedConnector = static_cast<uint16_t>(100 + std::max<uint16_t>(selectedHairScalp, 1));
                    if (sid != selectedConnector) {
                        // Keep fallback connector only when selected one does not exist on this model.
                        if (sid != 101 || allGeosets.count(selectedConnector) > 0) {
                            continue;
                        }
                    }
                }
                // Group 2 facial variants: keep selected variant; fallback only if missing.
                if (hasHumanoidExtra && group == 2) {
                    if (!wantsFacialHair) {
                        continue;
                    }
                    if (sid != selectedFacial200) {
                        if (sid != 200 && sid != 201) {
                            continue;
                        }
                        if (allGeosets.count(selectedFacial200) > 0) {
                            continue;
                        }
                    }
                }
                normalizedGeosets.insert(sid);
            }

            auto pickFromGroup = [&](uint16_t preferredSid, uint16_t group) -> uint16_t {
                if (allGeosets.count(preferredSid) > 0) return preferredSid;
                auto it = firstByGroup.find(group);
                if (it != firstByGroup.end()) return it->second;
                return 0;
            };

            // Intentionally do not add group 3 (glove/forearm accessory meshes).
            // Even "bare" variants can produce unwanted looped arm geometry on NPCs.

            if (hasGroup4) {
                uint16_t wantBoots = (equipFeetGG > 0) ? static_cast<uint16_t>(400 + equipFeetGG) : kGeosetBareForearms;
                uint16_t bootsSid = pickFromGroup(wantBoots, 4);
                if (bootsSid != 0) normalizedGeosets.insert(bootsSid);
            }

            // Add sleeve/wrist meshes when chest armor calls for them.
            if (hasGroup8 && equipChestGG > 0) {
                uint16_t wantSleeves = static_cast<uint16_t>(800 + equipChestGG);
                uint16_t sleeveSid = pickFromGroup(wantSleeves, 8);
                if (sleeveSid != 0) normalizedGeosets.insert(sleeveSid);
            }

            // Show tabard mesh only when CreatureDisplayInfoExtra equips one.
            if (hasGroup12 && hasEquippedTabard) {
                uint16_t wantTabard = kGeosetDefaultTabard;  // Default fallback

                // Try to read tabard geoset variant from ItemDisplayInfo.dbc (slot 9)
                if (hasHumanoidExtra && itDisplayData != displayDataMap_.end() &&
                    itDisplayData->second.extraDisplayId != 0) {
                    auto itExtra = humanoidExtraMap_.find(itDisplayData->second.extraDisplayId);
                    if (itExtra != humanoidExtraMap_.end()) {
                        uint32_t tabardDisplayId = itExtra->second.equipDisplayId[9];
                        if (tabardDisplayId != 0) {
                            auto itemDisplayDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
                            const auto* idiL = pipeline::getActiveDBCLayout()
                                ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                            if (itemDisplayDbc && idiL) {
                                int32_t tabardIdx = itemDisplayDbc->findRecordById(tabardDisplayId);
                                if (tabardIdx >= 0) {
                                    // Get geoset variant from ItemDisplayInfo GeosetGroup1 field
                                    const uint32_t ggField = (*idiL)["GeosetGroup1"];
                                    uint32_t tabardGG = itemDisplayDbc->getUInt32(static_cast<uint32_t>(tabardIdx), ggField);
                                    if (tabardGG > 0) {
                                        wantTabard = static_cast<uint16_t>(1200 + tabardGG);
                                    }
                                }
                            }
                        }
                    }
                }

                uint16_t tabardSid = pickFromGroup(wantTabard, 12);
                if (tabardSid != 0) normalizedGeosets.insert(tabardSid);
            }

            // Some mustache/goatee variants are authored in facial group 3xx.
            // Re-add selected facial 3xx plus low-index facial fallbacks.
            if (hasHumanoidExtra && wantsFacialHair) {
                // Prefer alt channel first (often chin-beard), then primary.
                uint16_t facial300Sid = pickFromGroup(selectedFacial300Alt, 3);
                if (facial300Sid == 0) facial300Sid = pickFromGroup(selectedFacial300, 3);
                if (facial300Sid != 0) normalizedGeosets.insert(facial300Sid);
                if (facial300Sid == 0) {
                    if (allGeosets.count(300) > 0) normalizedGeosets.insert(300);
                    else if (allGeosets.count(301) > 0) normalizedGeosets.insert(301);
                }
            }

            // Prefer trousers geoset; use covered variant when legs armor exists.
            if (hasGroup13) {
                uint16_t wantPants = (equipLegsGG > 0) ? static_cast<uint16_t>(1300 + equipLegsGG) : kGeosetBarePants;
                uint16_t pantsSid = pickFromGroup(wantPants, 13);
                if (pantsSid != 0) normalizedGeosets.insert(pantsSid);
            }

            // Prefer explicit cloak variant only when a cape is equipped.
            if (hasGroup15 && hasRenderableCape) {
                uint16_t capeSid = pickFromGroup(kGeosetWithCape, 15);
                if (capeSid != 0) normalizedGeosets.insert(capeSid);
            }

            if (!normalizedGeosets.empty()) {
                charRenderer->setActiveGeosets(instanceId, normalizedGeosets);
            }
        }
    }

    // Try attaching NPC held weapons; if update fields are not ready yet,
    // IN_GAME retry loop will attempt again shortly.
    bool weaponsAttachedNow = tryAttachCreatureVirtualWeapons(guid, instanceId);

    // Spawn in the correct pose. If the server marked this creature dead before
    // the queued spawn was processed, start directly in death animation.
    if (deadCreatureGuids_.count(guid)) {
        charRenderer->playAnimation(instanceId, rendering::anim::DEATH, false);
    } else {
        // Check if this NPC has a persistent emote state (e.g. working, eating, dancing)
        uint32_t npcEmote = 0;
        if (gameHandler_) {
            auto entity = gameHandler_->getEntityManager().getEntity(guid);
            if (entity && entity->getType() == game::ObjectType::UNIT) {
                npcEmote = std::static_pointer_cast<game::Unit>(entity)->getNpcEmoteState();
            }
        }
        if (npcEmote != 0 && charRenderer->hasAnimation(instanceId, npcEmote)) {
            charRenderer->playAnimation(instanceId, npcEmote, true);
        } else if (charRenderer->hasAnimation(instanceId, rendering::anim::BIRTH)) {
            // Play birth animation (one-shot) — will return to STAND after
            charRenderer->playAnimation(instanceId, rendering::anim::BIRTH, false);
        } else if (charRenderer->hasAnimation(instanceId, rendering::anim::SPAWN)) {
            charRenderer->playAnimation(instanceId, rendering::anim::SPAWN, false);
        } else {
            charRenderer->playAnimation(instanceId, rendering::anim::STAND, true);
        }
    }
    charRenderer->startFadeIn(instanceId, 0.5f);

    // Track instance
    creatureInstances_[guid] = instanceId;
    creatureModelIds_[guid] = modelId;
    creatureRenderPosCache_[guid] = renderPos;
    if (weaponsAttachedNow) {
        creatureWeaponsAttached_.insert(guid);
        creatureWeaponAttachAttempts_.erase(guid);
    } else {
        creatureWeaponsAttached_.erase(guid);
        creatureWeaponAttachAttempts_[guid] = 1;
    }
    LOG_DEBUG("Spawned creature: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

void EntitySpawner::spawnOnlinePlayer(uint64_t guid,
                                    uint8_t raceId,
                                    uint8_t genderId,
                                    uint32_t appearanceBytes,
                                    uint8_t facialFeatures,
                                    float x, float y, float z, float orientation) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;
    if (playerInstances_.count(guid)) return;

    // Skip local player — already spawned as the main character
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        uint64_t activeGuid = gameHandler_->getActiveCharacterGuid();
        if ((localGuid != 0 && guid == localGuid) ||
            (activeGuid != 0 && guid == activeGuid) ||
            (spawnedPlayerGuid_ != 0 && guid == spawnedPlayerGuid_)) {
            return;
        }
    }
    auto* charRenderer = renderer_->getCharacterRenderer();

    // Base geometry model: cache by (race, gender)
    uint32_t cacheKey = (static_cast<uint32_t>(raceId) << 8) | static_cast<uint32_t>(genderId & 0xFF);
    uint32_t modelId = 0;
    auto itCache = playerModelCache_.find(cacheKey);
    if (itCache != playerModelCache_.end()) {
        modelId = itCache->second;
    } else {
        game::Race race = static_cast<game::Race>(raceId);
        game::Gender gender = (genderId == 1) ? game::Gender::FEMALE : game::Gender::MALE;
        std::string m2Path = game::getPlayerModelPath(race, gender);
        if (m2Path.empty()) {
            LOG_WARNING("spawnOnlinePlayer: unknown race/gender for guid 0x", std::hex, guid, std::dec,
                        " race=", static_cast<int>(raceId), " gender=", static_cast<int>(genderId));
            return;
        }

        // Parse modelDir/baseName for skin/anim loading
        std::string modelDir;
        std::string baseName;
        {
            size_t slash = m2Path.rfind('\\');
            if (slash != std::string::npos) {
                modelDir = m2Path.substr(0, slash + 1);
                baseName = m2Path.substr(slash + 1);
            } else {
                baseName = m2Path;
            }
            size_t dot = baseName.rfind('.');
            if (dot != std::string::npos) baseName = baseName.substr(0, dot);
        }

        auto m2Data = assetManager_->readFile(m2Path);
        if (m2Data.empty()) {
            LOG_WARNING("spawnOnlinePlayer: failed to read M2: ", m2Path);
            return;
        }

        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.vertices.empty()) {
            LOG_WARNING("spawnOnlinePlayer: failed to parse M2: ", m2Path);
            return;
        }

        // Skin file (only for WotLK M2s - vanilla has embedded skin)
        std::string skinPath = modelDir + baseName + "00.skin";
        auto skinData = assetManager_->readFile(skinPath);
        if (!skinData.empty() && model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, model);
        }

        // After skin loading, full model must be valid (vertices + indices)
        if (!model.isValid()) {
            LOG_WARNING("spawnOnlinePlayer: failed to load skin for M2: ", m2Path);
            return;
        }

        // Load only core external animations (stand/walk/run) to avoid stalls
        for (uint32_t si = 0; si < model.sequences.size(); si++) {
            if (!(model.sequences[si].flags & 0x20)) {
                uint32_t animId = model.sequences[si].id;
                if (animId != rendering::anim::STAND && animId != rendering::anim::WALK && animId != rendering::anim::RUN) continue;
                char animFileName[256];
                snprintf(animFileName, sizeof(animFileName),
                         "%s%s%04u-%02u.anim",
                         modelDir.c_str(),
                         baseName.c_str(),
                         animId,
                         model.sequences[si].variationIndex);
                auto animData = assetManager_->readFileOptional(animFileName);
                if (!animData.empty()) {
                    pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
                }
            }
        }

        modelId = nextPlayerModelId_++;
        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("spawnOnlinePlayer: failed to load model to GPU: ", m2Path);
            return;
        }

        playerModelCache_[cacheKey] = modelId;
    }

    // Determine texture slots once per model
    {
        auto [slotIt, inserted] = playerTextureSlotsByModelId_.try_emplace(modelId);
        if (inserted) {
            PlayerTextureSlots slots;
            if (const auto* md = charRenderer->getModelData(modelId)) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    uint32_t t = md->textures[ti].type;
                    if (t == 1 && slots.skin < 0) slots.skin = static_cast<int>(ti);
                    else if (t == 6 && slots.hair < 0) slots.hair = static_cast<int>(ti);
                    else if (t == 8 && slots.underwear < 0) slots.underwear = static_cast<int>(ti);
                }
            }
            slotIt->second = slots;
        }
    }

    // Create instance at server position
    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    float renderYaw = orientation + glm::radians(90.0f);
    uint32_t instanceId = charRenderer->createInstance(modelId, renderPos, glm::vec3(0.0f, 0.0f, renderYaw), 1.0f);
    if (instanceId == 0) return;

    // Resolve skin/hair texture paths via CharSections, then apply as per-instance overrides
    const char* raceFolderName = "Human";
    switch (static_cast<game::Race>(raceId)) {
        case game::Race::HUMAN: raceFolderName = "Human"; break;
        case game::Race::ORC: raceFolderName = "Orc"; break;
        case game::Race::DWARF: raceFolderName = "Dwarf"; break;
        case game::Race::NIGHT_ELF: raceFolderName = "NightElf"; break;
        case game::Race::UNDEAD: raceFolderName = "Scourge"; break;
        case game::Race::TAUREN: raceFolderName = "Tauren"; break;
        case game::Race::GNOME: raceFolderName = "Gnome"; break;
        case game::Race::TROLL: raceFolderName = "Troll"; break;
        case game::Race::BLOOD_ELF: raceFolderName = "BloodElf"; break;
        case game::Race::DRAENEI: raceFolderName = "Draenei"; break;
        default: break;
    }
    const char* genderFolder = (genderId == 1) ? "Female" : "Male";
    std::string raceGender = std::string(raceFolderName) + genderFolder;
    std::string bodySkinPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "Skin00_00.blp";
    std::string pelvisPath = std::string("Character\\") + raceFolderName + "\\" + genderFolder + "\\" + raceGender + "NakedPelvisSkin00_00.blp";
    std::vector<std::string> underwearPaths;
    std::string hairTexturePath;
    std::string faceLowerPath;
    std::string faceUpperPath;

    uint8_t skinId = appearanceBytes & 0xFF;
    uint8_t faceId = (appearanceBytes >> 8) & 0xFF;
    uint8_t hairStyleId = (appearanceBytes >> 16) & 0xFF;
    uint8_t hairColorId = (appearanceBytes >> 24) & 0xFF;

    if (auto charSectionsDbc = assetManager_->loadDBC("CharSections.dbc"); charSectionsDbc && charSectionsDbc->isLoaded()) {
        const auto* csL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
        auto csF = pipeline::detectCharSectionsFields(charSectionsDbc.get(), csL);
        uint32_t targetRaceId = raceId;
        uint32_t targetSexId = genderId;

        bool foundSkin = false;
        bool foundUnderwear = false;
        bool foundHair = false;
        bool foundFaceLower = false;

        for (uint32_t r = 0; r < charSectionsDbc->getRecordCount(); r++) {
            uint32_t rRace = charSectionsDbc->getUInt32(r, csF.raceId);
            uint32_t rSex = charSectionsDbc->getUInt32(r, csF.sexId);
            uint32_t baseSection = charSectionsDbc->getUInt32(r, csF.baseSection);
            uint32_t variationIndex = charSectionsDbc->getUInt32(r, csF.variationIndex);
            uint32_t colorIndex = charSectionsDbc->getUInt32(r, csF.colorIndex);

            if (rRace != targetRaceId || rSex != targetSexId) continue;

            if (baseSection == 0 && !foundSkin && colorIndex == skinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                if (!tex1.empty()) { bodySkinPath = tex1; foundSkin = true; }
            } else if (baseSection == 3 && !foundHair &&
                       variationIndex == hairStyleId && colorIndex == hairColorId) {
                hairTexturePath = charSectionsDbc->getString(r, csF.texture1);
                if (!hairTexturePath.empty()) foundHair = true;
            } else if (baseSection == 4 && !foundUnderwear && colorIndex == skinId) {
                for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                    std::string tex = charSectionsDbc->getString(r, f);
                    if (!tex.empty()) underwearPaths.push_back(tex);
                }
                foundUnderwear = true;
            } else if (baseSection == 1 && !foundFaceLower &&
                       variationIndex == faceId && colorIndex == skinId) {
                std::string tex1 = charSectionsDbc->getString(r, csF.texture1);
                std::string tex2 = charSectionsDbc->getString(r, csF.texture2);
                if (!tex1.empty()) faceLowerPath = tex1;
                if (!tex2.empty()) faceUpperPath = tex2;
                foundFaceLower = true;
            }

            if (foundSkin && foundUnderwear && foundHair && foundFaceLower) break;
        }
    }

    // Composite base skin + face + underwear overlays
    rendering::VkTexture* compositeTex = nullptr;
    {
        std::vector<std::string> layers;
        layers.push_back(bodySkinPath);
        if (!faceLowerPath.empty()) layers.push_back(faceLowerPath);
        if (!faceUpperPath.empty()) layers.push_back(faceUpperPath);
        for (const auto& up : underwearPaths) layers.push_back(up);
        if (layers.size() > 1) {
            compositeTex = charRenderer->compositeTextures(layers);
        } else {
            compositeTex = charRenderer->loadTexture(bodySkinPath);
        }
    }

    rendering::VkTexture* hairTex = nullptr;
    if (!hairTexturePath.empty()) {
        hairTex = charRenderer->loadTexture(hairTexturePath);
    }
    rendering::VkTexture* underwearTex = nullptr;
    if (!underwearPaths.empty()) underwearTex = charRenderer->loadTexture(underwearPaths[0]);
    else underwearTex = charRenderer->loadTexture(pelvisPath);

    const PlayerTextureSlots& slots = playerTextureSlotsByModelId_[modelId];
    if (slots.skin >= 0 && compositeTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.skin), compositeTex);
    }
    if (slots.hair >= 0 && hairTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.hair), hairTex);
    }
    if (slots.underwear >= 0 && underwearTex) {
        charRenderer->setTextureSlotOverride(instanceId, static_cast<uint16_t>(slots.underwear), underwearTex);
    }

    // Geosets: body + hair/facial hair selections
    std::unordered_set<uint16_t> activeGeosets;
    // Body parts (group 0: IDs 0-99, some models use up to 27)
    for (uint16_t i = 0; i <= 99; i++) activeGeosets.insert(i);
    activeGeosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1));
    activeGeosets.insert(static_cast<uint16_t>(200 + facialFeatures + 1));
    activeGeosets.insert(kGeosetBareForearms);
    activeGeosets.insert(kGeosetBareShins);
    activeGeosets.insert(kGeosetDefaultEars);
    activeGeosets.insert(kGeosetBareSleeves);
    activeGeosets.insert(kGeosetDefaultKneepads);
    activeGeosets.insert(kGeosetBarePants);
    activeGeosets.insert(kGeosetWithCape);
    activeGeosets.insert(kGeosetBareFeet);
    charRenderer->setActiveGeosets(instanceId, activeGeosets);

    charRenderer->playAnimation(instanceId, rendering::anim::STAND, true);
    playerInstances_[guid] = instanceId;

    OnlinePlayerAppearanceState st;
    st.instanceId = instanceId;
    st.modelId = modelId;
    st.raceId = raceId;
    st.genderId = genderId;
    st.appearanceBytes = appearanceBytes;
    st.facialFeatures = facialFeatures;
    st.bodySkinPath = bodySkinPath;
    // Include face textures so compositeWithRegions can rebuild the full base
    if (!faceLowerPath.empty()) st.underwearPaths.push_back(faceLowerPath);
    if (!faceUpperPath.empty()) st.underwearPaths.push_back(faceUpperPath);
    for (const auto& up : underwearPaths) st.underwearPaths.push_back(up);
    onlinePlayerAppearance_[guid] = std::move(st);
}

void EntitySpawner::setOnlinePlayerEquipment(uint64_t guid,
                                          const std::array<uint32_t, 19>& displayInfoIds,
                                          const std::array<uint8_t, 19>& inventoryTypes) {
    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_ || !assetManager_->isInitialized()) return;

    // Skip local player — equipment handled by GameScreen::updateCharacterGeosets/Textures
    // via consumeOnlineEquipmentDirty(), which fires on the same server update.
    if (gameHandler_) {
        uint64_t localGuid = gameHandler_->getPlayerGuid();
        if (localGuid != 0 && guid == localGuid) return;
    }

    // If the player isn't spawned yet, store equipment until spawn.
    auto appIt = onlinePlayerAppearance_.find(guid);
    if (!playerInstances_.count(guid) || appIt == onlinePlayerAppearance_.end()) {
        pendingOnlinePlayerEquipment_[guid] = {displayInfoIds, inventoryTypes};
        return;
    }

    const OnlinePlayerAppearanceState& st = appIt->second;

    auto* charRenderer = renderer_->getCharacterRenderer();
    if (!charRenderer) return;
    if (st.instanceId == 0 || st.modelId == 0) return;

    if (st.bodySkinPath.empty()) {
        LOG_WARNING("setOnlinePlayerEquipment: bodySkinPath empty for guid=0x", std::hex, guid, std::dec,
                    " instanceId=", st.instanceId, " — skipping equipment");
        return;
    }

    int nonZeroDisplay = 0;
    for (uint32_t d : displayInfoIds) if (d != 0) nonZeroDisplay++;
    LOG_WARNING("setOnlinePlayerEquipment: guid=0x", std::hex, guid, std::dec,
                " instanceId=", st.instanceId, " nonZeroDisplayIds=", nonZeroDisplay,
                " head=", displayInfoIds[0], " chest=", displayInfoIds[4],
                " legs=", displayInfoIds[6], " mainhand=", displayInfoIds[15]);

    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return;
    const auto* idiL = pipeline::getActiveDBCLayout() ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    auto getGeosetGroup = [&](uint32_t displayInfoId, uint32_t fieldIdx) -> uint32_t {
        if (displayInfoId == 0) return 0;
        int32_t recIdx = displayInfoDbc->findRecordById(displayInfoId);
        if (recIdx < 0) return 0;
        return displayInfoDbc->getUInt32(static_cast<uint32_t>(recIdx), fieldIdx);
    };

    auto findDisplayIdByInvType = [&](std::initializer_list<uint8_t> types) -> uint32_t {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0 || displayInfoIds[s] == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return displayInfoIds[s];
            }
        }
        return 0;
    };

    auto hasInvType = [&](std::initializer_list<uint8_t> types) -> bool {
        for (int s = 0; s < 19; s++) {
            uint8_t inv = inventoryTypes[s];
            if (inv == 0) continue;
            for (uint8_t t : types) {
                if (inv == t) return true;
            }
        }
        return false;
    };

    // --- Geosets ---
    // Mirror the same group-range logic as CharacterPreview::applyEquipment to
    // keep other-player rendering consistent with the local character preview.
    // Group 4 (4xx) = forearms/gloves, 5 (5xx) = shins/boots, 8 (8xx) = wrists/sleeves,
    // 13 (13xx) = legs/trousers.  Missing defaults caused the shin-mesh gap (status.md).
    std::unordered_set<uint16_t> geosets;
    // Body parts (group 0: IDs 0-99, some models use up to 27)
    for (uint16_t i = 0; i <= 99; i++) geosets.insert(i);

    uint8_t hairStyleId = static_cast<uint8_t>((st.appearanceBytes >> 16) & 0xFF);
    geosets.insert(static_cast<uint16_t>(100 + hairStyleId + 1));
    geosets.insert(static_cast<uint16_t>(200 + st.facialFeatures + 1));
    geosets.insert(701);                  // Ears
    geosets.insert(kGeosetDefaultKneepads); // Kneepads
    geosets.insert(kGeosetBareFeet);        // Bare feet mesh

    const uint32_t geosetGroup1Field = idiL ? (*idiL)["GeosetGroup1"] : 7;
    const uint32_t geosetGroup3Field = idiL ? (*idiL)["GeosetGroup3"] : 9;

    // Per-group defaults — overridden below when equipment provides a geoset value.
    uint16_t geosetGloves  = kGeosetBareForearms;
    uint16_t geosetBoots   = kGeosetBareShins;
    uint16_t geosetSleeves = kGeosetBareSleeves;
    uint16_t geosetPants   = kGeosetBarePants;

    // Chest/Shirt/Robe (invType 4,5,20) → wrist/sleeve group 8
    {
        uint32_t did = findDisplayIdByInvType({4, 5, 20});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetSleeves = static_cast<uint16_t>(kGeosetBareSleeves + gg1);
        // Robe kilt → leg group 13
        uint32_t gg3 = getGeosetGroup(did, geosetGroup3Field);
        if (gg3 > 0) geosetPants = static_cast<uint16_t>(kGeosetBarePants + gg3);
    }

    // Legs (invType 7) → leg group 13
    {
        uint32_t did = findDisplayIdByInvType({7});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetPants = static_cast<uint16_t>(kGeosetBarePants + gg1);
    }

    // Feet/Boots (invType 8) → shin group 5
    {
        uint32_t did = findDisplayIdByInvType({8});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetBoots = static_cast<uint16_t>(501 + gg1);
    }

    // Hands/Gloves (invType 10) → forearm group 4
    {
        uint32_t did = findDisplayIdByInvType({10});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetGloves = static_cast<uint16_t>(kGeosetBareForearms + gg1);
    }

    // Wrists/Bracers (invType 9) → sleeve group 8 (only if chest/shirt didn't set it)
    {
        uint32_t did = findDisplayIdByInvType({9});
        if (did != 0 && geosetSleeves == kGeosetBareSleeves) {
            uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
            if (gg1 > 0) geosetSleeves = static_cast<uint16_t>(kGeosetBareSleeves + gg1);
        }
    }

    // Waist/Belt (invType 6) → buckle group 18
    uint16_t geosetBelt = 0;
    {
        uint32_t did = findDisplayIdByInvType({6});
        uint32_t gg1 = getGeosetGroup(did, geosetGroup1Field);
        if (gg1 > 0) geosetBelt = static_cast<uint16_t>(1801 + gg1);
    }

    geosets.insert(geosetGloves);
    geosets.insert(geosetBoots);
    geosets.insert(geosetSleeves);
    geosets.insert(geosetPants);
    if (geosetBelt != 0) geosets.insert(geosetBelt);
    // Back/Cloak (invType 16)
    geosets.insert(hasInvType({16}) ? kGeosetWithCape : kGeosetNoCape);
    // Tabard (invType 19)
    if (hasInvType({19})) geosets.insert(kGeosetDefaultTabard);

    // Hide hair under helmets: replace style-specific scalp with bald scalp
    // HEAD slot is index 0 in the 19-element equipment array
    if (displayInfoIds[0] != 0 && hairStyleId > 0) {
        uint16_t hairGeoset = static_cast<uint16_t>(hairStyleId + 1);
        geosets.erase(static_cast<uint16_t>(100 + hairGeoset)); // Remove style group 1
        geosets.insert(kGeosetDefaultConnector);  // Default group 1 connector
    }

    charRenderer->setActiveGeosets(st.instanceId, geosets);

    // --- Helmet model attachment ---
    // HEAD slot is index 0 in the 19-element equipment array.
    // Helmet M2s are race/gender-specific (e.g. Helm_Plate_B_01_HuM.m2 for Human Male).
    if (displayInfoIds[0] != 0) {
        // Detach any previously attached helmet before attaching a new one
        charRenderer->detachWeapon(st.instanceId, 0);
        charRenderer->detachWeapon(st.instanceId, 11);

        int32_t helmIdx = displayInfoDbc->findRecordById(displayInfoIds[0]);
        if (helmIdx >= 0) {
            const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
            std::string helmModelName = displayInfoDbc->getString(static_cast<uint32_t>(helmIdx), leftModelField);
            if (!helmModelName.empty()) {
                // Strip .mdx/.m2 extension
                size_t dotPos = helmModelName.rfind('.');
                if (dotPos != std::string::npos) helmModelName = helmModelName.substr(0, dotPos);

                // Race/gender suffix for helmet variants
                static const std::unordered_map<uint8_t, std::string> racePrefix = {
                    {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                    {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
                };
                std::string genderSuffix = (st.genderId == 0) ? "M" : "F";
                std::string raceSuffix;
                auto itRace = racePrefix.find(st.raceId);
                if (itRace != racePrefix.end()) {
                    raceSuffix = "_" + itRace->second + genderSuffix;
                }

                // Try race/gender-specific variant first, then base name
                std::string helmPath;
                pipeline::M2Model helmModel;
                if (!raceSuffix.empty()) {
                    helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(helmPath, helmModel)) helmModel = {};
                }
                if (!helmModel.isValid()) {
                    helmPath = "Item\\ObjectComponents\\Head\\" + helmModelName + ".m2";
                    loadWeaponM2(helmPath, helmModel);
                }

                if (helmModel.isValid()) {
                    uint32_t helmModelId = nextWeaponModelId_++;
                    // Get texture from ItemDisplayInfo (LeftModelTexture)
                    const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                    std::string helmTexName = displayInfoDbc->getString(static_cast<uint32_t>(helmIdx), leftTexField);
                    std::string helmTexPath;
                    if (!helmTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Head\\" + helmTexName + raceSuffix + ".blp";
                            if (assetManager_->fileExists(suffixedTex)) helmTexPath = suffixedTex;
                        }
                        if (helmTexPath.empty()) {
                            helmTexPath = "Item\\ObjectComponents\\Head\\" + helmTexName + ".blp";
                        }
                    }
                    // Attachment point 0 (head bone), fallback to 11 (explicit head attachment)
                    bool attached = charRenderer->attachWeapon(st.instanceId, 0, helmModel, helmModelId, helmTexPath);
                    if (!attached) {
                        attached = charRenderer->attachWeapon(st.instanceId, 11, helmModel, helmModelId, helmTexPath);
                    }
                    if (attached) {
                        LOG_DEBUG("Attached player helmet: ", helmPath, " tex: ", helmTexPath);
                    }
                }
            }
        }
    } else {
        // No helmet equipped — detach any existing helmet model
        charRenderer->detachWeapon(st.instanceId, 0);
        charRenderer->detachWeapon(st.instanceId, 11);
    }

    // --- Shoulder model attachment ---
    // SHOULDERS slot is index 2 in the 19-element equipment array.
    // Shoulders have TWO M2 models (left + right) attached at points 5 and 6.
    // ItemDisplayInfo.dbc: LeftModel → left shoulder, RightModel → right shoulder.
    if (displayInfoIds[2] != 0) {
        // Detach any previously attached shoulder models
        charRenderer->detachWeapon(st.instanceId, 5);
        charRenderer->detachWeapon(st.instanceId, 6);

        int32_t shoulderIdx = displayInfoDbc->findRecordById(displayInfoIds[2]);
        if (shoulderIdx >= 0) {
            const uint32_t leftModelField = idiL ? (*idiL)["LeftModel"] : 1u;
            const uint32_t rightModelField = idiL ? (*idiL)["RightModel"] : 2u;
            const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
            const uint32_t rightTexField = idiL ? (*idiL)["RightModelTexture"] : 4u;

            // Race/gender suffix for shoulder variants (same as helmets)
            static const std::unordered_map<uint8_t, std::string> shoulderRacePrefix = {
                {1, "Hu"}, {2, "Or"}, {3, "Dw"}, {4, "Ni"}, {5, "Sc"},
                {6, "Ta"}, {7, "Gn"}, {8, "Tr"}, {10, "Be"}, {11, "Dr"}
            };
            std::string genderSuffix = (st.genderId == 0) ? "M" : "F";
            std::string raceSuffix;
            auto itRace = shoulderRacePrefix.find(st.raceId);
            if (itRace != shoulderRacePrefix.end()) {
                raceSuffix = "_" + itRace->second + genderSuffix;
            }

            // Attach left shoulder (attachment point 5) using LeftModel
            std::string leftModelName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), leftModelField);
            if (!leftModelName.empty()) {
                size_t dotPos = leftModelName.rfind('.');
                if (dotPos != std::string::npos) leftModelName = leftModelName.substr(0, dotPos);

                std::string leftPath;
                pipeline::M2Model leftModel;
                if (!raceSuffix.empty()) {
                    leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(leftPath, leftModel)) leftModel = {};
                }
                if (!leftModel.isValid()) {
                    leftPath = "Item\\ObjectComponents\\Shoulder\\" + leftModelName + ".m2";
                    loadWeaponM2(leftPath, leftModel);
                }

                if (leftModel.isValid()) {
                    uint32_t leftModelId = nextWeaponModelId_++;
                    std::string leftTexName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), leftTexField);
                    std::string leftTexPath;
                    if (!leftTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + raceSuffix + ".blp";
                            if (assetManager_->fileExists(suffixedTex)) leftTexPath = suffixedTex;
                        }
                        if (leftTexPath.empty()) {
                            leftTexPath = "Item\\ObjectComponents\\Shoulder\\" + leftTexName + ".blp";
                        }
                    }
                    bool attached = charRenderer->attachWeapon(st.instanceId, 5, leftModel, leftModelId, leftTexPath);
                    if (attached) {
                        LOG_DEBUG("Attached left shoulder: ", leftPath, " tex: ", leftTexPath);
                    }
                }
            }

            // Attach right shoulder (attachment point 6) using RightModel
            std::string rightModelName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), rightModelField);
            if (!rightModelName.empty()) {
                size_t dotPos = rightModelName.rfind('.');
                if (dotPos != std::string::npos) rightModelName = rightModelName.substr(0, dotPos);

                std::string rightPath;
                pipeline::M2Model rightModel;
                if (!raceSuffix.empty()) {
                    rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + raceSuffix + ".m2";
                    if (!loadWeaponM2(rightPath, rightModel)) rightModel = {};
                }
                if (!rightModel.isValid()) {
                    rightPath = "Item\\ObjectComponents\\Shoulder\\" + rightModelName + ".m2";
                    loadWeaponM2(rightPath, rightModel);
                }

                if (rightModel.isValid()) {
                    uint32_t rightModelId = nextWeaponModelId_++;
                    std::string rightTexName = displayInfoDbc->getString(static_cast<uint32_t>(shoulderIdx), rightTexField);
                    std::string rightTexPath;
                    if (!rightTexName.empty()) {
                        if (!raceSuffix.empty()) {
                            std::string suffixedTex = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + raceSuffix + ".blp";
                            if (assetManager_->fileExists(suffixedTex)) rightTexPath = suffixedTex;
                        }
                        if (rightTexPath.empty()) {
                            rightTexPath = "Item\\ObjectComponents\\Shoulder\\" + rightTexName + ".blp";
                        }
                    }
                    bool attached = charRenderer->attachWeapon(st.instanceId, 6, rightModel, rightModelId, rightTexPath);
                    if (attached) {
                        LOG_DEBUG("Attached right shoulder: ", rightPath, " tex: ", rightTexPath);
                    }
                }
            }
        }
    } else {
        // No shoulders equipped — detach any existing shoulder models
        charRenderer->detachWeapon(st.instanceId, 5);
        charRenderer->detachWeapon(st.instanceId, 6);
    }

    // --- Cape texture (group 15 / texture type 2) ---
    // The geoset above enables the cape mesh, but without a texture it renders blank.
    if (hasInvType({16})) {
        // Back/cloak is WoW equipment slot 14 (BACK) in the 19-element array.
        uint32_t capeDid = displayInfoIds[14];
        if (capeDid != 0) {
            int32_t capeRecIdx = displayInfoDbc->findRecordById(capeDid);
            if (capeRecIdx >= 0) {
                const uint32_t leftTexField = idiL ? (*idiL)["LeftModelTexture"] : 3u;
                std::string capeName = displayInfoDbc->getString(
                    static_cast<uint32_t>(capeRecIdx), leftTexField);

                if (!capeName.empty()) {
                    std::replace(capeName.begin(), capeName.end(), '/', '\\');

                    auto hasBlpExt = [](const std::string& p) {
                        if (p.size() < 4) return false;
                        std::string ext = p.substr(p.size() - 4);
                        std::transform(ext.begin(), ext.end(), ext.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return ext == ".blp";
                    };

                    const bool hasDir = (capeName.find('\\') != std::string::npos);
                    const bool hasExt = hasBlpExt(capeName);

                    std::vector<std::string> capeCandidates;
                    auto addCapeCandidate = [&](const std::string& p) {
                        if (p.empty()) return;
                        if (std::find(capeCandidates.begin(), capeCandidates.end(), p) == capeCandidates.end()) {
                            capeCandidates.push_back(p);
                        }
                    };

                    if (hasDir) {
                        addCapeCandidate(capeName);
                        if (!hasExt) addCapeCandidate(capeName + ".blp");
                    } else {
                        std::string baseObj = "Item\\ObjectComponents\\Cape\\" + capeName;
                        std::string baseTex = "Item\\TextureComponents\\Cape\\" + capeName;
                        addCapeCandidate(baseObj);
                        addCapeCandidate(baseTex);
                        if (!hasExt) {
                            addCapeCandidate(baseObj + ".blp");
                            addCapeCandidate(baseTex + ".blp");
                        }
                        addCapeCandidate(baseObj + (st.genderId == 1 ? "_F.blp" : "_M.blp"));
                        addCapeCandidate(baseObj + "_U.blp");
                        addCapeCandidate(baseTex + (st.genderId == 1 ? "_F.blp" : "_M.blp"));
                        addCapeCandidate(baseTex + "_U.blp");
                    }

                    const rendering::VkTexture* whiteTex = charRenderer->loadTexture("");
                    rendering::VkTexture* capeTexture = nullptr;
                    for (const auto& candidate : capeCandidates) {
                        rendering::VkTexture* tex = charRenderer->loadTexture(candidate);
                        if (tex && tex != whiteTex) {
                            capeTexture = tex;
                            break;
                        }
                    }

                    if (capeTexture) {
                        charRenderer->setGroupTextureOverride(st.instanceId, 15, capeTexture);
                        if (const auto* md = charRenderer->getModelData(st.modelId)) {
                            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                                if (md->textures[ti].type == 2) {
                                    charRenderer->setTextureSlotOverride(
                                        st.instanceId, static_cast<uint16_t>(ti), capeTexture);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Textures (skin atlas compositing) ---
    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture",
        "ArmLowerTexture",
        "HandTexture",
        "TorsoUpperTexture",
        "TorsoLowerTexture",
        "LegUpperTexture",
        "LegLowerTexture",
        "FootTexture",
    };

    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);

    std::vector<std::pair<int, std::string>> regionLayers;
    const bool isFemale = (st.genderId == 1);

    for (int s = 0; s < 19; s++) {
        uint32_t did = displayInfoIds[s];
        if (did == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(did);
        if (recIdx < 0) continue;

        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;

            std::string base = "Item\\TextureComponents\\" + std::string(componentDirs[region]) + "\\" + texName;
            std::string genderPath = base + (isFemale ? "_F.blp" : "_M.blp");
            std::string unisexPath = base + "_U.blp";
            std::string fullPath;
            if (assetManager_->fileExists(genderPath)) fullPath = genderPath;
            else if (assetManager_->fileExists(unisexPath)) fullPath = unisexPath;
            else fullPath = base + ".blp";

            regionLayers.emplace_back(region, fullPath);
        }
    }

    const auto slotsIt = playerTextureSlotsByModelId_.find(st.modelId);
    if (slotsIt == playerTextureSlotsByModelId_.end()) return;
    const PlayerTextureSlots& slots = slotsIt->second;
    if (slots.skin < 0) return;

    rendering::VkTexture* newTex = charRenderer->compositeWithRegions(st.bodySkinPath, st.underwearPaths, regionLayers);
    if (newTex) {
        charRenderer->setTextureSlotOverride(st.instanceId, static_cast<uint16_t>(slots.skin), newTex);
    }

    // --- Weapon model attachment ---
    // Slot indices in the 19-element EquipSlot array:
    //   15 = MAIN_HAND → attachment 1 (right hand)
    //   16 = OFF_HAND  → attachment 2 (left hand)
    struct OnlineWeaponSlot {
        int slotIndex;
        uint32_t attachmentId;
    };
    static constexpr OnlineWeaponSlot weaponSlots[] = {
        { 15, 1 },  // MAIN_HAND → right hand
        { 16, 2 },  // OFF_HAND  → left hand
    };

    const uint32_t modelFieldL = idiL ? (*idiL)["LeftModel"] : 1u;
    const uint32_t modelFieldR = idiL ? (*idiL)["RightModel"] : 2u;
    const uint32_t texFieldL   = idiL ? (*idiL)["LeftModelTexture"] : 3u;
    const uint32_t texFieldR   = idiL ? (*idiL)["RightModelTexture"] : 4u;

    for (const auto& ws : weaponSlots) {
        uint32_t weapDisplayId = displayInfoIds[ws.slotIndex];
        if (weapDisplayId == 0) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        int32_t recIdx = displayInfoDbc->findRecordById(weapDisplayId);
        if (recIdx < 0) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        // Prefer LeftModel (full weapon), fall back to RightModel (hilt variants)
        std::string modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), modelFieldL);
        std::string textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texFieldL);
        if (modelName.empty()) {
            modelName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), modelFieldR);
            textureName = displayInfoDbc->getString(static_cast<uint32_t>(recIdx), texFieldR);
        }
        if (modelName.empty()) {
            charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
            continue;
        }

        // Convert .mdx → .m2
        std::string modelFile = modelName;
        {
            size_t dotPos = modelFile.rfind('.');
            if (dotPos != std::string::npos) modelFile = modelFile.substr(0, dotPos);
            modelFile += ".m2";
        }

        // Try Weapon directory first, then Shield
        std::string m2Path = "Item\\ObjectComponents\\Weapon\\" + modelFile;
        pipeline::M2Model weaponModel;
        if (!loadWeaponM2(m2Path, weaponModel)) {
            m2Path = "Item\\ObjectComponents\\Shield\\" + modelFile;
            if (!loadWeaponM2(m2Path, weaponModel)) {
                charRenderer->detachWeapon(st.instanceId, ws.attachmentId);
                continue;
            }
        }

        // Build texture path
        std::string texturePath;
        if (!textureName.empty()) {
            texturePath = "Item\\ObjectComponents\\Weapon\\" + textureName + ".blp";
            if (!assetManager_->fileExists(texturePath)) {
                texturePath = "Item\\ObjectComponents\\Shield\\" + textureName + ".blp";
                if (!assetManager_->fileExists(texturePath)) texturePath.clear();
            }
        }

        uint32_t weaponModelId = nextWeaponModelId_++;
        charRenderer->attachWeapon(st.instanceId, ws.attachmentId,
                                   weaponModel, weaponModelId, texturePath);
    }
}

void EntitySpawner::despawnPlayer(uint64_t guid) {
    if (!renderer_ || !renderer_->getCharacterRenderer()) return;
    auto it = playerInstances_.find(guid);
    if (it == playerInstances_.end()) return;
    renderer_->getCharacterRenderer()->removeInstance(it->second);
    playerInstances_.erase(it);
    onlinePlayerAppearance_.erase(guid);
    pendingOnlinePlayerEquipment_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
}

void EntitySpawner::spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale) {
    if (!renderer_ || !assetManager_) return;

    if (!gameObjectLookupsBuilt_) {
        buildGameObjectDisplayLookups();
    }
    if (!gameObjectLookupsBuilt_) return;

    auto goIt = gameObjectInstances_.find(guid);
    if (goIt != gameObjectInstances_.end()) {
        // Already have a render instance — update its position (e.g. transport re-creation)
        auto& info = goIt->second;
        glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
        LOG_DEBUG("GameObject position update: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
                 " pos=(", x, ", ", y, ", ", z, ")");
        if (renderer_) {
            if (info.isWmo) {
                if (auto* wr = renderer_->getWMORenderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    transform = glm::rotate(transform, orientation, glm::vec3(0, 0, 1));
                    wr->setInstanceTransform(info.instanceId, transform);
                }
            } else {
                if (auto* mr = renderer_->getM2Renderer()) {
                    glm::mat4 transform(1.0f);
                    transform = glm::translate(transform, renderPos);
                    mr->setInstanceTransform(info.instanceId, transform);
                }
            }
        }
        return;
    }

    std::string modelPath;

        // Override model path for transports with wrong displayIds (preloaded transports)
        // Check if this GUID is a known transport
        bool isTransport = gameHandler_ && gameHandler_->isTransportGuid(guid);
        if (isTransport) {
            // Map common transport displayIds to correct WMO paths
            // NOTE: displayIds 455/462 are elevators in Thunder Bluff and should NOT be forced to ships.
            // Keep ship/zeppelin overrides entry-driven where possible.
            // DisplayIds 807, 808 = Zeppelins
            // DisplayIds 2454, 1587 = Special ships/icebreakers
            if (entry == 20808 || entry == 176231 || entry == 176310) {
                modelPath = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
                LOG_INFO("Overriding transport entry/display ", entry, "/", displayId, " → transportship.wmo");
            } else if (displayId == 807 || displayId == 808 || displayId == 175080 || displayId == 176495 || displayId == 164871) {
                modelPath = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → transport_zeppelin.wmo");
            } else if (displayId == 1587) {
                modelPath = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → Transport_Horde_Zeppelin.wmo");
            } else if (displayId == 2454 || displayId == 181688 || displayId == 190536) {
                modelPath = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
                LOG_INFO("Overriding transport displayId ", displayId, " → Transport_Icebreaker_ship.wmo");
            } else if (displayId == 3831) {
                // Deeprun Tram car
                modelPath = "World\\Generic\\Gnome\\Passive Doodads\\Subway\\SubwayCar.m2";
                LOG_WARNING("Overriding transport displayId ", displayId, " → SubwayCar.m2");
            }
        }

    // Fallback to normal displayId lookup if not a transport or no override matched
    if (modelPath.empty()) {
        modelPath = getGameObjectModelPathForDisplayId(displayId);
    }

    if (modelPath.empty()) {
        LOG_WARNING("No model path for gameobject displayId ", displayId, " (guid 0x", std::hex, guid, std::dec, ")");
        return;
    }

    // Log spawns to help debug duplicate objects (e.g., cathedral issue)
    LOG_DEBUG("GameObject spawn: displayId=", displayId, " guid=0x", std::hex, guid, std::dec,
             " model=", modelPath, " pos=(", x, ", ", y, ", ", z, ")");

    std::string lowerPath = modelPath;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";

    glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(x, y, z));
    const float renderYawWmo = orientation;
    // M2 game objects: model default faces +renderX. renderYaw = canonical + 90° = server_yaw
    // (same offset as creature/character renderer_ so all M2 models face consistently)
    const float renderYawM2go = orientation + glm::radians(90.0f);

    bool loadedAsWmo = false;
    if (isWmo) {
        auto* wmoRenderer = renderer_->getWMORenderer();
        if (!wmoRenderer) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
            // Only use cached entry if the model is still resident in the renderer_
            if (wmoRenderer->isModelLoaded(modelId)) {
                loadedAsWmo = true;
            } else {
                gameObjectDisplayIdWmoCache_.erase(itCache);
                modelId = 0;
            }
        }
        if (!loadedAsWmo && modelId == 0) {
            auto wmoData = assetManager_->readFile(modelPath);
            if (!wmoData.empty()) {
                pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
                LOG_DEBUG("Gameobject WMO root loaded: ", modelPath, " nGroups=", wmoModel.nGroups);
                int loadedGroups = 0;
                if (wmoModel.nGroups > 0) {
                    std::string basePath = modelPath;
                    std::string extension;
                    if (basePath.size() > 4) {
                        extension = basePath.substr(basePath.size() - 4);
                        std::string extLower = extension;
                        for (char& c : extLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (extLower == ".wmo") {
                            basePath = basePath.substr(0, basePath.size() - 4);
                        }
                    }

                    for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                        char groupSuffix[16];
                        snprintf(groupSuffix, sizeof(groupSuffix), "_%03u%s", gi, extension.c_str());
                        std::string groupPath = basePath + groupSuffix;
                        std::vector<uint8_t> groupData = assetManager_->readFile(groupPath);
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
                            groupData = assetManager_->readFile(basePath + groupSuffix);
                        }
                        if (groupData.empty()) {
                            snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.WMO", gi);
                            groupData = assetManager_->readFile(basePath + groupSuffix);
                        }
                        if (!groupData.empty()) {
                            pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                            loadedGroups++;
                        } else {
                            LOG_WARNING("  Failed to load WMO group ", gi, " for: ", basePath);
                        }
                    }
                }

                if (loadedGroups > 0 || wmoModel.nGroups == 0) {
                    modelId = nextGameObjectWmoModelId_++;
                    if (wmoRenderer->loadModel(wmoModel, modelId)) {
                        gameObjectDisplayIdWmoCache_[displayId] = modelId;
                        loadedAsWmo = true;
                    } else {
                        LOG_WARNING("Failed to load gameobject WMO model: ", modelPath);
                    }
                } else {
                    LOG_WARNING("No WMO groups loaded for gameobject: ", modelPath,
                                " — falling back to M2");
                }
            } else {
                LOG_WARNING("Failed to read gameobject WMO: ", modelPath, " — falling back to M2");
            }
        }

        if (loadedAsWmo) {
            uint32_t instanceId = wmoRenderer->createInstance(modelId, renderPos,
                glm::vec3(0.0f, 0.0f, renderYawWmo), scale);
            if (instanceId == 0) {
                LOG_WARNING("Failed to create gameobject WMO instance for guid 0x", std::hex, guid, std::dec);
                return;
            }

            gameObjectInstances_[guid] = {modelId, instanceId, true};
            LOG_DEBUG("Spawned gameobject WMO: guid=0x", std::hex, guid, std::dec,
                     " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");

            // Spawn transport WMO doodads (chairs, furniture, etc.) as child M2 instances
            bool isTransport = false;
            if (gameHandler_) {
                std::string lowerModelPath = modelPath;
                std::transform(lowerModelPath.begin(), lowerModelPath.end(), lowerModelPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isTransport = (lowerModelPath.find("transport") != std::string::npos);
            }

            auto* m2Renderer = renderer_->getM2Renderer();
            if (m2Renderer && isTransport) {
                const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
                if (doodadTemplates && !doodadTemplates->empty()) {
                    constexpr size_t kMaxTransportDoodads = 192;
                    const size_t doodadBudget = std::min(doodadTemplates->size(), kMaxTransportDoodads);
                    LOG_DEBUG("Queueing ", doodadBudget, "/", doodadTemplates->size(),
                             " transport doodads for WMO instance ", instanceId);
                    pendingTransportDoodadBatches_.push_back(PendingTransportDoodadBatch{
                        guid,
                        modelId,
                        instanceId,
                        0,
                        doodadBudget,
                        0,
                        x, y, z,
                        orientation
                    });
                } else {
                LOG_DEBUG("Transport WMO has no doodads or templates not available");
            }
            }

            // Transport GameObjects are not always named "transport" in their WMO path
            // (e.g. elevators/lifts). If the server marks it as a transport, always
            // notify so TransportManager can animate/carry passengers.
            bool isTG = gameHandler_ && gameHandler_->isTransportGuid(guid);
            LOG_WARNING("WMO GO spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " isTransport=", isTG,
                       " pos=(", x, ", ", y, ", ", z, ")");
            if (isTG) {
                gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
            }

            return;
        }

        // WMO failed — fall through to try as M2
        // Convert .wmo path to .m2 for fallback
        modelPath = modelPath.substr(0, modelPath.size() - 4) + ".m2";
    }

    {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (!m2Renderer) return;

        // Skip displayIds that permanently failed to load (e.g. empty/unsupported M2s).
        // Without this guard the same empty model is re-parsed every frame, causing
        // sustained log spam and wasted CPU.
        if (gameObjectDisplayIdFailedCache_.count(displayId)) return;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdModelCache_.find(displayId);
        if (itCache != gameObjectDisplayIdModelCache_.end()) {
            modelId = itCache->second;
            if (!m2Renderer->hasModel(modelId)) {
                LOG_WARNING("GO M2 cache hit but model gone: displayId=", displayId,
                            " modelId=", modelId, " path=", modelPath,
                            " — reloading");
                gameObjectDisplayIdModelCache_.erase(itCache);
                itCache = gameObjectDisplayIdModelCache_.end();
            }
        }
        if (itCache == gameObjectDisplayIdModelCache_.end()) {
            modelId = nextGameObjectModelId_++;

            auto m2Data = assetManager_->readFile(modelPath);
            if (m2Data.empty()) {
                LOG_WARNING("Failed to read gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
            if (model.vertices.empty()) {
                LOG_WARNING("Failed to parse gameobject M2: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            std::string skinPath = modelPath.substr(0, modelPath.size() - 3) + "00.skin";
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty() && model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, model);
            }

            if (!m2Renderer->loadModel(model, modelId)) {
                LOG_WARNING("Failed to load gameobject model: ", modelPath);
                gameObjectDisplayIdFailedCache_.insert(displayId);
                return;
            }

            gameObjectDisplayIdModelCache_[displayId] = modelId;
        }

        uint32_t instanceId = m2Renderer->createInstance(modelId, renderPos,
            glm::vec3(0.0f, 0.0f, renderYawM2go), scale);
        if (instanceId == 0) {
            LOG_WARNING("Failed to create gameobject instance for guid 0x", std::hex, guid, std::dec);
            return;
        }

        // Freeze animation for static gameobjects, but let portals/effects/transports animate
        bool isTransportGO = gameHandler_ && gameHandler_->isTransportGuid(guid);
        std::string lowerPath = modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool isAnimatedEffect = (lowerPath.find("instanceportal") != std::string::npos ||
                                  lowerPath.find("instancenewportal") != std::string::npos ||
                                  lowerPath.find("portalfx") != std::string::npos ||
                                  lowerPath.find("spellportal") != std::string::npos);
        if (!isAnimatedEffect && !isTransportGO) {
            // Check for totem idle animations — totems should animate, not freeze
            bool isTotem = false;
            if (m2Renderer->hasAnimation(instanceId, 245)) {         // TOTEM_SMALL
                m2Renderer->setInstanceAnimation(instanceId, 245, true);
                isTotem = true;
            } else if (m2Renderer->hasAnimation(instanceId, 246)) {  // TOTEM_MEDIUM
                m2Renderer->setInstanceAnimation(instanceId, 246, true);
                isTotem = true;
            } else if (m2Renderer->hasAnimation(instanceId, 247)) {  // TOTEM_LARGE
                m2Renderer->setInstanceAnimation(instanceId, 247, true);
                isTotem = true;
            }
            if (!isTotem) {
                m2Renderer->setInstanceAnimationFrozen(instanceId, true);
            }
        }

        gameObjectInstances_[guid] = {modelId, instanceId, false};

        // Notify transport system for M2 transports (e.g. Deeprun Tram cars)
        if (gameHandler_ && gameHandler_->isTransportGuid(guid)) {
            LOG_WARNING("M2 transport spawned: guid=0x", std::hex, guid, std::dec,
                       " entry=", entry, " displayId=", displayId,
                       " instanceId=", instanceId);
            gameHandler_->notifyTransportSpawned(guid, entry, displayId, x, y, z, orientation);
        }
    }

    LOG_DEBUG("Spawned gameobject: guid=0x", std::hex, guid, std::dec,
             " displayId=", displayId, " at (", x, ", ", y, ", ", z, ")");
}

void EntitySpawner::processAsyncCreatureResults(bool unlimited) {
    // Check completed async model loads and finalize on main thread (GPU upload + instance creation).
    // Limit GPU model uploads per tick to avoid long main-thread stalls that can starve socket updates.
    // Even in unlimited mode (load screen), keep a small cap and budget to prevent multi-second stalls.
    static constexpr int kMaxModelUploadsPerTick = 1;
    static constexpr int kMaxModelUploadsPerTickWarmup = 1;
    static constexpr float kFinalizeBudgetMs = 2.0f;
    static constexpr float kFinalizeBudgetWarmupMs = 2.0f;
    const int maxUploadsThisTick = unlimited ? kMaxModelUploadsPerTickWarmup : kMaxModelUploadsPerTick;
    const float budgetMs = unlimited ? kFinalizeBudgetWarmupMs : kFinalizeBudgetMs;
    const auto tickStart = std::chrono::steady_clock::now();
    int modelUploads = 0;

    for (auto it = asyncCreatureLoads_.begin(); it != asyncCreatureLoads_.end(); ) {
        if (std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - tickStart).count() >= budgetMs) {
            break;
        }

        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        auto result = it->future.get();
        it = asyncCreatureLoads_.erase(it);
        asyncCreatureDisplayLoads_.erase(result.displayId);

        // Failures and cache hits need no GPU work — process them even when the
        // upload budget is exhausted. Previously the budget check was above this
        // point, blocking ALL ready futures (including zero-cost ones) after a
        // single upload, which throttled creature spawn throughput during world load.
        if (result.permanent_failure) {
            nonRenderableCreatureDisplayIds_.insert(result.displayId);
            creaturePermanentFailureGuids_.insert(result.guid);
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }
        if (!result.valid || !result.model) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }

        // Another async result may have already uploaded this displayId while this
        // task was still running; in that case, skip duplicate GPU upload.
        if (displayIdModelCache_.find(result.displayId) != displayIdModelCache_.end()) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            if (!creatureInstances_.count(result.guid) &&
                !creaturePermanentFailureGuids_.count(result.guid)) {
                PendingCreatureSpawn s{};
                s.guid = result.guid;
                s.displayId = result.displayId;
                s.x = result.x;
                s.y = result.y;
                s.z = result.z;
                s.orientation = result.orientation;
                s.scale = result.scale;
                pendingCreatureSpawns_.push_back(s);
                pendingCreatureSpawnGuids_.insert(result.guid);
            }
            continue;
        }

        // Only actual GPU uploads count toward the per-tick budget.
        if (modelUploads >= maxUploadsThisTick) {
            // Re-queue this result — it needs a GPU upload but we're at budget.
            // Push a new pending spawn so it's retried next frame.
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            PendingCreatureSpawn s{};
            s.guid = result.guid;
            s.displayId = result.displayId;
            s.x = result.x; s.y = result.y; s.z = result.z;
            s.orientation = result.orientation;
            s.scale = result.scale;
            pendingCreatureSpawns_.push_back(s);
            pendingCreatureSpawnGuids_.insert(result.guid);
            continue;
        }

        // Model parsed on background thread — upload to GPU on main thread.
        auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
        if (!charRenderer) {
            pendingCreatureSpawnGuids_.erase(result.guid);
            continue;
        }

        // Count upload attempts toward the frame budget even if upload fails.
        // Otherwise repeated failures can consume an unbounded amount of frame time.
        modelUploads++;

        // Upload model to GPU (must happen on main thread)
        // Use pre-decoded BLP cache to skip main-thread texture decode
        auto uploadStart = std::chrono::steady_clock::now();
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);
        if (!charRenderer->loadModel(*result.model, result.modelId)) {
            charRenderer->setPredecodedBLPCache(nullptr);
            nonRenderableCreatureDisplayIds_.insert(result.displayId);
            creaturePermanentFailureGuids_.insert(result.guid);
            pendingCreatureSpawnGuids_.erase(result.guid);
            creatureSpawnRetryCounts_.erase(result.guid);
            continue;
        }
        charRenderer->setPredecodedBLPCache(nullptr);
        {
            auto uploadEnd = std::chrono::steady_clock::now();
            float uploadMs = std::chrono::duration<float, std::milli>(uploadEnd - uploadStart).count();
            if (uploadMs > 100.0f) {
                LOG_WARNING("charRenderer->loadModel took ", uploadMs, "ms displayId=", result.displayId,
                            " preDecoded=", result.predecodedTextures.size());
            }
        }
        // Save remaining pre-decoded textures (display skins) for spawnOnlineCreature
        if (!result.predecodedTextures.empty()) {
            displayIdPredecodedTextures_[result.displayId] = std::move(result.predecodedTextures);
        }
        displayIdModelCache_[result.displayId] = result.modelId;
        pendingCreatureSpawnGuids_.erase(result.guid);
        creatureSpawnRetryCounts_.erase(result.guid);

        // Re-queue as a normal pending spawn — model is now cached, so sync spawn is fast
        // (only creates instance + applies textures, no file I/O).
        if (!creatureInstances_.count(result.guid) &&
            !creaturePermanentFailureGuids_.count(result.guid)) {
            PendingCreatureSpawn s{};
            s.guid = result.guid;
            s.displayId = result.displayId;
            s.x = result.x;
            s.y = result.y;
            s.z = result.z;
            s.orientation = result.orientation;
            s.scale = result.scale;
            pendingCreatureSpawns_.push_back(s);
            pendingCreatureSpawnGuids_.insert(result.guid);
        }
    }
}

void EntitySpawner::processAsyncNpcCompositeResults(bool unlimited) {
    auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!charRenderer) return;

    // Budget: 2ms per frame to avoid stalling when many NPCs complete skin compositing
    // simultaneously. In unlimited mode (load screen), process everything without cap.
    static constexpr float kCompositeBudgetMs = 2.0f;
    auto startTime = std::chrono::steady_clock::now();

    for (auto it = asyncNpcCompositeLoads_.begin(); it != asyncNpcCompositeLoads_.end(); ) {
        if (!unlimited) {
            float elapsed = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - startTime).count();
            if (elapsed >= kCompositeBudgetMs) break;
        }
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto result = it->future.get();
        it = asyncNpcCompositeLoads_.erase(it);

        const auto& info = result.info;

        // Set pre-decoded cache so texture loads skip synchronous BLP decode
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);

        // --- Apply skin to type-1 slots ---
        rendering::VkTexture* skinTex = nullptr;

        if (info.hasBakedSkin) {
            // Baked skin: load from pre-decoded cache
            skinTex = charRenderer->loadTexture(info.bakedSkinPath);
        }

        if (info.hasComposite) {
            // Composite with face/underwear/equipment regions on top of base skin
            rendering::VkTexture* compositeTex = nullptr;
            if (!info.regionLayers.empty()) {
                compositeTex = charRenderer->compositeWithRegions(info.basePath,
                    info.overlayPaths, info.regionLayers);
            } else if (!info.overlayPaths.empty()) {
                std::vector<std::string> skinLayers;
                skinLayers.push_back(info.basePath);
                for (const auto& op : info.overlayPaths) skinLayers.push_back(op);
                compositeTex = charRenderer->compositeTextures(skinLayers);
            }
            if (compositeTex) skinTex = compositeTex;
        } else if (info.hasSimpleSkin) {
            // Simple skin: just base texture, no compositing
            auto* baseTex = charRenderer->loadTexture(info.basePath);
            if (baseTex) skinTex = baseTex;
        }

        if (skinTex) {
            for (uint32_t slot : info.skinTextureSlots) {
                charRenderer->setModelTexture(info.modelId, slot, skinTex);
            }
        }

        // --- Apply hair texture to type-6 slots ---
        if (!info.hairTexturePath.empty()) {
            rendering::VkTexture* hairTex = charRenderer->loadTexture(info.hairTexturePath);
            rendering::VkTexture* whTex = charRenderer->loadTexture("");
            if (hairTex && hairTex != whTex) {
                for (uint32_t slot : info.hairTextureSlots) {
                    charRenderer->setModelTexture(info.modelId, slot, hairTex);
                }
            }
        } else if (info.useBakedForHair && skinTex) {
            // Bald NPC: use skin/baked texture for scalp cap
            for (uint32_t slot : info.hairTextureSlots) {
                charRenderer->setModelTexture(info.modelId, slot, skinTex);
            }
        }

        charRenderer->setPredecodedBLPCache(nullptr);
    }
}

void EntitySpawner::processCreatureSpawnQueue(bool unlimited) {
    auto startTime = std::chrono::steady_clock::now();
    // Budget: max 2ms per frame for creature spawning to prevent stutter.
    // In unlimited mode (load screen), process everything without budget cap.
    static constexpr float kSpawnBudgetMs = 2.0f;

    // First, finalize any async model loads that completed on background threads.
    processAsyncCreatureResults(unlimited);
    {
        auto now = std::chrono::steady_clock::now();
        float asyncMs = std::chrono::duration<float, std::milli>(now - startTime).count();
        if (asyncMs > 100.0f) {
            LOG_WARNING("processAsyncCreatureResults took ", asyncMs, "ms");
        }
    }

    if (pendingCreatureSpawns_.empty()) return;
    if (!creatureLookupsBuilt_) {
        buildCreatureDisplayLookups();
        if (!creatureLookupsBuilt_) return;
    }

    int processed = 0;
    int asyncLaunched = 0;
    size_t rotationsLeft = pendingCreatureSpawns_.size();
    while (!pendingCreatureSpawns_.empty() &&
           (unlimited || processed < MAX_SPAWNS_PER_FRAME) &&
           rotationsLeft > 0) {
        // Check time budget every iteration (including first — async results may
        // have already consumed the budget via GPU model uploads).
        if (!unlimited) {
            auto now = std::chrono::steady_clock::now();
            float elapsedMs = std::chrono::duration<float, std::milli>(now - startTime).count();
            if (elapsedMs >= kSpawnBudgetMs) break;
        }

        PendingCreatureSpawn s = pendingCreatureSpawns_.front();
        pendingCreatureSpawns_.pop_front();

        if (nonRenderableCreatureDisplayIds_.count(s.displayId)) {
            pendingCreatureSpawnGuids_.erase(s.guid);
            creatureSpawnRetryCounts_.erase(s.guid);
            processed++;
            rotationsLeft = pendingCreatureSpawns_.size();
            continue;
        }

        const bool needsNewModel = (displayIdModelCache_.find(s.displayId) == displayIdModelCache_.end());

        // For new models: launch async load on background thread instead of blocking.
        if (needsNewModel) {
            // Keep exactly one background load per displayId. Additional spawns for
            // the same displayId stay queued and will spawn once cache is populated.
            if (asyncCreatureDisplayLoads_.count(s.displayId)) {
                pendingCreatureSpawns_.push_back(s);
                rotationsLeft--;
                continue;
            }

            const int maxAsync = unlimited ? (MAX_ASYNC_CREATURE_LOADS * 4) : MAX_ASYNC_CREATURE_LOADS;
            if (static_cast<int>(asyncCreatureLoads_.size()) + asyncLaunched >= maxAsync) {
                // Too many in-flight — defer to next frame
                pendingCreatureSpawns_.push_back(s);
                rotationsLeft--;
                continue;
            }

            std::string m2Path = getModelPathForDisplayId(s.displayId);
            if (m2Path.empty()) {
                nonRenderableCreatureDisplayIds_.insert(s.displayId);
                creaturePermanentFailureGuids_.insert(s.guid);
                pendingCreatureSpawnGuids_.erase(s.guid);
                creatureSpawnRetryCounts_.erase(s.guid);
                processed++;
                rotationsLeft = pendingCreatureSpawns_.size();
                continue;
            }

            // Check for invisible stalkers
            {
                std::string lowerPath = m2Path;
                std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerPath.find("invisiblestalker") != std::string::npos ||
                    lowerPath.find("invisible_stalker") != std::string::npos) {
                    nonRenderableCreatureDisplayIds_.insert(s.displayId);
                    creaturePermanentFailureGuids_.insert(s.guid);
                    pendingCreatureSpawnGuids_.erase(s.guid);
                    processed++;
                    rotationsLeft = pendingCreatureSpawns_.size();
                    continue;
                }
            }

            // Launch async M2 load — file I/O and parsing happen off the main thread.
            uint32_t modelId = nextCreatureModelId_++;
            auto* am = assetManager_;

            // Collect display skin texture paths for background pre-decode
            std::vector<std::string> displaySkinPaths;
            {
                auto itDD = displayDataMap_.find(s.displayId);
                if (itDD != displayDataMap_.end()) {
                    std::string modelDir;
                    size_t lastSlash = m2Path.find_last_of("\\/");
                    if (lastSlash != std::string::npos) modelDir = m2Path.substr(0, lastSlash + 1);

                    auto resolveForAsync = [&](const std::string& skinField) {
                        if (skinField.empty()) return;
                        std::string raw = skinField;
                        std::replace(raw.begin(), raw.end(), '/', '\\');
                        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.erase(raw.begin());
                        while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.pop_back();
                        if (raw.empty()) return;
                        bool hasExt = raw.size() >= 4 && raw.substr(raw.size()-4) == ".blp";
                        bool hasDir = raw.find('\\') != std::string::npos;
                        std::vector<std::string> candidates;
                        if (hasDir) {
                            candidates.push_back(raw);
                            if (!hasExt) candidates.push_back(raw + ".blp");
                        } else {
                            candidates.push_back(modelDir + raw);
                            if (!hasExt) candidates.push_back(modelDir + raw + ".blp");
                            candidates.push_back(raw);
                            if (!hasExt) candidates.push_back(raw + ".blp");
                        }
                        for (const auto& c : candidates) {
                            if (am->fileExists(c)) { displaySkinPaths.push_back(c); return; }
                        }
                    };
                    resolveForAsync(itDD->second.skin1);
                    resolveForAsync(itDD->second.skin2);
                    resolveForAsync(itDD->second.skin3);

                    // Pre-decode humanoid NPC textures (bake, skin, face, underwear, hair, equipment)
                    if (itDD->second.extraDisplayId != 0) {
                        auto itHE = humanoidExtraMap_.find(itDD->second.extraDisplayId);
                        if (itHE != humanoidExtraMap_.end()) {
                            const auto& he = itHE->second;
                            // Baked texture
                            if (!he.bakeName.empty()) {
                                displaySkinPaths.push_back("Textures\\BakedNpcTextures\\" + he.bakeName);
                            }
                            // CharSections: skin, face, underwear
                            auto csDbc = am->loadDBC("CharSections.dbc");
                            if (csDbc) {
                                const auto* csL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("CharSections") : nullptr;
                                auto csF = pipeline::detectCharSectionsFields(csDbc.get(), csL);
                                uint32_t nRace = static_cast<uint32_t>(he.raceId);
                                uint32_t nSex = static_cast<uint32_t>(he.sexId);
                                uint32_t nSkin = static_cast<uint32_t>(he.skinId);
                                uint32_t nFace = static_cast<uint32_t>(he.faceId);
                                for (uint32_t r = 0; r < csDbc->getRecordCount(); r++) {
                                    uint32_t rId = csDbc->getUInt32(r, csF.raceId);
                                    uint32_t sId = csDbc->getUInt32(r, csF.sexId);
                                    if (rId != nRace || sId != nSex) continue;
                                    uint32_t section = csDbc->getUInt32(r, csF.baseSection);
                                    uint32_t variation = csDbc->getUInt32(r, csF.variationIndex);
                                    uint32_t color = csDbc->getUInt32(r, csF.colorIndex);
                                    if (section == 0 && color == nSkin) {
                                        std::string t = csDbc->getString(r, csF.texture1);
                                        if (!t.empty()) displaySkinPaths.push_back(t);
                                    } else if (section == 1 && variation == nFace && color == nSkin) {
                                        std::string t1 = csDbc->getString(r, csF.texture1);
                                        std::string t2 = csDbc->getString(r, csF.texture2);
                                        if (!t1.empty()) displaySkinPaths.push_back(t1);
                                        if (!t2.empty()) displaySkinPaths.push_back(t2);
                                    } else if (section == 3 && variation == static_cast<uint32_t>(he.hairStyleId)
                                               && color == static_cast<uint32_t>(he.hairColorId)) {
                                        std::string t = csDbc->getString(r, csF.texture1);
                                        if (!t.empty()) displaySkinPaths.push_back(t);
                                    } else if (section == 4 && color == nSkin) {
                                        for (uint32_t f = csF.texture1; f <= csF.texture1 + 2; f++) {
                                            std::string t = csDbc->getString(r, f);
                                            if (!t.empty()) displaySkinPaths.push_back(t);
                                        }
                                    }
                                }
                            }
                            // Equipment region textures
                            auto idiDbc = am->loadDBC("ItemDisplayInfo.dbc");
                            if (idiDbc) {
                                static constexpr const char* compDirs[] = {
                                    "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
                                    "TorsoUpperTexture", "TorsoLowerTexture",
                                    "LegUpperTexture", "LegLowerTexture", "FootTexture",
                                };
                                const auto* idiL = pipeline::getActiveDBCLayout()
                                    ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;
                                const uint32_t trf[8] = {
                                    idiL ? (*idiL)["TextureArmUpper"]  : 14u,
                                    idiL ? (*idiL)["TextureArmLower"]  : 15u,
                                    idiL ? (*idiL)["TextureHand"]      : 16u,
                                    idiL ? (*idiL)["TextureTorsoUpper"]: 17u,
                                    idiL ? (*idiL)["TextureTorsoLower"]: 18u,
                                    idiL ? (*idiL)["TextureLegUpper"]  : 19u,
                                    idiL ? (*idiL)["TextureLegLower"]  : 20u,
                                    idiL ? (*idiL)["TextureFoot"]      : 21u,
                                };
                                const bool isFem = (he.sexId == 1);
                                for (int eq = 0; eq < 11; eq++) {
                                    uint32_t did = he.equipDisplayId[eq];
                                    if (did == 0) continue;
                                    int32_t recIdx = idiDbc->findRecordById(did);
                                    if (recIdx < 0) continue;
                                    for (int region = 0; region < 8; region++) {
                                        std::string texName = idiDbc->getString(static_cast<uint32_t>(recIdx), trf[region]);
                                        if (texName.empty()) continue;
                                        std::string base = "Item\\TextureComponents\\" +
                                            std::string(compDirs[region]) + "\\" + texName;
                                        std::string gp = base + (isFem ? "_F.blp" : "_M.blp");
                                        std::string up = base + "_U.blp";
                                        if (am->fileExists(gp)) displaySkinPaths.push_back(gp);
                                        else if (am->fileExists(up)) displaySkinPaths.push_back(up);
                                        else displaySkinPaths.push_back(base + ".blp");
                                    }
                                }
                            }
                        }
                    }
                }
            }

            AsyncCreatureLoad load;
            load.future = std::async(std::launch::async,
                [am, m2Path, modelId, s, skinPaths = std::move(displaySkinPaths)]() -> PreparedCreatureModel {
                    PreparedCreatureModel result;
                    result.guid = s.guid;
                    result.displayId = s.displayId;
                    result.modelId = modelId;
                    result.x = s.x;
                    result.y = s.y;
                    result.z = s.z;
                    result.orientation = s.orientation;
                    result.scale = s.scale;

                    auto m2Data = am->readFile(m2Path);
                    if (m2Data.empty()) {
                        result.permanent_failure = true;
                        return result;
                    }

                    auto model = std::make_shared<pipeline::M2Model>(pipeline::M2Loader::load(m2Data));
                    if (model->vertices.empty()) {
                        result.permanent_failure = true;
                        return result;
                    }

                    // Load skin file
                    if (model->version >= 264) {
                        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
                        auto skinData = am->readFile(skinPath);
                        if (!skinData.empty()) {
                            pipeline::M2Loader::loadSkin(skinData, *model);
                        }
                    }

                    // Load external .anim files
                    std::string basePath = m2Path.substr(0, m2Path.size() - 3);
                    for (uint32_t si = 0; si < model->sequences.size(); si++) {
                        if (!(model->sequences[si].flags & 0x20)) {
                            char animFileName[256];
                            snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                                basePath.c_str(), model->sequences[si].id, model->sequences[si].variationIndex);
                            auto animData = am->readFileOptional(animFileName);
                            if (!animData.empty()) {
                                pipeline::M2Loader::loadAnimFile(m2Data, animData, si, *model);
                            }
                        }
                    }

                    // Pre-decode model textures on background thread
                    for (const auto& tex : model->textures) {
                        if (tex.filename.empty()) continue;
                        std::string texKey = tex.filename;
                        std::replace(texKey.begin(), texKey.end(), '/', '\\');
                        std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.find(texKey) != result.predecodedTextures.end()) continue;
                        auto blp = am->loadTexture(texKey);
                        if (blp.isValid()) {
                            result.predecodedTextures[texKey] = std::move(blp);
                        }
                    }

                    // Pre-decode display skin textures (skin1/skin2/skin3 from CreatureDisplayInfo)
                    for (const auto& sp : skinPaths) {
                        std::string key = sp;
                        std::replace(key.begin(), key.end(), '/', '\\');
                        std::transform(key.begin(), key.end(), key.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (result.predecodedTextures.count(key)) continue;
                        auto blp = am->loadTexture(key);
                        if (blp.isValid()) {
                            result.predecodedTextures[key] = std::move(blp);
                        }
                    }

                    result.model = std::move(model);
                    result.valid = true;
                    return result;
                });
            asyncCreatureLoads_.push_back(std::move(load));
            asyncCreatureDisplayLoads_.insert(s.displayId);
            asyncLaunched++;
            // Don't erase from pendingCreatureSpawnGuids_ — the async result handler will do it
            rotationsLeft = pendingCreatureSpawns_.size();
            processed++;
            continue;
        }

        // Cached model — spawn is fast (no file I/O, just instance creation + texture setup)
        {
            auto spawnStart = std::chrono::steady_clock::now();
            spawnOnlineCreature(s.guid, s.displayId, s.x, s.y, s.z, s.orientation, s.scale);
            auto spawnEnd = std::chrono::steady_clock::now();
            float spawnMs = std::chrono::duration<float, std::milli>(spawnEnd - spawnStart).count();
            if (spawnMs > 100.0f) {
                LOG_WARNING("spawnOnlineCreature took ", spawnMs, "ms displayId=", s.displayId);
            }
        }
        pendingCreatureSpawnGuids_.erase(s.guid);

        // If spawn still failed, retry for a limited number of frames.
        if (!creatureInstances_.count(s.guid)) {
            if (creaturePermanentFailureGuids_.erase(s.guid) > 0) {
                creatureSpawnRetryCounts_.erase(s.guid);
                processed++;
                continue;
            }
            uint16_t retries = 0;
            auto it = creatureSpawnRetryCounts_.find(s.guid);
            if (it != creatureSpawnRetryCounts_.end()) {
                retries = it->second;
            }
            if (retries < MAX_CREATURE_SPAWN_RETRIES) {
                creatureSpawnRetryCounts_[s.guid] = static_cast<uint16_t>(retries + 1);
                pendingCreatureSpawns_.push_back(s);
                pendingCreatureSpawnGuids_.insert(s.guid);
            } else {
                creatureSpawnRetryCounts_.erase(s.guid);
                LOG_WARNING("Dropping creature spawn after retries: guid=0x", std::hex, s.guid, std::dec,
                            " displayId=", s.displayId);
            }
        } else {
            creatureSpawnRetryCounts_.erase(s.guid);
        }
        rotationsLeft = pendingCreatureSpawns_.size();
        processed++;
    }
}

void EntitySpawner::processPlayerSpawnQueue() {
    if (pendingPlayerSpawns_.empty()) return;
    if (!assetManager_ || !assetManager_->isInitialized()) return;

    int processed = 0;
    while (!pendingPlayerSpawns_.empty() && processed < MAX_SPAWNS_PER_FRAME) {
        PendingPlayerSpawn s = pendingPlayerSpawns_.front();
        pendingPlayerSpawns_.erase(pendingPlayerSpawns_.begin());
        pendingPlayerSpawnGuids_.erase(s.guid);

        // Skip if already spawned (could have been spawned by a previous update this frame)
        if (playerInstances_.count(s.guid)) {
            processed++;
            continue;
        }

        spawnOnlinePlayer(s.guid, s.raceId, s.genderId, s.appearanceBytes, s.facialFeatures, s.x, s.y, s.z, s.orientation);
        // Apply any equipment updates that arrived before the player was spawned.
        auto pit = pendingOnlinePlayerEquipment_.find(s.guid);
        if (pit != pendingOnlinePlayerEquipment_.end()) {
            deferredEquipmentQueue_.push_back({s.guid, pit->second});
            pendingOnlinePlayerEquipment_.erase(pit);
        }
        processed++;
    }
}

std::vector<std::string> EntitySpawner::resolveEquipmentTexturePaths(uint64_t guid,
    const std::array<uint32_t, 19>& displayInfoIds,
    const std::array<uint8_t, 19>& /*inventoryTypes*/) const {
    std::vector<std::string> paths;

    auto it = onlinePlayerAppearance_.find(guid);
    if (it == onlinePlayerAppearance_.end()) return paths;
    const OnlinePlayerAppearanceState& st = it->second;

    // Add base skin + underwear paths
    if (!st.bodySkinPath.empty()) paths.push_back(st.bodySkinPath);
    for (const auto& up : st.underwearPaths) {
        if (!up.empty()) paths.push_back(up);
    }

    // Resolve equipment region texture paths (same logic as setOnlinePlayerEquipment)
    auto displayInfoDbc = assetManager_->loadDBC("ItemDisplayInfo.dbc");
    if (!displayInfoDbc) return paths;
    const auto* idiL = pipeline::getActiveDBCLayout()
        ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo") : nullptr;

    static constexpr const char* componentDirs[] = {
        "ArmUpperTexture", "ArmLowerTexture", "HandTexture",
        "TorsoUpperTexture", "TorsoLowerTexture",
        "LegUpperTexture", "LegLowerTexture", "FootTexture",
    };
    uint32_t texRegionFields[8];
    pipeline::getItemDisplayInfoTextureFields(*displayInfoDbc, idiL, texRegionFields);
    const bool isFemale = (st.genderId == 1);

    for (int s = 0; s < 19; s++) {
        uint32_t did = displayInfoIds[s];
        if (did == 0) continue;
        int32_t recIdx = displayInfoDbc->findRecordById(did);
        if (recIdx < 0) continue;
        for (int region = 0; region < 8; region++) {
            std::string texName = displayInfoDbc->getString(
                static_cast<uint32_t>(recIdx), texRegionFields[region]);
            if (texName.empty()) continue;
            std::string base = "Item\\TextureComponents\\" +
                std::string(componentDirs[region]) + "\\" + texName;
            std::string genderPath = base + (isFemale ? "_F.blp" : "_M.blp");
            std::string unisexPath = base + "_U.blp";
            if (assetManager_->fileExists(genderPath)) paths.push_back(genderPath);
            else if (assetManager_->fileExists(unisexPath)) paths.push_back(unisexPath);
            else paths.push_back(base + ".blp");
        }
    }
    return paths;
}

void EntitySpawner::processAsyncEquipmentResults() {
    for (auto it = asyncEquipmentLoads_.begin(); it != asyncEquipmentLoads_.end(); ) {
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto result = it->future.get();
        it = asyncEquipmentLoads_.erase(it);

        auto* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
        if (!charRenderer) continue;

        // Set pre-decoded cache so compositeWithRegions skips synchronous BLP decode
        charRenderer->setPredecodedBLPCache(&result.predecodedTextures);
        setOnlinePlayerEquipment(result.guid, result.displayInfoIds, result.inventoryTypes);
        charRenderer->setPredecodedBLPCache(nullptr);
    }
}

void EntitySpawner::processDeferredEquipmentQueue() {
    // First, finalize any completed async pre-decodes
    processAsyncEquipmentResults();

    if (deferredEquipmentQueue_.empty()) return;
    // Limit in-flight async equipment loads
    if (asyncEquipmentLoads_.size() >= 2) return;

    auto [guid, equipData] = deferredEquipmentQueue_.front();
    deferredEquipmentQueue_.erase(deferredEquipmentQueue_.begin());

    // Resolve all texture paths that compositeWithRegions will need
    auto texturePaths = resolveEquipmentTexturePaths(guid, equipData.first, equipData.second);

    if (texturePaths.empty()) {
        // No textures to pre-decode — just apply directly (fast path)
        LOG_WARNING("Equipment fast path for guid=0x", std::hex, guid, std::dec,
                    " (no textures to pre-decode)");
        setOnlinePlayerEquipment(guid, equipData.first, equipData.second);
        return;
    }
    LOG_WARNING("Equipment async pre-decode for guid=0x", std::hex, guid, std::dec,
                " textures=", texturePaths.size());

    // Launch background BLP pre-decode
    auto* am = assetManager_;
    auto displayInfoIds = equipData.first;
    auto inventoryTypes = equipData.second;
    AsyncEquipmentLoad load;
    load.future = std::async(std::launch::async,
        [am, guid, displayInfoIds, inventoryTypes, paths = std::move(texturePaths)]() -> PreparedEquipmentUpdate {
            PreparedEquipmentUpdate result;
            result.guid = guid;
            result.displayInfoIds = displayInfoIds;
            result.inventoryTypes = inventoryTypes;
            for (const auto& path : paths) {
                std::string key = path;
                std::replace(key.begin(), key.end(), '/', '\\');
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (result.predecodedTextures.count(key)) continue;
                auto blp = am->loadTexture(key);
                if (blp.isValid()) {
                    result.predecodedTextures[key] = std::move(blp);
                }
            }
            return result;
        });
    asyncEquipmentLoads_.push_back(std::move(load));
}

void EntitySpawner::processAsyncGameObjectResults() {
    for (auto it = asyncGameObjectLoads_.begin(); it != asyncGameObjectLoads_.end(); ) {
        if (!it->future.valid() ||
            it->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }

        auto result = it->future.get();
        it = asyncGameObjectLoads_.erase(it);

        if (!result.valid || !result.isWmo || !result.wmoModel) {
            // Fallback: spawn via sync path (likely an M2 or failed WMO)
            spawnOnlineGameObject(result.guid, result.entry, result.displayId,
                                 result.x, result.y, result.z, result.orientation, result.scale);
            continue;
        }

        // WMO parsed on background thread — do GPU upload + instance creation on main thread
        auto* wmoRenderer = renderer_ ? renderer_->getWMORenderer() : nullptr;
        if (!wmoRenderer) continue;

        uint32_t modelId = 0;
        auto itCache = gameObjectDisplayIdWmoCache_.find(result.displayId);
        if (itCache != gameObjectDisplayIdWmoCache_.end()) {
            modelId = itCache->second;
        } else {
            modelId = nextGameObjectWmoModelId_++;
            wmoRenderer->setPredecodedBLPCache(&result.predecodedTextures);
            if (!wmoRenderer->loadModel(*result.wmoModel, modelId)) {
                wmoRenderer->setPredecodedBLPCache(nullptr);
                LOG_WARNING("Failed to load async gameobject WMO: ", result.modelPath);
                continue;
            }
            wmoRenderer->setPredecodedBLPCache(nullptr);
            gameObjectDisplayIdWmoCache_[result.displayId] = modelId;
        }

        glm::vec3 renderPos = core::coords::canonicalToRender(
            glm::vec3(result.x, result.y, result.z));
        uint32_t instanceId = wmoRenderer->createInstance(
            modelId, renderPos, glm::vec3(0.0f, 0.0f, result.orientation), result.scale);
        if (instanceId == 0) continue;

        gameObjectInstances_[result.guid] = {modelId, instanceId, true};

        // Queue transport doodad loading if applicable
        std::string lowerPath = result.modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerPath.find("transport") != std::string::npos) {
            const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(modelId);
            if (doodadTemplates && !doodadTemplates->empty()) {
                PendingTransportDoodadBatch batch;
                batch.guid = result.guid;
                batch.modelId = modelId;
                batch.instanceId = instanceId;
                batch.x = result.x;
                batch.y = result.y;
                batch.z = result.z;
                batch.orientation = result.orientation;
                batch.doodadBudget = doodadTemplates->size();
                pendingTransportDoodadBatches_.push_back(batch);
            }
        }
    }
}

void EntitySpawner::processGameObjectSpawnQueue() {
    // Finalize any completed async WMO loads first
    processAsyncGameObjectResults();

    if (pendingGameObjectSpawns_.empty()) return;

    // Process spawns: cached WMOs and M2s go sync (cheap), uncached WMOs go async
    auto startTime = std::chrono::steady_clock::now();
    static constexpr float kBudgetMs = 2.0f;
    static constexpr int kMaxAsyncLoads = 2;

    while (!pendingGameObjectSpawns_.empty()) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kBudgetMs) break;

        auto& s = pendingGameObjectSpawns_.front();

        // Check if this is an uncached WMO that needs async loading
        std::string modelPath;
        if (gameObjectLookupsBuilt_) {
            // Check transport overrides first
            bool isTransport = gameHandler_ && gameHandler_->isTransportGuid(s.guid);
            if (isTransport) {
                if (s.entry == 20808 || s.entry == 176231 || s.entry == 176310)
                    modelPath = "World\\wmo\\transports\\transport_ship\\transportship.wmo";
                else if (s.displayId == 807 || s.displayId == 808 || s.displayId == 175080 || s.displayId == 176495 || s.displayId == 164871)
                    modelPath = "World\\wmo\\transports\\transport_zeppelin\\transport_zeppelin.wmo";
                else if (s.displayId == 1587)
                    modelPath = "World\\wmo\\transports\\transport_horde_zeppelin\\Transport_Horde_Zeppelin.wmo";
                else if (s.displayId == 2454 || s.displayId == 181688 || s.displayId == 190536)
                    modelPath = "World\\wmo\\transports\\icebreaker\\Transport_Icebreaker_ship.wmo";
            }
            if (modelPath.empty())
                modelPath = getGameObjectModelPathForDisplayId(s.displayId);
        }

        std::string lowerPath = modelPath;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool isWmo = lowerPath.size() >= 4 && lowerPath.substr(lowerPath.size() - 4) == ".wmo";
        bool isCached = isWmo && gameObjectDisplayIdWmoCache_.count(s.displayId);

        if (isWmo && !isCached && !modelPath.empty() &&
            static_cast<int>(asyncGameObjectLoads_.size()) < kMaxAsyncLoads) {
            // Launch async WMO load — file I/O + parse on background thread
            auto* am = assetManager_;
            PendingGameObjectSpawn capture = s;
            std::string capturePath = modelPath;
            AsyncGameObjectLoad load;
            load.future = std::async(std::launch::async,
                [am, capture, capturePath]() -> PreparedGameObjectWMO {
                    PreparedGameObjectWMO result;
                    result.guid = capture.guid;
                    result.entry = capture.entry;
                    result.displayId = capture.displayId;
                    result.x = capture.x;
                    result.y = capture.y;
                    result.z = capture.z;
                    result.orientation = capture.orientation;
                    result.scale = capture.scale;
                    result.modelPath = capturePath;
                    result.isWmo = true;

                    auto wmoData = am->readFile(capturePath);
                    if (wmoData.empty()) return result;

                    auto wmo = std::make_shared<pipeline::WMOModel>(
                        pipeline::WMOLoader::load(wmoData));

                    // Load groups
                    if (wmo->nGroups > 0) {
                        std::string basePath = capturePath;
                        std::string ext;
                        if (basePath.size() > 4) {
                            ext = basePath.substr(basePath.size() - 4);
                            basePath = basePath.substr(0, basePath.size() - 4);
                        }
                        for (uint32_t gi = 0; gi < wmo->nGroups; gi++) {
                            char suffix[16];
                            snprintf(suffix, sizeof(suffix), "_%03u%s", gi, ext.c_str());
                            auto groupData = am->readFile(basePath + suffix);
                            if (groupData.empty()) {
                                snprintf(suffix, sizeof(suffix), "_%03u.wmo", gi);
                                groupData = am->readFile(basePath + suffix);
                            }
                            if (!groupData.empty()) {
                                pipeline::WMOLoader::loadGroup(groupData, *wmo, gi);
                            }
                        }
                    }

                    // Pre-decode WMO textures on background thread
                    for (const auto& texPath : wmo->textures) {
                        if (texPath.empty()) continue;
                        std::string texKey = texPath;
                        size_t nul = texKey.find('\0');
                        if (nul != std::string::npos) texKey.resize(nul);
                        std::replace(texKey.begin(), texKey.end(), '/', '\\');
                        std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (texKey.empty()) continue;
                        // Convert to .blp extension
                        if (texKey.size() >= 4) {
                            std::string ext = texKey.substr(texKey.size() - 4);
                            if (ext == ".tga" || ext == ".dds") {
                                texKey = texKey.substr(0, texKey.size() - 4) + ".blp";
                            }
                        }
                        if (result.predecodedTextures.find(texKey) != result.predecodedTextures.end()) continue;
                        auto blp = am->loadTexture(texKey);
                        if (blp.isValid()) {
                            result.predecodedTextures[texKey] = std::move(blp);
                        }
                    }

                    result.wmoModel = wmo;
                    result.valid = true;
                    return result;
                });
            asyncGameObjectLoads_.push_back(std::move(load));
            pendingGameObjectSpawns_.erase(pendingGameObjectSpawns_.begin());
            continue;
        }

        // Cached WMO or M2 — spawn synchronously (cheap)
        spawnOnlineGameObject(s.guid, s.entry, s.displayId, s.x, s.y, s.z, s.orientation, s.scale);
        pendingGameObjectSpawns_.erase(pendingGameObjectSpawns_.begin());
    }
}

void EntitySpawner::processPendingTransportRegistrations() {
    if (pendingTransportRegistrations_.empty()) return;
    if (!gameHandler_ || !renderer_) return;

    auto* transportManager = gameHandler_->getTransportManager();
    if (!transportManager) return;

    auto startTime = std::chrono::steady_clock::now();
    static constexpr int kMaxRegistrationsPerFrame = 2;
    static constexpr float kRegistrationBudgetMs = 2.0f;
    int processed = 0;

    for (auto it = pendingTransportRegistrations_.begin();
         it != pendingTransportRegistrations_.end() && processed < kMaxRegistrationsPerFrame;) {
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kRegistrationBudgetMs) break;

        const PendingTransportRegistration pending = *it;
        auto goIt = gameObjectInstances_.find(pending.guid);
        if (goIt == gameObjectInstances_.end()) {
            it = pendingTransportRegistrations_.erase(it);
            continue;
        }

        if (transportManager->getTransport(pending.guid)) {
            transportManager->updateServerTransport(
                pending.guid, glm::vec3(pending.x, pending.y, pending.z), pending.orientation);
            it = pendingTransportRegistrations_.erase(it);
            continue;
        }

        const uint32_t wmoInstanceId = goIt->second.instanceId;
        LOG_WARNING("Registering server transport: GUID=0x", std::hex, pending.guid, std::dec,
                 " entry=", pending.entry, " displayId=", pending.displayId, " wmoInstance=", wmoInstanceId,
                 " pos=(", pending.x, ", ", pending.y, ", ", pending.z, ")");

        // TransportAnimation.dbc is indexed by GameObject entry.
        uint32_t pathId = pending.entry;
        const bool preferServerData = gameHandler_->hasServerTransportUpdate(pending.guid);

        bool clientAnim = transportManager->isClientSideAnimation();
        LOG_DEBUG("Transport spawn callback: clientAnimation=", clientAnim,
                 " guid=0x", std::hex, pending.guid, std::dec,
                 " entry=", pending.entry, " pathId=", pathId,
                 " preferServer=", preferServerData);

        glm::vec3 canonicalSpawnPos(pending.x, pending.y, pending.z);
        const bool shipOrZeppelinDisplay =
            (pending.displayId == 3015 || pending.displayId == 3031 || pending.displayId == 7546 ||
             pending.displayId == 7446 || pending.displayId == 1587 || pending.displayId == 2454 ||
             pending.displayId == 807 || pending.displayId == 808);
        bool hasUsablePath = transportManager->hasPathForEntry(pending.entry);
        if (shipOrZeppelinDisplay) {
            hasUsablePath = transportManager->hasUsableMovingPathForEntry(pending.entry, 25.0f);
        }

        LOG_WARNING("Transport path check: entry=", pending.entry, " hasUsablePath=", hasUsablePath,
                 " preferServerData=", preferServerData, " shipOrZepDisplay=", shipOrZeppelinDisplay);

        if (preferServerData) {
            if (!hasUsablePath) {
                std::vector<glm::vec3> path = { canonicalSpawnPos };
                transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                LOG_WARNING("Server-first strict registration: stationary fallback for GUID 0x",
                         std::hex, pending.guid, std::dec, " entry=", pending.entry);
            } else {
                LOG_WARNING("Server-first transport registration: using entry DBC path for entry ", pending.entry);
            }
        } else if (!hasUsablePath) {
            bool allowZOnly = (pending.displayId == 455 || pending.displayId == 462);
            uint32_t inferredPath = transportManager->inferDbcPathForSpawn(
                canonicalSpawnPos, 1200.0f, allowZOnly);
            if (inferredPath != 0) {
                pathId = inferredPath;
                LOG_WARNING("Using inferred transport path ", pathId, " for entry ", pending.entry);
            } else {
                uint32_t remappedPath = transportManager->pickFallbackMovingPath(pending.entry, pending.displayId);
                if (remappedPath != 0) {
                    pathId = remappedPath;
                    LOG_WARNING("Using remapped fallback transport path ", pathId,
                             " for entry ", pending.entry, " displayId=", pending.displayId,
                             " (usableEntryPath=", transportManager->hasPathForEntry(pending.entry), ")");
                } else {
                    LOG_WARNING("No TransportAnimation.dbc path for entry ", pending.entry,
                                " - transport will be stationary");
                    std::vector<glm::vec3> path = { canonicalSpawnPos };
                    transportManager->loadPathFromNodes(pathId, path, false, 0.0f);
                }
            }
        } else {
            LOG_WARNING("Using real transport path from TransportAnimation.dbc for entry ", pending.entry);
        }

        transportManager->registerTransport(pending.guid, wmoInstanceId, pathId, canonicalSpawnPos, pending.entry);

        if (!goIt->second.isWmo) {
            if (auto* tr = transportManager->getTransport(pending.guid)) {
                tr->isM2 = true;
            }
        }

        transportManager->updateServerTransport(
            pending.guid, glm::vec3(pending.x, pending.y, pending.z), pending.orientation);

        auto moveIt = pendingTransportMoves_.find(pending.guid);
        if (moveIt != pendingTransportMoves_.end()) {
            const PendingTransportMove latestMove = moveIt->second;
            transportManager->updateServerTransport(
                pending.guid, glm::vec3(latestMove.x, latestMove.y, latestMove.z), latestMove.orientation);
            LOG_DEBUG("Replayed queued transport move for GUID=0x", std::hex, pending.guid, std::dec,
                     " pos=(", latestMove.x, ", ", latestMove.y, ", ", latestMove.z,
                     ") orientation=", latestMove.orientation);
            pendingTransportMoves_.erase(moveIt);
        }

        if (glm::dot(canonicalSpawnPos, canonicalSpawnPos) < 1.0f) {
            auto goData = gameHandler_->getCachedGameObjectInfo(pending.entry);
            if (goData && goData->type == 15 && goData->hasData && goData->data[0] != 0) {
                uint32_t taxiPathId = goData->data[0];
                if (transportManager->hasTaxiPath(taxiPathId)) {
                    transportManager->assignTaxiPathToTransport(pending.entry, taxiPathId);
                    LOG_DEBUG("Assigned cached TaxiPathNode path for MO_TRANSPORT entry=", pending.entry,
                             " taxiPathId=", taxiPathId);
                }
            }
        }

        if (auto* tr = transportManager->getTransport(pending.guid); tr) {
            LOG_WARNING("Transport registered: guid=0x", std::hex, pending.guid, std::dec,
                     " entry=", pending.entry, " displayId=", pending.displayId,
                     " pathId=", tr->pathId,
                     " mode=", (tr->useClientAnimation ? "client" : "server"),
                     " serverUpdates=", tr->serverUpdateCount);
        } else {
            LOG_DEBUG("Transport registered: guid=0x", std::hex, pending.guid, std::dec,
                     " entry=", pending.entry, " displayId=", pending.displayId,
                     " (TransportManager instance missing)");
        }

        ++processed;
        it = pendingTransportRegistrations_.erase(it);
    }
}

void EntitySpawner::processPendingTransportDoodads() {
    if (pendingTransportDoodadBatches_.empty()) return;
    if (!renderer_ || !assetManager_) return;

    auto* wmoRenderer = renderer_->getWMORenderer();
    auto* m2Renderer = renderer_->getM2Renderer();
    if (!wmoRenderer || !m2Renderer) return;

    auto startTime = std::chrono::steady_clock::now();
    static constexpr float kDoodadBudgetMs = 4.0f;

    // Batch all GPU uploads into a single async command buffer submission so that
    // N doodads with multiple textures each don't each block on vkQueueSubmit +
    // vkWaitForFences. Without batching, 30+ doodads × several textures = hundreds
    // of sync GPU submits → the 490ms stall that preceded the VK_ERROR_DEVICE_LOST.
    auto* vkCtx = renderer_->getVkContext();
    if (vkCtx) vkCtx->beginUploadBatch();

    size_t budgetLeft = MAX_TRANSPORT_DOODADS_PER_FRAME;
    for (auto it = pendingTransportDoodadBatches_.begin();
         it != pendingTransportDoodadBatches_.end() && budgetLeft > 0;) {
        // Time budget check
        float elapsedMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsedMs >= kDoodadBudgetMs) break;
        auto goIt = gameObjectInstances_.find(it->guid);
        if (goIt == gameObjectInstances_.end() || !goIt->second.isWmo ||
            goIt->second.instanceId != it->instanceId || goIt->second.modelId != it->modelId) {
            it = pendingTransportDoodadBatches_.erase(it);
            continue;
        }

        const auto* doodadTemplates = wmoRenderer->getDoodadTemplates(it->modelId);
        if (!doodadTemplates || doodadTemplates->empty()) {
            it = pendingTransportDoodadBatches_.erase(it);
            continue;
        }

        const size_t maxIndex = std::min(it->doodadBudget, doodadTemplates->size());
        while (it->nextIndex < maxIndex && budgetLeft > 0) {
            // Per-doodad time budget (each does synchronous file I/O + parse + GPU upload)
            float innerMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - startTime).count();
            if (innerMs >= kDoodadBudgetMs) { budgetLeft = 0; break; }

            const auto& doodadTemplate = (*doodadTemplates)[it->nextIndex];
            it->nextIndex++;
            budgetLeft--;

            uint32_t doodadModelId = static_cast<uint32_t>(std::hash<std::string>{}(doodadTemplate.m2Path));
            auto m2Data = assetManager_->readFile(doodadTemplate.m2Path);
            if (m2Data.empty()) continue;

            pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
            std::string skinPath = doodadTemplate.m2Path.substr(0, doodadTemplate.m2Path.size() - 3) + "00.skin";
            std::vector<uint8_t> skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty() && m2Model.version >= 264) {
                pipeline::M2Loader::loadSkin(skinData, m2Model);
            }
            if (!m2Model.isValid()) continue;

            if (!m2Renderer->loadModel(m2Model, doodadModelId)) continue;
            uint32_t m2InstanceId = m2Renderer->createInstance(doodadModelId, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f);
            if (m2InstanceId == 0) continue;
            m2Renderer->setSkipCollision(m2InstanceId, true);

            wmoRenderer->addDoodadToInstance(it->instanceId, m2InstanceId, doodadTemplate.localTransform);
            it->spawnedDoodads++;
        }

        if (it->nextIndex >= maxIndex) {
            if (it->spawnedDoodads > 0) {
                LOG_DEBUG("Spawned ", it->spawnedDoodads,
                         " transport doodads for WMO instance ", it->instanceId);
                glm::vec3 renderPos = core::coords::canonicalToRender(glm::vec3(it->x, it->y, it->z));
                glm::mat4 wmoTransform(1.0f);
                wmoTransform = glm::translate(wmoTransform, renderPos);
                wmoTransform = glm::rotate(wmoTransform, it->orientation, glm::vec3(0, 0, 1));
                wmoRenderer->setInstanceTransform(it->instanceId, wmoTransform);
            }
            it = pendingTransportDoodadBatches_.erase(it);
        } else {
            ++it;
        }
    }

    // Finalize the upload batch — submit all GPU copies in one shot (async, no wait).
    if (vkCtx) vkCtx->endUploadBatch();
}

void EntitySpawner::processPendingMount() {
    if (pendingMountDisplayId_ == 0) return;
    uint32_t mountDisplayId = pendingMountDisplayId_;
    pendingMountDisplayId_ = 0;
    LOG_INFO("processPendingMount: loading displayId ", mountDisplayId);

    if (!renderer_ || !renderer_->getCharacterRenderer() || !assetManager_) return;
    auto* charRenderer = renderer_->getCharacterRenderer();

    std::string m2Path = getModelPathForDisplayId(mountDisplayId);
    if (m2Path.empty()) {
        LOG_WARNING("No model path for mount displayId ", mountDisplayId);
        return;
    }

    // Check model cache
    uint32_t modelId = 0;
    auto cacheIt = displayIdModelCache_.find(mountDisplayId);
    if (cacheIt != displayIdModelCache_.end()) {
        modelId = cacheIt->second;
    } else {
        modelId = nextCreatureModelId_++;

        auto m2Data = assetManager_->readFile(m2Path);
        if (m2Data.empty()) {
            LOG_WARNING("Failed to read mount M2: ", m2Path);
            return;
        }

        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.vertices.empty()) {
            LOG_WARNING("Failed to parse mount M2: ", m2Path);
            return;
        }

        // Load skin file (only for WotLK M2s - vanilla has embedded skin)
        if (model.version >= 264) {
            std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
            auto skinData = assetManager_->readFile(skinPath);
            if (!skinData.empty()) {
                pipeline::M2Loader::loadSkin(skinData, model);
            } else {
                LOG_WARNING("Missing skin file for WotLK mount M2: ", skinPath);
            }
        }

        // Load external .anim files (only idle + run needed for mounts)
        std::string basePath = m2Path.substr(0, m2Path.size() - 3);
        for (uint32_t si = 0; si < model.sequences.size(); si++) {
            if (!(model.sequences[si].flags & 0x20)) {
                uint32_t animId = model.sequences[si].id;
                // Only load stand, walk, run anims to avoid hang
                if (animId != rendering::anim::STAND && animId != rendering::anim::WALK && animId != rendering::anim::RUN) continue;
                char animFileName[256];
                snprintf(animFileName, sizeof(animFileName), "%s%04u-%02u.anim",
                    basePath.c_str(), animId, model.sequences[si].variationIndex);
                auto animData = assetManager_->readFileOptional(animFileName);
                if (!animData.empty()) {
                    pipeline::M2Loader::loadAnimFile(m2Data, animData, si, model);
                }
            }
        }

        if (!charRenderer->loadModel(model, modelId)) {
            LOG_WARNING("Failed to load mount model: ", m2Path);
            return;
        }

        displayIdModelCache_[mountDisplayId] = modelId;
    }

    // Apply creature skin textures from CreatureDisplayInfo.dbc.
    // Re-apply even for cached models so transient failures can self-heal.
    std::string modelDir;
    size_t lastSlash = m2Path.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        modelDir = m2Path.substr(0, lastSlash + 1);
    }

    auto itDisplayData = displayDataMap_.find(mountDisplayId);
    bool haveDisplayData = false;
    CreatureDisplayData dispData{};
    if (itDisplayData != displayDataMap_.end()) {
        dispData = itDisplayData->second;
        haveDisplayData = true;
    } else {
        // Some taxi mount display IDs are sparse; recover skins by matching model path.
        std::string lowerMountPath = m2Path;
        std::transform(lowerMountPath.begin(), lowerMountPath.end(), lowerMountPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int bestScore = -1;
        for (const auto& [dispId, data] : displayDataMap_) {
            auto pit = modelIdToPath_.find(data.modelId);
            if (pit == modelIdToPath_.end()) continue;
            std::string p = pit->second;
            std::transform(p.begin(), p.end(), p.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (p != lowerMountPath) continue;
            int score = 0;
            if (!data.skin1.empty()) {
                std::string p1 = modelDir + data.skin1 + ".blp";
                score += assetManager_->fileExists(p1) ? 30 : 3;
            }
            if (!data.skin2.empty()) {
                std::string p2 = modelDir + data.skin2 + ".blp";
                score += assetManager_->fileExists(p2) ? 20 : 2;
            }
            if (!data.skin3.empty()) {
                std::string p3 = modelDir + data.skin3 + ".blp";
                score += assetManager_->fileExists(p3) ? 10 : 1;
            }
            if (score > bestScore) {
                bestScore = score;
                dispData = data;
                haveDisplayData = true;
            }
        }
        if (haveDisplayData) {
            LOG_INFO("Recovered mount display data by model path for displayId=", mountDisplayId,
                     " skin1='", dispData.skin1, "' skin2='", dispData.skin2,
                     "' skin3='", dispData.skin3, "'");
        }
    }
    if (haveDisplayData) {
        // If this displayId has no skins, try to find another displayId for the same model with skins.
        if (dispData.skin1.empty() && dispData.skin2.empty() && dispData.skin3.empty()) {
            uint32_t sourceModelId = dispData.modelId;
            int bestScore = -1;
            for (const auto& [dispId, data] : displayDataMap_) {
                if (data.modelId != sourceModelId) continue;
                int score = 0;
                if (!data.skin1.empty()) {
                    std::string p = modelDir + data.skin1 + ".blp";
                    score += assetManager_->fileExists(p) ? 30 : 3;
                }
                if (!data.skin2.empty()) {
                    std::string p = modelDir + data.skin2 + ".blp";
                    score += assetManager_->fileExists(p) ? 20 : 2;
                }
                if (!data.skin3.empty()) {
                    std::string p = modelDir + data.skin3 + ".blp";
                    score += assetManager_->fileExists(p) ? 10 : 1;
                }
                if (score > bestScore) {
                    bestScore = score;
                    dispData = data;
                }
            }
            LOG_INFO("Mount skin fallback for displayId=", mountDisplayId,
                     " modelId=", sourceModelId, " skin1='", dispData.skin1,
                     "' skin2='", dispData.skin2, "' skin3='", dispData.skin3, "'");
        }
        const auto* md = charRenderer->getModelData(modelId);
        if (md) {
            LOG_INFO("Mount model textures: ", md->textures.size(), " slots, skin1='", dispData.skin1,
                     "' skin2='", dispData.skin2, "' skin3='", dispData.skin3, "'");
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                LOG_INFO("  tex[", ti, "] type=", md->textures[ti].type,
                         " filename='", md->textures[ti].filename, "'");
            }

            int replaced = 0;
            for (size_t ti = 0; ti < md->textures.size(); ti++) {
                const auto& tex = md->textures[ti];
                std::string texPath;
                if (tex.type == 11 && !dispData.skin1.empty()) {
                    texPath = modelDir + dispData.skin1 + ".blp";
                } else if (tex.type == 12 && !dispData.skin2.empty()) {
                    texPath = modelDir + dispData.skin2 + ".blp";
                } else if (tex.type == 13 && !dispData.skin3.empty()) {
                    texPath = modelDir + dispData.skin3 + ".blp";
                }
                if (!texPath.empty()) {
                    rendering::VkTexture* skinTex = charRenderer->loadTexture(texPath);
                    if (skinTex) {
                        charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                        LOG_INFO("  Applied skin texture slot ", ti, ": ", texPath);
                        replaced++;
                    } else {
                        LOG_WARNING("  Failed to load skin texture slot ", ti, ": ", texPath);
                    }
                }
            }

            // Force skin textures onto type-0 (hardcoded) slots that have no filename
            if (replaced == 0) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    const auto& tex = md->textures[ti];
                    if (tex.type == 0 && tex.filename.empty()) {
                        // Empty hardcoded slot — try skin1 then skin2
                        std::string texPath;
                        if (!dispData.skin1.empty() && replaced == 0) {
                            texPath = modelDir + dispData.skin1 + ".blp";
                        } else if (!dispData.skin2.empty()) {
                            texPath = modelDir + dispData.skin2 + ".blp";
                        }
                        if (!texPath.empty()) {
                            rendering::VkTexture* skinTex = charRenderer->loadTexture(texPath);
                            if (skinTex) {
                                charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), skinTex);
                                LOG_INFO("  Forced skin on empty hardcoded slot ", ti, ": ", texPath);
                                replaced++;
                            }
                        }
                    }
                }
            }

            // If still no textures, try hardcoded model texture filenames
            if (replaced == 0) {
                for (size_t ti = 0; ti < md->textures.size(); ti++) {
                    if (!md->textures[ti].filename.empty()) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(md->textures[ti].filename);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, static_cast<uint32_t>(ti), texId);
                            LOG_INFO("  Used model embedded texture slot ", ti, ": ", md->textures[ti].filename);
                            replaced++;
                        }
                    }
                }
            }

            // Final fallback for gryphon/wyvern: try well-known skin texture names
            if (replaced == 0 && !md->textures.empty()) {
                std::string lowerMountPath = m2Path;
                std::transform(lowerMountPath.begin(), lowerMountPath.end(), lowerMountPath.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerMountPath.find("gryphon") != std::string::npos) {
                    const char* gryphonSkins[] = {
                        "Creature\\Gryphon\\Gryphon_Skin.blp",
                        "Creature\\Gryphon\\Gryphon_Skin01.blp",
                        "Creature\\Gryphon\\GRYPHON_SKIN01.BLP",
                        nullptr
                    };
                    for (const char** p = gryphonSkins; *p; ++p) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(*p);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, 0, texId);
                            LOG_INFO("  Forced gryphon skin fallback: ", *p);
                            replaced++;
                            break;
                        }
                    }
                } else if (lowerMountPath.find("wyvern") != std::string::npos) {
                    const char* wyvernSkins[] = {
                        "Creature\\Wyvern\\Wyvern_Skin.blp",
                        "Creature\\Wyvern\\Wyvern_Skin01.blp",
                        nullptr
                    };
                    for (const char** p = wyvernSkins; *p; ++p) {
                        rendering::VkTexture* texId = charRenderer->loadTexture(*p);
                        if (texId) {
                            charRenderer->setModelTexture(modelId, 0, texId);
                            LOG_INFO("  Forced wyvern skin fallback: ", *p);
                            replaced++;
                            break;
                        }
                    }
                }
            }
            LOG_INFO("Mount texture setup: ", replaced, " textures applied");
        }
    }

    mountModelId_ = modelId;

    // Create mount instance at player position
    glm::vec3 mountPos = renderer_->getCharacterPosition();
    float yawRad = glm::radians(renderer_->getCharacterYaw());
    uint32_t instanceId = charRenderer->createInstance(modelId, mountPos,
        glm::vec3(0.0f, 0.0f, yawRad), 1.0f);

    if (instanceId == 0) {
        LOG_WARNING("Failed to create mount instance");
        return;
    }

    mountInstanceId_ = instanceId;

    // Compute height offset — place player above mount's back
    // Use tight bounds from actual vertices (M2 header bounds can be inaccurate)
    const auto* modelData = charRenderer->getModelData(modelId);
    float heightOffset = 1.8f;
    if (modelData && !modelData->vertices.empty()) {
        float minZ =  std::numeric_limits<float>::max();
        float maxZ = -std::numeric_limits<float>::max();
        for (const auto& v : modelData->vertices) {
            if (v.position.z < minZ) minZ = v.position.z;
            if (v.position.z > maxZ) maxZ = v.position.z;
        }
        float extentZ = maxZ - minZ;
        LOG_INFO("Mount tight bounds: minZ=", minZ, " maxZ=", maxZ, " extentZ=", extentZ);
        if (extentZ > 0.5f) {
            // Saddle point is roughly 75% up the model, measured from model origin
            heightOffset = maxZ * 0.8f;
            if (heightOffset < 1.0f) heightOffset = extentZ * 0.75f;
            if (heightOffset < 1.0f) heightOffset = 1.8f;
        }
    }

    renderer_->setMounted(instanceId, mountDisplayId, heightOffset, m2Path);

    // For taxi mounts, start with flying animation; for ground mounts, start with stand
    bool isTaxi = gameHandler_ && gameHandler_->isOnTaxiFlight();
    uint32_t startAnim = rendering::anim::STAND;
    if (isTaxi) {
        // Try WotLK fly anims first, then Vanilla-friendly fallbacks
        using namespace rendering::anim;
        uint32_t taxiCandidates[] = {FLY_FORWARD, FLY_IDLE, FLY_RUN_2, FLY_SPELL, FLY_RISE, SPELL_KNEEL_LOOP, FLY_CUSTOM_SPELL_10, DEAD, RUN};
        for (uint32_t anim : taxiCandidates) {
            if (charRenderer->hasAnimation(instanceId, anim)) {
                startAnim = anim;
                break;
            }
        }
        // If none found, startAnim stays 0 (Stand/hover) which is fine for flying creatures
    }
    charRenderer->playAnimation(instanceId, startAnim, true);

    LOG_INFO("processPendingMount: DONE displayId=", mountDisplayId, " model=", m2Path, " heightOffset=", heightOffset);
}

void EntitySpawner::despawnCreature(uint64_t guid) {
    // If this guid is a PLAYER, it will be tracked in playerInstances_.
    // Route to the correct despawn path so we don't leak instances.
    if (playerInstances_.count(guid)) {
        despawnPlayer(guid);
        return;
    }

    pendingCreatureSpawnGuids_.erase(guid);
    creatureSpawnRetryCounts_.erase(guid);
    creaturePermanentFailureGuids_.erase(guid);
    deadCreatureGuids_.erase(guid);

    auto it = creatureInstances_.find(guid);
    if (it == creatureInstances_.end()) return;

    if (renderer_ && renderer_->getCharacterRenderer()) {
        renderer_->getCharacterRenderer()->removeInstance(it->second);
    }

    creatureInstances_.erase(it);
    creatureModelIds_.erase(guid);
    creatureRenderPosCache_.erase(guid);
    creatureWeaponsAttached_.erase(guid);
    creatureWeaponAttachAttempts_.erase(guid);
    creatureWasMoving_.erase(guid);
    creatureWasSwimming_.erase(guid);
    creatureWasFlying_.erase(guid);
    creatureWasWalking_.erase(guid);
    creatureSwimmingState_.erase(guid);
    creatureWalkingState_.erase(guid);
    creatureFlyingState_.erase(guid);

    LOG_DEBUG("Despawned creature: guid=0x", std::hex, guid, std::dec);
}

void EntitySpawner::despawnGameObject(uint64_t guid) {
    pendingTransportDoodadBatches_.erase(
        std::remove_if(pendingTransportDoodadBatches_.begin(), pendingTransportDoodadBatches_.end(),
                       [guid](const PendingTransportDoodadBatch& b) { return b.guid == guid; }),
        pendingTransportDoodadBatches_.end());

    auto it = gameObjectInstances_.find(guid);
    if (it == gameObjectInstances_.end()) return;

    if (renderer_) {
        if (it->second.isWmo) {
            if (auto* wmoRenderer = renderer_->getWMORenderer()) {
                wmoRenderer->removeInstance(it->second.instanceId);
            }
        } else {
            if (auto* m2Renderer = renderer_->getM2Renderer()) {
                m2Renderer->removeInstance(it->second.instanceId);
            }
        }
    }

    gameObjectInstances_.erase(it);

    LOG_DEBUG("Despawned gameobject: guid=0x", std::hex, guid, std::dec);
}

bool EntitySpawner::loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel) {
    auto m2Data = assetManager_->readFile(m2Path);
    if (m2Data.empty()) return false;
    outModel = pipeline::M2Loader::load(m2Data);
    // Load skin (WotLK+ M2 format): strip .m2, append 00.skin
    std::string skinPath = m2Path;
    size_t dotPos = skinPath.rfind('.');
    if (dotPos != std::string::npos) skinPath = skinPath.substr(0, dotPos);
    skinPath += "00.skin";
    auto skinData = assetManager_->readFile(skinPath);
    if (!skinData.empty() && outModel.version >= 264)
        pipeline::M2Loader::loadSkin(skinData, outModel);
    return outModel.isValid();
}


} // namespace core
} // namespace wowee
