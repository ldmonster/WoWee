#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class MovementHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit MovementHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---

    void sendMovement(Opcode opcode);
    void setPosition(float x, float y, float z);
    void setOrientation(float orientation);
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Follow target (moved from GameHandler)
    void followTarget();
    void cancelFollow();

    // Area trigger detection
    void loadAreaTriggerDbc();
    void checkAreaTriggers();

    // Transport attachment
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation);
    void clearTransportAttachment(uint64_t childGuid);
    void updateAttachedTransportChildren(float deltaTime);

    // Movement info accessors
    const MovementInfo& getMovementInfo() const { return movementInfo; }
    MovementInfo& getMovementInfoMut() { return movementInfo; }

    // Speed accessors
    float getServerRunSpeed() const { return serverRunSpeed_; }
    float getServerWalkSpeed() const { return serverWalkSpeed_; }
    float getServerSwimSpeed() const { return serverSwimSpeed_; }
    float getServerSwimBackSpeed() const { return serverSwimBackSpeed_; }
    float getServerFlightSpeed() const { return serverFlightSpeed_; }
    float getServerFlightBackSpeed() const { return serverFlightBackSpeed_; }
    float getServerRunBackSpeed() const { return serverRunBackSpeed_; }
    float getServerTurnRate() const { return serverTurnRate_; }

    // Movement flag queries
    bool isPlayerRooted() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::ROOT)) != 0;
    }
    bool isGravityDisabled() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::LEVITATING)) != 0;
    }
    bool isFeatherFalling() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::FEATHER_FALL)) != 0;
    }
    bool isWaterWalking() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::WATER_WALK)) != 0;
    }
    bool isPlayerFlying() const {
        const uint32_t flyMask = static_cast<uint32_t>(MovementFlags::CAN_FLY) |
                                 static_cast<uint32_t>(MovementFlags::FLYING);
        return (movementInfo.flags & flyMask) == flyMask;
    }
    bool isHovering() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::HOVER)) != 0;
    }
    bool isSwimming() const {
        return (movementInfo.flags & static_cast<uint32_t>(MovementFlags::SWIMMING)) != 0;
    }

    // Taxi / Flight Paths
    bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();
    void activateTaxi(uint32_t destNodeId);
    bool isOnTaxiFlight() const { return onTaxiFlight_; }
    bool isTaxiMountActive() const { return taxiMountActive_; }
    bool isTaxiActivationPending() const { return taxiActivatePending_; }
    void forceClearTaxiAndMovementState();
    const std::string& getTaxiDestName() const { return taxiDestName_; }
    const ShowTaxiNodesData& getTaxiData() const { return currentTaxiData_; }
    uint32_t getTaxiCurrentNode() const { return currentTaxiData_.nearestNode; }

    struct TaxiNode {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
        std::string name;
        uint32_t mountDisplayIdAlliance = 0;
        uint32_t mountDisplayIdHorde = 0;
    };
    struct TaxiPathEdge {
        uint32_t pathId = 0;
        uint32_t fromNode = 0, toNode = 0;
        uint32_t cost = 0;
    };
    struct TaxiPathNode {
        uint32_t id = 0;
        uint32_t pathId = 0;
        uint32_t nodeIndex = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
    };

    const std::unordered_map<uint32_t, TaxiNode>& getTaxiNodes() const { return taxiNodes_; }
    // WotLK 3.3.5a TaxiNodes.dbc has 384 entries; the known-taxi bitmask
    // is 12 × uint32 = 384 bits. Node IDs outside this range are invalid.
    static constexpr uint32_t kMaxTaxiNodeId = 384;
    bool isKnownTaxiNode(uint32_t nodeId) const {
        if (nodeId == 0 || nodeId > kMaxTaxiNodeId) return false;
        uint32_t idx = nodeId - 1;
        return (knownTaxiMask_[idx / 32] & (1u << (idx % 32))) != 0;
    }
    uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    void updateClientTaxi(float deltaTime);
    uint32_t nextMovementTimestampMs();
    void sanitizeMovementForTaxi();

    // Heartbeat / movement timing (for GameHandler::update())
    float& timeSinceLastMoveHeartbeatRef() { return timeSinceLastMoveHeartbeat_; }
    float getMoveHeartbeatInterval() const { return moveHeartbeatInterval_; }
    bool isServerMovementAllowed() const { return serverMovementAllowed_; }
    void setServerMovementAllowed(bool v) { serverMovementAllowed_ = v; }
    uint32_t& monsterMovePacketsThisTickRef() { return monsterMovePacketsThisTick_; }
    uint32_t& monsterMovePacketsDroppedThisTickRef() { return monsterMovePacketsDroppedThisTick_; }

    // Taxi state references for GameHandler update/processing
    bool& onTaxiFlightRef() { return onTaxiFlight_; }
    bool& taxiMountActiveRef() { return taxiMountActive_; }
    uint32_t& taxiMountDisplayIdRef() { return taxiMountDisplayId_; }
    bool& taxiActivatePendingRef() { return taxiActivatePending_; }
    float& taxiActivateTimerRef() { return taxiActivateTimer_; }
    bool& taxiClientActiveRef() { return taxiClientActive_; }
    float& taxiLandingCooldownRef() { return taxiLandingCooldown_; }
    float& taxiStartGraceRef() { return taxiStartGrace_; }
    bool& taxiRecoverPendingRef() { return taxiRecoverPending_; }
    uint32_t& taxiRecoverMapIdRef() { return taxiRecoverMapId_; }
    glm::vec3& taxiRecoverPosRef() { return taxiRecoverPos_; }
    std::unordered_map<uint64_t, bool>& taxiNpcHasRoutesRef() { return taxiNpcHasRoutes_; }
    uint32_t* knownTaxiMaskPtr() { return knownTaxiMask_; }
    bool& taxiMaskInitializedRef() { return taxiMaskInitialized_; }
    uint64_t& taxiNpcGuidRef() { return taxiNpcGuid_; }

    // Other-player movement timing (for cleanup on despawn etc.)
    std::unordered_map<uint64_t, uint32_t>& otherPlayerMoveTimeMsRef() { return otherPlayerMoveTimeMs_; }
    std::unordered_map<uint64_t, float>& otherPlayerSmoothedIntervalMsRef() { return otherPlayerSmoothedIntervalMs_; }

    // Methods also called from GameHandler's registerOpcodeHandlers
    void handleCompressedMoves(network::Packet& packet);
    void handleForceMoveFlagChange(network::Packet& packet, const char* name, Opcode ackOpcode, uint32_t flag, bool set);
    void handleMoveSetCollisionHeight(network::Packet& packet);
    void applyTaxiMountForCurrentNode();

private:
    // --- Packet handlers ---
    void handleMonsterMove(network::Packet& packet);
    void handleMonsterMoveTransport(network::Packet& packet);
    void handleOtherPlayerMovement(network::Packet& packet);
    void handleMoveSetSpeed(network::Packet& packet);
    void handleForceRunSpeedChange(network::Packet& packet);
    network::Packet buildForceAck(Opcode ackOpcode, uint32_t counter);
    void handleForceSpeedChange(network::Packet& packet, const char* name, Opcode ackOpcode, float* speedStorage);
    void handleForceMoveRootState(network::Packet& packet, bool rooted);
    void handleMoveKnockBack(network::Packet& packet);
    void handleTeleportAck(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void handleShowTaxiNodes(network::Packet& packet);
    void handleClientControlUpdate(network::Packet& packet);
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // --- Private helpers ---
    void buildTaxiCostMap();
    void startClientTaxiPath(const std::vector<uint32_t>& pathNodes);

    friend class GameHandler;

    GameHandler& owner_;

    // --- Movement state ---
    // Reference to GameHandler's movementInfo to avoid desync
    MovementInfo& movementInfo;
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;
    uint32_t monsterMovePacketsThisTick_ = 0;
    uint32_t monsterMovePacketsDroppedThisTick_ = 0;

    // Fall/jump tracking
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;

    // Heartbeat timing
    int heartbeatLogCount_ = 0;  // periodic position audit counter
    float timeSinceLastMoveHeartbeat_ = 0.0f;
    float moveHeartbeatInterval_ = 0.5f;
    uint32_t lastHeartbeatSendTimeMs_ = 0;
    float lastHeartbeatX_ = 0.0f;
    float lastHeartbeatY_ = 0.0f;
    float lastHeartbeatZ_ = 0.0f;
    uint32_t lastHeartbeatFlags_ = 0;
    uint64_t lastHeartbeatTransportGuid_ = 0;
    uint32_t lastNonHeartbeatMoveSendTimeMs_ = 0;
    uint32_t lastFacingSendTimeMs_ = 0;
    float lastFacingSentOrientation_ = 0.0f;

    // Speed state
    float serverRunSpeed_ = 7.0f;
    float serverWalkSpeed_ = 2.5f;
    float serverRunBackSpeed_ = 4.5f;
    float serverSwimSpeed_ = 4.722f;
    float serverSwimBackSpeed_ = 2.5f;
    float serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f;
    float serverTurnRate_ = 3.14159f;
    float serverPitchRate_ = 3.14159f;

    // Other-player movement smoothing
    std::unordered_map<uint64_t, uint32_t> otherPlayerMoveTimeMs_;
    std::unordered_map<uint64_t, float>    otherPlayerSmoothedIntervalMs_;

    // --- Taxi / Flight Path state ---
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;
    std::unordered_map<uint32_t, TaxiNode> taxiNodes_;
    std::vector<TaxiPathEdge> taxiPathEdges_;
    std::unordered_map<uint32_t, std::vector<TaxiPathNode>> taxiPathNodes_;
    bool taxiDbcLoaded_ = false;
    bool taxiWindowOpen_ = false;
    ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    bool onTaxiFlight_ = false;
    std::string taxiDestName_;
    bool taxiMountActive_ = false;
    uint32_t taxiMountDisplayId_ = 0;
    bool taxiActivatePending_ = false;
    float taxiActivateTimer_ = 0.0f;
    bool taxiClientActive_ = false;
    float taxiLandingCooldown_ = 0.0f;
    float taxiStartGrace_ = 0.0f;
    size_t taxiClientIndex_ = 0;
    std::vector<glm::vec3> taxiClientPath_;
    float taxiClientSpeed_ = 32.0f;
    float taxiClientSegmentProgress_ = 0.0f;
    bool taxiRecoverPending_ = false;
    uint32_t taxiRecoverMapId_ = 0;
    glm::vec3 taxiRecoverPos_{0.0f};
    uint32_t knownTaxiMask_[12] = {};
    bool taxiMaskInitialized_ = false;
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_;
};

} // namespace game
} // namespace wowee
