#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include "math/spline.hpp"

namespace wowee {
namespace game {

/**
 * Object type IDs for WoW 3.3.5a
 */
enum class ObjectType : uint8_t {
    OBJECT = 0,
    ITEM = 1,
    CONTAINER = 2,
    UNIT = 3,
    PLAYER = 4,
    GAMEOBJECT = 5,
    DYNAMICOBJECT = 6,
    CORPSE = 7
};

/**
 * Object type masks for update packets
 */
enum class TypeMask : uint16_t {
    OBJECT = 0x0001,
    ITEM = 0x0002,
    CONTAINER = 0x0004,
    UNIT = 0x0008,
    PLAYER = 0x0010,
    GAMEOBJECT = 0x0020,
    DYNAMICOBJECT = 0x0040,
    CORPSE = 0x0080
};

/**
 * Update types for SMSG_UPDATE_OBJECT
 */
enum class UpdateType : uint8_t {
    VALUES = 0,              // Partial update (changed fields only)
    MOVEMENT = 1,            // Movement update
    CREATE_OBJECT = 2,       // Create new object (full data)
    CREATE_OBJECT2 = 3,      // Create new object (alternate format)
    OUT_OF_RANGE_OBJECTS = 4, // Objects left view range
    NEAR_OBJECTS = 5         // Objects entered view range
};

/**
 * Base entity class for all game objects
 */
class Entity {
public:
    Entity() = default;
    explicit Entity(uint64_t guid) : guid(guid) {}
    virtual ~Entity() = default;

    // GUID access
    uint64_t getGuid() const { return guid; }
    void setGuid(uint64_t g) { guid = g; }

    // Position
    float getX() const { return x; }
    float getY() const { return y; }
    float getZ() const { return z; }
    float getOrientation() const { return orientation; }
    // Update orientation only, without disrupting an in-progress movement interpolation.
    void setOrientation(float o) { orientation = o; }

    void setPosition(float px, float py, float pz, float o) {
        x = px;
        y = py;
        z = pz;
        orientation = o;
        isMoving_ = false; // Instant position set cancels interpolation
        usePathMode_ = false;
        activeSpline_.reset();
    }

    // Multi-segment path movement (Catmull-Rom spline interpolation)
    void startMoveAlongPath(const std::vector<std::array<float, 3>>& path, float destO, float totalDuration) {
        if (path.empty()) return;
        if (path.size() == 1 || totalDuration <= 0.0f) {
            startMoveTo(path.back()[0], path.back()[1], path.back()[2], destO, totalDuration);
            return;
        }
        // Build cumulative distances for proportional time assignment
        std::vector<float> cumDist(path.size(), 0.0f);
        float totalDist = 0.0f;
        for (size_t i = 1; i < path.size(); i++) {
            float dx = path[i][0] - path[i - 1][0];
            float dy = path[i][1] - path[i - 1][1];
            float dz = path[i][2] - path[i - 1][2];
            totalDist += std::sqrt(dx * dx + dy * dy + dz * dz);
            cumDist[i] = totalDist;
        }
        if (totalDist < 0.001f) {
            startMoveTo(path.back()[0], path.back()[1], path.back()[2], destO, totalDuration);
            return;
        }
        // Build SplineKeys with distance-proportional time
        uint32_t durationMs = static_cast<uint32_t>(totalDuration * 1000.0f);
        std::vector<math::SplineKey> keys(path.size());
        for (size_t i = 0; i < path.size(); i++) {
            float fraction = cumDist[i] / totalDist;
            keys[i].timeMs = static_cast<uint32_t>(fraction * durationMs);
            keys[i].position = {path[i][0], path[i][1], path[i][2]};
        }
        activeSpline_.emplace(std::move(keys), /*timeClosed=*/false);
        splineDurationMs_ = durationMs;

        // Snap position if in overrun phase
        if (isMoving_ && moveElapsed_ >= moveDuration_) {
            x = moveEndX_; y = moveEndY_; z = moveEndZ_;
        }
        moveEndX_ = path.back()[0]; moveEndY_ = path.back()[1]; moveEndZ_ = path.back()[2];
        moveDuration_ = totalDuration;
        moveElapsed_ = 0.0f;
        orientation = destO;
        isMoving_ = true;
        usePathMode_ = true;
        // Velocity for dead-reckoning after path completes
        float fromX = isMoving_ ? moveEndX_ : x;
        float fromY = isMoving_ ? moveEndY_ : y;
        float impliedVX = (path.back()[0] - fromX) / totalDuration;
        float impliedVY = (path.back()[1] - fromY) / totalDuration;
        float impliedVZ = (path.back()[2] - path[0][2]) / totalDuration;
        const float alpha = 0.65f;
        velX_ = alpha * impliedVX + (1.0f - alpha) * velX_;
        velY_ = alpha * impliedVY + (1.0f - alpha) * velY_;
        velZ_ = alpha * impliedVZ + (1.0f - alpha) * velZ_;
    }

    // Movement interpolation (syncs entity position with renderer during movement)
    void startMoveTo(float destX, float destY, float destZ, float destO, float durationSec) {
        usePathMode_ = false;
        activeSpline_.reset();
        if (durationSec <= 0.0f) {
            setPosition(destX, destY, destZ, destO);
            return;
        }
        // If we're in the dead-reckoning overrun phase, snap x/y/z back to the
        // destination before using them as the new start.  The renderer was showing
        // the entity at moveEnd (via getLatest) during overrun, so the new
        // interpolation must start there to avoid a visible teleport.
        if (isMoving_ && moveElapsed_ >= moveDuration_) {
            x = moveEndX_;
            y = moveEndY_;
            z = moveEndZ_;
        }
        // Derive velocity from the displacement this packet implies.
        // Use the previous destination (not current lerped pos) as the "from" so
        // variable network timing doesn't inflate/shrink the implied speed.
        float fromX = isMoving_ ? moveEndX_ : x;
        float fromY = isMoving_ ? moveEndY_ : y;
        float fromZ = isMoving_ ? moveEndZ_ : z;
        float impliedVX = (destX - fromX) / durationSec;
        float impliedVY = (destY - fromY) / durationSec;
        float impliedVZ = (destZ - fromZ) / durationSec;
        // Exponential moving average on velocity — 65% new sample, 35% previous.
        // Smooths out jitter from irregular server update intervals (~200-600ms)
        // without introducing visible lag on direction changes.
        const float alpha = 0.65f;
        velX_ = alpha * impliedVX + (1.0f - alpha) * velX_;
        velY_ = alpha * impliedVY + (1.0f - alpha) * velY_;
        velZ_ = alpha * impliedVZ + (1.0f - alpha) * velZ_;

        moveStartX_ = x; moveStartY_ = y; moveStartZ_ = z;
        moveEndX_ = destX; moveEndY_ = destY; moveEndZ_ = destZ;
        moveDuration_ = durationSec;
        moveElapsed_ = 0.0f;
        orientation = destO;
        isMoving_ = true;
    }

    void updateMovement(float deltaTime) {
        if (!isMoving_) return;
        moveElapsed_ += deltaTime;
        if (moveElapsed_ < moveDuration_) {
            if (usePathMode_ && activeSpline_) {
                // Catmull-Rom spline interpolation
                uint32_t pathTimeMs = static_cast<uint32_t>(moveElapsed_ * 1000.0f);
                if (pathTimeMs >= splineDurationMs_) pathTimeMs = splineDurationMs_ - 1;
                glm::vec3 pos = activeSpline_->evaluatePosition(pathTimeMs);
                x = pos.x;
                y = pos.y;
                z = pos.z;
            } else {
                // Single-segment linear interpolation
                float t = moveElapsed_ / moveDuration_;
                x = moveStartX_ + (moveEndX_ - moveStartX_) * t;
                y = moveStartY_ + (moveEndY_ - moveStartY_) * t;
                z = moveStartZ_ + (moveEndZ_ - moveStartZ_) * t;
            }
        } else {
            // Past the interpolation window: dead-reckon at the smoothed velocity
            // rather than freezing in place. Cap to one extra interval so we don't
            // drift endlessly if the entity stops sending packets.
            float overrun = moveElapsed_ - moveDuration_;
            if (overrun < moveDuration_) {
                x = moveEndX_ + velX_ * overrun;
                y = moveEndY_ + velY_ * overrun;
                z = moveEndZ_ + velZ_ * overrun;
            } else {
                // Two intervals with no update — entity has probably stopped.
                x = moveEndX_; y = moveEndY_; z = moveEndZ_;
                velX_ = 0.0f; velY_ = 0.0f; velZ_ = 0.0f;
                isMoving_ = false;
            }
        }
    }

    bool isEntityMoving() const { return isMoving_; }

    /// True only during the active interpolation phase (before reaching destination).
    /// Unlike isEntityMoving(), this does NOT include the dead-reckoning overrun window,
    /// so animations (Run/Walk) should use this to avoid "running in place" after arrival.
    bool isActivelyMoving() const {
        return isMoving_ && moveElapsed_ < moveDuration_;
    }

    // Returns the latest server-authoritative position: destination if moving, current if not.
    // Unlike getX/Y/Z (which only update via updateMovement), this always reflects the
    // last known server position regardless of distance culling.
    float getLatestX() const { return isMoving_ ? moveEndX_ : x; }
    float getLatestY() const { return isMoving_ ? moveEndY_ : y; }
    float getLatestZ() const { return isMoving_ ? moveEndZ_ : z; }

    // Object type
    ObjectType getType() const { return type; }
    void setType(ObjectType t) { type = t; }

    /// True if this entity is a Unit or Player (both derive from Unit).
    bool isUnit() const { return type == ObjectType::UNIT || type == ObjectType::PLAYER; }

    // Fields (for update values)
    void setField(uint16_t index, uint32_t value) {
        fields[index] = value;
    }

    uint32_t getField(uint16_t index) const {
        auto it = fields.find(index);
        return (it != fields.end()) ? it->second : 0;
    }

    bool hasField(uint16_t index) const {
        return fields.find(index) != fields.end();
    }

    const std::map<uint16_t, uint32_t>& getFields() const {
        return fields;
    }

protected:
    uint64_t guid = 0;
    ObjectType type = ObjectType::OBJECT;

    // Position
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;

    // Update fields (dynamic values)
    std::map<uint16_t, uint32_t> fields;

    // Movement interpolation state
    bool isMoving_ = false;
    bool usePathMode_ = false;
    float moveStartX_ = 0, moveStartY_ = 0, moveStartZ_ = 0;
    float moveEndX_ = 0, moveEndY_ = 0, moveEndZ_ = 0;
    float moveDuration_ = 0;
    float moveElapsed_ = 0;
    float velX_ = 0, velY_ = 0, velZ_ = 0;  // Smoothed velocity for dead reckoning
    // CatmullRom spline for multi-segment path movement (replaces linear pathPoints_/pathSegDists_)
    std::optional<math::CatmullRomSpline> activeSpline_;
    uint32_t splineDurationMs_ = 0;
};

/**
 * Unit entity (NPCs, creatures, players)
 */
class Unit : public Entity {
public:
    Unit() { type = ObjectType::UNIT; }
    explicit Unit(uint64_t guid) : Entity(guid) { type = ObjectType::UNIT; }

    // Name
    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    // Health
    uint32_t getHealth() const { return health; }
    void setHealth(uint32_t h) { health = h; }

    uint32_t getMaxHealth() const { return maxHealth; }
    void setMaxHealth(uint32_t h) { maxHealth = h; }

    // Power (mana/rage/energy) — indexed by power type (0-6)
    uint32_t getPower() const { return powers[powerType < 7 ? powerType : 0]; }
    void setPower(uint32_t p) { powers[powerType < 7 ? powerType : 0] = p; }
    void setPowerByType(uint8_t type, uint32_t p) { if (type < 7) powers[type] = p; }

    uint32_t getMaxPower() const { return maxPowers[powerType < 7 ? powerType : 0]; }
    void setMaxPower(uint32_t p) { maxPowers[powerType < 7 ? powerType : 0] = p; }
    void setMaxPowerByType(uint8_t type, uint32_t p) { if (type < 7) maxPowers[type] = p; }

    uint32_t getPowerByType(uint8_t type) const { return type < 7 ? powers[type] : 0; }
    uint32_t getMaxPowerByType(uint8_t type) const { return type < 7 ? maxPowers[type] : 0; }

    uint8_t getPowerType() const { return powerType; }
    void setPowerType(uint8_t t) { powerType = t; }

    // Level
    uint32_t getLevel() const { return level; }
    void setLevel(uint32_t l) { level = l; }

    // Entry ID (creature template entry)
    uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    // Display ID (model display)
    uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }

    // Mount display ID (UNIT_FIELD_MOUNTDISPLAYID, index 69)
    uint32_t getMountDisplayId() const { return mountDisplayId; }
    void setMountDisplayId(uint32_t id) { mountDisplayId = id; }

    // Unit flags (UNIT_FIELD_FLAGS, index 59)
    uint32_t getUnitFlags() const { return unitFlags; }
    void setUnitFlags(uint32_t f) { unitFlags = f; }

    // Dynamic flags (UNIT_DYNAMIC_FLAGS, index 147)
    uint32_t getDynamicFlags() const { return dynamicFlags; }
    void setDynamicFlags(uint32_t f) { dynamicFlags = f; }

    // NPC flags (UNIT_NPC_FLAGS, index 82)
    uint32_t getNpcFlags() const { return npcFlags; }
    void setNpcFlags(uint32_t f) { npcFlags = f; }

    // NPC emote state (UNIT_NPC_EMOTESTATE) — persistent looping animation for NPCs
    uint32_t getNpcEmoteState() const { return npcEmoteState; }
    void setNpcEmoteState(uint32_t e) { npcEmoteState = e; }

    // Returns true if NPC has interaction flags (gossip/vendor/quest/trainer)
    bool isInteractable() const { return npcFlags != 0; }

    // Faction-based hostility
    uint32_t getFactionTemplate() const { return factionTemplate; }
    void setFactionTemplate(uint32_t f) { factionTemplate = f; }
    bool isHostile() const { return hostile; }
    void setHostile(bool h) { hostile = h; }

protected:
    std::string name;
    uint32_t health = 0;
    uint32_t maxHealth = 0;
    uint32_t powers[7] = {};     // Indexed by power type (0=mana,1=rage,2=focus,3=energy,4=happiness,5=runes,6=runic)
    uint32_t maxPowers[7] = {};  // Max values per power type
    uint8_t powerType = 0;       // Active power type
    uint32_t level = 1;
    uint32_t entry = 0;
    uint32_t displayId = 0;
    uint32_t mountDisplayId = 0;
    uint32_t unitFlags = 0;
    uint32_t dynamicFlags = 0;
    uint32_t npcFlags = 0;
    uint32_t npcEmoteState = 0;
    uint32_t factionTemplate = 0;
    bool hostile = false;
};

/**
 * Player entity
 * Name is inherited from Unit — do NOT redeclare it here or the
 * shadowed field will diverge from Unit::name, causing nameplates
 * and other Unit*-based lookups to read an empty string.
 */
class Player : public Unit {
public:
    Player() { type = ObjectType::PLAYER; }
    explicit Player(uint64_t guid) : Unit(guid) { type = ObjectType::PLAYER; }
};

/**
 * GameObject entity (doors, chests, etc.)
 */
class GameObject : public Entity {
public:
    GameObject() { type = ObjectType::GAMEOBJECT; }
    explicit GameObject(uint64_t guid) : Entity(guid) { type = ObjectType::GAMEOBJECT; }

    const std::string& getName() const { return name; }
    void setName(const std::string& n) { name = n; }

    uint32_t getEntry() const { return entry; }
    void setEntry(uint32_t e) { entry = e; }

    uint32_t getDisplayId() const { return displayId; }
    void setDisplayId(uint32_t id) { displayId = id; }

protected:
    std::string name;
    uint32_t entry = 0;
    uint32_t displayId = 0;
};

/**
 * Entity manager for tracking all entities in view
 */
class EntityManager {
public:
    // Add entity
    void addEntity(uint64_t guid, std::shared_ptr<Entity> entity);

    // Remove entity
    void removeEntity(uint64_t guid);

    // Get entity
    std::shared_ptr<Entity> getEntity(uint64_t guid) const;

    // Check if entity exists
    bool hasEntity(uint64_t guid) const;

    // Get all entities
    const std::unordered_map<uint64_t, std::shared_ptr<Entity>>& getEntities() const {
        return entities;
    }

    // Clear all entities
    void clear() {
        entities.clear();
    }

    // Get entity count
    size_t getEntityCount() const {
        return entities.size();
    }

private:
    // MAIN-THREAD-ONLY: all entity map mutations happen via dispatchQueuedPackets()
    // which runs on the main thread.  Do NOT access from the async network pump thread.
    std::unordered_map<uint64_t, std::shared_ptr<Entity>> entities;
};

} // namespace game
} // namespace wowee
