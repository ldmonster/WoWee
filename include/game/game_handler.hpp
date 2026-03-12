#pragma once

#include "game/world_packets.hpp"
#include "game/character.hpp"
#include "game/opcode_table.hpp"
#include "game/update_field_table.hpp"
#include "game/inventory.hpp"
#include "game/spell_defines.hpp"
#include "game/group_defines.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <functional>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <optional>
#include <algorithm>
#include <chrono>

namespace wowee::game {
    class TransportManager;
    class WardenCrypto;
    class WardenMemory;
    class WardenModule;
    class WardenModuleManager;
    class PacketParsers;
}

namespace wowee {
namespace network { class WorldSocket; class Packet; }

namespace game {

struct PlayerSkill {
    uint32_t skillId = 0;
    uint16_t value = 0;
    uint16_t maxValue = 0;
};

/**
 * Quest giver status values (WoW 3.3.5a)
 */
enum class QuestGiverStatus : uint8_t {
    NONE = 0,
    UNAVAILABLE = 1,
    INCOMPLETE = 5,    // ? (gray)
    REWARD_REP = 6,
    AVAILABLE_LOW = 7, // ! (gray, low-level)
    AVAILABLE = 8,     // ! (yellow)
    REWARD = 10        // ? (yellow)
};

/**
 * A single contact list entry (friend, ignore, or mute).
 */
struct ContactEntry {
    uint64_t    guid     = 0;
    std::string name;
    std::string note;
    uint32_t    flags    = 0;   // 0x1=friend, 0x2=ignore, 0x4=mute
    uint8_t     status   = 0;   // 0=offline, 1=online, 2=AFK, 3=DND
    uint32_t    areaId   = 0;
    uint32_t    level    = 0;
    uint32_t    classId  = 0;

    bool isFriend() const { return (flags & 0x1) != 0; }
    bool isIgnored() const { return (flags & 0x2) != 0; }
    bool isOnline()  const { return status != 0; }
};

/**
 * World connection state
 */
enum class WorldState {
    DISCONNECTED,           // Not connected
    CONNECTING,             // TCP connection in progress
    CONNECTED,              // Connected, waiting for challenge
    CHALLENGE_RECEIVED,     // Received SMSG_AUTH_CHALLENGE
    AUTH_SENT,              // Sent CMSG_AUTH_SESSION, encryption initialized
    AUTHENTICATED,          // Received SMSG_AUTH_RESPONSE success
    READY,                  // Ready for character/world operations
    CHAR_LIST_REQUESTED,    // CMSG_CHAR_ENUM sent
    CHAR_LIST_RECEIVED,     // SMSG_CHAR_ENUM received
    ENTERING_WORLD,         // CMSG_PLAYER_LOGIN sent
    IN_WORLD,               // In game world
    FAILED                  // Connection or authentication failed
};

/**
 * World connection callbacks
 */
using WorldConnectSuccessCallback = std::function<void()>;
using WorldConnectFailureCallback = std::function<void(const std::string& reason)>;

/**
 * GameHandler - Manages world server connection and game protocol
 *
 * Handles:
 * - Connection to world server
 * - Authentication with session key from auth server
 * - RC4 header encryption
 * - Character enumeration
 * - World entry
 * - Game packets
 */
class GameHandler {
public:
    // Talent data structures (must be public for use in templates)
    struct TalentEntry {
        uint32_t talentId = 0;
        uint32_t tabId = 0;           // Which talent tree
        uint8_t row = 0;              // Tier (0-10)
        uint8_t column = 0;           // Column (0-3)
        uint32_t rankSpells[5] = {};  // Spell IDs for ranks 1-5
        uint32_t prereqTalent[3] = {}; // Required talents
        uint8_t prereqRank[3] = {};   // Required ranks
        uint8_t maxRank = 0;          // Number of ranks (1-5)
    };

    struct TalentTabEntry {
        uint32_t tabId = 0;
        std::string name;
        uint32_t classMask = 0;       // Which classes can use this tab
        uint8_t orderIndex = 0;       // Display order (0-2)
        std::string backgroundFile;   // Texture path
    };

    GameHandler();
    ~GameHandler();

    /** Access the active opcode table (wire ↔ logical mapping). */
    const OpcodeTable& getOpcodeTable() const { return opcodeTable_; }
    OpcodeTable& getOpcodeTable() { return opcodeTable_; }
    const UpdateFieldTable& getUpdateFieldTable() const { return updateFieldTable_; }
    UpdateFieldTable& getUpdateFieldTable() { return updateFieldTable_; }
    PacketParsers* getPacketParsers() { return packetParsers_.get(); }
    void setPacketParsers(std::unique_ptr<PacketParsers> parsers);

    /**
     * Connect to world server
     *
     * @param host World server hostname/IP
     * @param port World server port (default 8085)
     * @param sessionKey 40-byte session key from auth server
     * @param accountName Account name (will be uppercased)
     * @param build Client build number (default 12340 for 3.3.5a)
     * @return true if connection initiated
     */
    bool connect(const std::string& host,
                 uint16_t port,
                 const std::vector<uint8_t>& sessionKey,
                 const std::string& accountName,
                 uint32_t build = 12340,
                 uint32_t realmId = 0);

    /**
     * Disconnect from world server
     */
    void disconnect();

    /**
     * Check if connected to world server
     */
    bool isConnected() const;

    /**
     * Get current connection state
     */
    WorldState getState() const { return state; }

    /**
     * Request character list from server
     * Must be called when state is READY or AUTHENTICATED
     */
    void requestCharacterList();

    /**
     * Get list of characters (available after CHAR_LIST_RECEIVED state)
     */
    const std::vector<Character>& getCharacters() const { return characters; }

    void createCharacter(const CharCreateData& data);
    void deleteCharacter(uint64_t characterGuid);

    using CharCreateCallback = std::function<void(bool success, const std::string& message)>;
    void setCharCreateCallback(CharCreateCallback cb) { charCreateCallback_ = std::move(cb); }

    using CharDeleteCallback = std::function<void(bool success)>;
    void setCharDeleteCallback(CharDeleteCallback cb) { charDeleteCallback_ = std::move(cb); }
    uint8_t getLastCharDeleteResult() const { return lastCharDeleteResult_; }

    using CharLoginFailCallback = std::function<void(const std::string& reason)>;
    void setCharLoginFailCallback(CharLoginFailCallback cb) { charLoginFailCallback_ = std::move(cb); }

    /**
     * Select and log in with a character
     * @param characterGuid GUID of character to log in with
     */
    void selectCharacter(uint64_t characterGuid);
    void setActiveCharacterGuid(uint64_t guid) { activeCharacterGuid_ = guid; }
    uint64_t getActiveCharacterGuid() const { return activeCharacterGuid_; }
    const Character* getActiveCharacter() const;
    const Character* getFirstCharacter() const;

    /**
     * Get current player movement info
     */
    const MovementInfo& getMovementInfo() const { return movementInfo; }
    uint32_t getCurrentMapId() const { return currentMapId_; }
    bool getHomeBind(uint32_t& mapId, glm::vec3& pos) const {
        if (!hasHomeBind_) return false;
        mapId = homeBindMapId_;
        pos = homeBindPos_;
        return true;
    }

    /**
     * Send a movement packet
     * @param opcode Movement opcode (MSG_MOVE_START_FORWARD, etc.)
     */
    void sendMovement(Opcode opcode);

    /**
     * Update player position
     * @param x X coordinate
     * @param y Y coordinate
     * @param z Z coordinate
     */
    void setPosition(float x, float y, float z);

    /**
     * Update player orientation
     * @param orientation Facing direction in radians
     */
    void setOrientation(float orientation);

    /**
     * Get entity manager (for accessing entities in view)
     */
    EntityManager& getEntityManager() { return entityManager; }
    const EntityManager& getEntityManager() const { return entityManager; }

    /**
     * Send a chat message
     * @param type Chat type (SAY, YELL, WHISPER, etc.)
     * @param message Message text
     * @param target Target name (for whispers, empty otherwise)
     */
    void sendChatMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid = 0);
    void joinChannel(const std::string& channelName, const std::string& password = "");
    void leaveChannel(const std::string& channelName);
    const std::vector<std::string>& getJoinedChannels() const { return joinedChannels_; }
    std::string getChannelByIndex(int index) const;
    int getChannelIndex(const std::string& channelName) const;

    // Chat auto-join settings (set by UI before autoJoinDefaultChannels)
    struct ChatAutoJoin {
        bool general = true;
        bool trade = true;
        bool localDefense = true;
        bool lfg = true;
        bool local = true;
    };
    ChatAutoJoin chatAutoJoin;

    // Chat bubble callback: (senderGuid, message, isYell)
    using ChatBubbleCallback = std::function<void(uint64_t, const std::string&, bool)>;
    void setChatBubbleCallback(ChatBubbleCallback cb) { chatBubbleCallback_ = std::move(cb); }

    // Emote animation callback: (entityGuid, animationId)
    using EmoteAnimCallback = std::function<void(uint64_t, uint32_t)>;
    void setEmoteAnimCallback(EmoteAnimCallback cb) { emoteAnimCallback_ = std::move(cb); }

    /**
     * Get chat history (recent messages)
     * @param maxMessages Maximum number of messages to return (0 = all)
     * @return Vector of chat messages
     */
    const std::deque<MessageChatData>& getChatHistory() const { return chatHistory; }

    /**
     * Add a locally-generated chat message (e.g., emote feedback)
     */
    void addLocalChatMessage(const MessageChatData& msg);

    // Money (copper)
    uint64_t getMoneyCopper() const { return playerMoneyCopper_; }

    // Server-authoritative armor (UNIT_FIELD_RESISTANCES[0])
    int32_t getArmorRating() const { return playerArmorRating_; }

    // Server-authoritative primary stats (UNIT_FIELD_STAT0-4: STR, AGI, STA, INT, SPI).
    // Returns -1 if the server hasn't sent the value yet.
    int32_t getPlayerStat(int idx) const {
        if (idx < 0 || idx > 4) return -1;
        return playerStats_[idx];
    }

    // Inventory
    Inventory& getInventory() { return inventory; }
    const Inventory& getInventory() const { return inventory; }
    bool consumeOnlineEquipmentDirty() { bool d = onlineEquipDirty_; onlineEquipDirty_ = false; return d; }
    void resetEquipmentDirtyTracking() { lastEquipDisplayIds_ = {}; onlineEquipDirty_ = true; }
    void unequipToBackpack(EquipSlot equipSlot);

    // Targeting
    void setTarget(uint64_t guid);
    void clearTarget();
    uint64_t getTargetGuid() const { return targetGuid; }
    std::shared_ptr<Entity> getTarget() const;
    bool hasTarget() const { return targetGuid != 0; }
    void tabTarget(float playerX, float playerY, float playerZ);

    // Focus targeting
    void setFocus(uint64_t guid);
    void clearFocus();
    uint64_t getFocusGuid() const { return focusGuid; }
    std::shared_ptr<Entity> getFocus() const;
    bool hasFocus() const { return focusGuid != 0; }

    // Advanced targeting
    void targetLastTarget();
    void targetEnemy(bool reverse = false);
    void targetFriend(bool reverse = false);

    // Inspection
    void inspectTarget();

    // Server info commands
    void queryServerTime();
    void requestPlayedTime();
    void queryWho(const std::string& playerName = "");

    // Social commands
    void addFriend(const std::string& playerName, const std::string& note = "");
    void removeFriend(const std::string& playerName);
    void setFriendNote(const std::string& playerName, const std::string& note);
    void addIgnore(const std::string& playerName);
    void removeIgnore(const std::string& playerName);

    // Random roll
    void randomRoll(uint32_t minRoll = 1, uint32_t maxRoll = 100);

    // Battleground queue slot (public so UI can read invite details)
    struct BgQueueSlot {
        uint32_t queueSlot = 0;
        uint32_t bgTypeId = 0;
        uint8_t arenaType = 0;
        uint32_t statusId = 0;  // 0=none, 1=wait_queue, 2=wait_join, 3=in_progress
        uint32_t inviteTimeout = 80;
        std::chrono::steady_clock::time_point inviteReceivedTime{};
    };

    // Battleground
    bool hasPendingBgInvite() const;
    void acceptBattlefield(uint32_t queueSlot = 0xFFFFFFFF);
    void declineBattlefield(uint32_t queueSlot = 0xFFFFFFFF);
    const std::array<BgQueueSlot, 3>& getBgQueues() const { return bgQueues_; }

    // Network latency (milliseconds, updated each PONG response)
    uint32_t getLatencyMs() const { return lastLatency; }

    // Logout commands
    void requestLogout();
    void cancelLogout();

    // Stand state
    void setStandState(uint8_t state);  // 0=stand, 1=sit, 2=sit_chair, 3=sleep, 4=sit_low_chair, 5=sit_medium_chair, 6=sit_high_chair, 7=dead, 8=kneel, 9=submerged
    uint8_t getStandState() const { return standState_; }
    bool isSitting() const { return standState_ >= 1 && standState_ <= 6; }
    bool isDead() const { return standState_ == 7; }
    bool isKneeling() const { return standState_ == 8; }

    // Display toggles
    void toggleHelm();
    void toggleCloak();

    // Follow/Assist
    void followTarget();
    void assistTarget();

    // PvP
    void togglePvp();

    // Guild commands
    void requestGuildInfo();
    void requestGuildRoster();
    void setGuildMotd(const std::string& motd);
    void promoteGuildMember(const std::string& playerName);
    void demoteGuildMember(const std::string& playerName);
    void leaveGuild();
    void inviteToGuild(const std::string& playerName);
    void kickGuildMember(const std::string& playerName);
    void disbandGuild();
    void setGuildLeader(const std::string& name);
    void setGuildPublicNote(const std::string& name, const std::string& note);
    void setGuildOfficerNote(const std::string& name, const std::string& note);
    void acceptGuildInvite();
    void declineGuildInvite();
    void queryGuildInfo(uint32_t guildId);
    void createGuild(const std::string& guildName);
    void addGuildRank(const std::string& rankName);
    void deleteGuildRank();
    void requestPetitionShowlist(uint64_t npcGuid);
    void buyPetition(uint64_t npcGuid, const std::string& guildName);

    // Guild state accessors
    bool isInGuild() const {
        if (!guildName_.empty()) return true;
        const Character* ch = getActiveCharacter();
        return ch && ch->hasGuild();
    }
    const std::string& getGuildName() const { return guildName_; }
    const GuildRosterData& getGuildRoster() const { return guildRoster_; }
    bool hasGuildRoster() const { return hasGuildRoster_; }
    const std::vector<std::string>& getGuildRankNames() const { return guildRankNames_; }
    bool hasPendingGuildInvite() const { return pendingGuildInvite_; }
    const std::string& getPendingGuildInviterName() const { return pendingGuildInviterName_; }
    const std::string& getPendingGuildInviteGuildName() const { return pendingGuildInviteGuildName_; }
    const GuildInfoData& getGuildInfoData() const { return guildInfoData_; }
    const GuildQueryResponseData& getGuildQueryData() const { return guildQueryData_; }
    bool hasGuildInfoData() const { return guildInfoData_.isValid(); }
    bool hasPetitionShowlist() const { return showPetitionDialog_; }
    void clearPetitionDialog() { showPetitionDialog_ = false; }
    uint32_t getPetitionCost() const { return petitionCost_; }
    uint64_t getPetitionNpcGuid() const { return petitionNpcGuid_; }

    // Ready check
    void initiateReadyCheck();
    void respondToReadyCheck(bool ready);
    bool hasPendingReadyCheck() const { return pendingReadyCheck_; }
    void dismissReadyCheck() { pendingReadyCheck_ = false; }
    const std::string& getReadyCheckInitiator() const { return readyCheckInitiator_; }

    // Duel
    void forfeitDuel();

    // AFK/DND status
    void toggleAfk(const std::string& message = "");
    void toggleDnd(const std::string& message = "");
    void replyToLastWhisper(const std::string& message);
    std::string getLastWhisperSender() const { return lastWhisperSender_; }
    void setLastWhisperSender(const std::string& name) { lastWhisperSender_ = name; }

    // Party/Raid management
    void uninvitePlayer(const std::string& playerName);
    void leaveParty();
    void setMainTank(uint64_t targetGuid);
    void setMainAssist(uint64_t targetGuid);
    void clearMainTank();
    void clearMainAssist();
    void requestRaidInfo();

    // Combat and Trade
    void proposeDuel(uint64_t targetGuid);
    void initiateTrade(uint64_t targetGuid);
    void stopCasting();

    // ---- Phase 1: Name queries ----
    void queryPlayerName(uint64_t guid);
    void queryCreatureInfo(uint32_t entry, uint64_t guid);
    void queryGameObjectInfo(uint32_t entry, uint64_t guid);
    const GameObjectQueryResponseData* getCachedGameObjectInfo(uint32_t entry) const {
        auto it = gameObjectInfoCache_.find(entry);
        return (it != gameObjectInfoCache_.end()) ? &it->second : nullptr;
    }
    std::string getCachedPlayerName(uint64_t guid) const;
    std::string getCachedCreatureName(uint32_t entry) const;

    // ---- Phase 2: Combat ----
    void startAutoAttack(uint64_t targetGuid);
    void stopAutoAttack();
    bool isAutoAttacking() const { return autoAttacking; }
    bool hasAutoAttackIntent() const { return autoAttackRequested_; }
    bool isInCombat() const { return autoAttacking || !hostileAttackers_.empty(); }
    bool isInCombatWith(uint64_t guid) const {
        return guid != 0 &&
               ((autoAttacking && autoAttackTarget == guid) ||
                (hostileAttackers_.count(guid) > 0));
    }
    uint64_t getAutoAttackTargetGuid() const { return autoAttackTarget; }
    bool isAggressiveTowardPlayer(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    const std::vector<CombatTextEntry>& getCombatText() const { return combatText; }
    void updateCombatText(float deltaTime);

    // ---- Phase 3: Spells ----
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);
    void cancelCast();
    void cancelAura(uint32_t spellId);
    void dismissPet();
    bool hasPet() const { return petGuid_ != 0; }
    uint64_t getPetGuid() const { return petGuid_; }

    // ---- Pet state (populated by SMSG_PET_SPELLS / SMSG_PET_MODE) ----
    // 10 action bar slots; each entry is a packed uint32:
    //   bits 0-23  = spell ID (or 0 for empty)
    //   bits 24-31 = action type (0x00=cast, 0xC0=autocast on, 0x40=autocast off)
    static constexpr int PET_ACTION_BAR_SLOTS = 10;
    uint32_t getPetActionSlot(int idx) const {
        if (idx < 0 || idx >= PET_ACTION_BAR_SLOTS) return 0;
        return petActionSlots_[idx];
    }
    // Pet command/react state from SMSG_PET_MODE or SMSG_PET_SPELLS
    uint8_t getPetCommand() const { return petCommand_; }   // 0=stay,1=follow,2=attack,3=dismiss
    uint8_t getPetReact()   const { return petReact_; }     // 0=passive,1=defensive,2=aggressive
    // Spells the pet knows (from SMSG_PET_SPELLS spell list)
    const std::vector<uint32_t>& getPetSpells() const { return petSpellList_; }
    // Pet autocast set (spellIds that have autocast enabled)
    bool isPetSpellAutocast(uint32_t spellId) const {
        return petAutocastSpells_.count(spellId) != 0;
    }
    // Send CMSG_PET_ACTION to issue a pet command
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells; }

    // Player proficiency bitmasks (from SMSG_SET_PROFICIENCY)
    // itemClass 2 = Weapon (subClassMask bits: 0=Axe1H,1=Axe2H,2=Bow,3=Gun,4=Mace1H,5=Mace2H,6=Polearm,7=Sword1H,8=Sword2H,10=Staff,13=Fist,14=Misc,15=Dagger,16=Thrown,17=Crossbow,18=Wand,19=Fishing)
    // itemClass 4 = Armor (subClassMask bits: 1=Cloth,2=Leather,3=Mail,4=Plate,6=Shield)
    uint32_t getWeaponProficiency() const { return weaponProficiency_; }
    uint32_t getArmorProficiency()  const { return armorProficiency_; }
    bool canUseWeaponSubclass(uint32_t subClass) const { return (weaponProficiency_ >> subClass) & 1u; }
    bool canUseArmorSubclass(uint32_t subClass)  const { return (armorProficiency_  >> subClass) & 1u; }

    // Minimap pings from party members
    struct MinimapPing {
        uint64_t senderGuid = 0;
        float    wowX       = 0.0f;  // canonical WoW X (north)
        float    wowY       = 0.0f;  // canonical WoW Y (west)
        float    age        = 0.0f;  // seconds since received
        static constexpr float LIFETIME = 5.0f;
        bool isExpired() const { return age >= LIFETIME; }
    };
    const std::vector<MinimapPing>& getMinimapPings() const { return minimapPings_; }
    void tickMinimapPings(float dt) {
        for (auto& p : minimapPings_) p.age += dt;
        minimapPings_.erase(
            std::remove_if(minimapPings_.begin(), minimapPings_.end(),
                           [](const MinimapPing& p){ return p.isExpired(); }),
            minimapPings_.end());
    }

    bool isCasting() const { return casting; }
    bool isGameObjectInteractionCasting() const {
        return casting && currentCastSpellId == 0 && pendingGameObjectInteractGuid_ != 0;
    }
    uint32_t getCurrentCastSpellId() const { return currentCastSpellId; }
    float getCastProgress() const { return castTimeTotal > 0 ? (castTimeTotal - castTimeRemaining) / castTimeTotal : 0.0f; }
    float getCastTimeRemaining() const { return castTimeRemaining; }

    // Unit cast state (tracked per GUID for target frame + boss frames)
    struct UnitCastState {
        bool     casting         = false;
        uint32_t spellId         = 0;
        float    timeRemaining   = 0.0f;
        float    timeTotal       = 0.0f;
    };
    // Returns cast state for any unit by GUID (empty/non-casting if not found)
    const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    // Convenience helpers for the current target
    bool isTargetCasting() const { return getUnitCastState(targetGuid) != nullptr; }
    uint32_t getTargetCastSpellId() const {
        auto* s = getUnitCastState(targetGuid);
        return s ? s->spellId : 0;
    }
    float getTargetCastProgress() const {
        auto* s = getUnitCastState(targetGuid);
        return (s && s->timeTotal > 0.0f)
            ? (s->timeTotal - s->timeRemaining) / s->timeTotal : 0.0f;
    }
    float getTargetCastTimeRemaining() const {
        auto* s = getUnitCastState(targetGuid);
        return s ? s->timeRemaining : 0.0f;
    }

    // Talents
    uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }
    uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }
    void loadTalentDbc();

    // Action bar — 4 bars × 12 slots = 48 total
    // Bar 0 (slots  0-11): main bottom bar (1-0, -, =)
    // Bar 1 (slots 12-23): second bar above main (Shift+1 ... Shift+=)
    // Bar 2 (slots 24-35): right side vertical bar
    // Bar 3 (slots 36-47): left side vertical bar
    static constexpr int SLOTS_PER_BAR    = 12;
    static constexpr int ACTION_BARS      = 4;
    static constexpr int ACTION_BAR_SLOTS = SLOTS_PER_BAR * ACTION_BARS;   // 48
    std::array<ActionBarSlot, ACTION_BAR_SLOTS>& getActionBar() { return actionBar; }
    const std::array<ActionBarSlot, ACTION_BAR_SLOTS>& getActionBar() const { return actionBar; }
    void setActionBarSlot(int slot, ActionBarSlot::Type type, uint32_t id);

    void saveCharacterConfig();
    void loadCharacterConfig();
    static std::string getCharacterConfigDir();

    // Auras
    const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras; }
    const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras; }

    // Completed quests (populated from SMSG_QUERY_QUESTS_COMPLETED_RESPONSE)
    bool isQuestCompleted(uint32_t questId) const { return completedQuests_.count(questId) > 0; }
    const std::unordered_set<uint32_t>& getCompletedQuests() const { return completedQuests_; }

    // NPC death callback (for animations)
    using NpcDeathCallback = std::function<void(uint64_t guid)>;
    void setNpcDeathCallback(NpcDeathCallback cb) { npcDeathCallback_ = std::move(cb); }

    using NpcAggroCallback = std::function<void(uint64_t guid, const glm::vec3& position)>;
    void setNpcAggroCallback(NpcAggroCallback cb) { npcAggroCallback_ = std::move(cb); }

    // NPC respawn callback (health 0 → >0, resets animation to idle)
    using NpcRespawnCallback = std::function<void(uint64_t guid)>;
    void setNpcRespawnCallback(NpcRespawnCallback cb) { npcRespawnCallback_ = std::move(cb); }

    // Stand state animation callback — fired when SMSG_STANDSTATE_UPDATE confirms a new state
    // standState: 0=stand, 1-6=sit variants, 7=dead, 8=kneel
    using StandStateCallback = std::function<void(uint8_t standState)>;
    void setStandStateCallback(StandStateCallback cb) { standStateCallback_ = std::move(cb); }

    // Ghost state callback — fired when player enters or leaves ghost (spirit) form
    using GhostStateCallback = std::function<void(bool isGhost)>;
    void setGhostStateCallback(GhostStateCallback cb) { ghostStateCallback_ = std::move(cb); }

    // Melee swing callback (for driving animation/SFX)
    using MeleeSwingCallback = std::function<void()>;
    void setMeleeSwingCallback(MeleeSwingCallback cb) { meleeSwingCallback_ = std::move(cb); }

    // Spell cast animation callbacks — true=start cast/channel, false=finish/cancel
    // guid: caster (may be player or another unit), isChannel: channel vs regular cast
    using SpellCastAnimCallback = std::function<void(uint64_t guid, bool start, bool isChannel)>;
    void setSpellCastAnimCallback(SpellCastAnimCallback cb) { spellCastAnimCallback_ = std::move(cb); }

    // Unit animation hint: signal jump (animId=38) for other players/NPCs
    using UnitAnimHintCallback = std::function<void(uint64_t guid, uint32_t animId)>;
    void setUnitAnimHintCallback(UnitAnimHintCallback cb) { unitAnimHintCallback_ = std::move(cb); }

    // Unit move-flags callback: fired on every MSG_MOVE_* for other players with the raw flags field.
    // Drives Walk(4) vs Run(5) selection and swim state initialization from heartbeat packets.
    using UnitMoveFlagsCallback = std::function<void(uint64_t guid, uint32_t moveFlags)>;
    void setUnitMoveFlagsCallback(UnitMoveFlagsCallback cb) { unitMoveFlagsCallback_ = std::move(cb); }

    // NPC swing callback (plays attack animation on NPC)
    using NpcSwingCallback = std::function<void(uint64_t guid)>;
    void setNpcSwingCallback(NpcSwingCallback cb) { npcSwingCallback_ = std::move(cb); }

    // NPC greeting callback (plays voice line when NPC is clicked)
    using NpcGreetingCallback = std::function<void(uint64_t guid, const glm::vec3& position)>;
    void setNpcGreetingCallback(NpcGreetingCallback cb) { npcGreetingCallback_ = std::move(cb); }

    using NpcFarewellCallback = std::function<void(uint64_t guid, const glm::vec3& position)>;
    void setNpcFarewellCallback(NpcFarewellCallback cb) { npcFarewellCallback_ = std::move(cb); }

    using NpcVendorCallback = std::function<void(uint64_t guid, const glm::vec3& position)>;
    void setNpcVendorCallback(NpcVendorCallback cb) { npcVendorCallback_ = std::move(cb); }

    // XP tracking
    uint32_t getPlayerXp() const { return playerXp_; }
    uint32_t getPlayerNextLevelXp() const { return playerNextLevelXp_; }
    uint32_t getPlayerRestedXp() const { return playerRestedXp_; }
    bool isPlayerResting() const { return isResting_; }
    uint32_t getPlayerLevel() const { return serverPlayerLevel_; }
    const std::vector<uint32_t>& getPlayerExploredZoneMasks() const { return playerExploredZones_; }
    bool hasPlayerExploredZoneMasks() const { return hasPlayerExploredZones_; }
    static uint32_t killXp(uint32_t playerLevel, uint32_t victimLevel);

    // Server time (for deterministic moon phases, etc.)
    float getGameTime() const { return gameTime_; }
    float getTimeSpeed() const { return timeSpeed_; }

    // Weather state (updated by SMSG_WEATHER)
    // weatherType: 0=clear, 1=rain, 2=snow, 3=storm/fog
    uint32_t getWeatherType() const { return weatherType_; }
    float getWeatherIntensity() const { return weatherIntensity_; }
    bool isRaining() const { return weatherType_ == 1 && weatherIntensity_ > 0.05f; }
    bool isSnowing() const { return weatherType_ == 2 && weatherIntensity_ > 0.05f; }
    uint32_t getOverrideLightId() const { return overrideLightId_; }
    uint32_t getOverrideLightTransMs() const { return overrideLightTransMs_; }

    // Player skills
    const std::map<uint32_t, PlayerSkill>& getPlayerSkills() const { return playerSkills_; }
    const std::string& getSkillName(uint32_t skillId) const;
    uint32_t getSkillCategory(uint32_t skillId) const;

    // World entry callback (online mode - triggered when entering world)
    // Parameters: mapId, x, y, z (canonical WoW coords), isInitialEntry=true on first login or reconnect
    using WorldEntryCallback = std::function<void(uint32_t mapId, float x, float y, float z, bool isInitialEntry)>;
    void setWorldEntryCallback(WorldEntryCallback cb) { worldEntryCallback_ = std::move(cb); }

    // Knockback callback: called when server sends SMSG_MOVE_KNOCK_BACK for the player.
    // Parameters: vcos, vsin (render-space direction), hspeed, vspeed (raw from packet).
    using KnockBackCallback = std::function<void(float vcos, float vsin, float hspeed, float vspeed)>;
    void setKnockBackCallback(KnockBackCallback cb) { knockBackCallback_ = std::move(cb); }

    // Unstuck callback (resets player Z to floor height)
    using UnstuckCallback = std::function<void()>;
    void setUnstuckCallback(UnstuckCallback cb) { unstuckCallback_ = std::move(cb); }
    void unstuck();
    void setUnstuckGyCallback(UnstuckCallback cb) { unstuckGyCallback_ = std::move(cb); }
    void unstuckGy();
    void setUnstuckHearthCallback(UnstuckCallback cb) { unstuckHearthCallback_ = std::move(cb); }
    void unstuckHearth();
    using BindPointCallback = std::function<void(uint32_t mapId, float x, float y, float z)>;
    void setBindPointCallback(BindPointCallback cb) { bindPointCallback_ = std::move(cb); }

    // Called when the player starts casting Hearthstone so terrain at the bind
    // point can be pre-loaded during the cast time.
    // Parameters: mapId and canonical (x, y, z) of the bind location.
    using HearthstonePreloadCallback = std::function<void(uint32_t mapId, float x, float y, float z)>;
    void setHearthstonePreloadCallback(HearthstonePreloadCallback cb) { hearthstonePreloadCallback_ = std::move(cb); }

    // Creature spawn callback (online mode - triggered when creature enters view)
    // Parameters: guid, displayId, x, y, z (canonical), orientation, scale (OBJECT_FIELD_SCALE_X)
    using CreatureSpawnCallback = std::function<void(uint64_t guid, uint32_t displayId, float x, float y, float z, float orientation, float scale)>;
    void setCreatureSpawnCallback(CreatureSpawnCallback cb) { creatureSpawnCallback_ = std::move(cb); }

    // Creature despawn callback (online mode - triggered when creature leaves view)
    using CreatureDespawnCallback = std::function<void(uint64_t guid)>;
    void setCreatureDespawnCallback(CreatureDespawnCallback cb) { creatureDespawnCallback_ = std::move(cb); }

    // Player spawn callback (online mode - triggered when a player enters view).
    // Players need appearance data so the renderer can build the right body/hair textures.
    using PlayerSpawnCallback = std::function<void(uint64_t guid,
                                                   uint32_t displayId,
                                                   uint8_t raceId,
                                                   uint8_t genderId,
                                                   uint32_t appearanceBytes,
                                                   uint8_t facialFeatures,
                                                   float x, float y, float z, float orientation)>;
    void setPlayerSpawnCallback(PlayerSpawnCallback cb) { playerSpawnCallback_ = std::move(cb); }

    using PlayerDespawnCallback = std::function<void(uint64_t guid)>;
    void setPlayerDespawnCallback(PlayerDespawnCallback cb) { playerDespawnCallback_ = std::move(cb); }

    // Online player equipment visuals callback.
    // Sends a best-effort view of equipped items for players in view using ItemDisplayInfo IDs.
    // Arrays are indexed by EquipSlot (0..18). Values are 0 when unknown/unavailable.
    using PlayerEquipmentCallback = std::function<void(uint64_t guid,
                                                      const std::array<uint32_t, 19>& displayInfoIds,
                                                      const std::array<uint8_t, 19>& inventoryTypes)>;
    void setPlayerEquipmentCallback(PlayerEquipmentCallback cb) { playerEquipmentCallback_ = std::move(cb); }

    // GameObject spawn callback (online mode - triggered when gameobject enters view)
    // Parameters: guid, entry, displayId, x, y, z (canonical), orientation, scale (OBJECT_FIELD_SCALE_X)
    using GameObjectSpawnCallback = std::function<void(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation, float scale)>;
    void setGameObjectSpawnCallback(GameObjectSpawnCallback cb) { gameObjectSpawnCallback_ = std::move(cb); }

    // GameObject move callback (online mode - triggered when gameobject position updates)
    // Parameters: guid, x, y, z (canonical), orientation
    using GameObjectMoveCallback = std::function<void(uint64_t guid, float x, float y, float z, float orientation)>;
    void setGameObjectMoveCallback(GameObjectMoveCallback cb) { gameObjectMoveCallback_ = std::move(cb); }

    // GameObject despawn callback (online mode - triggered when gameobject leaves view)
    using GameObjectDespawnCallback = std::function<void(uint64_t guid)>;
    void setGameObjectDespawnCallback(GameObjectDespawnCallback cb) { gameObjectDespawnCallback_ = std::move(cb); }

    using GameObjectCustomAnimCallback = std::function<void(uint64_t guid, uint32_t animId)>;
    void setGameObjectCustomAnimCallback(GameObjectCustomAnimCallback cb) { gameObjectCustomAnimCallback_ = std::move(cb); }

    // Faction hostility map (populated from FactionTemplate.dbc by Application)
    void setFactionHostileMap(std::unordered_map<uint32_t, bool> map) { factionHostileMap_ = std::move(map); }

    // Creature move callback (online mode - triggered by SMSG_MONSTER_MOVE)
    // Parameters: guid, x, y, z (canonical), duration_ms (0 = instant)
    using CreatureMoveCallback = std::function<void(uint64_t guid, float x, float y, float z, uint32_t durationMs)>;
    void setCreatureMoveCallback(CreatureMoveCallback cb) { creatureMoveCallback_ = std::move(cb); }

    // Transport move callback (online mode - triggered when transport position updates)
    // Parameters: guid, x, y, z (canonical), orientation
    using TransportMoveCallback = std::function<void(uint64_t guid, float x, float y, float z, float orientation)>;
    void setTransportMoveCallback(TransportMoveCallback cb) { transportMoveCallback_ = std::move(cb); }

    // Transport spawn callback (online mode - triggered when transport GameObject is first detected)
    // Parameters: guid, entry, displayId, x, y, z (canonical), orientation
    using TransportSpawnCallback = std::function<void(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation)>;
    void setTransportSpawnCallback(TransportSpawnCallback cb) { transportSpawnCallback_ = std::move(cb); }

    // Notify that a transport has been spawned (called after WMO instance creation)
    void notifyTransportSpawned(uint64_t guid, uint32_t entry, uint32_t displayId, float x, float y, float z, float orientation) {
        if (transportSpawnCallback_) {
            transportSpawnCallback_(guid, entry, displayId, x, y, z, orientation);
        }
    }

    // Transport state for player-on-transport
    bool isOnTransport() const { return playerTransportGuid_ != 0; }
    uint64_t getPlayerTransportGuid() const { return playerTransportGuid_; }
    glm::vec3 getPlayerTransportOffset() const { return playerTransportOffset_; }

    // Check if a GUID is a known transport
    bool isTransportGuid(uint64_t guid) const { return transportGuids_.count(guid) > 0; }
    bool hasServerTransportUpdate(uint64_t guid) const { return serverUpdatedTransportGuids_.count(guid) > 0; }
    glm::vec3 getComposedWorldPosition();  // Compose transport transform * local offset
    TransportManager* getTransportManager() { return transportManager_.get(); }
    void setPlayerOnTransport(uint64_t transportGuid, const glm::vec3& localOffset) {
        // Validate transport is registered before attaching player
        // (defer if transport not yet registered to prevent desyncs)
        if (transportGuid != 0 && !isTransportGuid(transportGuid)) {
            return;  // Transport not yet registered; skip attachment
        }
        playerTransportGuid_ = transportGuid;
        playerTransportOffset_ = localOffset;
        playerTransportStickyGuid_ = transportGuid;
        playerTransportStickyTimer_ = 8.0f;
        movementInfo.transportGuid = transportGuid;
    }
    void setPlayerTransportOffset(const glm::vec3& offset) {
        playerTransportOffset_ = offset;
    }
    void clearPlayerTransport() {
        if (playerTransportGuid_ != 0) {
            playerTransportStickyGuid_ = playerTransportGuid_;
            playerTransportStickyTimer_ = std::max(playerTransportStickyTimer_, 1.5f);
        }
        playerTransportGuid_ = 0;
        playerTransportOffset_ = glm::vec3(0.0f);
        movementInfo.transportGuid = 0;
    }

    // Cooldowns
    float getSpellCooldown(uint32_t spellId) const;

    // Player GUID
    uint64_t getPlayerGuid() const { return playerGuid; }

    // Look up a display name for any guid: checks playerNameCache then entity manager.
    // Returns empty string if unknown. Used by chat display to resolve names at render time.
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

    uint8_t getPlayerClass() const {
        const Character* ch = getActiveCharacter();
        return ch ? static_cast<uint8_t>(ch->characterClass) : 0;
    }
    void setPlayerGuid(uint64_t guid) { playerGuid = guid; }

    // Player death state
    bool isPlayerDead() const { return playerDead_; }
    bool isPlayerGhost() const { return releasedSpirit_; }
    bool showDeathDialog() const { return playerDead_ && !releasedSpirit_; }
    bool showResurrectDialog() const { return resurrectRequestPending_; }
    const std::string& getResurrectCasterName() const { return resurrectCasterName_; }
    bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }
    /** True when ghost is within 40 yards of corpse position (same map). */
    bool canReclaimCorpse() const;
    /** Send CMSG_RECLAIM_CORPSE; noop if not a ghost or not near corpse. */
    void reclaimCorpse();
    void releaseSpirit();
    void acceptResurrect();
    void declineResurrect();

    // ---- Phase 4: Group ----
    void inviteToGroup(const std::string& playerName);
    void acceptGroupInvite();
    void declineGroupInvite();
    void leaveGroup();
    bool isInGroup() const { return !partyData.isEmpty(); }
    const GroupListData& getPartyData() const { return partyData; }
    const std::vector<ContactEntry>& getContacts() const { return contacts_; }
    bool hasPendingGroupInvite() const { return pendingGroupInvite; }
    const std::string& getPendingInviterName() const { return pendingInviterName; }

    // ---- Item text (books / readable items) ----
    bool isItemTextOpen() const { return itemTextOpen_; }
    const std::string& getItemText() const { return itemText_; }
    void closeItemText() { itemTextOpen_ = false; }
    void queryItemText(uint64_t itemGuid);

    // ---- Shared Quest ----
    bool hasPendingSharedQuest() const { return pendingSharedQuest_; }
    uint32_t getSharedQuestId() const { return sharedQuestId_; }
    const std::string& getSharedQuestTitle() const { return sharedQuestTitle_; }
    const std::string& getSharedQuestSharerName() const { return sharedQuestSharerName_; }
    void acceptSharedQuest();
    void declineSharedQuest();

    // ---- Summon ----
    bool hasPendingSummonRequest() const { return pendingSummonRequest_; }
    const std::string& getSummonerName() const { return summonerName_; }
    float getSummonTimeoutSec() const { return summonTimeoutSec_; }
    void acceptSummon();
    void declineSummon();
    void tickSummonTimeout(float dt) {
        if (!pendingSummonRequest_) return;
        summonTimeoutSec_ -= dt;
        if (summonTimeoutSec_ <= 0.0f) {
            pendingSummonRequest_ = false;
            summonTimeoutSec_ = 0.0f;
        }
    }

    // ---- Trade ----
    enum class TradeStatus : uint8_t {
        None = 0, PendingIncoming, Open, Accepted, Complete
    };

    static constexpr int TRADE_SLOT_COUNT = 6;  // WoW has 6 normal trade slots + slot 6 for non-trade item

    struct TradeSlot {
        uint32_t itemId      = 0;
        uint32_t displayId   = 0;
        uint32_t stackCount  = 0;
        uint64_t itemGuid    = 0;
        uint8_t  bag         = 0xFF;   // 0xFF = not set
        uint8_t  bagSlot     = 0xFF;
        bool     occupied    = false;
    };

    TradeStatus getTradeStatus() const { return tradeStatus_; }
    bool hasPendingTradeRequest() const { return tradeStatus_ == TradeStatus::PendingIncoming; }
    bool isTradeOpen() const { return tradeStatus_ == TradeStatus::Open || tradeStatus_ == TradeStatus::Accepted; }
    const std::string& getTradePeerName() const { return tradePeerName_; }

    // My trade slots (what I'm offering)
    const std::array<TradeSlot, TRADE_SLOT_COUNT>& getMyTradeSlots() const { return myTradeSlots_; }
    // Peer's trade slots (what they're offering)
    const std::array<TradeSlot, TRADE_SLOT_COUNT>& getPeerTradeSlots() const { return peerTradeSlots_; }
    uint64_t getMyTradeGold() const { return myTradeGold_; }
    uint64_t getPeerTradeGold() const { return peerTradeGold_; }

    void acceptTradeRequest();   // respond to incoming SMSG_TRADE_STATUS(1) with CMSG_BEGIN_TRADE
    void declineTradeRequest();  // respond with CMSG_CANCEL_TRADE
    void acceptTrade();          // lock in offer: CMSG_ACCEPT_TRADE
    void cancelTrade();          // CMSG_CANCEL_TRADE
    void setTradeItem(uint8_t tradeSlot, uint8_t bag, uint8_t bagSlot);
    void clearTradeItem(uint8_t tradeSlot);
    void setTradeGold(uint64_t copper);

    // ---- Duel ----
    bool hasPendingDuelRequest() const { return pendingDuelRequest_; }
    const std::string& getDuelChallengerName() const { return duelChallengerName_; }
    void acceptDuel();
    // forfeitDuel() already declared at line ~399

    // ---- Instance lockouts ----
    struct InstanceLockout {
        uint32_t mapId       = 0;
        uint32_t difficulty  = 0;  // 0=normal,1=heroic/10man,2=25man,3=25man heroic
        uint64_t resetTime   = 0;  // Unix timestamp of instance reset
        bool     locked      = false;
        bool     extended    = false;
    };
    const std::vector<InstanceLockout>& getInstanceLockouts() const { return instanceLockouts_; }

    // Boss encounter unit tracking (SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT)
    static constexpr uint32_t kMaxEncounterSlots = 5;
    // Returns boss unit guid for the given encounter slot (0 if none)
    uint64_t getEncounterUnitGuid(uint32_t slot) const {
        return (slot < kMaxEncounterSlots) ? encounterUnitGuids_[slot] : 0;
    }

    // Raid target markers (MSG_RAID_TARGET_UPDATE)
    // Icon indices 0-7: Star, Circle, Diamond, Triangle, Moon, Square, Cross, Skull
    static constexpr uint32_t kRaidMarkCount = 8;
    // Returns the GUID marked with the given icon (0 = no mark)
    uint64_t getRaidMarkGuid(uint32_t icon) const {
        return (icon < kRaidMarkCount) ? raidTargetGuids_[icon] : 0;
    }
    // Returns the raid mark icon for a given guid (0xFF = no mark)
    uint8_t getEntityRaidMark(uint64_t guid) const {
        if (guid == 0) return 0xFF;
        for (uint32_t i = 0; i < kRaidMarkCount; ++i)
            if (raidTargetGuids_[i] == guid) return static_cast<uint8_t>(i);
        return 0xFF;
    }

    // ---- LFG / Dungeon Finder ----
    enum class LfgState : uint8_t {
        None           = 0,
        RoleCheck      = 1,
        Queued         = 2,
        Proposal       = 3,
        Boot           = 4,
        InDungeon      = 5,
        FinishedDungeon= 6,
        RaidBrowser    = 7,
    };

    // roles bitmask: 0x02=tank, 0x04=healer, 0x08=dps; pass LFGDungeonEntry ID
    void lfgJoin(uint32_t dungeonId, uint8_t roles);
    void lfgLeave();
    void lfgAcceptProposal(uint32_t proposalId, bool accept);
    void lfgSetBootVote(bool vote);
    void lfgTeleport(bool toLfgDungeon = true);
    LfgState getLfgState() const { return lfgState_; }
    bool isLfgQueued()    const { return lfgState_ == LfgState::Queued; }
    bool isLfgInDungeon() const { return lfgState_ == LfgState::InDungeon; }
    uint32_t getLfgDungeonId()   const { return lfgDungeonId_; }
    uint32_t getLfgProposalId()  const { return lfgProposalId_; }
    int32_t  getLfgAvgWaitSec()  const { return lfgAvgWaitSec_; }
    uint32_t getLfgTimeInQueueMs() const { return lfgTimeInQueueMs_; }

    // ---- Phase 5: Loot ----
    void lootTarget(uint64_t guid);
    void lootItem(uint8_t slotIndex);
    void closeLoot();
    void activateSpiritHealer(uint64_t npcGuid);
    bool isLootWindowOpen() const { return lootWindowOpen; }
    const LootResponseData& getCurrentLoot() const { return currentLoot; }
    void setAutoLoot(bool enabled) { autoLoot_ = enabled; }
    bool isAutoLoot() const { return autoLoot_; }

    // Group loot roll
    struct LootRollEntry {
        uint64_t objectGuid    = 0;
        uint32_t slot          = 0;
        uint32_t itemId        = 0;
        std::string itemName;
        uint8_t  itemQuality   = 0;
    };
    bool hasPendingLootRoll() const { return pendingLootRollActive_; }
    const LootRollEntry& getPendingLootRoll() const { return pendingLootRoll_; }
    void sendLootRoll(uint64_t objectGuid, uint32_t slot, uint8_t rollType);
    // rollType: 0=need, 1=greed, 2=disenchant, 96=pass

    // NPC Gossip
    void interactWithNpc(uint64_t guid);
    void interactWithGameObject(uint64_t guid);
    void selectGossipOption(uint32_t optionId);
    void selectGossipQuest(uint32_t questId);
    void acceptQuest();
    void declineQuest();
    void closeGossip();
    bool isGossipWindowOpen() const { return gossipWindowOpen; }
    const GossipMessageData& getCurrentGossip() const { return currentGossip; }
    bool isQuestDetailsOpen() {
        // Check if delayed opening timer has expired
        if (questDetailsOpen) return true;
        if (questDetailsOpenTime != std::chrono::steady_clock::time_point{}) {
            if (std::chrono::steady_clock::now() >= questDetailsOpenTime) {
                questDetailsOpen = true;
                questDetailsOpenTime = std::chrono::steady_clock::time_point{};
                return true;
            }
        }
        return false;
    }
    const QuestDetailsData& getQuestDetails() const { return currentQuestDetails; }

    // Gossip / quest map POI markers (SMSG_GOSSIP_POI)
    struct GossipPoi {
        float    x     = 0.0f;   // WoW canonical X (north)
        float    y     = 0.0f;   // WoW canonical Y (west)
        uint32_t icon  = 0;      // POI icon type
        uint32_t data  = 0;
        std::string name;
    };
    const std::vector<GossipPoi>& getGossipPois() const { return gossipPois_; }
    void clearGossipPois() { gossipPois_.clear(); }

    // Quest turn-in
    bool isQuestRequestItemsOpen() const { return questRequestItemsOpen_; }
    const QuestRequestItemsData& getQuestRequestItems() const { return currentQuestRequestItems_; }
    void completeQuest();       // Send CMSG_QUESTGIVER_COMPLETE_QUEST
    void closeQuestRequestItems();

    bool isQuestOfferRewardOpen() const { return questOfferRewardOpen_; }
    const QuestOfferRewardData& getQuestOfferReward() const { return currentQuestOfferReward_; }
    void chooseQuestReward(uint32_t rewardIndex);  // Send CMSG_QUESTGIVER_CHOOSE_REWARD
    void closeQuestOfferReward();

    // Quest log
    struct QuestLogEntry {
        uint32_t questId = 0;
        std::string title;
        std::string objectives;
        bool complete = false;
        // Objective kill counts: npcOrGoEntry -> (current, required)
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> killCounts;
        // Quest item progress: itemId -> current count
        std::unordered_map<uint32_t, uint32_t> itemCounts;
        // Server-authoritative quest item requirements from REQUEST_ITEMS
        std::unordered_map<uint32_t, uint32_t> requiredItemCounts;
        // Structured kill objectives parsed from SMSG_QUEST_QUERY_RESPONSE.
        // Index 0-3 map to the server's objective slot order (packed into update fields).
        // npcOrGoId != 0 => entity objective (kill NPC or interact with GO).
        struct KillObjective {
            int32_t npcOrGoId = 0;  // negative = game-object entry
            uint32_t required = 0;
        };
        std::array<KillObjective, 4> killObjectives{};  // zeroed by default
        // Required item objectives parsed from SMSG_QUEST_QUERY_RESPONSE.
        // itemId != 0 => collect items of that type.
        struct ItemObjective {
            uint32_t itemId = 0;
            uint32_t required = 0;
        };
        std::array<ItemObjective, 6> itemObjectives{};  // zeroed by default
    };
    const std::vector<QuestLogEntry>& getQuestLog() const { return questLog_; }
    void abandonQuest(uint32_t questId);
    bool requestQuestQuery(uint32_t questId, bool force = false);
    bool isQuestTracked(uint32_t questId) const { return trackedQuestIds_.count(questId) > 0; }
    void setQuestTracked(uint32_t questId, bool tracked) {
        if (tracked) trackedQuestIds_.insert(questId);
        else trackedQuestIds_.erase(questId);
    }
    const std::unordered_set<uint32_t>& getTrackedQuestIds() const { return trackedQuestIds_; }
    bool isQuestQueryPending(uint32_t questId) const {
        return pendingQuestQueryIds_.count(questId) > 0;
    }
    void clearQuestQueryPending(uint32_t questId) { pendingQuestQueryIds_.erase(questId); }
    const std::unordered_map<uint32_t, uint32_t>& getWorldStates() const { return worldStates_; }
    std::optional<uint32_t> getWorldState(uint32_t key) const {
        auto it = worldStates_.find(key);
        if (it == worldStates_.end()) return std::nullopt;
        return it->second;
    }
    uint32_t getWorldStateMapId() const { return worldStateMapId_; }
    uint32_t getWorldStateZoneId() const { return worldStateZoneId_; }

    // Mirror timers (0=fatigue, 1=breath, 2=feigndeath)
    struct MirrorTimer {
        int32_t value    = 0;
        int32_t maxValue = 0;
        int32_t scale    = 0;     // +1 = counting up, -1 = counting down
        bool    paused   = false;
        bool    active   = false;
    };
    const MirrorTimer& getMirrorTimer(int type) const {
        static MirrorTimer empty;
        return (type >= 0 && type < 3) ? mirrorTimers_[type] : empty;
    }

    // Combo points
    uint8_t  getComboPoints() const { return comboPoints_; }
    uint64_t getComboTarget() const { return comboTarget_; }

    // Death Knight rune state (6 runes: 0-1=Blood, 2-3=Unholy, 4-5=Frost; may become Death=3)
    enum class RuneType : uint8_t { Blood = 0, Unholy = 1, Frost = 2, Death = 3 };
    struct RuneSlot {
        RuneType type = RuneType::Blood;
        bool     ready = true;          // Server-confirmed ready state
        float    readyFraction = 1.0f;  // 0.0=depleted → 1.0=full (from server sync)
    };
    const std::array<RuneSlot, 6>& getPlayerRunes() const { return playerRunes_; }

    struct FactionStandingInit {
        uint8_t flags = 0;
        int32_t standing = 0;
    };
    const std::vector<FactionStandingInit>& getInitialFactions() const { return initialFactions_; }
    const std::unordered_map<uint32_t, int32_t>& getFactionStandings() const { return factionStandings_; }
    const std::string& getFactionNamePublic(uint32_t factionId) const;
    uint32_t getLastContactListMask() const { return lastContactListMask_; }
    uint32_t getLastContactListCount() const { return lastContactListCount_; }
    bool isServerMovementAllowed() const { return serverMovementAllowed_; }

    // Quest giver status (! and ? markers)
    QuestGiverStatus getQuestGiverStatus(uint64_t guid) const {
        auto it = npcQuestStatus_.find(guid);
        return (it != npcQuestStatus_.end()) ? it->second : QuestGiverStatus::NONE;
    }
    const std::unordered_map<uint64_t, QuestGiverStatus>& getNpcQuestStatuses() const { return npcQuestStatus_; }

    // Charge callback — fires when player casts a charge spell toward target
    // Parameters: targetGuid, targetX, targetY, targetZ (canonical WoW coordinates)
    using ChargeCallback = std::function<void(uint64_t targetGuid, float x, float y, float z)>;
    void setChargeCallback(ChargeCallback cb) { chargeCallback_ = std::move(cb); }

    // Level-up callback — fires when the player gains a level (newLevel > 1)
    using LevelUpCallback = std::function<void(uint32_t newLevel)>;
    void setLevelUpCallback(LevelUpCallback cb) { levelUpCallback_ = std::move(cb); }

    // Other player level-up callback — fires when another player gains a level
    using OtherPlayerLevelUpCallback = std::function<void(uint64_t guid, uint32_t newLevel)>;
    void setOtherPlayerLevelUpCallback(OtherPlayerLevelUpCallback cb) { otherPlayerLevelUpCallback_ = std::move(cb); }

    // Achievement earned callback — fires when SMSG_ACHIEVEMENT_EARNED is received
    using AchievementEarnedCallback = std::function<void(uint32_t achievementId, const std::string& name)>;
    void setAchievementEarnedCallback(AchievementEarnedCallback cb) { achievementEarnedCallback_ = std::move(cb); }
    const std::unordered_set<uint32_t>& getEarnedAchievements() const { return earnedAchievements_; }

    // Server-triggered music callback — fires when SMSG_PLAY_MUSIC is received.
    // The soundId corresponds to a SoundEntries.dbc record. The receiver is
    // responsible for looking up the file path and forwarding to MusicManager.
    using PlayMusicCallback = std::function<void(uint32_t soundId)>;
    void setPlayMusicCallback(PlayMusicCallback cb) { playMusicCallback_ = std::move(cb); }

    // Server-triggered 2-D sound effect callback — fires when SMSG_PLAY_SOUND is received.
    // The soundId corresponds to a SoundEntries.dbc record.
    using PlaySoundCallback = std::function<void(uint32_t soundId)>;
    void setPlaySoundCallback(PlaySoundCallback cb) { playSoundCallback_ = std::move(cb); }

    // Server-triggered 3-D positional sound callback — fires for SMSG_PLAY_OBJECT_SOUND and
    // SMSG_PLAY_SPELL_IMPACT. Includes sourceGuid so the receiver can look up world position.
    using PlayPositionalSoundCallback = std::function<void(uint32_t soundId, uint64_t sourceGuid)>;
    void setPlayPositionalSoundCallback(PlayPositionalSoundCallback cb) { playPositionalSoundCallback_ = std::move(cb); }

    // Mount state
    using MountCallback = std::function<void(uint32_t mountDisplayId)>;  // 0 = dismount
    void setMountCallback(MountCallback cb) { mountCallback_ = std::move(cb); }

    // Taxi terrain precaching callback
    using TaxiPrecacheCallback = std::function<void(const std::vector<glm::vec3>&)>;
    void setTaxiPrecacheCallback(TaxiPrecacheCallback cb) { taxiPrecacheCallback_ = std::move(cb); }

    // Taxi orientation callback (for mount rotation: yaw, pitch, roll in radians)
    using TaxiOrientationCallback = std::function<void(float yaw, float pitch, float roll)>;
    void setTaxiOrientationCallback(TaxiOrientationCallback cb) { taxiOrientationCallback_ = std::move(cb); }

    // Callback for when taxi flight is about to start (after mounting delay, before movement begins)
    using TaxiFlightStartCallback = std::function<void()>;
    void setTaxiFlightStartCallback(TaxiFlightStartCallback cb) { taxiFlightStartCallback_ = std::move(cb); }

    bool isMounted() const { return currentMountDisplayId_ != 0; }
    bool isHostileAttacker(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    float getServerRunSpeed() const { return serverRunSpeed_; }
    float getServerWalkSpeed() const { return serverWalkSpeed_; }
    float getServerSwimSpeed() const { return serverSwimSpeed_; }
    float getServerSwimBackSpeed() const { return serverSwimBackSpeed_; }
    float getServerFlightSpeed() const { return serverFlightSpeed_; }
    float getServerFlightBackSpeed() const { return serverFlightBackSpeed_; }
    float getServerRunBackSpeed() const { return serverRunBackSpeed_; }
    float getServerTurnRate() const { return serverTurnRate_; }
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
    // Set the character pitch angle (radians) for movement packets (flight / swimming).
    // Positive = nose up, negative = nose down.
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Taxi / Flight Paths
    bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();
    void activateTaxi(uint32_t destNodeId);
    bool isOnTaxiFlight() const { return onTaxiFlight_; }
    bool isTaxiMountActive() const { return taxiMountActive_; }
    bool isTaxiActivationPending() const { return taxiActivatePending_; }
    void forceClearTaxiAndMovementState();
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
    uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    // Vendor
    void openVendor(uint64_t npcGuid);
    void closeVendor();
    void buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count);
    void sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count);
    void sellItemBySlot(int backpackIndex);
    void sellItemInBag(int bagIndex, int slotIndex);
    struct BuybackItem {
        uint64_t itemGuid = 0;
        ItemDef item;
        uint32_t count = 1;
    };
    void buyBackItem(uint32_t buybackSlot);
    void repairItem(uint64_t vendorGuid, uint64_t itemGuid);
    void repairAll(uint64_t vendorGuid, bool useGuildBank = false);
    const std::deque<BuybackItem>& getBuybackItems() const { return buybackItems_; }
    void autoEquipItemBySlot(int backpackIndex);
    void autoEquipItemInBag(int bagIndex, int slotIndex);
    void useItemBySlot(int backpackIndex);
    void useItemInBag(int bagIndex, int slotIndex);
    void destroyItem(uint8_t bag, uint8_t slot, uint8_t count = 1);
    void swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag, uint8_t dstSlot);
    void swapBagSlots(int srcBagIndex, int dstBagIndex);
    void useItemById(uint32_t itemId);
    bool isVendorWindowOpen() const { return vendorWindowOpen; }
    const ListInventoryData& getVendorItems() const { return currentVendorItems; }
    void setVendorCanRepair(bool v) { currentVendorItems.canRepair = v; }

    // Mail
    bool isMailboxOpen() const { return mailboxOpen_; }
    const std::vector<MailMessage>& getMailInbox() const { return mailInbox_; }
    int getSelectedMailIndex() const { return selectedMailIndex_; }
    void setSelectedMailIndex(int idx) { selectedMailIndex_ = idx; }
    bool isMailComposeOpen() const { return showMailCompose_; }
    void openMailCompose() { showMailCompose_ = true; clearMailAttachments(); }
    void closeMailCompose() { showMailCompose_ = false; clearMailAttachments(); }
    bool hasNewMail() const { return hasNewMail_; }
    void closeMailbox();
    void sendMail(const std::string& recipient, const std::string& subject,
                  const std::string& body, uint32_t money, uint32_t cod = 0);

    // Mail attachments (max 12 per WotLK)
    static constexpr int MAIL_MAX_ATTACHMENTS = 12;
    struct MailAttachSlot {
        uint64_t itemGuid = 0;
        game::ItemDef item;
        uint8_t srcBag = 0xFF;   // source container for return
        uint8_t srcSlot = 0;
        bool occupied() const { return itemGuid != 0; }
    };
    bool attachItemFromBackpack(int backpackIndex);
    bool attachItemFromBag(int bagIndex, int slotIndex);
    bool detachMailAttachment(int attachIndex);
    void clearMailAttachments();
    const std::array<MailAttachSlot, 12>& getMailAttachments() const { return mailAttachments_; }
    int getMailAttachmentCount() const;
    void mailTakeMoney(uint32_t mailId);
    void mailTakeItem(uint32_t mailId, uint32_t itemIndex);
    void mailDelete(uint32_t mailId);
    void mailMarkAsRead(uint32_t mailId);
    void refreshMailList();

    // Bank
    void openBank(uint64_t guid);
    void closeBank();
    void buyBankSlot();
    void depositItem(uint8_t srcBag, uint8_t srcSlot);
    void withdrawItem(uint8_t srcBag, uint8_t srcSlot);
    bool isBankOpen() const { return bankOpen_; }
    uint64_t getBankerGuid() const { return bankerGuid_; }
    int getEffectiveBankSlots() const { return effectiveBankSlots_; }
    int getEffectiveBankBagSlots() const { return effectiveBankBagSlots_; }

    // Guild Bank
    void openGuildBank(uint64_t guid);
    void closeGuildBank();
    void queryGuildBankTab(uint8_t tabId);
    void buyGuildBankTab();
    void depositGuildBankMoney(uint32_t amount);
    void withdrawGuildBankMoney(uint32_t amount);
    void guildBankWithdrawItem(uint8_t tabId, uint8_t bankSlot, uint8_t destBag, uint8_t destSlot);
    void guildBankDepositItem(uint8_t tabId, uint8_t bankSlot, uint8_t srcBag, uint8_t srcSlot);
    bool isGuildBankOpen() const { return guildBankOpen_; }
    const GuildBankData& getGuildBankData() const { return guildBankData_; }
    uint8_t getGuildBankActiveTab() const { return guildBankActiveTab_; }
    void setGuildBankActiveTab(uint8_t tab) { guildBankActiveTab_ = tab; }

    // Auction House
    void openAuctionHouse(uint64_t guid);
    void closeAuctionHouse();
    void auctionSearch(const std::string& name, uint8_t levelMin, uint8_t levelMax,
                       uint32_t quality, uint32_t itemClass, uint32_t itemSubClass,
                       uint32_t invTypeMask, uint8_t usableOnly, uint32_t offset = 0);
    void auctionSellItem(uint64_t itemGuid, uint32_t stackCount, uint32_t bid,
                         uint32_t buyout, uint32_t duration);
    void auctionPlaceBid(uint32_t auctionId, uint32_t amount);
    void auctionBuyout(uint32_t auctionId, uint32_t buyoutPrice);
    void auctionCancelItem(uint32_t auctionId);
    void auctionListOwnerItems(uint32_t offset = 0);
    void auctionListBidderItems(uint32_t offset = 0);
    bool isAuctionHouseOpen() const { return auctionOpen_; }
    uint64_t getAuctioneerGuid() const { return auctioneerGuid_; }
    const AuctionListResult& getAuctionBrowseResults() const { return auctionBrowseResults_; }
    const AuctionListResult& getAuctionOwnerResults() const { return auctionOwnerResults_; }
    const AuctionListResult& getAuctionBidderResults() const { return auctionBidderResults_; }
    int getAuctionActiveTab() const { return auctionActiveTab_; }
    void setAuctionActiveTab(int tab) { auctionActiveTab_ = tab; }
    float getAuctionSearchDelay() const { return auctionSearchDelayTimer_; }

    // Trainer
    bool isTrainerWindowOpen() const { return trainerWindowOpen_; }
    const TrainerListData& getTrainerSpells() const { return currentTrainerList_; }
    void trainSpell(uint32_t spellId);
    void closeTrainer();
    const std::string& getSpellName(uint32_t spellId) const;
    const std::string& getSpellRank(uint32_t spellId) const;
    const std::string& getSkillLineName(uint32_t spellId) const;

    struct TrainerTab {
        std::string name;
        std::vector<const TrainerSpell*> spells;
    };
    const std::vector<TrainerTab>& getTrainerTabs() const { return trainerTabs_; }
    const ItemQueryResponseData* getItemInfo(uint32_t itemId) const {
        auto it = itemInfoCache_.find(itemId);
        return (it != itemInfoCache_.end()) ? &it->second : nullptr;
    }
    // Request item info from server if not already cached/pending
    void ensureItemInfo(uint32_t entry) {
        if (entry == 0 || itemInfoCache_.count(entry) || pendingItemQueries_.count(entry)) return;
        queryItemInfo(entry, 0);
    }
    uint64_t getBackpackItemGuid(int index) const {
        if (index < 0 || index >= static_cast<int>(backpackSlotGuids_.size())) return 0;
        return backpackSlotGuids_[index];
    }
    uint64_t getVendorGuid() const { return currentVendorItems.vendorGuid; }

    /**
     * Set callbacks
     */
    void setOnSuccess(WorldConnectSuccessCallback callback) { onSuccess = callback; }
    void setOnFailure(WorldConnectFailureCallback callback) { onFailure = callback; }

    /**
     * Update - call regularly (e.g., each frame)
     *
     * @param deltaTime Time since last update in seconds
     */
    void update(float deltaTime);

    /**
     * Reset DBC-backed caches so they reload from new expansion data.
     * Called by Application when the expansion profile changes.
     */
    void resetDbcCaches();

private:
    void autoTargetAttacker(uint64_t attackerGuid);

    /**
     * Handle incoming packet from world server
     */
    void handlePacket(network::Packet& packet);

    /**
     * Handle SMSG_AUTH_CHALLENGE from server
     */
    void handleAuthChallenge(network::Packet& packet);

    /**
     * Handle SMSG_AUTH_RESPONSE from server
     */
    void handleAuthResponse(network::Packet& packet);

    /**
     * Handle SMSG_CHAR_ENUM from server
     */
    void handleCharEnum(network::Packet& packet);

    /**
     * Handle SMSG_CHARACTER_LOGIN_FAILED from server
     */
    void handleCharLoginFailed(network::Packet& packet);

    /**
     * Handle SMSG_LOGIN_VERIFY_WORLD from server
     */
    void handleLoginVerifyWorld(network::Packet& packet);

    /**
     * Handle SMSG_CLIENTCACHE_VERSION from server
     */
    void handleClientCacheVersion(network::Packet& packet);

    /**
     * Handle SMSG_TUTORIAL_FLAGS from server
     */
    void handleTutorialFlags(network::Packet& packet);

    /**
     * Handle SMSG_WARDEN_DATA gate packet from server.
     * We do not implement anti-cheat exchange for third-party realms.
     */
    void handleWardenData(network::Packet& packet);

    /**
     * Handle SMSG_ACCOUNT_DATA_TIMES from server
     */
    void handleAccountDataTimes(network::Packet& packet);

    /**
     * Handle SMSG_MOTD from server
     */
    void handleMotd(network::Packet& packet);

    /** Handle SMSG_NOTIFICATION (vanilla/classic server notification string) */
    void handleNotification(network::Packet& packet);

    /**
     * Handle SMSG_PONG from server
     */
    void handlePong(network::Packet& packet);

    /**
     * Handle SMSG_UPDATE_OBJECT from server
     */
    void handleUpdateObject(network::Packet& packet);

    /**
     * Handle SMSG_COMPRESSED_UPDATE_OBJECT from server
     */
    void handleCompressedUpdateObject(network::Packet& packet);

    /**
     * Handle SMSG_DESTROY_OBJECT from server
     */
    void handleDestroyObject(network::Packet& packet);

    /**
     * Handle SMSG_MESSAGECHAT from server
     */
    void handleMessageChat(network::Packet& packet);
    void handleTextEmote(network::Packet& packet);
    void handleChannelNotify(network::Packet& packet);
    void autoJoinDefaultChannels();

    // ---- Phase 1 handlers ----
    void handleNameQueryResponse(network::Packet& packet);
    void handleCreatureQueryResponse(network::Packet& packet);
    void handleGameObjectQueryResponse(network::Packet& packet);
    void handleGameObjectPageText(network::Packet& packet);
    void handlePageTextQueryResponse(network::Packet& packet);
    void handleItemQueryResponse(network::Packet& packet);
    void handleInspectResults(network::Packet& packet);
    void queryItemInfo(uint32_t entry, uint64_t guid);
    void rebuildOnlineInventory();
    void maybeDetectVisibleItemLayout();
    void updateOtherPlayerVisibleItems(uint64_t guid, const std::map<uint16_t, uint32_t>& fields);
    void emitOtherPlayerEquipment(uint64_t guid);
    void emitAllOtherPlayerEquipment();
    void detectInventorySlotBases(const std::map<uint16_t, uint32_t>& fields);
    bool applyInventoryFields(const std::map<uint16_t, uint32_t>& fields);
    void extractContainerFields(uint64_t containerGuid, const std::map<uint16_t, uint32_t>& fields);
    uint64_t resolveOnlineItemGuid(uint32_t itemId) const;

    // ---- Phase 2 handlers ----
    void handleAttackStart(network::Packet& packet);
    void handleAttackStop(network::Packet& packet);
    void handleAttackerStateUpdate(network::Packet& packet);
    void handleSpellDamageLog(network::Packet& packet);
    void handleSpellHealLog(network::Packet& packet);

    // ---- Equipment set handler ----
    void handleEquipmentSetList(network::Packet& packet);
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);
    void handleSetForcedReactions(network::Packet& packet);

    // ---- Phase 3 handlers ----
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);

    // ---- Talent handlers ----
    void handleTalentsInfo(network::Packet& packet);

    // ---- Phase 4 handlers ----
    void handleGroupInvite(network::Packet& packet);
    void handleGroupDecline(network::Packet& packet);
    void handleGroupList(network::Packet& packet);
    void handleGroupUninvite(network::Packet& packet);
    void handlePartyCommandResult(network::Packet& packet);
    void handlePartyMemberStats(network::Packet& packet, bool isFull);

    // ---- Guild handlers ----
    void handleGuildInfo(network::Packet& packet);
    void handleGuildRoster(network::Packet& packet);
    void handleGuildQueryResponse(network::Packet& packet);
    void handleGuildEvent(network::Packet& packet);
    void handleGuildInvite(network::Packet& packet);
    void handleGuildCommandResult(network::Packet& packet);
    void handlePetitionShowlist(network::Packet& packet);
    void handlePetSpells(network::Packet& packet);
    void handleTurnInPetitionResults(network::Packet& packet);

    // ---- Character creation handler ----
    void handleCharCreateResponse(network::Packet& packet);

    // ---- XP handler ----
    void handleXpGain(network::Packet& packet);

    // ---- Creature movement handler ----
    void handleMonsterMove(network::Packet& packet);
    void handleCompressedMoves(network::Packet& packet);
    void handleMonsterMoveTransport(network::Packet& packet);

    // ---- Other player movement (MSG_MOVE_* from server) ----
    void handleOtherPlayerMovement(network::Packet& packet);

    // ---- Phase 5 handlers ----
    void handleLootResponse(network::Packet& packet);
    void handleLootReleaseResponse(network::Packet& packet);
    void handleLootRemoved(network::Packet& packet);
    void handleGossipMessage(network::Packet& packet);
    void handleQuestgiverQuestList(network::Packet& packet);
    void handleGossipComplete(network::Packet& packet);
    void handleQuestPoiQueryResponse(network::Packet& packet);
    void handleQuestDetails(network::Packet& packet);
    void handleQuestRequestItems(network::Packet& packet);
    void handleQuestOfferReward(network::Packet& packet);
    void clearPendingQuestAccept(uint32_t questId);
    void triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason);
    bool hasQuestInLog(uint32_t questId) const;
    int findQuestLogSlotIndexFromServer(uint32_t questId) const;
    void addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives);
    bool resyncQuestLogFromServerSlots(bool forceQueryMetadata);
    void handleListInventory(network::Packet& packet);
    void addMoneyCopper(uint32_t amount);

    // ---- Teleport handler ----
    void handleTeleportAck(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);

    // ---- Movement ACK handlers ----
    void handleForceRunSpeedChange(network::Packet& packet);
    void handleForceSpeedChange(network::Packet& packet, const char* name, Opcode ackOpcode, float* speedStorage);
    void handleForceMoveRootState(network::Packet& packet, bool rooted);
    void handleForceMoveFlagChange(network::Packet& packet, const char* name, Opcode ackOpcode, uint32_t flag, bool set);
    void handleMoveSetCollisionHeight(network::Packet& packet);
    void handleMoveKnockBack(network::Packet& packet);

    // ---- Area trigger detection ----
    void loadAreaTriggerDbc();
    void checkAreaTriggers();

    // ---- Instance lockout handler ----
    void handleRaidInstanceInfo(network::Packet& packet);
    void handleItemTextQueryResponse(network::Packet& packet);
    void handleQuestConfirmAccept(network::Packet& packet);
    void handleSummonRequest(network::Packet& packet);
    void handleTradeStatus(network::Packet& packet);
    void handleTradeStatusExtended(network::Packet& packet);
    void resetTradeState();
    void handleDuelRequested(network::Packet& packet);
    void handleDuelComplete(network::Packet& packet);
    void handleDuelWinner(network::Packet& packet);
    void handleLootRoll(network::Packet& packet);
    void handleLootRollWon(network::Packet& packet);

    // ---- LFG / Dungeon Finder handlers ----
    void handleLfgJoinResult(network::Packet& packet);
    void handleLfgQueueStatus(network::Packet& packet);
    void handleLfgProposalUpdate(network::Packet& packet);
    void handleLfgRoleCheckUpdate(network::Packet& packet);
    void handleLfgUpdatePlayer(network::Packet& packet);
    void handleLfgPlayerReward(network::Packet& packet);
    void handleLfgBootProposalUpdate(network::Packet& packet);
    void handleLfgTeleportDenied(network::Packet& packet);

    // ---- Arena / Battleground handlers ----
    void handleBattlefieldStatus(network::Packet& packet);
    void handleInstanceDifficulty(network::Packet& packet);
    void handleArenaTeamCommandResult(network::Packet& packet);
    void handleArenaTeamQueryResponse(network::Packet& packet);
    void handleArenaTeamInvite(network::Packet& packet);
    void handleArenaTeamEvent(network::Packet& packet);
    void handleArenaError(network::Packet& packet);

    // ---- Bank handlers ----
    void handleShowBank(network::Packet& packet);
    void handleBuyBankSlotResult(network::Packet& packet);

    // ---- Guild Bank handlers ----
    void handleGuildBankList(network::Packet& packet);

    // ---- Auction House handlers ----
    void handleAuctionHello(network::Packet& packet);
    void handleAuctionListResult(network::Packet& packet);
    void handleAuctionOwnerListResult(network::Packet& packet);
    void handleAuctionBidderListResult(network::Packet& packet);
    void handleAuctionCommandResult(network::Packet& packet);

    // ---- Mail handlers ----
    void handleShowMailbox(network::Packet& packet);
    void handleMailListResult(network::Packet& packet);
    void handleSendMailResult(network::Packet& packet);
    void handleReceivedMail(network::Packet& packet);
    void handleQueryNextMailTime(network::Packet& packet);

    // ---- Taxi handlers ----
    void handleShowTaxiNodes(network::Packet& packet);
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // ---- Server info handlers ----
    void handleQueryTimeResponse(network::Packet& packet);
    void handlePlayedTime(network::Packet& packet);
    void handleWho(network::Packet& packet);

    // ---- Social handlers ----
    void handleFriendList(network::Packet& packet);   // Classic SMSG_FRIEND_LIST
    void handleContactList(network::Packet& packet);  // WotLK SMSG_CONTACT_LIST (full parse)
    void handleFriendStatus(network::Packet& packet);
    void handleRandomRoll(network::Packet& packet);

    // ---- Logout handlers ----
    void handleLogoutResponse(network::Packet& packet);
    void handleLogoutComplete(network::Packet& packet);

    void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource);
    void addSystemChatMessage(const std::string& message);

    /**
     * Send CMSG_PING to server (heartbeat)
     */
    void sendPing();

    /**
     * Send CMSG_AUTH_SESSION to server
     */
    void sendAuthSession();

    /**
     * Generate random client seed
     */
    uint32_t generateClientSeed();

    /**
     * Change state with logging
     */
    void setState(WorldState newState);

    /**
     * Fail connection with reason
     */
    void fail(const std::string& reason);
    void updateAttachedTransportChildren(float deltaTime);
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation);
    void clearTransportAttachment(uint64_t childGuid);

    // Opcode translation table (expansion-specific wire ↔ logical mapping)
    OpcodeTable opcodeTable_;

    // Update field table (expansion-specific field index mapping)
    UpdateFieldTable updateFieldTable_;

    // Packet parsers (expansion-specific binary format handling)
    std::unique_ptr<PacketParsers> packetParsers_;

    // Network
    std::unique_ptr<network::WorldSocket> socket;

    // State
    WorldState state = WorldState::DISCONNECTED;

    // Authentication data
    std::vector<uint8_t> sessionKey;    // 40-byte session key from auth server
    std::string accountName;             // Account name
    uint32_t build = 12340;              // Client build (3.3.5a)
    uint32_t realmId_ = 0;               // Realm ID from auth REALM_LIST (used in WotLK AUTH_SESSION)
    uint32_t clientSeed = 0;             // Random seed generated by client
    uint32_t serverSeed = 0;             // Seed from SMSG_AUTH_CHALLENGE

    // Characters
    std::vector<Character> characters;       // Character list from SMSG_CHAR_ENUM

    // Movement
    MovementInfo movementInfo;               // Current player movement state
    uint32_t movementTime = 0;               // Movement timestamp counter
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;

    // Fall/jump tracking for movement packet correctness.
    // fallTime must be the elapsed ms since the FALLING flag was set; the server
    // uses it for fall-damage calculations and anti-cheat validation.
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;  // movementInfo.time value when FALLING started

    // Inventory
    Inventory inventory;

    // Entity tracking
    EntityManager entityManager;             // Manages all entities in view

    // Chat
    std::deque<MessageChatData> chatHistory;    // Recent chat messages
    size_t maxChatHistory = 100;             // Maximum chat messages to keep
    std::vector<std::string> joinedChannels_;   // Active channel memberships
    ChatBubbleCallback chatBubbleCallback_;
    EmoteAnimCallback emoteAnimCallback_;

    // Targeting
    uint64_t targetGuid = 0;
    uint64_t focusGuid = 0;              // Focus target
    uint64_t lastTargetGuid = 0;         // Previous target
    std::vector<uint64_t> tabCycleList;
    int tabCycleIndex = -1;
    bool tabCycleStale = true;

    // Heartbeat
    uint32_t pingSequence = 0;               // Ping sequence number (increments)
    float timeSinceLastPing = 0.0f;          // Time since last ping sent (seconds)
    float pingInterval = 30.0f;              // Ping interval (30 seconds)
    float timeSinceLastMoveHeartbeat_ = 0.0f; // Periodic movement heartbeat to keep server position synced
    float moveHeartbeatInterval_ = 0.5f;
    uint32_t lastLatency = 0;                // Last measured latency (milliseconds)

    // Player GUID and map
    uint64_t playerGuid = 0;
    uint32_t currentMapId_ = 0;
    bool hasHomeBind_ = false;
    uint32_t homeBindMapId_ = 0;
    glm::vec3 homeBindPos_{0.0f};

    // ---- Phase 1: Name caches ----
    std::unordered_map<uint64_t, std::string> playerNameCache;
    std::unordered_set<uint64_t> pendingNameQueries;
    std::unordered_map<uint32_t, CreatureQueryResponseData> creatureInfoCache;
    std::unordered_set<uint32_t> pendingCreatureQueries;
    std::unordered_map<uint32_t, GameObjectQueryResponseData> gameObjectInfoCache_;
    std::unordered_set<uint32_t> pendingGameObjectQueries_;

    // ---- Friend/contact list cache ----
    std::unordered_map<std::string, uint64_t> friendsCache;  // name -> guid
    std::unordered_set<uint64_t> friendGuids_;               // all known friend GUIDs (for name backfill)
    uint32_t lastContactListMask_ = 0;
    uint32_t lastContactListCount_ = 0;
    std::vector<ContactEntry> contacts_;                     // structured contact list (friends + ignores)

    // ---- World state and faction initialization snapshots ----
    uint32_t worldStateMapId_ = 0;
    uint32_t worldStateZoneId_ = 0;
    std::unordered_map<uint32_t, uint32_t> worldStates_;
    std::vector<FactionStandingInit> initialFactions_;

    // ---- Ignore list cache ----
    std::unordered_map<std::string, uint64_t> ignoreCache;  // name -> guid

    // ---- Logout state ----
    bool loggingOut_ = false;

    // ---- Display state ----
    bool helmVisible_ = true;
    bool cloakVisible_ = true;
    uint8_t standState_ = 0;  // 0=stand, 1=sit, ..., 7=dead, 8=kneel (server-confirmed)

    // ---- Follow state ----
    uint64_t followTargetGuid_ = 0;

    // ---- AFK/DND status ----
    bool afkStatus_ = false;
    bool dndStatus_ = false;
    std::string afkMessage_;
    std::string dndMessage_;
    std::string lastWhisperSender_;

    // ---- Online item tracking ----
    struct OnlineItemInfo {
        uint32_t entry = 0;
        uint32_t stackCount = 1;
        uint32_t curDurability = 0;
        uint32_t maxDurability = 0;
    };
    std::unordered_map<uint64_t, OnlineItemInfo> onlineItems_;
    std::unordered_map<uint32_t, ItemQueryResponseData> itemInfoCache_;
    std::unordered_set<uint32_t> pendingItemQueries_;
    std::array<uint64_t, 23> equipSlotGuids_{};
    std::array<uint64_t, 16> backpackSlotGuids_{};
    // Container (bag) contents: containerGuid -> array of item GUIDs per slot
    struct ContainerInfo {
        uint32_t numSlots = 0;
        std::array<uint64_t, 36> slotGuids{};  // max 36 slots
    };
    std::unordered_map<uint64_t, ContainerInfo> containerContents_;
    int invSlotBase_ = -1;
    int packSlotBase_ = -1;
    std::map<uint16_t, uint32_t> lastPlayerFields_;
    bool onlineEquipDirty_ = false;
    std::array<uint32_t, 19> lastEquipDisplayIds_{};

    // Visible equipment for other players: detect the update-field layout (base + stride)
    // using the local player's own equipped items, then decode other players by index.
    // Default to known WotLK 3.3.5a layout: UNIT_END(148) + 0x0088 = 284, stride 2.
    // The heuristic in maybeDetectVisibleItemLayout() can still override if needed.
    int visibleItemEntryBase_ = 284;
    int visibleItemStride_ = 2;
    bool visibleItemLayoutVerified_ = false;  // true once heuristic confirms/overrides default
    std::unordered_map<uint64_t, std::array<uint32_t, 19>> otherPlayerVisibleItemEntries_;
    std::unordered_set<uint64_t> otherPlayerVisibleDirty_;
    std::unordered_map<uint64_t, uint32_t> otherPlayerMoveTimeMs_;
    std::unordered_map<uint64_t, float>    otherPlayerSmoothedIntervalMs_;  // EMA of packet intervals

    // Inspect fallback (when visible item fields are missing/unreliable)
    std::unordered_map<uint64_t, std::array<uint32_t, 19>> inspectedPlayerItemEntries_;
    std::unordered_set<uint64_t> pendingAutoInspect_;
    float inspectRateLimit_ = 0.0f;

    // ---- Phase 2: Combat ----
    bool autoAttacking = false;
    bool autoAttackRequested_ = false;   // local intent (CMSG_ATTACKSWING sent)
    uint64_t autoAttackTarget = 0;
    bool autoAttackOutOfRange_ = false;
    float autoAttackOutOfRangeTime_ = 0.0f;
    float autoAttackRangeWarnCooldown_ = 0.0f;
    float autoAttackResendTimer_ = 0.0f;  // Re-send CMSG_ATTACKSWING every ~1s while attacking
    float autoAttackFacingSyncTimer_ = 0.0f; // Periodic facing sync while meleeing
    std::unordered_set<uint64_t> hostileAttackers_;
    std::vector<CombatTextEntry> combatText;

    // ---- Phase 3: Spells ----
    WorldEntryCallback worldEntryCallback_;
    KnockBackCallback knockBackCallback_;
    UnstuckCallback unstuckCallback_;
    UnstuckCallback unstuckGyCallback_;
    UnstuckCallback unstuckHearthCallback_;
    BindPointCallback bindPointCallback_;
    HearthstonePreloadCallback hearthstonePreloadCallback_;
    CreatureSpawnCallback creatureSpawnCallback_;
    CreatureDespawnCallback creatureDespawnCallback_;
    PlayerSpawnCallback playerSpawnCallback_;
    PlayerDespawnCallback playerDespawnCallback_;
    PlayerEquipmentCallback playerEquipmentCallback_;
    CreatureMoveCallback creatureMoveCallback_;
    TransportMoveCallback transportMoveCallback_;
    TransportSpawnCallback transportSpawnCallback_;
    GameObjectSpawnCallback gameObjectSpawnCallback_;
    GameObjectMoveCallback gameObjectMoveCallback_;
    GameObjectDespawnCallback gameObjectDespawnCallback_;
    GameObjectCustomAnimCallback gameObjectCustomAnimCallback_;

    // Transport tracking
    struct TransportAttachment {
        ObjectType type = ObjectType::OBJECT;
        uint64_t transportGuid = 0;
        glm::vec3 localOffset{0.0f};
        float localOrientation = 0.0f;
        bool hasLocalOrientation = false;
    };
    std::unordered_map<uint64_t, TransportAttachment> transportAttachments_;
    std::unordered_set<uint64_t> transportGuids_;  // GUIDs of known transport GameObjects
    std::unordered_set<uint64_t> serverUpdatedTransportGuids_;
    uint64_t playerTransportGuid_ = 0;             // Transport the player is riding (0 = none)
    glm::vec3 playerTransportOffset_ = glm::vec3(0.0f); // Player offset on transport
    uint64_t playerTransportStickyGuid_ = 0;       // Last transport player was on (temporary retention)
    float playerTransportStickyTimer_ = 0.0f;      // Seconds to keep sticky transport alive after transient clears
    std::unique_ptr<TransportManager> transportManager_;  // Transport movement manager
    std::unordered_set<uint32_t> knownSpells;
    std::unordered_map<uint32_t, float> spellCooldowns;    // spellId -> remaining seconds
    uint32_t weaponProficiency_ = 0;  // bitmask from SMSG_SET_PROFICIENCY itemClass=2
    uint32_t armorProficiency_  = 0;  // bitmask from SMSG_SET_PROFICIENCY itemClass=4
    std::vector<MinimapPing> minimapPings_;
    uint8_t castCount = 0;
    bool casting = false;
    uint32_t currentCastSpellId = 0;
    float castTimeRemaining = 0.0f;
    // Per-unit cast state (keyed by GUID, populated from SMSG_SPELL_START)
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;
    uint64_t pendingGameObjectInteractGuid_ = 0;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;                              // Currently active spec (0 or 1)
    uint8_t unspentTalentPoints_[2] = {0, 0};                   // Unspent points per spec
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];  // Learned talents per spec
    std::unordered_map<uint32_t, TalentEntry> talentCache_;      // talentId -> entry
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_; // tabId -> entry
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;                           // Reset on world entry; guards first-spec selection

    // ---- Area trigger detection ----
    struct AreaTriggerEntry {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;   // canonical WoW coords (converted from DBC)
        float radius = 0;
        float boxLength = 0, boxWidth = 0, boxHeight = 0;
        float boxYaw = 0;
    };
    bool areaTriggerDbcLoaded_ = false;
    std::vector<AreaTriggerEntry> areaTriggers_;
    std::unordered_set<uint32_t> activeAreaTriggers_;  // triggers player is currently inside
    float areaTriggerCheckTimer_ = 0.0f;
    bool areaTriggerSuppressFirst_ = false;  // suppress first check after map transfer

    float castTimeTotal = 0.0f;
    std::array<ActionBarSlot, ACTION_BAR_SLOTS> actionBar{};
    std::vector<AuraSlot> playerAuras;
    std::vector<AuraSlot> targetAuras;
    uint64_t petGuid_ = 0;
    uint32_t petActionSlots_[10] = {};   // SMSG_PET_SPELLS action bar (10 slots)
    uint8_t  petCommand_ = 1;            // 0=stay,1=follow,2=attack,3=dismiss
    uint8_t  petReact_   = 1;            // 0=passive,1=defensive,2=aggressive
    std::vector<uint32_t> petSpellList_; // known pet spells
    std::unordered_set<uint32_t> petAutocastSpells_;  // spells with autocast on

    // ---- Battleground queue state ----
    std::array<BgQueueSlot, 3> bgQueues_{};

    // Instance difficulty
    uint32_t instanceDifficulty_ = 0;
    bool instanceIsHeroic_ = false;

    // Raid target markers (icon 0-7 -> guid; 0 = empty slot)
    std::array<uint64_t, kRaidMarkCount> raidTargetGuids_ = {};

    // Mirror timers (0=fatigue, 1=breath, 2=feigndeath)
    MirrorTimer mirrorTimers_[3];

    // Combo points (rogues/druids)
    uint8_t  comboPoints_ = 0;
    uint64_t comboTarget_ = 0;

    // Instance / raid lockouts
    std::vector<InstanceLockout> instanceLockouts_;

    // Instance encounter boss units (slots 0-4 from SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT)
    std::array<uint64_t, kMaxEncounterSlots> encounterUnitGuids_ = {};  // 0 = empty slot

    // LFG / Dungeon Finder state
    LfgState lfgState_        = LfgState::None;
    uint32_t lfgDungeonId_    = 0;   // current dungeon entry
    uint32_t lfgProposalId_   = 0;   // pending proposal id (0 = none)
    int32_t  lfgAvgWaitSec_   = -1;  // estimated wait, -1=unknown
    uint32_t lfgTimeInQueueMs_= 0;   // ms already in queue

    // Ready check state
    bool        pendingReadyCheck_       = false;
    uint32_t    readyCheckReadyCount_    = 0;
    uint32_t    readyCheckNotReadyCount_ = 0;
    std::string readyCheckInitiator_;

    // Faction standings (factionId → absolute standing value)
    std::unordered_map<uint32_t, int32_t> factionStandings_;
    // Faction name cache (factionId → name), populated lazily from Faction.dbc
    std::unordered_map<uint32_t, std::string> factionNameCache_;
    bool factionNameCacheLoaded_ = false;
    void loadFactionNameCache();
    std::string getFactionName(uint32_t factionId) const;

    // ---- Phase 4: Group ----
    GroupListData partyData;
    bool pendingGroupInvite = false;
    std::string pendingInviterName;

    // Item text state
    bool        itemTextOpen_   = false;
    std::string itemText_;

    // Shared quest state
    bool        pendingSharedQuest_       = false;
    uint32_t    sharedQuestId_            = 0;
    std::string sharedQuestTitle_;
    std::string sharedQuestSharerName_;
    uint64_t    sharedQuestSharerGuid_    = 0;

    // Summon state
    bool        pendingSummonRequest_ = false;
    uint64_t    summonerGuid_         = 0;
    std::string summonerName_;
    float       summonTimeoutSec_     = 0.0f;

    // Trade state
    TradeStatus tradeStatus_  = TradeStatus::None;
    uint64_t    tradePeerGuid_= 0;
    std::string tradePeerName_;
    std::array<TradeSlot, TRADE_SLOT_COUNT> myTradeSlots_{};
    std::array<TradeSlot, TRADE_SLOT_COUNT> peerTradeSlots_{};
    uint64_t myTradeGold_   = 0;
    uint64_t peerTradeGold_ = 0;

    // Duel state
    bool pendingDuelRequest_    = false;
    uint64_t duelChallengerGuid_= 0;
    uint64_t duelFlagGuid_      = 0;
    std::string duelChallengerName_;

    // ---- Guild state ----
    std::string guildName_;
    std::vector<std::string> guildRankNames_;
    GuildRosterData guildRoster_;
    GuildInfoData guildInfoData_;
    GuildQueryResponseData guildQueryData_;
    bool hasGuildRoster_ = false;
    bool pendingGuildInvite_ = false;
    std::string pendingGuildInviterName_;
    std::string pendingGuildInviteGuildName_;
    bool showPetitionDialog_ = false;
    uint32_t petitionCost_ = 0;
    uint64_t petitionNpcGuid_ = 0;

    uint64_t activeCharacterGuid_ = 0;
    Race playerRace_ = Race::HUMAN;

    // ---- Phase 5: Loot ----
    bool lootWindowOpen = false;
    bool autoLoot_ = false;
    LootResponseData currentLoot;

    // Group loot roll state
    bool          pendingLootRollActive_ = false;
    LootRollEntry pendingLootRoll_;
    struct LocalLootState {
        LootResponseData data;
        bool moneyTaken = false;
    };
    std::unordered_map<uint64_t, LocalLootState> localLootState_;
    struct PendingLootRetry {
        uint64_t guid = 0;
        float timer = 0.0f;
        uint8_t remainingRetries = 0;
        bool sendLoot = false;
    };
    std::vector<PendingLootRetry> pendingGameObjectLootRetries_;
    struct PendingLootOpen {
        uint64_t guid = 0;
        float timer = 0.0f;
    };
    std::vector<PendingLootOpen> pendingGameObjectLootOpens_;
    uint64_t pendingLootMoneyGuid_ = 0;
    uint32_t pendingLootMoneyAmount_ = 0;
    float pendingLootMoneyNotifyTimer_ = 0.0f;
    std::unordered_map<uint64_t, float> recentLootMoneyAnnounceCooldowns_;
    uint64_t playerMoneyCopper_ = 0;
    int32_t playerArmorRating_ = 0;
    // Server-authoritative primary stats: [0]=STR [1]=AGI [2]=STA [3]=INT [4]=SPI; -1 = not received yet
    int32_t playerStats_[5] = {-1, -1, -1, -1, -1};
    // Some servers/custom clients shift update field indices. We can auto-detect coinage by correlating
    // money-notify deltas with update-field diffs and then overriding UF::PLAYER_FIELD_COINAGE at runtime.
    uint32_t pendingMoneyDelta_ = 0;
    float pendingMoneyDeltaTimer_ = 0.0f;

    // Gossip
    bool gossipWindowOpen = false;
    GossipMessageData currentGossip;
    std::vector<GossipPoi> gossipPois_;

    void performGameObjectInteractionNow(uint64_t guid);

    // Quest details
    bool questDetailsOpen = false;
    std::chrono::steady_clock::time_point questDetailsOpenTime{};  // Delayed opening to allow item data to load
    QuestDetailsData currentQuestDetails;

    // Quest turn-in
    bool questRequestItemsOpen_ = false;
    QuestRequestItemsData currentQuestRequestItems_;
    uint32_t pendingTurnInQuestId_ = 0;
    uint64_t pendingTurnInNpcGuid_ = 0;
    bool pendingTurnInRewardRequest_ = false;
    std::unordered_map<uint32_t, float> pendingQuestAcceptTimeouts_;
    std::unordered_map<uint32_t, uint64_t> pendingQuestAcceptNpcGuids_;
    bool questOfferRewardOpen_ = false;
    QuestOfferRewardData currentQuestOfferReward_;

    // Quest log
    std::vector<QuestLogEntry> questLog_;
    std::unordered_set<uint32_t> pendingQuestQueryIds_;
    std::unordered_set<uint32_t> trackedQuestIds_;
    bool pendingLoginQuestResync_ = false;
    float pendingLoginQuestResyncTimeout_ = 0.0f;

    // Quest giver status per NPC
    std::unordered_map<uint64_t, QuestGiverStatus> npcQuestStatus_;

    // Faction hostility lookup (populated from FactionTemplate.dbc)
    std::unordered_map<uint32_t, bool> factionHostileMap_;
    bool isHostileFaction(uint32_t factionTemplateId) const {
        auto it = factionHostileMap_.find(factionTemplateId);
        return it != factionHostileMap_.end() ? it->second : true; // default hostile if unknown
    }

    // Taxi / Flight Paths
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;  // guid -> has new/available routes
    std::unordered_map<uint32_t, TaxiNode> taxiNodes_;
    std::vector<TaxiPathEdge> taxiPathEdges_;
    std::unordered_map<uint32_t, std::vector<TaxiPathNode>> taxiPathNodes_;  // pathId -> ordered waypoints
    bool taxiDbcLoaded_ = false;
    bool taxiWindowOpen_ = false;
    ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    bool onTaxiFlight_ = false;
    bool taxiMountActive_ = false;
    uint32_t taxiMountDisplayId_ = 0;
    bool taxiActivatePending_ = false;
    float taxiActivateTimer_ = 0.0f;
    bool taxiClientActive_ = false;
    float taxiLandingCooldown_ = 0.0f;  // Prevent re-entering taxi right after landing
    float taxiStartGrace_ = 0.0f;       // Ignore transient landing/dismount checks right after takeoff
    size_t taxiClientIndex_ = 0;
    std::vector<glm::vec3> taxiClientPath_;
    float taxiClientSpeed_ = 32.0f;
    float taxiClientSegmentProgress_ = 0.0f;
    bool taxiRecoverPending_ = false;
    uint32_t taxiRecoverMapId_ = 0;
    glm::vec3 taxiRecoverPos_{0.0f};
    uint32_t knownTaxiMask_[12] = {};  // Track previously known nodes for discovery alerts
    bool taxiMaskInitialized_ = false; // First SMSG_SHOWTAXINODES seeds mask without alerts
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_; // destNodeId -> total cost in copper
    void buildTaxiCostMap();
    void applyTaxiMountForCurrentNode();
    uint32_t nextMovementTimestampMs();
    void sanitizeMovementForTaxi();
    void startClientTaxiPath(const std::vector<uint32_t>& pathNodes);
    void updateClientTaxi(float deltaTime);

    // Mail
    bool mailboxOpen_ = false;
    uint64_t mailboxGuid_ = 0;
    std::vector<MailMessage> mailInbox_;
    int selectedMailIndex_ = -1;
    bool showMailCompose_ = false;
    bool hasNewMail_ = false;
    std::array<MailAttachSlot, MAIL_MAX_ATTACHMENTS> mailAttachments_{};

    // Bank
    bool bankOpen_ = false;
    uint64_t bankerGuid_ = 0;
    std::array<uint64_t, 28> bankSlotGuids_{};
    std::array<uint64_t, 7> bankBagSlotGuids_{};
    int effectiveBankSlots_ = 28;     // 24 for Classic, 28 for TBC/WotLK
    int effectiveBankBagSlots_ = 7;   // 6 for Classic, 7 for TBC/WotLK

    // Guild Bank
    bool guildBankOpen_ = false;
    uint64_t guildBankerGuid_ = 0;
    GuildBankData guildBankData_;
    uint8_t guildBankActiveTab_ = 0;

    // Auction House
    bool auctionOpen_ = false;
    uint64_t auctioneerGuid_ = 0;
    uint32_t auctionHouseId_ = 0;
    AuctionListResult auctionBrowseResults_;
    AuctionListResult auctionOwnerResults_;
    AuctionListResult auctionBidderResults_;
    int auctionActiveTab_ = 0;  // 0=Browse, 1=Bids, 2=Auctions
    float auctionSearchDelayTimer_ = 0.0f;
    // Last search params for re-query (pagination, auto-refresh after bid/buyout)
    struct AuctionSearchParams {
        std::string name;
        uint8_t levelMin = 0, levelMax = 0;
        uint32_t quality = 0xFFFFFFFF;
        uint32_t itemClass = 0xFFFFFFFF;
        uint32_t itemSubClass = 0xFFFFFFFF;
        uint32_t invTypeMask = 0;
        uint8_t usableOnly = 0;
        uint32_t offset = 0;
    };
    AuctionSearchParams lastAuctionSearch_;
    // Routing: which result vector to populate from next SMSG_AUCTION_LIST_RESULT
    enum class AuctionResultTarget { BROWSE, OWNER, BIDDER };
    AuctionResultTarget pendingAuctionTarget_ = AuctionResultTarget::BROWSE;

    // Vendor
    bool vendorWindowOpen = false;
    ListInventoryData currentVendorItems;
    std::deque<BuybackItem> buybackItems_;
    std::unordered_map<uint64_t, BuybackItem> pendingSellToBuyback_;
    int pendingBuybackSlot_ = -1;
    uint32_t pendingBuybackWireSlot_ = 0;
    uint32_t pendingBuyItemId_ = 0;
    uint32_t pendingBuyItemSlot_ = 0;

    // Trainer
    bool trainerWindowOpen_ = false;
    TrainerListData currentTrainerList_;
    struct SpellNameEntry { std::string name; std::string rank; uint32_t schoolMask = 0; };
    std::unordered_map<uint32_t, SpellNameEntry> spellNameCache_;
    bool spellNameCacheLoaded_ = false;

    // Achievement name cache (lazy-loaded from Achievement.dbc on first earned event)
    std::unordered_map<uint32_t, std::string> achievementNameCache_;
    bool achievementNameCacheLoaded_ = false;
    void loadAchievementNameCache();
    // Set of achievement IDs earned by the player (populated from SMSG_ALL_ACHIEVEMENT_DATA)
    std::unordered_set<uint32_t> earnedAchievements_;
    void handleAllAchievementData(network::Packet& packet);

    // Area name cache (lazy-loaded from WorldMapArea.dbc; maps AreaTable ID → display name)
    std::unordered_map<uint32_t, std::string> areaNameCache_;
    bool areaNameCacheLoaded_ = false;
    void loadAreaNameCache();
    std::string getAreaName(uint32_t areaId) const;
    std::vector<TrainerTab> trainerTabs_;
    void handleTrainerList(network::Packet& packet);
    void loadSpellNameCache();
    void categorizeTrainerSpells();

    // Callbacks
    WorldConnectSuccessCallback onSuccess;
    WorldConnectFailureCallback onFailure;
    CharCreateCallback charCreateCallback_;
    CharDeleteCallback charDeleteCallback_;
    CharLoginFailCallback charLoginFailCallback_;
    uint8_t lastCharDeleteResult_ = 0xFF;
    bool pendingCharCreateResult_ = false;
    bool pendingCharCreateSuccess_ = false;
    std::string pendingCharCreateMsg_;
    bool requiresWarden_ = false;
    bool wardenGateSeen_ = false;
    float wardenGateElapsed_ = 0.0f;
    float wardenGateNextStatusLog_ = 2.0f;
    uint32_t wardenPacketsAfterGate_ = 0;
    bool wardenCharEnumBlockedLogged_ = false;
    std::unique_ptr<WardenCrypto> wardenCrypto_;
    std::unique_ptr<WardenMemory> wardenMemory_;
    std::unique_ptr<WardenModuleManager> wardenModuleManager_;

    // Warden module download state
    enum class WardenState {
        WAIT_MODULE_USE,     // Waiting for first SMSG (MODULE_USE)
        WAIT_MODULE_CACHE,   // Sent MODULE_MISSING, receiving module chunks
        WAIT_HASH_REQUEST,   // Module received, waiting for HASH_REQUEST
        WAIT_CHECKS,         // Hash sent, waiting for check requests
    };
    WardenState wardenState_ = WardenState::WAIT_MODULE_USE;
    std::vector<uint8_t> wardenModuleHash_;    // 16 bytes MD5
    std::vector<uint8_t> wardenModuleKey_;     // 16 bytes RC4
    uint32_t wardenModuleSize_ = 0;
    std::vector<uint8_t> wardenModuleData_;    // Downloaded module chunks
    std::vector<uint8_t> wardenLoadedModuleImage_; // Parsed module image for key derivation
    std::shared_ptr<WardenModule> wardenLoadedModule_; // Loaded Warden module

    // Pre-computed challenge/response entries from .cr file
    struct WardenCREntry {
        uint8_t seed[16];
        uint8_t reply[20];
        uint8_t clientKey[16];  // Encrypt key (client→server)
        uint8_t serverKey[16]; // Decrypt key (server→client)
    };
    std::vector<WardenCREntry> wardenCREntries_;
    // Module-specific check type opcodes [9]: MEM, PAGE_A, PAGE_B, MPQ, LUA, DRIVER, TIMING, PROC, MODULE
    uint8_t wardenCheckOpcodes_[9] = {};
    bool loadWardenCRFile(const std::string& moduleHashHex);

    // ---- XP tracking ----
    uint32_t playerXp_ = 0;
    uint32_t playerNextLevelXp_ = 0;
    uint32_t playerRestedXp_ = 0;
    bool isResting_ = false;
    uint32_t serverPlayerLevel_ = 1;
    static uint32_t xpForLevel(uint32_t level);

    // ---- Server time tracking (for deterministic celestial/sky systems) ----
    float gameTime_ = 0.0f;       // Server game time in seconds
    float timeSpeed_ = 0.0166f;   // Time scale (default: 1 game day = 1 real hour)
    void handleLoginSetTimeSpeed(network::Packet& packet);

    // ---- Weather state (SMSG_WEATHER) ----
    uint32_t weatherType_ = 0;       // 0=clear, 1=rain, 2=snow, 3=storm
    float weatherIntensity_ = 0.0f;  // 0.0 to 1.0

    // ---- Light override (SMSG_OVERRIDE_LIGHT) ----
    uint32_t overrideLightId_ = 0;      // 0 = no override
    uint32_t overrideLightTransMs_ = 0;

    // ---- Player skills ----
    std::map<uint32_t, PlayerSkill> playerSkills_;
    std::unordered_map<uint32_t, std::string> skillLineNames_;
    std::unordered_map<uint32_t, uint32_t> skillLineCategories_;
    std::unordered_map<uint32_t, uint32_t> spellToSkillLine_;      // spellID -> skillLineID
    bool skillLineDbcLoaded_ = false;
    bool skillLineAbilityLoaded_ = false;
    static constexpr size_t PLAYER_EXPLORED_ZONES_COUNT = 128;
    std::vector<uint32_t> playerExploredZones_ =
        std::vector<uint32_t>(PLAYER_EXPLORED_ZONES_COUNT, 0u);
    bool hasPlayerExploredZones_ = false;
    void loadSkillLineDbc();
    void loadSkillLineAbilityDbc();
    void extractSkillFields(const std::map<uint16_t, uint32_t>& fields);
    void extractExploredZoneFields(const std::map<uint16_t, uint32_t>& fields);
    void applyQuestStateFromFields(const std::map<uint16_t, uint32_t>& fields);
    // Apply packed kill counts from player update fields to a quest entry that has
    // already had its killObjectives populated from SMSG_QUEST_QUERY_RESPONSE.
    void applyPackedKillCountsFromFields(QuestLogEntry& quest);

    NpcDeathCallback npcDeathCallback_;
    NpcAggroCallback npcAggroCallback_;
    NpcRespawnCallback npcRespawnCallback_;
    StandStateCallback standStateCallback_;
    GhostStateCallback ghostStateCallback_;
    MeleeSwingCallback meleeSwingCallback_;
    SpellCastAnimCallback spellCastAnimCallback_;
    UnitAnimHintCallback unitAnimHintCallback_;
    UnitMoveFlagsCallback unitMoveFlagsCallback_;
    NpcSwingCallback npcSwingCallback_;
    NpcGreetingCallback npcGreetingCallback_;
    NpcFarewellCallback npcFarewellCallback_;
    NpcVendorCallback npcVendorCallback_;
    ChargeCallback chargeCallback_;
    LevelUpCallback levelUpCallback_;
    OtherPlayerLevelUpCallback otherPlayerLevelUpCallback_;
    AchievementEarnedCallback achievementEarnedCallback_;
    MountCallback mountCallback_;
    TaxiPrecacheCallback taxiPrecacheCallback_;
    TaxiOrientationCallback taxiOrientationCallback_;
    TaxiFlightStartCallback taxiFlightStartCallback_;
    uint32_t currentMountDisplayId_ = 0;
    uint32_t mountAuraSpellId_ = 0;       // Spell ID of the aura that caused mounting (for CMSG_CANCEL_AURA fallback)
    float serverRunSpeed_ = 7.0f;
    float serverWalkSpeed_ = 2.5f;
    float serverRunBackSpeed_ = 4.5f;
    float serverSwimSpeed_ = 4.722f;
    float serverSwimBackSpeed_ = 2.5f;
    float serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f;
    float serverTurnRate_ = 3.14159f;
    float serverPitchRate_ = 3.14159f;
    bool playerDead_ = false;
    bool releasedSpirit_ = false;
    uint32_t corpseMapId_ = 0;
    float corpseX_ = 0.0f, corpseY_ = 0.0f, corpseZ_ = 0.0f;
    // Death Knight runes (class 6): slots 0-1=Blood, 2-3=Unholy, 4-5=Frost initially
    std::array<RuneSlot, 6> playerRunes_ = [] {
        std::array<RuneSlot, 6> r{};
        r[0].type = r[1].type = RuneType::Blood;
        r[2].type = r[3].type = RuneType::Unholy;
        r[4].type = r[5].type = RuneType::Frost;
        return r;
    }();
    uint64_t pendingSpiritHealerGuid_ = 0;
    bool resurrectPending_ = false;
    bool resurrectRequestPending_ = false;
    // ---- Talent wipe confirm dialog ----
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;
    bool resurrectIsSpiritHealer_ = false;  // true = SMSG_SPIRIT_HEALER_CONFIRM, false = SMSG_RESURRECT_REQUEST
    uint64_t resurrectCasterGuid_ = 0;
    std::string resurrectCasterName_;
    bool repopPending_ = false;
    uint64_t lastRepopRequestMs_ = 0;

    // ---- Completed quest IDs (SMSG_QUERY_QUESTS_COMPLETED_RESPONSE) ----
    std::unordered_set<uint32_t> completedQuests_;

    // ---- Equipment sets (SMSG_EQUIPMENT_SET_LIST) ----
    struct EquipmentSet {
        uint64_t setGuid = 0;
        uint32_t setId = 0;
        std::string name;
        std::string iconName;
        uint32_t ignoreSlotMask = 0;
        std::array<uint64_t, 19> itemGuids{};
    };
    std::vector<EquipmentSet> equipmentSets_;

    // ---- Forced faction reactions (SMSG_SET_FORCED_REACTIONS) ----
    std::unordered_map<uint32_t, uint8_t> forcedReactions_;  // factionId -> reaction tier

    // ---- Server-triggered audio ----
    PlayMusicCallback playMusicCallback_;
    PlaySoundCallback playSoundCallback_;
    PlayPositionalSoundCallback playPositionalSoundCallback_;
};

} // namespace game
} // namespace wowee
