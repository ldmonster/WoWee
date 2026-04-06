#pragma once

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/handler_types.hpp"
#include "audio/spell_sound_manager.hpp"
#include "network/packet.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SpellHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        std::vector<uint32_t> spellIds; // spells in this tab
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // --- Public API (delegated from GameHandler) ---
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);
    void cancelCast();
    void cancelAura(uint32_t spellId);

    // Known spells
    const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }
    const std::unordered_map<uint32_t, float>& getSpellCooldowns() const { return spellCooldowns_; }
    float getSpellCooldown(uint32_t spellId) const;

    // Cast state
    bool isCasting() const { return casting_; }
    bool isChanneling() const { return casting_ && castIsChannel_; }
    bool isGameObjectInteractionCasting() const;
    uint32_t getCurrentCastSpellId() const { return currentCastSpellId_; }
    float getCastProgress() const { return castTimeTotal_ > 0 ? (castTimeTotal_ - castTimeRemaining_) / castTimeTotal_ : 0.0f; }
    float getCastTimeRemaining() const { return castTimeRemaining_; }
    float getCastTimeTotal() const { return castTimeTotal_; }

    // Repeat-craft queue
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    int getCraftQueueRemaining() const { return craftQueueRemaining_; }
    uint32_t getCraftQueueSpellId() const { return craftQueueSpellId_; }

    // Spell queue (400ms window)
    uint32_t getQueuedSpellId() const { return queuedSpellId_; }
    void cancelQueuedSpell() { queuedSpellId_ = 0; queuedSpellTarget_ = 0; }

    // Unit cast state (tracked per GUID for target frame + boss frames)
    const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) { knownSpells_.insert(spellId); }
    bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }

    // Target aura mutation (formerly accessed via friend)
    void clearTargetAuras() { for (auto& slot : targetAuras_) slot = AuraSlot{}; }

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }

    // Target cast helpers
    bool isTargetCasting() const;
    uint32_t getTargetCastSpellId() const;
    float getTargetCastProgress() const;
    float getTargetCastTimeRemaining() const;
    bool isTargetCastInterruptible() const;

    // Talents
    uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
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

    // Auras
    const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Global Cooldown (GCD)
    float getGCDRemaining() const {
        if (gcdTotal_ <= 0.0f) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gcdStartedAt_).count() / 1000.0f;
        float rem = gcdTotal_ - elapsed;
        return rem > 0.0f ? rem : 0.0f;
    }
    float getGCDTotal() const { return gcdTotal_; }
    bool isGCDActive() const { return getGCDRemaining() > 0.0f; }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();

    // Talent wipe confirm dialog
    bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Pet talent respec confirm
    bool showPetUnlearnDialog() const { return petUnlearnPending_; }
    uint32_t getPetUnlearnCost() const { return petUnlearnCost_; }
    void confirmPetUnlearn();
    void cancelPetUnlearn() { petUnlearnPending_ = false; }

    // Item use
    void useItemBySlot(int backpackIndex);
    void useItemInBag(int bagIndex, int slotIndex);
    void useItemById(uint32_t itemId);

    // Equipment sets — canonical data owned by InventoryHandler;
    // GameHandler::getEquipmentSets() delegates to inventoryHandler_.

    // Pet spells
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    void dismissPet();
    void togglePetSpellAutocast(uint32_t spellId);
    void renamePet(const std::string& newName);

    // Spell DBC accessors
    const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    float getSpellDuration(uint32_t spellId) const;
    const std::string& getSpellName(uint32_t spellId) const;
    const std::string& getSpellRank(uint32_t spellId) const;
    const std::string& getSpellDescription(uint32_t spellId) const;
    std::string getEnchantName(uint32_t enchantId) const;
    uint8_t getSpellDispelType(uint32_t spellId) const;
    bool isSpellInterruptible(uint32_t spellId) const;
    uint32_t getSpellSchoolMask(uint32_t spellId) const;
    const std::string& getSkillLineName(uint32_t spellId) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc();
    void extractSkillFields(const std::map<uint16_t, uint32_t>& fields);
    void extractExploredZoneFields(const std::map<uint16_t, uint32_t>& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);

    // Packet handlers dispatched from GameHandler's opcode table
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);

    // Pet stable commands (called via GameHandler delegation)
    void requestStabledPetList();
    void stablePet(uint8_t slot);
    void unstablePet(uint32_t petNumber);

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    void loadSkillLineAbilityDbc();
    void categorizeTrainerSpells();

private:
    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleItemEnchantTimeUpdate(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    // Resolve the magic school for a spell (for audio playback).
    // Returns MagicSchool from the spell name cache, defaulting to ARCANE.
    audio::SpellSoundManager::MagicSchool resolveSpellSchool(uint32_t spellId);

    // Play a spell cast or impact sound via audioCoordinator, if available.
    void playSpellCastSound(uint32_t spellId);
    void playSpellImpactSound(uint32_t spellId);

    // --- handleSpellLogExecute per-effect parsers (extracted to reduce nesting) ---
    void parseEffectPowerDrain(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                               bool usesFullGuid);
    void parseEffectHealthLeech(network::Packet& packet, uint32_t effectLogCount,
                                uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                bool usesFullGuid);
    void parseEffectCreateItem(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectInterruptCast(network::Packet& packet, uint32_t effectLogCount,
                                  uint64_t caster, uint32_t spellId, bool isPlayerCaster,
                                  bool usesFullGuid);
    void parseEffectFeedPet(network::Packet& packet, uint32_t effectLogCount,
                            uint64_t caster, uint32_t spellId, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    uint32_t findOnUseSpellId(uint32_t itemId) const;
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    GameHandler& owner_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    std::unordered_map<uint32_t, float> spellCooldowns_;    // spellId -> remaining seconds
    uint8_t castCount_ = 0;
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;

    // Repeat-craft queue
    uint32_t craftQueueSpellId_ = 0;
    int craftQueueRemaining_ = 0;

    // Spell queue (400ms window)
    uint32_t queuedSpellId_ = 0;
    uint64_t queuedSpellTarget_ = 0;

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Global Cooldown
    float gcdTotal_ = 0.0f;
    std::chrono::steady_clock::time_point gcdStartedAt_{};

    // Spell book tabs
    std::vector<SpellBookTab> spellBookTabs_;
    size_t lastSpellCount_ = 0;
    bool spellBookTabsDirty_ = true;

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    // Pet talent respec confirm dialog
    bool petUnlearnPending_ = false;
    uint64_t petUnlearnGuid_ = 0;
    uint32_t petUnlearnCost_ = 0;
};

} // namespace game
} // namespace wowee
