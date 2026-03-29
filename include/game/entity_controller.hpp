#pragma once

#include "game/world_packets.hpp"
#include "game/entity.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace game {

class GameHandler;

class EntityController {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit EntityController(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Entity Manager access ---
    EntityManager& getEntityManager() { return entityManager; }
    const EntityManager& getEntityManager() const { return entityManager; }

    // --- Name / info cache queries ---
    void queryPlayerName(uint64_t guid);
    void queryCreatureInfo(uint32_t entry, uint64_t guid);
    void queryGameObjectInfo(uint32_t entry, uint64_t guid);
    std::string getCachedPlayerName(uint64_t guid) const;
    std::string getCachedCreatureName(uint32_t entry) const;
    void invalidatePlayerName(uint64_t guid) { playerNameCache.erase(guid); }

    // Read-only cache access for other handlers
    const std::unordered_map<uint64_t, std::string>& getPlayerNameCache() const { return playerNameCache; }
    const std::unordered_map<uint32_t, CreatureQueryResponseData>& getCreatureInfoCache() const { return creatureInfoCache; }
    std::string getCachedCreatureSubName(uint32_t entry) const {
        auto it = creatureInfoCache.find(entry);
        return (it != creatureInfoCache.end()) ? it->second.subName : "";
    }
    int getCreatureRank(uint32_t entry) const {
        auto it = creatureInfoCache.find(entry);
        return (it != creatureInfoCache.end()) ? static_cast<int>(it->second.rank) : -1;
    }
    uint32_t getCreatureType(uint32_t entry) const {
        auto it = creatureInfoCache.find(entry);
        return (it != creatureInfoCache.end()) ? it->second.creatureType : 0;
    }
    uint32_t getCreatureFamily(uint32_t entry) const {
        auto it = creatureInfoCache.find(entry);
        return (it != creatureInfoCache.end()) ? it->second.family : 0;
    }
    const GameObjectQueryResponseData* getCachedGameObjectInfo(uint32_t entry) const {
        auto it = gameObjectInfoCache_.find(entry);
        return (it != gameObjectInfoCache_.end()) ? &it->second : nullptr;
    }

    // Name lookup (checks cache then entity manager)
    const std::string& lookupName(uint64_t guid) const {
        static const std::string kEmpty;
        auto it = playerNameCache.find(guid);
        if (it != playerNameCache.end()) return it->second;
        auto entity = entityManager.getEntity(guid);
        if (entity) {
            if (auto* unit = dynamic_cast<const Unit*>(entity.get())) {
                if (!unit->getName().empty()) return unit->getName();
            }
        }
        return kEmpty;
    }
    uint8_t lookupPlayerClass(uint64_t guid) const {
        auto it = playerClassRaceCache_.find(guid);
        return it != playerClassRaceCache_.end() ? it->second.classId : 0;
    }
    uint8_t lookupPlayerRace(uint64_t guid) const {
        auto it = playerClassRaceCache_.find(guid);
        return it != playerClassRaceCache_.end() ? it->second.raceId : 0;
    }

    // --- Transport GUID tracking ---
    bool isTransportGuid(uint64_t guid) const { return transportGuids_.count(guid) > 0; }
    bool hasServerTransportUpdate(uint64_t guid) const { return serverUpdatedTransportGuids_.count(guid) > 0; }

    // --- Update object work queue ---
    void enqueueUpdateObjectWork(UpdateObjectData&& data);
    void processPendingUpdateObjectWork(const std::chrono::steady_clock::time_point& start,
                                        float budgetMs);
    bool hasPendingUpdateObjectWork() const { return !pendingUpdateObjectWork_.empty(); }

    // --- Reset all state (called on disconnect / character switch) ---
    void clearAll();

private:
    GameHandler& owner_;

    // --- Entity tracking ---
    EntityManager entityManager;              // Manages all entities in view

    // ---- Name caches ----
    std::unordered_map<uint64_t, std::string> playerNameCache;
    // Class/race cache from SMSG_NAME_QUERY_RESPONSE (guid → {classId, raceId})
    struct PlayerClassRace { uint8_t classId = 0; uint8_t raceId = 0; };
    std::unordered_map<uint64_t, PlayerClassRace> playerClassRaceCache_;
    std::unordered_set<uint64_t> pendingNameQueries;
    std::unordered_map<uint32_t, CreatureQueryResponseData> creatureInfoCache;
    std::unordered_set<uint32_t> pendingCreatureQueries;
    std::unordered_map<uint32_t, GameObjectQueryResponseData> gameObjectInfoCache_;
    std::unordered_set<uint32_t> pendingGameObjectQueries_;

    // --- Update Object work queue ---
    struct PendingUpdateObjectWork {
        UpdateObjectData data;
        size_t nextBlockIndex = 0;
        bool outOfRangeProcessed = false;
        bool newItemCreated = false;
    };
    std::deque<PendingUpdateObjectWork> pendingUpdateObjectWork_;

    // --- Transport GUID tracking ---
    std::unordered_set<uint64_t> transportGuids_;  // GUIDs of known transport GameObjects
    std::unordered_set<uint64_t> serverUpdatedTransportGuids_;

    // --- Packet handlers ---
    void handleUpdateObject(network::Packet& packet);
    void handleCompressedUpdateObject(network::Packet& packet);
    void handleDestroyObject(network::Packet& packet);
    void handleNameQueryResponse(network::Packet& packet);
    void handleCreatureQueryResponse(network::Packet& packet);
    void handleGameObjectQueryResponse(network::Packet& packet);
    void handleGameObjectPageText(network::Packet& packet);
    void handlePageTextQueryResponse(network::Packet& packet);

    // --- Entity lifecycle ---
    void processOutOfRangeObjects(const std::vector<uint64_t>& guids);
    void applyUpdateObjectBlock(const UpdateBlock& block, bool& newItemCreated);
    void finalizeUpdateObjectBatch(bool newItemCreated);
};

} // namespace game
} // namespace wowee
