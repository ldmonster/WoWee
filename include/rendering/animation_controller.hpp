#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace wowee {
namespace audio { enum class FootstepSurface : uint8_t; }
namespace rendering {

class Renderer;

/// Ranged weapon type for animation selection (bow/gun/crossbow/thrown)
enum class RangedWeaponType : uint8_t { NONE = 0, BOW, GUN, CROSSBOW, THROWN };

// ============================================================================
// AnimationController — extracted from Renderer (§4.2)
//
// Owns the character locomotion state machine, mount animation state,
// emote system, footstep triggering, surface detection, melee combat
// animation, and activity SFX transition tracking.
// ============================================================================
class AnimationController {
public:
    AnimationController();
    ~AnimationController();

    void initialize(Renderer* renderer);

    // ── Per-frame update hooks (called from Renderer::update) ──────────────
    // Runs the character animation state machine (mounted + unmounted).
    void updateCharacterAnimation();
    // Processes animation-driven footstep events (player + mount).
    void updateFootsteps(float deltaTime);
    // Tracks state transitions for activity SFX (jump, landing, swim) and
    // mount ambient sounds.
    void updateSfxState(float deltaTime);
    // Decrements melee swing timer / cooldown.
    void updateMeleeTimers(float deltaTime);
    // Store per-frame delta time (used inside animation state machine).
    void setDeltaTime(float dt) { lastDeltaTime_ = dt; }

    // ── Character follow ───────────────────────────────────────────────────
    void onCharacterFollow(uint32_t instanceId);

    // ── Emote support ──────────────────────────────────────────────────────
    void playEmote(const std::string& emoteName);
    void cancelEmote();
    bool isEmoteActive() const { return emoteActive_; }
    static std::string getEmoteText(const std::string& emoteName,
                                    const std::string* targetName = nullptr);
    static uint32_t getEmoteDbcId(const std::string& emoteName);
    static std::string getEmoteTextByDbcId(uint32_t dbcId,
                                           const std::string& senderName,
                                           const std::string* targetName = nullptr);
    static uint32_t getEmoteAnimByDbcId(uint32_t dbcId);

    // ── Targeting / combat ─────────────────────────────────────────────────
    void setTargetPosition(const glm::vec3* pos);
    void setInCombat(bool combat) { inCombat_ = combat; }
    bool isInCombat() const { return inCombat_; }
    const glm::vec3* getTargetPosition() const { return targetPosition_; }
    void resetCombatVisualState();
    bool isMoving() const;

    // ── Melee combat ───────────────────────────────────────────────────────
    void triggerMeleeSwing();
    /// inventoryType: WoW inventory type (0=unarmed, 13=1H, 17=2H, 21=main-hand, …)
    /// is2HLoose: true for polearms/staves (use ATTACK_2H_LOOSE instead of ATTACK_2H)
    void setEquippedWeaponType(uint32_t inventoryType, bool is2HLoose = false,
                               bool isFist = false, bool isDagger = false,
                               bool hasOffHand = false, bool hasShield = false) {
        equippedWeaponInvType_ = inventoryType;
        equippedIs2HLoose_ = is2HLoose;
        equippedIsFist_ = isFist;
        equippedIsDagger_ = isDagger;
        equippedHasOffHand_ = hasOffHand;
        equippedHasShield_ = hasShield;
        meleeAnimId_ = 0;  // Force re-resolve on next swing
    }
    /// Play a special attack animation for a melee ability (spellId → SPECIAL_1H/2H/SHIELD_BASH/WHIRLWIND)
    void triggerSpecialAttack(uint32_t spellId);

    // ── Sprint aura animation ────────────────────────────────────────────
    void setSprintAuraActive(bool active) { sprintAuraActive_ = active; }

    // ── Ranged combat ──────────────────────────────────────────────────────
    void setEquippedRangedType(RangedWeaponType type) {
        equippedRangedType_ = type;
        rangedAnimId_ = 0;  // Force re-resolve
    }
    /// Trigger a ranged shot animation (Auto Shot, Shoot, Throw)
    void triggerRangedShot();
    RangedWeaponType getEquippedRangedType() const { return equippedRangedType_; }
    void setCharging(bool charging) { charging_ = charging; }
    bool isCharging() const { return charging_; }

    // ── Spell casting ──────────────────────────────────────────────────────
    /// Enter spell cast animation sequence:
    ///   precastAnimId (one-shot wind-up) → castAnimId (looping hold) → finalizeAnimId (one-shot release)
    /// Any phase can be 0 to skip it.
    void startSpellCast(uint32_t precastAnimId, uint32_t castAnimId, bool castLoop,
                        uint32_t finalizeAnimId = 0);
    /// Leave spell cast animation state → plays finalization anim then idle.
    void stopSpellCast();

    // ── Loot animation ─────────────────────────────────────────────────────
    void startLooting();
    void stopLooting();

    // ── Hit reactions ──────────────────────────────────────────────────────
    /// Play a one-shot hit reaction animation (wound, dodge, block, etc.)
    /// on the player character.  The state machine returns to the previous
    /// state once the reaction animation finishes.
    void triggerHitReaction(uint32_t animId);

    // ── Crowd control ──────────────────────────────────────────────────────
    /// Enter/exit stunned state (loops STUN animation until cleared).
    void setStunned(bool stunned);
    bool isStunned() const { return stunned_; }

    // ── Health-based idle ──────────────────────────────────────────────────
    /// When true, idle/combat-idle will prefer STAND_WOUND if the model has it.
    void setLowHealth(bool low) { lowHealth_ = low; }

    // ── Stand state (sit/sleep/kneel transitions) ──────────────────────────
    // WoW UnitStandStateType constants
    static constexpr uint8_t STAND_STATE_STAND      = 0;
    static constexpr uint8_t STAND_STATE_SIT         = 1;
    static constexpr uint8_t STAND_STATE_SIT_CHAIR   = 2;
    static constexpr uint8_t STAND_STATE_SLEEP       = 3;
    static constexpr uint8_t STAND_STATE_SIT_LOW     = 4;
    static constexpr uint8_t STAND_STATE_SIT_MED     = 5;
    static constexpr uint8_t STAND_STATE_SIT_HIGH    = 6;
    static constexpr uint8_t STAND_STATE_DEAD        = 7;
    static constexpr uint8_t STAND_STATE_KNEEL       = 8;
    static constexpr uint8_t STAND_STATE_SUBMERGED   = 9;
    void setStandState(uint8_t state);

    // ── Stealth ────────────────────────────────────────────────────────────
    /// When true, idle/walk/run use stealth animation variants.
    void setStealthed(bool stealth);

    // ── Effect triggers ────────────────────────────────────────────────────
    void triggerLevelUpEffect(const glm::vec3& position);
    void startChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void emitChargeEffect(const glm::vec3& position, const glm::vec3& direction);
    void stopChargeEffect();

    // ── Mount ──────────────────────────────────────────────────────────────
    void setMounted(uint32_t mountInstId, uint32_t mountDisplayId,
                    float heightOffset, const std::string& modelPath = "");
    void setTaxiFlight(bool onTaxi) { taxiFlight_ = onTaxi; }
    void setMountPitchRoll(float pitch, float roll) { mountPitch_ = pitch; mountRoll_ = roll; }
    void clearMount();
    bool isMounted() const { return mountInstanceId_ != 0; }
    uint32_t getMountInstanceId() const { return mountInstanceId_; }

    // ── Query helpers (used by Renderer) ───────────────────────────────────
    bool isFootstepAnimationState() const;
    float getMeleeSwingTimer() const { return meleeSwingTimer_; }
    float getMountHeightOffset() const { return mountHeightOffset_; }
    bool isTaxiFlight() const { return taxiFlight_; }

private:
    Renderer* renderer_ = nullptr;

    // Character animation state machine
    enum class CharAnimState {
        IDLE, WALK, RUN, JUMP_START, JUMP_MID, JUMP_END, SIT_DOWN, SITTING,
        SIT_UP, EMOTE, SWIM_IDLE, SWIM, MELEE_SWING, MOUNT, CHARGE, COMBAT_IDLE,
        SPELL_PRECAST, SPELL_CASTING, SPELL_FINALIZE, HIT_REACTION, STUNNED, LOOTING,
        UNSHEATHE, SHEATHE,  // Weapon draw/put-away one-shot transitions
        RANGED_SHOOT, RANGED_LOAD  // Ranged attack sequence: shoot → reload
    };
    CharAnimState charAnimState_ = CharAnimState::IDLE;
    float locomotionStopGraceTimer_ = 0.0f;
    bool locomotionWasSprinting_ = false;
    uint32_t lastPlayerAnimRequest_ = UINT32_MAX;
    bool lastPlayerAnimLoopRequest_ = true;

    // Emote state
    bool emoteActive_ = false;
    uint32_t emoteAnimId_ = 0;
    bool emoteLoop_ = false;

    // Spell cast sequence state (PRECAST → CASTING → FINALIZE)
    uint32_t spellPrecastAnimId_ = 0;   // One-shot wind-up (phase 1)
    uint32_t spellCastAnimId_ = 0;      // Looping cast hold (phase 2)
    uint32_t spellFinalizeAnimId_ = 0;  // One-shot release (phase 3)
    bool spellCastLoop_ = false;

    // Hit reaction state
    uint32_t hitReactionAnimId_ = 0;

    // Crowd control
    bool stunned_ = false;

    // Health-based idle
    bool lowHealth_ = false;

    // Stand state (sit/sleep/kneel)
    uint8_t standState_ = 0;
    uint32_t sitDownAnim_ = 0;   // Transition-in animation (one-shot)
    uint32_t sitLoopAnim_ = 0;   // Looping pose animation
    uint32_t sitUpAnim_ = 0;     // Transition-out animation (one-shot)

    // Stealth
    bool stealthed_ = false;

    // Target facing
    const glm::vec3* targetPosition_ = nullptr;
    bool inCombat_ = false;

    // Footstep event tracking (animation-driven)
    uint32_t footstepLastAnimationId_ = 0;
    float footstepLastNormTime_ = 0.0f;
    bool footstepNormInitialized_ = false;

    // Footstep surface cache (avoid expensive queries every step)
    mutable audio::FootstepSurface cachedFootstepSurface_{};
    mutable glm::vec3 cachedFootstepPosition_{0.0f, 0.0f, 0.0f};
    mutable float cachedFootstepUpdateTimer_{999.0f};

    // Mount footstep tracking (separate from player's)
    uint32_t mountFootstepLastAnimId_ = 0;
    float mountFootstepLastNormTime_ = 0.0f;
    bool mountFootstepNormInitialized_ = false;

    // SFX transition state
    bool sfxStateInitialized_ = false;
    bool sfxPrevGrounded_ = true;
    bool sfxPrevJumping_ = false;
    bool sfxPrevFalling_ = false;
    bool sfxPrevSwimming_ = false;

    // Melee combat
    bool charging_ = false;
    float meleeSwingTimer_ = 0.0f;
    float meleeSwingCooldown_ = 0.0f;
    float meleeAnimDurationMs_ = 0.0f;
    uint32_t meleeAnimId_ = 0;
    uint32_t specialAttackAnimId_ = 0; // Non-zero during special attack (overrides resolveMeleeAnimId)
    uint32_t equippedWeaponInvType_ = 0;
    bool equippedIs2HLoose_ = false;  // Polearm or staff
    bool equippedIsFist_ = false;     // Fist weapon
    bool equippedIsDagger_ = false;   // Dagger (uses pierce variants)
    bool equippedHasOffHand_ = false; // Has off-hand weapon (dual wield)
    bool equippedHasShield_ = false;  // Has shield equipped (for SHIELD_BASH)
    bool meleeOffHandTurn_ = false;   // Alternates main/off-hand swings

    // Ranged weapon state
    RangedWeaponType equippedRangedType_ = RangedWeaponType::NONE;
    float rangedShootTimer_ = 0.0f;    // Countdown for ranged attack animation
    uint32_t rangedAnimId_ = 0;        // Cached ranged attack animation

    // Mount animation capabilities (discovered at mount time, varies per model)
    struct MountAnimSet {
        uint32_t jumpStart = 0;  // Jump start animation
        uint32_t jumpLoop = 0;   // Jump airborne loop
        uint32_t jumpEnd = 0;    // Jump landing
        uint32_t rearUp = 0;     // Rear-up / special flourish
        uint32_t run = 0;        // Run animation (discovered, don't assume)
        uint32_t stand = 0;      // Stand animation (discovered)
        // Flight animations (discovered from mount model)
        uint32_t flyIdle = 0;
        uint32_t flyForward = 0;
        uint32_t flyBackwards = 0;
        uint32_t flyLeft = 0;
        uint32_t flyRight = 0;
        uint32_t flyUp = 0;
        uint32_t flyDown = 0;
        std::vector<uint32_t> fidgets;  // Idle fidget animations (head turn, tail swish, etc.)
    };

    enum class MountAction { None, Jump, RearUp };

    uint32_t mountInstanceId_ = 0;
    float mountHeightOffset_ = 0.0f;
    float mountPitch_ = 0.0f;  // Up/down tilt (radians)
    float mountRoll_ = 0.0f;   // Left/right banking (radians)
    int mountSeatAttachmentId_ = -1;  // -1 unknown, -2 unavailable
    glm::vec3 smoothedMountSeatPos_ = glm::vec3(0.0f);
    bool mountSeatSmoothingInit_ = false;
    float prevMountYaw_ = 0.0f; // Previous yaw for turn rate calculation (procedural lean)
    float lastDeltaTime_ = 0.0f; // Cached for use in updateCharacterAnimation()
    MountAction mountAction_ = MountAction::None;  // Current mount action (jump/rear-up)
    uint32_t mountActionPhase_ = 0;  // 0=start, 1=loop, 2=end (for jump chaining)
    MountAnimSet mountAnims_;  // Cached animation IDs for current mount
    float mountIdleFidgetTimer_ = 0.0f;  // Timer for random idle fidgets
    float mountIdleSoundTimer_ = 0.0f;   // Timer for ambient idle sounds
    uint32_t mountActiveFidget_ = 0;     // Currently playing fidget animation ID (0 = none)
    bool taxiFlight_ = false;
    bool taxiAnimsLogged_ = false;
    bool sprintAuraActive_ = false;  // Sprint/Dash aura active → use SPRINT anim

    // Private animation helpers
    bool shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs);
    audio::FootstepSurface resolveFootstepSurface() const;
    uint32_t resolveMeleeAnimId();
};

} // namespace rendering
} // namespace wowee
