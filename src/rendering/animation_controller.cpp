#include "rendering/animation_controller.hpp"
#include "rendering/animation_ids.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/levelup_effect.hpp"
#include "rendering/charge_effect.hpp"
#include "rendering/spell_visual_system.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "game/inventory.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "rendering/swim_effects.hpp"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <random>
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

// ── Static emote data (shared across all AnimationController instances) ──────

struct EmoteInfo {
    uint32_t animId = 0;
    uint32_t dbcId = 0;
    bool loop = false;
    std::string textNoTarget;
    std::string textTarget;
    std::string othersNoTarget;
    std::string othersTarget;
    std::string command;
};

static std::unordered_map<std::string, EmoteInfo> EMOTE_TABLE;
static std::unordered_map<uint32_t, const EmoteInfo*> EMOTE_BY_DBCID;
static bool emoteTableLoaded = false;

static std::vector<std::string> parseEmoteCommands(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool isLoopingEmote(const std::string& command) {
    static const std::unordered_set<std::string> kLooping = {
        "dance", "train", "dead", "eat", "work",
    };
    return kLooping.find(command) != kLooping.end();
}

// Map one-shot emote animation IDs to their persistent EMOTE_STATE_* looping variants.
// When a looping emote is played, we prefer the STATE variant if the model has it.
static uint32_t getEmoteStateVariant(uint32_t oneShotAnimId) {
    static const std::unordered_map<uint32_t, uint32_t> kStateMap = {
        {anim::EMOTE_DANCE,         anim::EMOTE_STATE_DANCE},
        {anim::EMOTE_LAUGH,         anim::EMOTE_STATE_LAUGH},
        {anim::EMOTE_POINT,         anim::EMOTE_STATE_POINT},
        {anim::EMOTE_EAT,           anim::EMOTE_STATE_EAT},
        {anim::EMOTE_ROAR,          anim::EMOTE_STATE_ROAR},
        {anim::EMOTE_APPLAUD,       anim::EMOTE_STATE_APPLAUD},
        {anim::EMOTE_WORK,          anim::EMOTE_STATE_WORK},
        {anim::EMOTE_USE_STANDING,  anim::EMOTE_STATE_USE_STANDING},
        {anim::EATING_LOOP,         anim::EMOTE_STATE_EAT},
    };
    auto it = kStateMap.find(oneShotAnimId);
    return it != kStateMap.end() ? it->second : 0;
}

static void loadFallbackEmotes() {
    if (!EMOTE_TABLE.empty()) return;
    EMOTE_TABLE = {
        {"wave",    {anim::EMOTE_WAVE,    0, false, "You wave.", "You wave at %s.", "%s waves.", "%s waves at %s.", "wave"}},
        {"bow",     {anim::EMOTE_BOW,     0, false, "You bow down graciously.", "You bow down before %s.", "%s bows down graciously.", "%s bows down before %s.", "bow"}},
        {"laugh",   {anim::EMOTE_LAUGH,   0, false, "You laugh.", "You laugh at %s.", "%s laughs.", "%s laughs at %s.", "laugh"}},
        {"point",   {anim::EMOTE_POINT,   0, false, "You point over yonder.", "You point at %s.", "%s points over yonder.", "%s points at %s.", "point"}},
        {"cheer",   {anim::EMOTE_CHEER,   0, false, "You cheer!", "You cheer at %s.", "%s cheers!", "%s cheers at %s.", "cheer"}},
        {"dance",   {anim::EMOTE_DANCE,   0, true,  "You burst into dance.", "You dance with %s.", "%s bursts into dance.", "%s dances with %s.", "dance"}},
        {"kneel",   {anim::EMOTE_KNEEL,   0, false, "You kneel down.", "You kneel before %s.", "%s kneels down.", "%s kneels before %s.", "kneel"}},
        {"applaud", {anim::EMOTE_APPLAUD, 0, false, "You applaud. Bravo!", "You applaud at %s. Bravo!", "%s applauds. Bravo!", "%s applauds at %s. Bravo!", "applaud"}},
        {"shout",   {anim::EMOTE_SHOUT,   0, false, "You shout.", "You shout at %s.", "%s shouts.", "%s shouts at %s.", "shout"}},
        {"chicken", {anim::EMOTE_CHICKEN, 0, false, "With arms flapping, you strut around. Cluck, Cluck, Chicken!",
                     "With arms flapping, you strut around %s. Cluck, Cluck, Chicken!",
                     "%s struts around. Cluck, Cluck, Chicken!", "%s struts around %s. Cluck, Cluck, Chicken!", "chicken"}},
        {"cry",     {anim::EMOTE_CRY,     0, false, "You cry.", "You cry on %s's shoulder.", "%s cries.", "%s cries on %s's shoulder.", "cry"}},
        {"kiss",    {anim::EMOTE_KISS,    0, false, "You blow a kiss into the wind.", "You blow a kiss to %s.", "%s blows a kiss into the wind.", "%s blows a kiss to %s.", "kiss"}},
        {"roar",    {anim::EMOTE_ROAR,    0, false, "You roar with bestial vigor. So fierce!", "You roar with bestial vigor at %s. So fierce!", "%s roars with bestial vigor. So fierce!", "%s roars with bestial vigor at %s. So fierce!", "roar"}},
        {"salute",  {anim::EMOTE_SALUTE,  0, false, "You salute.", "You salute %s with respect.", "%s salutes.", "%s salutes %s with respect.", "salute"}},
        {"rude",    {anim::EMOTE_RUDE,    0, false, "You make a rude gesture.", "You make a rude gesture at %s.", "%s makes a rude gesture.", "%s makes a rude gesture at %s.", "rude"}},
        {"flex",    {anim::EMOTE_FLEX,    0, false, "You flex your muscles. Oooooh so strong!", "You flex at %s. Oooooh so strong!", "%s flexes. Oooooh so strong!", "%s flexes at %s. Oooooh so strong!", "flex"}},
        {"shy",     {anim::EMOTE_SHY,     0, false, "You smile shyly.", "You smile shyly at %s.", "%s smiles shyly.", "%s smiles shyly at %s.", "shy"}},
        {"beg",     {anim::EMOTE_BEG,     0, false, "You beg everyone around you. How pathetic.", "You beg %s. How pathetic.", "%s begs everyone around. How pathetic.", "%s begs %s. How pathetic.", "beg"}},
        {"eat",     {anim::EMOTE_EAT,     0, true,  "You begin to eat.", "You begin to eat in front of %s.", "%s begins to eat.", "%s begins to eat in front of %s.", "eat"}},
        {"talk",    {anim::EMOTE_TALK,    0, false, "You talk.", "You talk to %s.", "%s talks.", "%s talks to %s.", "talk"}},
        {"work",    {anim::EMOTE_WORK,    0, true,  "You begin to work.", "You begin to work near %s.", "%s begins to work.", "%s begins to work near %s.", "work"}},
        {"train",   {anim::EMOTE_TRAIN,   0, true,  "You let off a train whistle. Choo Choo!", "You let off a train whistle at %s. Choo Choo!", "%s lets off a train whistle. Choo Choo!", "%s lets off a train whistle at %s. Choo Choo!", "train"}},
        {"dead",    {anim::EMOTE_DEAD,    0, true,  "You play dead.", "You play dead in front of %s.", "%s plays dead.", "%s plays dead in front of %s.", "dead"}},
    };
}

static std::string replacePlaceholders(const std::string& text, const std::string* targetName) {
    if (text.empty()) return text;
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == 's') {
            if (targetName && !targetName->empty()) out += *targetName;
            i++;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

static void loadEmotesFromDbc() {
    if (emoteTableLoaded) return;
    emoteTableLoaded = true;

    auto* assetManager = core::Application::getInstance().getAssetManager();
    if (!assetManager) {
        LOG_WARNING("Emotes: no AssetManager");
        loadFallbackEmotes();
        return;
    }

    auto emotesTextDbc = assetManager->loadDBC("EmotesText.dbc");
    auto emotesTextDataDbc = assetManager->loadDBC("EmotesTextData.dbc");
    if (!emotesTextDbc || !emotesTextDataDbc || !emotesTextDbc->isLoaded() || !emotesTextDataDbc->isLoaded()) {
        LOG_WARNING("Emotes: DBCs not available (EmotesText/EmotesTextData)");
        loadFallbackEmotes();
        return;
    }

    const auto* activeLayout = pipeline::getActiveDBCLayout();
    const auto* etdL = activeLayout ? activeLayout->getLayout("EmotesTextData") : nullptr;
    const auto* emL  = activeLayout ? activeLayout->getLayout("Emotes") : nullptr;
    const auto* etL  = activeLayout ? activeLayout->getLayout("EmotesText") : nullptr;

    std::unordered_map<uint32_t, std::string> textData;
    textData.reserve(emotesTextDataDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDataDbc->getRecordCount(); ++r) {
        uint32_t id = emotesTextDataDbc->getUInt32(r, etdL ? (*etdL)["ID"] : 0);
        std::string text = emotesTextDataDbc->getString(r, etdL ? (*etdL)["Text"] : 1);
        if (!text.empty()) textData.emplace(id, std::move(text));
    }

    std::unordered_map<uint32_t, uint32_t> emoteIdToAnim;
    if (auto emotesDbc = assetManager->loadDBC("Emotes.dbc"); emotesDbc && emotesDbc->isLoaded()) {
        emoteIdToAnim.reserve(emotesDbc->getRecordCount());
        for (uint32_t r = 0; r < emotesDbc->getRecordCount(); ++r) {
            uint32_t emoteId = emotesDbc->getUInt32(r, emL ? (*emL)["ID"] : 0);
            uint32_t animId = emotesDbc->getUInt32(r, emL ? (*emL)["AnimID"] : 2);
            if (animId != 0) emoteIdToAnim[emoteId] = animId;
        }
    }

    EMOTE_TABLE.clear();
    EMOTE_TABLE.reserve(emotesTextDbc->getRecordCount());
    for (uint32_t r = 0; r < emotesTextDbc->getRecordCount(); ++r) {
        uint32_t recordId = emotesTextDbc->getUInt32(r, etL ? (*etL)["ID"] : 0);
        std::string cmdRaw = emotesTextDbc->getString(r, etL ? (*etL)["Command"] : 1);
        if (cmdRaw.empty()) continue;

        uint32_t emoteRef = emotesTextDbc->getUInt32(r, etL ? (*etL)["EmoteRef"] : 2);
        uint32_t animId = 0;
        auto animIt = emoteIdToAnim.find(emoteRef);
        if (animIt != emoteIdToAnim.end()) {
            animId = animIt->second;
        } else {
            animId = emoteRef;
        }

        uint32_t senderTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderTargetTextID"] : 5);
        uint32_t senderNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["SenderNoTargetTextID"] : 9);
        uint32_t othersTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersTargetTextID"] : 3);
        uint32_t othersNoTargetTextId = emotesTextDbc->getUInt32(r, etL ? (*etL)["OthersNoTargetTextID"] : 7);

        std::string textTarget, textNoTarget, oTarget, oNoTarget;
        if (auto it = textData.find(senderTargetTextId); it != textData.end()) textTarget = it->second;
        if (auto it = textData.find(senderNoTargetTextId); it != textData.end()) textNoTarget = it->second;
        if (auto it = textData.find(othersTargetTextId); it != textData.end()) oTarget = it->second;
        if (auto it = textData.find(othersNoTargetTextId); it != textData.end()) oNoTarget = it->second;

        for (const std::string& cmd : parseEmoteCommands(cmdRaw)) {
            if (cmd.empty()) continue;
            EmoteInfo info;
            info.animId = animId;
            info.dbcId = recordId;
            info.loop = isLoopingEmote(cmd);
            info.textNoTarget = textNoTarget;
            info.textTarget = textTarget;
            info.othersNoTarget = oNoTarget;
            info.othersTarget = oTarget;
            info.command = cmd;
            EMOTE_TABLE.emplace(cmd, std::move(info));
        }
    }

    if (EMOTE_TABLE.empty()) {
        LOG_WARNING("Emotes: DBC loaded but no commands parsed, using fallback list");
        loadFallbackEmotes();
    } else {
        LOG_INFO("Emotes: loaded ", EMOTE_TABLE.size(), " commands from DBC");
    }

    EMOTE_BY_DBCID.clear();
    for (auto& [cmd, info] : EMOTE_TABLE) {
        if (info.dbcId != 0) {
            EMOTE_BY_DBCID.emplace(info.dbcId, &info);
        }
    }
}

// ── AnimationController implementation ───────────────────────────────────────

AnimationController::AnimationController() = default;
AnimationController::~AnimationController() = default;

void AnimationController::initialize(Renderer* renderer) {
    renderer_ = renderer;
}

void AnimationController::onCharacterFollow(uint32_t /*instanceId*/) {
    // Reset animation state when follow target changes
}

// ── Emote support ────────────────────────────────────────────────────────────

void AnimationController::playEmote(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it == EMOTE_TABLE.end()) return;

    const auto& info = it->second;
    if (info.animId == 0) return;
    emoteActive_ = true;
    emoteAnimId_ = info.animId;
    emoteLoop_ = info.loop;

    // For looping emotes, prefer the EMOTE_STATE_* variant if the model has it
    if (emoteLoop_) {
        uint32_t stateVariant = getEmoteStateVariant(emoteAnimId_);
        if (stateVariant != 0) {
            auto* characterRenderer = renderer_->getCharacterRenderer();
            uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
            if (characterRenderer && characterInstanceId > 0 &&
                characterRenderer->hasAnimation(characterInstanceId, stateVariant)) {
                emoteAnimId_ = stateVariant;
            }
        }
    }

    charAnimState_ = CharAnimState::EMOTE;

    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (characterRenderer && characterInstanceId > 0) {
        characterRenderer->playAnimation(characterInstanceId, emoteAnimId_, emoteLoop_);
    }
}

void AnimationController::cancelEmote() {
    emoteActive_ = false;
    emoteAnimId_ = 0;
    emoteLoop_ = false;
}

void AnimationController::startSpellCast(uint32_t precastAnimId, uint32_t castAnimId, bool castLoop,
                                         uint32_t finalizeAnimId) {
    spellPrecastAnimId_ = precastAnimId;
    spellCastAnimId_ = castAnimId;
    spellCastLoop_ = castLoop;
    spellFinalizeAnimId_ = finalizeAnimId;

    // Start with precast phase if available, otherwise go straight to cast
    if (spellPrecastAnimId_ != 0) {
        charAnimState_ = CharAnimState::SPELL_PRECAST;
    } else {
        charAnimState_ = CharAnimState::SPELL_CASTING;
    }
    // Force immediate animation update by invalidating the last request
    lastPlayerAnimRequest_ = UINT32_MAX;
}

void AnimationController::stopSpellCast() {
    if (charAnimState_ != CharAnimState::SPELL_PRECAST &&
        charAnimState_ != CharAnimState::SPELL_CASTING) return;

    if (spellFinalizeAnimId_ != 0) {
        // Transition to finalization phase — one-shot release animation
        charAnimState_ = CharAnimState::SPELL_FINALIZE;
        lastPlayerAnimRequest_ = UINT32_MAX;
    } else if (spellCastLoop_) {
        // No finalize anim — let current cast cycle finish as one-shot
        spellCastLoop_ = false;
        charAnimState_ = CharAnimState::SPELL_FINALIZE;
        lastPlayerAnimRequest_ = UINT32_MAX;
    } else {
        // Instant cast (no finalize, no loop) — wait for completion in current state
        charAnimState_ = CharAnimState::SPELL_FINALIZE;
        lastPlayerAnimRequest_ = UINT32_MAX;
    }
}

void AnimationController::startLooting() {
    // Don't override jump, swim, stun, or death states
    if (charAnimState_ == CharAnimState::JUMP_START ||
        charAnimState_ == CharAnimState::JUMP_MID ||
        charAnimState_ == CharAnimState::JUMP_END ||
        charAnimState_ == CharAnimState::SWIM ||
        charAnimState_ == CharAnimState::SWIM_IDLE ||
        charAnimState_ == CharAnimState::STUNNED) return;
    charAnimState_ = CharAnimState::LOOTING;
    lastPlayerAnimRequest_ = UINT32_MAX;
}

void AnimationController::stopLooting() {
    if (charAnimState_ != CharAnimState::LOOTING) return;
    charAnimState_ = CharAnimState::IDLE;
    lastPlayerAnimRequest_ = UINT32_MAX;
}

void AnimationController::triggerHitReaction(uint32_t animId) {
    // Hit reactions interrupt spell casting but not jumps/swimming/stun
    if (charAnimState_ == CharAnimState::JUMP_START ||
        charAnimState_ == CharAnimState::JUMP_MID ||
        charAnimState_ == CharAnimState::JUMP_END ||
        charAnimState_ == CharAnimState::SWIM ||
        charAnimState_ == CharAnimState::SWIM_IDLE ||
        charAnimState_ == CharAnimState::STUNNED) return;
    if (charAnimState_ == CharAnimState::SPELL_CASTING ||
        charAnimState_ == CharAnimState::SPELL_PRECAST ||
        charAnimState_ == CharAnimState::SPELL_FINALIZE) {
        spellPrecastAnimId_ = 0;
        spellCastAnimId_ = 0;
        spellCastLoop_ = false;
        spellFinalizeAnimId_ = 0;
    }
    hitReactionAnimId_ = animId;
    charAnimState_ = CharAnimState::HIT_REACTION;
    lastPlayerAnimRequest_ = UINT32_MAX;
}

void AnimationController::setStunned(bool stunned) {
    stunned_ = stunned;
    if (stunned) {
        // Stun overrides most states (not swimming/jumping — those are physics)
        if (charAnimState_ == CharAnimState::SWIM ||
            charAnimState_ == CharAnimState::SWIM_IDLE) return;
        // Interrupt spell casting
        if (charAnimState_ == CharAnimState::SPELL_CASTING ||
            charAnimState_ == CharAnimState::SPELL_PRECAST ||
            charAnimState_ == CharAnimState::SPELL_FINALIZE) {
            spellPrecastAnimId_ = 0;
            spellCastAnimId_ = 0;
            spellCastLoop_ = false;
            spellFinalizeAnimId_ = 0;
        }
        hitReactionAnimId_ = 0;
        charAnimState_ = CharAnimState::STUNNED;
        lastPlayerAnimRequest_ = UINT32_MAX;
    } else {
        if (charAnimState_ == CharAnimState::STUNNED) {
            charAnimState_ = inCombat_ ? CharAnimState::COMBAT_IDLE : CharAnimState::IDLE;
            lastPlayerAnimRequest_ = UINT32_MAX;
        }
    }
}

void AnimationController::setStandState(uint8_t state) {
    if (state == standState_) return;
    standState_ = state;

    if (state == STAND_STATE_STAND) {
        // Standing up — exit animation handled by state machine (!sitting → SIT_UP)
        // sitUpAnim_ is retained from the entry so the correct exit animation plays.
        return;
    }

    // Configure transition/loop/exit animations per stand-state type
    if (state == STAND_STATE_SIT) {
        // Ground sit
        sitDownAnim_ = anim::SIT_GROUND_DOWN;
        sitLoopAnim_ = anim::SITTING;
        sitUpAnim_   = anim::SIT_GROUND_UP;
        charAnimState_ = CharAnimState::SIT_DOWN;
    } else if (state == STAND_STATE_SLEEP) {
        // Sleep
        sitDownAnim_ = anim::SLEEP_DOWN;
        sitLoopAnim_ = anim::SLEEP;
        sitUpAnim_   = anim::SLEEP_UP;
        charAnimState_ = CharAnimState::SIT_DOWN;
    } else if (state == STAND_STATE_KNEEL) {
        // Kneel
        sitDownAnim_ = anim::KNEEL_START;
        sitLoopAnim_ = anim::KNEEL_LOOP;
        sitUpAnim_   = anim::KNEEL_END;
        charAnimState_ = CharAnimState::SIT_DOWN;
    } else if (state >= STAND_STATE_SIT_CHAIR && state <= STAND_STATE_SIT_HIGH) {
        // Chair variants — no transition animation, go directly to loop
        sitDownAnim_ = 0;
        sitUpAnim_   = 0;
        if (state == STAND_STATE_SIT_LOW) {
            sitLoopAnim_ = anim::SIT_CHAIR_LOW;
        } else if (state == STAND_STATE_SIT_HIGH) {
            sitLoopAnim_ = anim::SIT_CHAIR_HIGH;
        } else {
            sitLoopAnim_ = anim::SIT_CHAIR_MED;
        }
        charAnimState_ = CharAnimState::SITTING;
    } else if (state == STAND_STATE_DEAD) {
        // Dead — leave to death handling elsewhere
        sitDownAnim_ = 0;
        sitLoopAnim_ = 0;
        sitUpAnim_   = 0;
        return;
    }
    lastPlayerAnimRequest_ = UINT32_MAX;
}

void AnimationController::setStealthed(bool stealth) {
    if (stealthed_ == stealth) return;
    stealthed_ = stealth;
    lastPlayerAnimRequest_ = UINT32_MAX;
}

std::string AnimationController::getEmoteText(const std::string& emoteName, const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        const auto& info = it->second;
        const std::string& base = (targetName ? info.textTarget : info.textNoTarget);
        if (!base.empty()) {
            return replacePlaceholders(base, targetName);
        }
        if (targetName && !targetName->empty()) {
            return "You " + info.command + " at " + *targetName + ".";
        }
        return "You " + info.command + ".";
    }
    return "";
}

uint32_t AnimationController::getEmoteDbcId(const std::string& emoteName) {
    loadEmotesFromDbc();
    auto it = EMOTE_TABLE.find(emoteName);
    if (it != EMOTE_TABLE.end()) {
        return it->second.dbcId;
    }
    return 0;
}

std::string AnimationController::getEmoteTextByDbcId(uint32_t dbcId, const std::string& senderName,
                                                      const std::string* targetName) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it == EMOTE_BY_DBCID.end()) return "";

    const EmoteInfo& info = *it->second;

    if (targetName && !targetName->empty()) {
        if (!info.othersTarget.empty()) {
            std::string out;
            out.reserve(info.othersTarget.size() + senderName.size() + targetName->size());
            bool firstReplaced = false;
            for (size_t i = 0; i < info.othersTarget.size(); ++i) {
                if (info.othersTarget[i] == '%' && i + 1 < info.othersTarget.size() && info.othersTarget[i + 1] == 's') {
                    out += firstReplaced ? *targetName : senderName;
                    firstReplaced = true;
                    ++i;
                } else {
                    out.push_back(info.othersTarget[i]);
                }
            }
            return out;
        }
        return senderName + " " + info.command + "s at " + *targetName + ".";
    } else {
        if (!info.othersNoTarget.empty()) {
            return replacePlaceholders(info.othersNoTarget, &senderName);
        }
        return senderName + " " + info.command + "s.";
    }
}

uint32_t AnimationController::getEmoteAnimByDbcId(uint32_t dbcId) {
    loadEmotesFromDbc();
    auto it = EMOTE_BY_DBCID.find(dbcId);
    if (it != EMOTE_BY_DBCID.end()) {
        return it->second->animId;
    }
    return 0;
}

// ── Targeting / combat ───────────────────────────────────────────────────────

void AnimationController::setTargetPosition(const glm::vec3* pos) {
    targetPosition_ = pos;
}

void AnimationController::resetCombatVisualState() {
    inCombat_ = false;
    targetPosition_ = nullptr;
    meleeSwingTimer_ = 0.0f;
    meleeSwingCooldown_ = 0.0f;
    specialAttackAnimId_ = 0;
    rangedShootTimer_ = 0.0f;
    rangedAnimId_ = 0;
    spellPrecastAnimId_ = 0;
    spellCastAnimId_ = 0;
    spellCastLoop_ = false;
    spellFinalizeAnimId_ = 0;
    hitReactionAnimId_ = 0;
    stunned_ = false;
    lowHealth_ = false;
    if (charAnimState_ == CharAnimState::SPELL_CASTING ||
        charAnimState_ == CharAnimState::SPELL_PRECAST ||
        charAnimState_ == CharAnimState::SPELL_FINALIZE ||
        charAnimState_ == CharAnimState::HIT_REACTION ||
        charAnimState_ == CharAnimState::STUNNED ||
        charAnimState_ == CharAnimState::RANGED_SHOOT)
        charAnimState_ = CharAnimState::IDLE;
    if (auto* svs = renderer_->getSpellVisualSystem()) svs->reset();
}

bool AnimationController::isMoving() const {
    auto* cameraController = renderer_->getCameraController();
    return cameraController && cameraController->isMoving();
}

// ── Melee combat ─────────────────────────────────────────────────────────────

void AnimationController::triggerMeleeSwing() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown_ > 0.0f) return;
    if (emoteActive_) {
        cancelEmote();
    }
    specialAttackAnimId_ = 0;  // Clear any special attack override
    resolveMeleeAnimId();
    meleeSwingCooldown_ = 0.1f;
    float durationSec = meleeAnimDurationMs_ > 0.0f ? meleeAnimDurationMs_ / 1000.0f : 0.6f;
    if (durationSec < 0.25f) durationSec = 0.25f;
    if (durationSec > 1.0f) durationSec = 1.0f;
    meleeSwingTimer_ = durationSec;
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

void AnimationController::triggerSpecialAttack(uint32_t /*spellId*/) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (meleeSwingCooldown_ > 0.0f) return;
    if (emoteActive_) {
        cancelEmote();
    }

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Choose special attack animation based on equipped weapon type
    uint32_t specAnim = 0;
    if (equippedHasShield_ && has(anim::SHIELD_BASH)) {
        specAnim = anim::SHIELD_BASH;
    } else if ((equippedWeaponInvType_ == game::InvType::TWO_HAND || equippedIs2HLoose_) && has(anim::SPECIAL_2H)) {
        specAnim = anim::SPECIAL_2H;
    } else if (equippedWeaponInvType_ != game::InvType::NON_EQUIP && has(anim::SPECIAL_1H)) {
        specAnim = anim::SPECIAL_1H;
    } else if (has(anim::SPECIAL_UNARMED)) {
        specAnim = anim::SPECIAL_UNARMED;
    } else if (has(anim::SPECIAL_1H)) {
        specAnim = anim::SPECIAL_1H;
    }

    if (specAnim == 0) {
        // No special animation available — fall back to regular melee swing
        triggerMeleeSwing();
        return;
    }

    specialAttackAnimId_ = specAnim;
    meleeSwingCooldown_ = 0.1f;
    // Query the special attack animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == specAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    if (dur > 1.0f) dur = 1.0f;
    meleeSwingTimer_ = dur;
    if (renderer_->getAudioCoordinator()->getActivitySoundManager()) {
        renderer_->getAudioCoordinator()->getActivitySoundManager()->playMeleeSwing();
    }
}

// ── Ranged combat ────────────────────────────────────────────────────────────

void AnimationController::triggerRangedShot() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) return;
    if (rangedShootTimer_ > 0.0f) return;
    if (emoteActive_) cancelEmote();

    auto has = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };

    // Resolve ranged attack animation based on weapon type
    uint32_t shootAnim = 0;
    switch (equippedRangedType_) {
        case RangedWeaponType::BOW:
            if (has(anim::FIRE_BOW))        shootAnim = anim::FIRE_BOW;
            else if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::GUN:
            if (has(anim::ATTACK_RIFLE))    shootAnim = anim::ATTACK_RIFLE;
            break;
        case RangedWeaponType::CROSSBOW:
            if (has(anim::ATTACK_CROSSBOW)) shootAnim = anim::ATTACK_CROSSBOW;
            else if (has(anim::ATTACK_BOW)) shootAnim = anim::ATTACK_BOW;
            break;
        case RangedWeaponType::THROWN:
            if (has(anim::ATTACK_THROWN))    shootAnim = anim::ATTACK_THROWN;
            break;
        default: break;
    }
    if (shootAnim == 0) return;  // Model has no ranged animation

    rangedAnimId_ = shootAnim;

    // Query animation duration
    std::vector<pipeline::M2Sequence> sequences;
    float dur = 0.6f;
    if (characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        for (const auto& seq : sequences) {
            if (seq.id == shootAnim && seq.duration > 0) {
                dur = static_cast<float>(seq.duration) / 1000.0f;
                break;
            }
        }
    }
    if (dur < 0.25f) dur = 0.25f;
    if (dur > 1.5f) dur = 1.5f;
    rangedShootTimer_ = dur;
}

uint32_t AnimationController::resolveMeleeAnimId() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();
    if (!characterRenderer || characterInstanceId == 0) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    // When dual-wielding, bypass cache to alternate main/off-hand animations
    if (!equippedHasOffHand_ && meleeAnimId_ != 0 && characterRenderer->hasAnimation(characterInstanceId, meleeAnimId_)) {
        return meleeAnimId_;
    }

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer->getAnimationSequences(characterInstanceId, sequences)) {
        meleeAnimId_ = 0;
        meleeAnimDurationMs_ = 0.0f;
        return 0;
    }

    auto findDuration = [&](uint32_t id) -> float {
        for (const auto& seq : sequences) {
            if (seq.id == id && seq.duration > 0) {
                return static_cast<float>(seq.duration);
            }
        }
        return 0.0f;
    };

    const uint32_t* attackCandidates;
    size_t candidateCount;
    static const uint32_t candidates2H[] = {anim::ATTACK_2H, anim::ATTACK_1H, anim::ATTACK_UNARMED, anim::ATTACK_2H_LOOSE, anim::PARRY_UNARMED, anim::PARRY_1H};
    static const uint32_t candidates2HLoosePierce[] = {anim::ATTACK_2H_LOOSE_PIERCE, anim::ATTACK_2H_LOOSE, anim::ATTACK_2H, anim::ATTACK_1H, anim::ATTACK_UNARMED};
    static const uint32_t candidates1H[] = {anim::ATTACK_1H, anim::ATTACK_2H, anim::ATTACK_UNARMED, anim::ATTACK_2H_LOOSE, anim::PARRY_UNARMED, anim::PARRY_1H};
    static const uint32_t candidatesDagger[] = {anim::ATTACK_1H_PIERCE, anim::ATTACK_1H, anim::ATTACK_UNARMED};
    static const uint32_t candidatesUnarmed[] = {anim::ATTACK_UNARMED, anim::ATTACK_1H, anim::ATTACK_2H, anim::ATTACK_2H_LOOSE, anim::PARRY_UNARMED, anim::PARRY_1H};
    static const uint32_t candidatesFist[] = {anim::ATTACK_FIST_1H, anim::ATTACK_FIST_1H_OFF, anim::ATTACK_1H, anim::ATTACK_UNARMED, anim::PARRY_FIST_1H, anim::PARRY_1H};
    // Off-hand attack variants (used when dual-wielding on off-hand turn)
    static const uint32_t candidatesOffHand[] = {anim::ATTACK_OFF, anim::ATTACK_1H, anim::ATTACK_UNARMED};
    static const uint32_t candidatesOffHandPierce[] = {anim::ATTACK_OFF_PIERCE, anim::ATTACK_OFF, anim::ATTACK_1H_PIERCE, anim::ATTACK_1H};
    static const uint32_t candidatesOffHandFist[] = {anim::ATTACK_FIST_1H_OFF, anim::ATTACK_OFF, anim::ATTACK_FIST_1H, anim::ATTACK_1H};
    static const uint32_t candidatesOffHandUnarmed[] = {anim::ATTACK_UNARMED_OFF, anim::ATTACK_UNARMED, anim::ATTACK_OFF, anim::ATTACK_1H};

    // Dual-wield: alternate main-hand and off-hand swings
    bool useOffHand = equippedHasOffHand_ && meleeOffHandTurn_;
    meleeOffHandTurn_ = equippedHasOffHand_ ? !meleeOffHandTurn_ : false;

    if (useOffHand) {
        if (equippedIsFist_) {
            attackCandidates = candidatesOffHandFist;
            candidateCount = 4;
        } else if (equippedIsDagger_) {
            attackCandidates = candidatesOffHandPierce;
            candidateCount = 4;
        } else if (equippedWeaponInvType_ == game::InvType::NON_EQUIP) {
            attackCandidates = candidatesOffHandUnarmed;
            candidateCount = 4;
        } else {
            attackCandidates = candidatesOffHand;
            candidateCount = 3;
        }
    } else if (equippedIsFist_) {
        attackCandidates = candidatesFist;
        candidateCount = 6;
    } else if (equippedIsDagger_) {
        attackCandidates = candidatesDagger;
        candidateCount = 3;
    } else if (equippedIs2HLoose_) {
        // Polearm thrust uses pierce variant
        attackCandidates = candidates2HLoosePierce;
        candidateCount = 5;
    } else if (equippedWeaponInvType_ == game::InvType::TWO_HAND) {
        attackCandidates = candidates2H;
        candidateCount = 6;
    } else if (equippedWeaponInvType_ == game::InvType::NON_EQUIP) {
        attackCandidates = candidatesUnarmed;
        candidateCount = 6;
    } else {
        attackCandidates = candidates1H;
        candidateCount = 6;
    }
    for (size_t ci = 0; ci < candidateCount; ci++) {
        uint32_t id = attackCandidates[ci];
        if (characterRenderer->hasAnimation(characterInstanceId, id)) {
            meleeAnimId_ = id;
            meleeAnimDurationMs_ = findDuration(id);
            return meleeAnimId_;
        }
    }

    const uint32_t avoidIds[] = {anim::STAND, anim::DEATH, anim::WALK, anim::RUN, anim::SHUFFLE_LEFT, anim::SHUFFLE_RIGHT, anim::WALK_BACKWARDS, anim::JUMP_START, anim::JUMP, anim::JUMP_END, anim::SWIM_IDLE, anim::SWIM, anim::SITTING};
    auto isAvoid = [&](uint32_t id) -> bool {
        for (uint32_t avoid : avoidIds) {
            if (id == avoid) return true;
        }
        return false;
    };

    uint32_t bestId = 0;
    uint32_t bestDuration = 0;
    for (const auto& seq : sequences) {
        if (seq.duration == 0) continue;
        if (isAvoid(seq.id)) continue;
        if (seq.movingSpeed > 0.1f) continue;
        if (seq.duration < 150 || seq.duration > 2000) continue;
        if (bestId == 0 || seq.duration < bestDuration) {
            bestId = seq.id;
            bestDuration = seq.duration;
        }
    }

    if (bestId == 0) {
        for (const auto& seq : sequences) {
            if (seq.duration == 0) continue;
            if (isAvoid(seq.id)) continue;
            if (bestId == 0 || seq.duration < bestDuration) {
                bestId = seq.id;
                bestDuration = seq.duration;
            }
        }
    }

    meleeAnimId_ = bestId;
    meleeAnimDurationMs_ = static_cast<float>(bestDuration);
    return meleeAnimId_;
}

// ── Effect triggers ──────────────────────────────────────────────────────────

void AnimationController::triggerLevelUpEffect(const glm::vec3& position) {
    auto* levelUpEffect = renderer_->getLevelUpEffect();
    if (!levelUpEffect) return;

    if (!levelUpEffect->isModelLoaded()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (!assetManager) {
                LOG_WARNING("LevelUpEffect: no asset manager available");
            } else {
                auto m2Data = assetManager->readFile("Spells\\LevelUp\\LevelUp.m2");
                auto skinData = assetManager->readFile("Spells\\LevelUp\\LevelUp00.skin");
                LOG_INFO("LevelUpEffect: m2Data=", m2Data.size(), " skinData=", skinData.size());
                if (!m2Data.empty()) {
                    levelUpEffect->loadModel(m2Renderer, m2Data, skinData);
                } else {
                    LOG_WARNING("LevelUpEffect: failed to read Spell\\LevelUp\\LevelUp.m2");
                }
            }
        }
    }

    levelUpEffect->trigger(position);
}

void AnimationController::startChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    auto* chargeEffect = renderer_->getChargeEffect();
    if (!chargeEffect) return;

    if (!chargeEffect->isActive()) {
        auto* m2Renderer = renderer_->getM2Renderer();
        if (m2Renderer) {
            auto* assetManager = core::Application::getInstance().getAssetManager();
            if (assetManager) {
                chargeEffect->tryLoadM2Models(m2Renderer, assetManager);
            }
        }
    }

    chargeEffect->start(position, direction);
}

void AnimationController::emitChargeEffect(const glm::vec3& position, const glm::vec3& direction) {
    if (auto* chargeEffect = renderer_->getChargeEffect()) {
        chargeEffect->emit(position, direction);
    }
}

void AnimationController::stopChargeEffect() {
    if (auto* chargeEffect = renderer_->getChargeEffect()) {
        chargeEffect->stop();
    }
}

// ── Mount ────────────────────────────────────────────────────────────────────

void AnimationController::setMounted(uint32_t mountInstId, uint32_t mountDisplayId, float heightOffset, const std::string& modelPath) {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();

    mountInstanceId_ = mountInstId;
    mountHeightOffset_ = heightOffset;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = renderer_->getCharacterPosition();
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;
    mountActionPhase_ = 0;
    charAnimState_ = CharAnimState::MOUNT;
    if (cameraController) {
        cameraController->setMounted(true);
        cameraController->setMountHeightOffset(heightOffset);
    }

    if (characterRenderer && mountInstId > 0) {
        characterRenderer->dumpAnimations(mountInstId);
    }

    // Discover mount animation capabilities (property-based, not hardcoded IDs)
    LOG_DEBUG("=== Mount Animation Dump (Display ID ", mountDisplayId, ") ===");
    if (characterRenderer) characterRenderer->dumpAnimations(mountInstId);

    std::vector<pipeline::M2Sequence> sequences;
    if (!characterRenderer || !characterRenderer->getAnimationSequences(mountInstId, sequences)) {
        LOG_WARNING("Failed to get animation sequences for mount, using fallback IDs");
        sequences.clear();
    }

    auto findFirst = [&](std::initializer_list<uint32_t> candidates) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer && characterRenderer->hasAnimation(mountInstId, id)) {
                return id;
            }
        }
        return 0;
    };

    // Property-based jump animation discovery with chain-based scoring
    auto discoverJumpSet = [&]() {
        LOG_DEBUG("=== Full sequence table for mount ===");
        for (const auto& seq : sequences) {
            LOG_DEBUG("SEQ id=", seq.id,
                     " dur=", seq.duration,
                     " flags=0x", std::hex, seq.flags, std::dec,
                     " moveSpd=", seq.movingSpeed,
                     " blend=", seq.blendTime,
                     " next=", seq.nextAnimation,
                     " alias=", seq.aliasNext);
        }
        LOG_DEBUG("=== End sequence table ===");

        std::set<uint32_t> forbiddenIds = {53, 54, 16};

        auto scoreNear = [](int a, int b) -> int {
            int d = std::abs(a - b);
            return (d <= 8) ? (20 - d) : 0;
        };

        auto isForbidden = [&](uint32_t id) {
            return forbiddenIds.count(id) != 0;
        };

        auto findSeqById = [&](uint32_t id) -> const pipeline::M2Sequence* {
            for (const auto& s : sequences) {
                if (s.id == id) return &s;
            }
            return nullptr;
        };

        uint32_t runId = findFirst({anim::RUN, anim::WALK});
        uint32_t standId = findFirst({anim::STAND});

        std::vector<uint32_t> loops;
        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop && seq.duration >= 350 && seq.duration <= 1000 &&
                seq.id != runId && seq.id != standId) {
                loops.push_back(seq.id);
            }
        }

        uint32_t loop = 0;
        if (!loops.empty()) {
            uint32_t best = loops[0];
            int bestScore = -999;
            for (uint32_t id : loops) {
                int sc = 0;
                sc += scoreNear(static_cast<int>(id), 38);
                const auto* s = findSeqById(id);
                if (s) sc += (s->duration >= 500 && s->duration <= 800) ? 5 : 0;
                if (sc > bestScore) {
                    bestScore = sc;
                    best = id;
                }
            }
            loop = best;
        }

        uint32_t start = 0, end = 0;
        int bestStart = -999, bestEnd = -999;

        for (const auto& seq : sequences) {
            if (isForbidden(seq.id)) continue;
            bool isLoop = (seq.flags & 0x01) == 0;
            if (isLoop) continue;

            if (seq.duration >= 450 && seq.duration <= 1100) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (loop && (seq.nextAnimation == static_cast<int16_t>(loop) || seq.aliasNext == loop)) sc += 30;
                if (loop && scoreNear(seq.nextAnimation, static_cast<int>(loop)) > 0) sc += 10;
                if (seq.blendTime > 400) sc -= 5;

                if (sc > bestStart) {
                    bestStart = sc;
                    start = seq.id;
                }
            }

            if (seq.duration >= 650 && seq.duration <= 1600) {
                int sc = 0;
                if (loop) sc += scoreNear(static_cast<int>(seq.id), static_cast<int>(loop));
                if (seq.nextAnimation == static_cast<int16_t>(runId) || seq.nextAnimation == static_cast<int16_t>(standId)) sc += 10;
                if (seq.nextAnimation < 0) sc += 5;
                if (sc > bestEnd) {
                    bestEnd = sc;
                    end = seq.id;
                }
            }
        }

        LOG_DEBUG("Property-based jump discovery: start=", start, " loop=", loop, " end=", end,
                 " scores: start=", bestStart, " end=", bestEnd);
        return std::make_tuple(start, loop, end);
    };

    auto [discoveredStart, discoveredLoop, discoveredEnd] = discoverJumpSet();

    mountAnims_.jumpStart = discoveredStart > 0 ? discoveredStart : findFirst({anim::FALL, anim::JUMP_START});
    mountAnims_.jumpLoop  = discoveredLoop > 0 ? discoveredLoop : findFirst({anim::JUMP});
    mountAnims_.jumpEnd   = discoveredEnd > 0 ? discoveredEnd : findFirst({anim::JUMP_END});
    mountAnims_.rearUp    = findFirst({anim::MOUNT_SPECIAL, anim::RUN_RIGHT, anim::FALL});
    mountAnims_.run       = findFirst({anim::RUN, anim::WALK});
    mountAnims_.stand     = findFirst({anim::STAND});
    // Discover flight animations (flying mounts only — may all be 0 for ground mounts)
    mountAnims_.flyIdle      = findFirst({anim::FLY_IDLE});
    mountAnims_.flyForward   = findFirst({anim::FLY_FORWARD, anim::FLY_RUN_2});
    mountAnims_.flyBackwards = findFirst({anim::FLY_BACKWARDS, anim::FLY_WALK_BACKWARDS});
    mountAnims_.flyLeft      = findFirst({anim::FLY_LEFT, anim::FLY_SHUFFLE_LEFT});
    mountAnims_.flyRight     = findFirst({anim::FLY_RIGHT, anim::FLY_SHUFFLE_RIGHT});
    mountAnims_.flyUp        = findFirst({anim::FLY_UP, anim::FLY_RISE});
    mountAnims_.flyDown      = findFirst({anim::FLY_DOWN});

    // Discover idle fidget animations using proper WoW M2 metadata
    mountAnims_.fidgets.clear();
    core::Logger::getInstance().debug("Scanning for fidget animations in ", sequences.size(), " sequences");

    core::Logger::getInstance().debug("=== ALL potential fidgets (no metadata filter) ===");
    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary) {
            core::Logger::getInstance().debug("  ALL: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " flags=0x", std::hex, seq.flags, std::dec,
                " next=", seq.nextAnimation);
        }
    }

    for (const auto& seq : sequences) {
        bool isLoop = (seq.flags & 0x01) == 0;
        bool hasFrequency = seq.frequency > 0;
        bool hasReplay = seq.replayMax > 0;
        bool isStationary = std::abs(seq.movingSpeed) < 0.05f;
        bool reasonableDuration = seq.duration >= 400 && seq.duration <= 2500;

        if (!isLoop && reasonableDuration && isStationary && (hasFrequency || hasReplay)) {
            core::Logger::getInstance().debug("  Candidate: id=", seq.id,
                " dur=", seq.duration, "ms",
                " freq=", seq.frequency,
                " replay=", seq.replayMin, "-", seq.replayMax,
                " next=", seq.nextAnimation,
                " speed=", seq.movingSpeed);
        }

        bool isDeathOrWound = (seq.id >= 5 && seq.id <= 9);
        bool isAttackOrCombat = (seq.id >= 11 && seq.id <= 21);
        bool isSpecial = (seq.id == 2 || seq.id == 3);

        if (!isLoop && (hasFrequency || hasReplay) && isStationary && reasonableDuration &&
            !isDeathOrWound && !isAttackOrCombat && !isSpecial) {
            bool chainsToStand = (seq.nextAnimation == static_cast<int16_t>(mountAnims_.stand)) ||
                                 (seq.aliasNext == mountAnims_.stand) ||
                                 (seq.nextAnimation == -1);

            mountAnims_.fidgets.push_back(seq.id);
            core::Logger::getInstance().debug("  >> Selected fidget: id=", seq.id,
                (chainsToStand ? " (chains to stand)" : ""));
        }
    }

    if (mountAnims_.run == 0) mountAnims_.run = mountAnims_.stand;

    core::Logger::getInstance().debug("Mount animation set: jumpStart=", mountAnims_.jumpStart,
        " jumpLoop=", mountAnims_.jumpLoop,
        " jumpEnd=", mountAnims_.jumpEnd,
        " rearUp=", mountAnims_.rearUp,
        " run=", mountAnims_.run,
        " stand=", mountAnims_.stand,
        " fidgets=", mountAnims_.fidgets.size());

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        bool isFlying = taxiFlight_;
        renderer_->getAudioCoordinator()->getMountSoundManager()->onMount(mountDisplayId, isFlying, modelPath);
    }
}

void AnimationController::clearMount() {
    mountInstanceId_ = 0;
    mountHeightOffset_ = 0.0f;
    mountPitch_ = 0.0f;
    mountRoll_ = 0.0f;
    mountSeatAttachmentId_ = -1;
    smoothedMountSeatPos_ = glm::vec3(0.0f);
    mountSeatSmoothingInit_ = false;
    mountAction_ = MountAction::None;
    mountActionPhase_ = 0;
    charAnimState_ = CharAnimState::IDLE;
    if (auto* cameraController = renderer_->getCameraController()) {
        cameraController->setMounted(false);
        cameraController->setMountHeightOffset(0.0f);
    }

    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->onDismount();
    }
}

// ── Query helpers ────────────────────────────────────────────────────────────

bool AnimationController::isFootstepAnimationState() const {
    return charAnimState_ == CharAnimState::WALK || charAnimState_ == CharAnimState::RUN;
}

// ── Melee timers ─────────────────────────────────────────────────────────────

void AnimationController::updateMeleeTimers(float deltaTime) {
    if (meleeSwingCooldown_ > 0.0f) {
        meleeSwingCooldown_ = std::max(0.0f, meleeSwingCooldown_ - deltaTime);
    }
    if (meleeSwingTimer_ > 0.0f) {
        meleeSwingTimer_ = std::max(0.0f, meleeSwingTimer_ - deltaTime);
        if (meleeSwingTimer_ <= 0.0f) specialAttackAnimId_ = 0;
    }
    // Ranged shot timer (same pattern as melee)
    if (rangedShootTimer_ > 0.0f) {
        rangedShootTimer_ = std::max(0.0f, rangedShootTimer_ - deltaTime);
    }
}

// ── Character animation state machine ────────────────────────────────────────

void AnimationController::updateCharacterAnimation() {
    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();


    CharAnimState newState = charAnimState_;

    const bool rawMoving = cameraController->isMoving();
    const bool rawSprinting = cameraController->isSprinting();
    constexpr float kLocomotionStopGraceSec = 0.12f;
    if (rawMoving) {
        locomotionStopGraceTimer_ = kLocomotionStopGraceSec;
        locomotionWasSprinting_ = rawSprinting;
    } else {
        locomotionStopGraceTimer_ = std::max(0.0f, locomotionStopGraceTimer_ - lastDeltaTime_);
    }
    bool moving = rawMoving || locomotionStopGraceTimer_ > 0.0f;
    bool movingForward = cameraController->isMovingForward();
    bool movingBackward = cameraController->isMovingBackward();
    bool autoRunning = cameraController->isAutoRunning();
    bool strafeLeft = cameraController->isStrafingLeft();
    bool strafeRight = cameraController->isStrafingRight();
    bool pureStrafe = !movingForward && !movingBackward && !autoRunning;
    bool anyStrafeLeft = strafeLeft && !strafeRight && pureStrafe;
    bool anyStrafeRight = strafeRight && !strafeLeft && pureStrafe;
    bool grounded = cameraController->isGrounded();
    bool jumping = cameraController->isJumping();
    bool sprinting = rawSprinting || (!rawMoving && moving && locomotionWasSprinting_);
    bool sitting = cameraController->isSitting();
    bool swim = cameraController->isSwimming();
    bool forceMelee = meleeSwingTimer_ > 0.0f && grounded && !swim;
    bool forceRanged = rangedShootTimer_ > 0.0f && grounded && !swim;

    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
    float characterYaw = renderer_->getCharacterYaw();

    // When mounted, force MOUNT state and skip normal transitions
    if (isMounted()) {
        newState = CharAnimState::MOUNT;
        charAnimState_ = newState;

        // Rider animation — defaults to MOUNT, but uses MOUNT_FLIGHT_* variants when flying
        uint32_t riderAnim = anim::MOUNT;
        if (cameraController->isFlyingActive()) {
            auto hasRider = [&](uint32_t id) { return characterRenderer->hasAnimation(characterInstanceId, id); };
            if (moving) {
                if (cameraController->isAscending() && hasRider(anim::MOUNT_FLIGHT_UP))
                    riderAnim = anim::MOUNT_FLIGHT_UP;
                else if (cameraController->isDescending() && hasRider(anim::MOUNT_FLIGHT_DOWN))
                    riderAnim = anim::MOUNT_FLIGHT_DOWN;
                else if (hasRider(anim::MOUNT_FLIGHT_FORWARD))
                    riderAnim = anim::MOUNT_FLIGHT_FORWARD;
            } else {
                if (hasRider(anim::MOUNT_FLIGHT_IDLE))
                    riderAnim = anim::MOUNT_FLIGHT_IDLE;
            }
        }

        uint32_t currentAnimId = 0;
        float currentAnimTimeMs = 0.0f, currentAnimDurationMs = 0.0f;
        bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
        if (!haveState || currentAnimId != riderAnim) {
            characterRenderer->playAnimation(characterInstanceId, riderAnim, true);
        }

        float mountBob = 0.0f;
        float mountYawRad = glm::radians(characterYaw);
        if (mountInstanceId_ > 0) {
            characterRenderer->setInstancePosition(mountInstanceId_, characterPosition);

            if (!taxiFlight_ && moving && lastDeltaTime_ > 0.0f) {
                float currentYawDeg = characterYaw;
                float turnRate = (currentYawDeg - prevMountYaw_) / lastDeltaTime_;
                while (turnRate > 180.0f) turnRate -= 360.0f;
                while (turnRate < -180.0f) turnRate += 360.0f;

                float targetLean = glm::clamp(turnRate * 0.15f, -0.25f, 0.25f);
                mountRoll_ = glm::mix(mountRoll_, targetLean, lastDeltaTime_ * 6.0f);
                prevMountYaw_ = currentYawDeg;
            } else {
                mountRoll_ = glm::mix(mountRoll_, 0.0f, lastDeltaTime_ * 8.0f);
            }

            characterRenderer->setInstanceRotation(mountInstanceId_, glm::vec3(mountPitch_, mountRoll_, mountYawRad));

            auto pickMountAnim = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
                for (uint32_t id : candidates) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, id)) {
                        return id;
                    }
                }
                return fallback;
            };

            uint32_t mountAnimId = anim::STAND;

            uint32_t curMountAnim = 0;
            float curMountTime = 0, curMountDur = 0;
            bool haveMountState = characterRenderer->getAnimationState(mountInstanceId_, curMountAnim, curMountTime, curMountDur);

            if (taxiFlight_) {
                if (!taxiAnimsLogged_) {
                    taxiAnimsLogged_ = true;
                    LOG_INFO("Taxi flight active: mountInstanceId_=", mountInstanceId_,
                             " curMountAnim=", curMountAnim, " haveMountState=", haveMountState);
                    std::vector<pipeline::M2Sequence> seqs;
                    if (characterRenderer->getAnimationSequences(mountInstanceId_, seqs)) {
                        std::string animList;
                        for (const auto& s : seqs) {
                            if (!animList.empty()) animList += ", ";
                            animList += std::to_string(s.id);
                        }
                        LOG_INFO("Taxi mount available animations: [", animList, "]");
                    }
                }

                uint32_t flyAnims[] = {anim::FLY_FORWARD, anim::FLY_IDLE, anim::FLY_RUN_2, anim::FLY_SPELL, anim::FLY_RISE, anim::SPELL_KNEEL_LOOP, anim::FLY_CUSTOM_SPELL_10, anim::DEAD, anim::RUN};
                mountAnimId = anim::STAND;
                for (uint32_t fa : flyAnims) {
                    if (characterRenderer->hasAnimation(mountInstanceId_, fa)) {
                        mountAnimId = fa;
                        break;
                    }
                }

                if (!haveMountState || curMountAnim != mountAnimId) {
                    LOG_INFO("Taxi mount: playing animation ", mountAnimId);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                }

                goto taxi_mount_done;
            } else {
                taxiAnimsLogged_ = false;
            }

            // Check for jump trigger
            if (cameraController->isJumpKeyPressed() && grounded && mountAction_ == MountAction::None) {
                if (moving && mountAnims_.jumpLoop > 0) {
                    LOG_DEBUG("Mount jump triggered while moving: using jumpLoop anim ", mountAnims_.jumpLoop);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                    mountAction_ = MountAction::Jump;
                    mountActionPhase_ = 1;
                    mountAnimId = mountAnims_.jumpLoop;
                    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                        renderer_->getAudioCoordinator()->getMountSoundManager()->playJumpSound();
                    }
                    if (cameraController) {
                        cameraController->triggerMountJump();
                    }
                } else if (!moving && mountAnims_.rearUp > 0) {
                    LOG_DEBUG("Mount rear-up triggered: playing rearUp anim ", mountAnims_.rearUp);
                    characterRenderer->playAnimation(mountInstanceId_, mountAnims_.rearUp, false);
                    mountAction_ = MountAction::RearUp;
                    mountActionPhase_ = 0;
                    mountAnimId = mountAnims_.rearUp;
                    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                        renderer_->getAudioCoordinator()->getMountSoundManager()->playRearUpSound();
                    }
                }
            }

            // Handle active mount actions (jump chaining or rear-up)
            if (mountAction_ != MountAction::None) {
                bool animFinished = haveMountState && curMountDur > 0.1f &&
                                   (curMountTime >= curMountDur - 0.05f);

                if (mountAction_ == MountAction::Jump) {
                    if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop > 0) {
                        LOG_DEBUG("Mount jump: phase 0→1 (JumpStart→JumpLoop anim ", mountAnims_.jumpLoop, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpLoop, true);
                        mountActionPhase_ = 1;
                        mountAnimId = mountAnims_.jumpLoop;
                    } else if (mountActionPhase_ == 0 && animFinished && mountAnims_.jumpLoop == 0) {
                        LOG_DEBUG("Mount jump: phase 0→1 (no JumpLoop, holding JumpStart)");
                        mountActionPhase_ = 1;
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd > 0) {
                        LOG_DEBUG("Mount jump: phase 1→2 (landed, JumpEnd anim ", mountAnims_.jumpEnd, ")");
                        characterRenderer->playAnimation(mountInstanceId_, mountAnims_.jumpEnd, false);
                        mountActionPhase_ = 2;
                        mountAnimId = mountAnims_.jumpEnd;
                        if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
                            renderer_->getAudioCoordinator()->getMountSoundManager()->playLandSound();
                        }
                    } else if (mountActionPhase_ == 1 && grounded && mountAnims_.jumpEnd == 0) {
                        LOG_DEBUG("Mount jump: phase 1→done (landed, no JumpEnd, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else if (mountActionPhase_ == 2 && animFinished) {
                        LOG_DEBUG("Mount jump: phase 2→done (JumpEnd finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand), ")");
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;
                    }
                } else if (mountAction_ == MountAction::RearUp) {
                    if (animFinished) {
                        LOG_DEBUG("Mount rear-up: finished, returning to ",
                                 moving ? "run" : "stand", " anim ", (moving ? mountAnims_.run : mountAnims_.stand));
                        mountAction_ = MountAction::None;
                        mountAnimId = moving ? mountAnims_.run : mountAnims_.stand;
                        characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
                    } else {
                        mountAnimId = curMountAnim;
                    }
                }
            } else if (moving) {
                const bool flying = cameraController->isFlyingActive();
                const bool mountSwim = cameraController->isSwimming();
                if (flying) {
                    // Directional flying animations for mount
                    if (cameraController->isAscending()) {
                        mountAnimId = pickMountAnim({anim::FLY_UP, anim::FLY_FORWARD}, anim::RUN);
                    } else if (cameraController->isDescending()) {
                        mountAnimId = pickMountAnim({anim::FLY_DOWN, anim::FLY_FORWARD}, anim::RUN);
                    } else if (anyStrafeLeft) {
                        mountAnimId = pickMountAnim({anim::FLY_LEFT, anim::FLY_SHUFFLE_LEFT, anim::FLY_FORWARD}, anim::RUN);
                    } else if (anyStrafeRight) {
                        mountAnimId = pickMountAnim({anim::FLY_RIGHT, anim::FLY_SHUFFLE_RIGHT, anim::FLY_FORWARD}, anim::RUN);
                    } else if (movingBackward) {
                        mountAnimId = pickMountAnim({anim::FLY_BACKWARDS, anim::FLY_WALK_BACKWARDS, anim::FLY_FORWARD}, anim::RUN);
                    } else {
                        mountAnimId = pickMountAnim({anim::FLY_FORWARD, anim::FLY_IDLE}, anim::RUN);
                    }
                } else if (mountSwim) {
                    // Mounted swimming animations
                    if (anyStrafeLeft) {
                        mountAnimId = pickMountAnim({anim::MOUNT_SWIM_LEFT, anim::SWIM_LEFT, anim::MOUNT_SWIM}, anim::RUN);
                    } else if (anyStrafeRight) {
                        mountAnimId = pickMountAnim({anim::MOUNT_SWIM_RIGHT, anim::SWIM_RIGHT, anim::MOUNT_SWIM}, anim::RUN);
                    } else if (movingBackward) {
                        mountAnimId = pickMountAnim({anim::MOUNT_SWIM_BACKWARDS, anim::SWIM_BACKWARDS, anim::MOUNT_SWIM}, anim::RUN);
                    } else {
                        mountAnimId = pickMountAnim({anim::MOUNT_SWIM, anim::SWIM}, anim::RUN);
                    }
                } else if (anyStrafeLeft) {
                    mountAnimId = pickMountAnim({anim::MOUNT_RUN_LEFT, anim::RUN_LEFT, anim::SHUFFLE_LEFT, anim::RUN}, anim::RUN);
                } else if (anyStrafeRight) {
                    mountAnimId = pickMountAnim({anim::MOUNT_RUN_RIGHT, anim::RUN_RIGHT, anim::SHUFFLE_RIGHT, anim::RUN}, anim::RUN);
                } else if (movingBackward) {
                    mountAnimId = pickMountAnim({anim::MOUNT_WALK_BACKWARDS, anim::WALK_BACKWARDS}, anim::RUN);
                } else {
                    mountAnimId = anim::RUN;
                }
            } else if (!moving && cameraController->isSwimming()) {
                // Mounted swim idle
                mountAnimId = pickMountAnim({anim::MOUNT_SWIM_IDLE, anim::SWIM_IDLE}, anim::STAND);
            } else if (!moving && cameraController->isFlyingActive()) {
                // Hovering in flight — use FLY_IDLE instead of STAND
                if (cameraController->isAscending()) {
                    mountAnimId = pickMountAnim({anim::FLY_UP, anim::FLY_IDLE}, anim::STAND);
                } else if (cameraController->isDescending()) {
                    mountAnimId = pickMountAnim({anim::FLY_DOWN, anim::FLY_IDLE}, anim::STAND);
                } else {
                    mountAnimId = pickMountAnim({anim::FLY_IDLE, anim::FLY_FORWARD}, anim::STAND);
                }
            }

            // Cancel active fidget immediately if movement starts
            if (moving && mountActiveFidget_ != 0) {
                mountActiveFidget_ = 0;
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, true);
            }

            // Check if active fidget has completed
            if (!moving && mountActiveFidget_ != 0) {
                uint32_t curAnim = 0;
                float curTime = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(mountInstanceId_, curAnim, curTime, curDur)) {
                    if (curAnim != mountActiveFidget_ || curTime >= curDur * 0.95f) {
                        mountActiveFidget_ = 0;
                        LOG_DEBUG("Mount fidget completed");
                    }
                }
            }

            // Idle fidgets
            if (!moving && mountAction_ == MountAction::None && mountActiveFidget_ == 0 && !mountAnims_.fidgets.empty()) {
                mountIdleFidgetTimer_ += lastDeltaTime_;
                static std::mt19937 idleRng(std::random_device{}());
                static float nextFidgetTime = std::uniform_real_distribution<float>(6.0f, 12.0f)(idleRng);

                if (mountIdleFidgetTimer_ >= nextFidgetTime) {
                    std::uniform_int_distribution<size_t> dist(0, mountAnims_.fidgets.size() - 1);
                    uint32_t fidgetAnim = mountAnims_.fidgets[dist(idleRng)];

                    characterRenderer->playAnimation(mountInstanceId_, fidgetAnim, false);
                    mountActiveFidget_ = fidgetAnim;
                    mountIdleFidgetTimer_ = 0.0f;
                    nextFidgetTime = std::uniform_real_distribution<float>(6.0f, 12.0f)(idleRng);

                    LOG_DEBUG("Mount idle fidget: playing anim ", fidgetAnim);
                }
            }
            if (moving) {
                mountIdleFidgetTimer_ = 0.0f;
            }

            // Idle ambient sounds
            if (!moving && renderer_->getAudioCoordinator()->getMountSoundManager()) {
                mountIdleSoundTimer_ += lastDeltaTime_;
                static std::mt19937 soundRng(std::random_device{}());
                static float nextIdleSoundTime = std::uniform_real_distribution<float>(45.0f, 90.0f)(soundRng);

                if (mountIdleSoundTimer_ >= nextIdleSoundTime) {
                    renderer_->getAudioCoordinator()->getMountSoundManager()->playIdleSound();
                    mountIdleSoundTimer_ = 0.0f;
                    nextIdleSoundTime = std::uniform_real_distribution<float>(45.0f, 90.0f)(soundRng);
                }
            } else if (moving) {
                mountIdleSoundTimer_ = 0.0f;
            }

            // Only update animation if changed and not in action or fidget
            if (mountAction_ == MountAction::None && mountActiveFidget_ == 0 && (!haveMountState || curMountAnim != mountAnimId)) {
                bool loop = true;
                characterRenderer->playAnimation(mountInstanceId_, mountAnimId, loop);
            }

            taxi_mount_done:
            mountBob = 0.0f;
            if (moving && haveMountState && curMountDur > 1.0f) {
                float wrappedTime = curMountTime;
                while (wrappedTime >= curMountDur) {
                    wrappedTime -= curMountDur;
                }
                float norm = wrappedTime / curMountDur;
                float bobSpeed = taxiFlight_ ? 2.0f : 1.0f;
                mountBob = std::sin(norm * 2.0f * 3.14159f * bobSpeed) * 0.12f;
            }
        }

        // Use mount's attachment point for proper bone-driven rider positioning.
        if (taxiFlight_) {
            glm::mat4 mountSeatTransform(1.0f);
            bool haveSeat = false;
            static constexpr uint32_t kTaxiSeatAttachmentId = 0;
            if (mountSeatAttachmentId_ == -1) {
                mountSeatAttachmentId_ = static_cast<int>(kTaxiSeatAttachmentId);
            }
            if (mountSeatAttachmentId_ >= 0) {
                haveSeat = characterRenderer->getAttachmentTransform(
                    mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }

            if (haveSeat) {
                glm::vec3 targetRiderPos = glm::vec3(mountSeatTransform[3]) + glm::vec3(0.0f, 0.0f, 0.02f);
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
                characterRenderer->setInstancePosition(characterInstanceId, targetRiderPos);
            } else {
                mountSeatSmoothingInit_ = false;
                glm::vec3 playerPos = characterPosition + glm::vec3(0.0f, 0.0f, mountHeightOffset_ + 0.10f);
                characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            }

            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            float mountYawRadVal = glm::radians(characterYaw);
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, mountYawRadVal));
            return;
        }

        // Ground mounts: try a seat attachment first.
        glm::mat4 mountSeatTransform;
        bool haveSeat = false;
        if (mountSeatAttachmentId_ >= 0) {
            haveSeat = characterRenderer->getAttachmentTransform(
                mountInstanceId_, static_cast<uint32_t>(mountSeatAttachmentId_), mountSeatTransform);
        } else if (mountSeatAttachmentId_ == -1) {
            static constexpr uint32_t kSeatAttachments[] = {0, 5, 6, 7, 8};
            for (uint32_t attId : kSeatAttachments) {
                if (characterRenderer->getAttachmentTransform(mountInstanceId_, attId, mountSeatTransform)) {
                    mountSeatAttachmentId_ = static_cast<int>(attId);
                    haveSeat = true;
                    break;
                }
            }
            if (!haveSeat) {
                mountSeatAttachmentId_ = -2;
            }
        }

        if (haveSeat) {
            glm::vec3 mountSeatPos = glm::vec3(mountSeatTransform[3]);
            glm::vec3 seatOffset = glm::vec3(0.0f, 0.0f, taxiFlight_ ? 0.04f : 0.08f);
            glm::vec3 targetRiderPos = mountSeatPos + seatOffset;
            if (moving) {
                mountSeatSmoothingInit_ = false;
                smoothedMountSeatPos_ = targetRiderPos;
            } else if (!mountSeatSmoothingInit_) {
                smoothedMountSeatPos_ = targetRiderPos;
                mountSeatSmoothingInit_ = true;
            } else {
                float smoothHz = taxiFlight_ ? 10.0f : 14.0f;
                float alpha = 1.0f - std::exp(-smoothHz * std::max(lastDeltaTime_, 0.001f));
                smoothedMountSeatPos_ = glm::mix(smoothedMountSeatPos_, targetRiderPos, alpha);
            }

            characterRenderer->setInstancePosition(characterInstanceId, smoothedMountSeatPos_);

            float yawRad = glm::radians(characterYaw);
            float riderPitch = mountPitch_ * 0.35f;
            float riderRoll = mountRoll_ * 0.35f;
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(riderPitch, riderRoll, yawRad));
        } else {
            mountSeatSmoothingInit_ = false;
            float yawRad = glm::radians(characterYaw);
            glm::mat4 mountRotation = glm::mat4(1.0f);
            mountRotation = glm::rotate(mountRotation, yawRad, glm::vec3(0.0f, 0.0f, 1.0f));
            mountRotation = glm::rotate(mountRotation, mountRoll_, glm::vec3(1.0f, 0.0f, 0.0f));
            mountRotation = glm::rotate(mountRotation, mountPitch_, glm::vec3(0.0f, 1.0f, 0.0f));
            glm::vec3 localOffset(0.0f, 0.0f, mountHeightOffset_ + mountBob);
            glm::vec3 worldOffset = glm::vec3(mountRotation * glm::vec4(localOffset, 0.0f));
            glm::vec3 playerPos = characterPosition + worldOffset;
            characterRenderer->setInstancePosition(characterInstanceId, playerPos);
            characterRenderer->setInstanceRotation(characterInstanceId, glm::vec3(mountPitch_, mountRoll_, yawRad));
        }
        return;
    }

    if (!forceMelee && !forceRanged) switch (charAnimState_) {
        case CharAnimState::IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (inCombat_ && grounded) {
                // Play unsheathe one-shot before entering combat idle
                if (characterRenderer && characterInstanceId > 0 &&
                    characterRenderer->hasAnimation(characterInstanceId, anim::UNSHEATHE)) {
                    newState = CharAnimState::UNSHEATHE;
                } else {
                    newState = CharAnimState::COMBAT_IDLE;
                }
            }
            break;

        case CharAnimState::WALK:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (sprinting) {
                newState = CharAnimState::RUN;
            }
            break;

        case CharAnimState::RUN:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (!moving) {
                newState = CharAnimState::IDLE;
            } else if (!sprinting) {
                newState = CharAnimState::WALK;
            }
            break;

        case CharAnimState::JUMP_START:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            } else {
                newState = CharAnimState::JUMP_MID;
            }
            break;

        case CharAnimState::JUMP_MID:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (grounded) {
                newState = CharAnimState::JUMP_END;
            }
            break;

        case CharAnimState::JUMP_END:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::SIT_DOWN:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                // Stand up requested — play exit animation if available and not moving
                if (sitUpAnim_ != 0 && !moving) {
                    newState = CharAnimState::SIT_UP;
                } else {
                    newState = CharAnimState::IDLE;
                }
            } else if (sitDownAnim_ != 0 && characterRenderer && characterInstanceId > 0) {
                // Auto-chain: when sit-down one-shot finishes → enter loop
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    // Renderer auto-returns one-shots to STAND — detect that OR normal completion
                    if (curId != sitDownAnim_ || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        newState = CharAnimState::SITTING;
                    }
                }
            }
            break;

        case CharAnimState::SITTING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!sitting) {
                if (sitUpAnim_ != 0 && !moving) {
                    newState = CharAnimState::SIT_UP;
                } else {
                    newState = CharAnimState::IDLE;
                }
            }
            break;

        case CharAnimState::SIT_UP:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (moving) {
                // Movement cancels exit animation
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    // Renderer auto-returns one-shots to STAND — detect that OR normal completion
                    if (curId != (sitUpAnim_ ? sitUpAnim_ : anim::SIT_GROUND_UP)
                            || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        newState = CharAnimState::IDLE;
                    }
                }
            }
            break;

        case CharAnimState::EMOTE:
            if (swim) {
                cancelEmote();
                newState = CharAnimState::SWIM_IDLE;
            } else if (jumping || !grounded) {
                cancelEmote();
                newState = CharAnimState::JUMP_START;
            } else if (moving) {
                cancelEmote();
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (sitting) {
                cancelEmote();
                newState = CharAnimState::SIT_DOWN;
            } else if (!emoteLoop_ && characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    // Renderer auto-returns one-shots to STAND — detect that OR normal completion
                    if (curId != emoteAnimId_ || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        cancelEmote();
                        newState = CharAnimState::IDLE;
                    }
                }
            }
            break;

        case CharAnimState::LOOTING:
            // Cancel loot animation on movement, jump, swim, combat
            if (swim) {
                stopLooting();
                newState = CharAnimState::SWIM_IDLE;
            } else if (jumping || !grounded) {
                stopLooting();
                newState = CharAnimState::JUMP_START;
            } else if (moving) {
                stopLooting();
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            }
            break;

        case CharAnimState::SWIM_IDLE:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (moving) {
                newState = CharAnimState::SWIM;
            }
            break;

        case CharAnimState::SWIM:
            if (!swim) {
                newState = moving ? CharAnimState::WALK : CharAnimState::IDLE;
            } else if (!moving) {
                newState = CharAnimState::SWIM_IDLE;
            }
            break;

        case CharAnimState::MELEE_SWING:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (sitting) {
                newState = CharAnimState::SIT_DOWN;
            } else if (inCombat_) {
                newState = CharAnimState::COMBAT_IDLE;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::RANGED_SHOOT:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (inCombat_) {
                newState = CharAnimState::RANGED_LOAD;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::RANGED_LOAD:
            if (swim) {
                newState = CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (inCombat_) {
                newState = CharAnimState::COMBAT_IDLE;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::MOUNT:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (sitting && grounded) {
                newState = CharAnimState::SIT_DOWN;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else {
                newState = CharAnimState::IDLE;
            }
            break;

        case CharAnimState::COMBAT_IDLE:
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                newState = CharAnimState::JUMP_MID;
            } else if (moving && sprinting) {
                newState = CharAnimState::RUN;
            } else if (moving) {
                newState = CharAnimState::WALK;
            } else if (!inCombat_) {
                // Play sheathe one-shot before returning to idle
                if (characterRenderer && characterInstanceId > 0 &&
                    characterRenderer->hasAnimation(characterInstanceId, anim::SHEATHE)) {
                    newState = CharAnimState::SHEATHE;
                } else {
                    newState = CharAnimState::IDLE;
                }
            }
            break;

        case CharAnimState::CHARGE:
            break;

        case CharAnimState::UNSHEATHE:
            // One-shot weapon draw: when complete → COMBAT_IDLE
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving) {
                newState = inCombat_ ? (sprinting ? CharAnimState::RUN : CharAnimState::WALK)
                                     : (sprinting ? CharAnimState::RUN : CharAnimState::WALK);
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    if (curId != anim::UNSHEATHE || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        newState = CharAnimState::COMBAT_IDLE;
                    }
                }
            }
            break;

        case CharAnimState::SHEATHE:
            // One-shot weapon put-away: when complete → IDLE
            if (swim) {
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving) {
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (inCombat_) {
                // Re-entered combat during sheathe — go straight to combat idle
                newState = CharAnimState::COMBAT_IDLE;
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    if (curId != anim::SHEATHE || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        newState = CharAnimState::IDLE;
                    }
                }
            }
            break;

        case CharAnimState::SPELL_PRECAST:
            // One-shot wind-up: auto-advance to SPELL_CASTING when complete
            if (swim) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = CharAnimState::JUMP_MID;
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                uint32_t expectedAnim = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    if (curId != expectedAnim || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        // Precast finished → advance to casting phase
                        newState = CharAnimState::SPELL_CASTING;
                    }
                }
            }
            break;

        case CharAnimState::SPELL_CASTING:
            // Spell cast loop holds until interrupted by movement, jump, swim, or stopSpellCast()
            if (swim) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = CharAnimState::JUMP_START;
            } else if (!grounded) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = CharAnimState::JUMP_MID;
            } else if (moving) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            }
            // Looping cast stays until stopSpellCast() is called externally
            break;

        case CharAnimState::SPELL_FINALIZE: {
            // One-shot release: play finalize anim completely, then return to idle
            if (swim) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (!grounded && jumping) {
                spellPrecastAnimId_ = 0; spellCastAnimId_ = 0; spellFinalizeAnimId_ = 0;
                newState = CharAnimState::JUMP_START;
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                // Determine which animation we expect to be playing
                uint32_t expectedAnim = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                                      : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    if (curId != expectedAnim || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        // Finalization complete → return to idle
                        spellPrecastAnimId_ = 0;
                        spellCastAnimId_ = 0;
                        spellFinalizeAnimId_ = 0;
                        newState = inCombat_ ? CharAnimState::COMBAT_IDLE : CharAnimState::IDLE;
                    }
                }
            }
            break;
        }

        case CharAnimState::HIT_REACTION:
            // One-shot reaction: exit when animation finishes
            if (swim) {
                hitReactionAnimId_ = 0;
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            } else if (moving) {
                hitReactionAnimId_ = 0;
                newState = sprinting ? CharAnimState::RUN : CharAnimState::WALK;
            } else if (characterRenderer && characterInstanceId > 0) {
                uint32_t curId = 0; float curT = 0.0f, curDur = 0.0f;
                uint32_t expectedHitAnim = hitReactionAnimId_ ? hitReactionAnimId_ : anim::COMBAT_WOUND;
                if (characterRenderer->getAnimationState(characterInstanceId, curId, curT, curDur)) {
                    // Renderer auto-returns one-shots to STAND — detect that OR normal completion
                    if (curId != expectedHitAnim || (curDur > 0.1f && curT >= curDur - 0.05f)) {
                        hitReactionAnimId_ = 0;
                        newState = inCombat_ ? CharAnimState::COMBAT_IDLE : CharAnimState::IDLE;
                    }
                }
            }
            break;

        case CharAnimState::STUNNED:
            // Stun holds until setStunned(false) is called.
            // Only swim can break it (physics override).
            if (swim) {
                stunned_ = false;
                newState = moving ? CharAnimState::SWIM : CharAnimState::SWIM_IDLE;
            }
            break;
    }

    // Stun overrides melee/charge (can't act while stunned)
    if (stunned_ && newState != CharAnimState::SWIM && newState != CharAnimState::SWIM_IDLE
        && newState != CharAnimState::STUNNED) {
        newState = CharAnimState::STUNNED;
    }

    if (forceMelee && !stunned_) {
        newState = CharAnimState::MELEE_SWING;
        spellPrecastAnimId_ = 0;
        spellCastAnimId_ = 0;
        spellFinalizeAnimId_ = 0;
        hitReactionAnimId_ = 0;
    }

    if (forceRanged && !stunned_ && !forceMelee) {
        newState = CharAnimState::RANGED_SHOOT;
        spellPrecastAnimId_ = 0;
        spellCastAnimId_ = 0;
        spellFinalizeAnimId_ = 0;
        hitReactionAnimId_ = 0;
    }

    if (charging_ && !stunned_) {
        newState = CharAnimState::CHARGE;
        spellPrecastAnimId_ = 0;
        spellCastAnimId_ = 0;
        spellFinalizeAnimId_ = 0;
        hitReactionAnimId_ = 0;
    }

    if (newState != charAnimState_) {
        charAnimState_ = newState;
    }

    auto pickFirstAvailable = [&](std::initializer_list<uint32_t> candidates, uint32_t fallback) -> uint32_t {
        for (uint32_t id : candidates) {
            if (characterRenderer->hasAnimation(characterInstanceId, id)) {
                return id;
            }
        }
        return fallback;
    };

    uint32_t animId = anim::STAND;
    bool loop = true;

    switch (charAnimState_) {
        case CharAnimState::IDLE:
            if (lowHealth_ && characterRenderer->hasAnimation(characterInstanceId, anim::STAND_WOUND)) {
                animId = anim::STAND_WOUND;
            } else {
                animId = anim::STAND;
            }
            loop = true;
            break;
        case CharAnimState::WALK:
            if (movingBackward) {
                animId = pickFirstAvailable({anim::WALK_BACKWARDS}, anim::WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({anim::SHUFFLE_LEFT, anim::RUN_LEFT}, anim::WALK);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({anim::SHUFFLE_RIGHT, anim::RUN_RIGHT}, anim::WALK);
            } else {
                animId = pickFirstAvailable({anim::WALK, anim::RUN}, anim::STAND);
            }
            loop = true;
            break;
        case CharAnimState::RUN:
            if (movingBackward) {
                animId = pickFirstAvailable({anim::WALK_BACKWARDS}, anim::WALK);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({anim::RUN_LEFT}, anim::RUN);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({anim::RUN_RIGHT}, anim::RUN);
            } else if (sprintAuraActive_) {
                animId = pickFirstAvailable({anim::SPRINT, anim::RUN, anim::WALK}, anim::STAND);
            } else {
                animId = pickFirstAvailable({anim::RUN, anim::WALK}, anim::STAND);
            }
            loop = true;
            break;
        case CharAnimState::JUMP_START: animId = anim::JUMP_START; loop = false; break;
        case CharAnimState::JUMP_MID:   animId = anim::JUMP;       loop = false; break;
        case CharAnimState::JUMP_END:   animId = anim::JUMP_END;   loop = false; break;
        case CharAnimState::SIT_DOWN:
            animId = sitDownAnim_ ? sitDownAnim_ : anim::SIT_GROUND_DOWN;
            loop = false;
            break;
        case CharAnimState::SITTING:
            animId = sitLoopAnim_ ? sitLoopAnim_ : anim::SITTING;
            loop = true;
            break;
        case CharAnimState::SIT_UP:
            animId = sitUpAnim_ ? sitUpAnim_ : anim::SIT_GROUND_UP;
            loop = false;
            break;
        case CharAnimState::EMOTE:      animId = emoteAnimId_;    loop = emoteLoop_; break;
        case CharAnimState::LOOTING:    animId = anim::LOOT;      loop = true;  break;
        case CharAnimState::SWIM_IDLE:  animId = anim::SWIM_IDLE;  loop = true;  break;
        case CharAnimState::SWIM:
            if (movingBackward) {
                animId = pickFirstAvailable({anim::SWIM_BACKWARDS}, anim::SWIM);
            } else if (anyStrafeLeft) {
                animId = pickFirstAvailable({anim::SWIM_LEFT}, anim::SWIM);
            } else if (anyStrafeRight) {
                animId = pickFirstAvailable({anim::SWIM_RIGHT}, anim::SWIM);
            } else {
                animId = anim::SWIM;
            }
            loop = true;
            break;
        case CharAnimState::MELEE_SWING:
            if (specialAttackAnimId_ != 0) {
                animId = specialAttackAnimId_;
            } else {
                animId = resolveMeleeAnimId();
            }
            if (animId == 0) {
                animId = anim::STAND;
            }
            loop = false;
            break;
        case CharAnimState::RANGED_SHOOT:
            animId = rangedAnimId_ ? rangedAnimId_ : anim::ATTACK_BOW;
            loop = false;
            break;
        case CharAnimState::RANGED_LOAD:
            switch (equippedRangedType_) {
                case RangedWeaponType::BOW:
                    animId = pickFirstAvailable({anim::LOAD_BOW}, anim::STAND); break;
                case RangedWeaponType::GUN:
                    animId = pickFirstAvailable({anim::LOAD_RIFLE}, anim::STAND); break;
                case RangedWeaponType::CROSSBOW:
                    animId = pickFirstAvailable({anim::LOAD_BOW}, anim::STAND); break;
                default:
                    animId = anim::STAND; break;
            }
            loop = false;
            break;
        case CharAnimState::MOUNT:      animId = anim::MOUNT;      loop = true;  break;
        case CharAnimState::COMBAT_IDLE:
            // Wounded idle overrides combat stance when HP < 20%
            if (lowHealth_ && characterRenderer->hasAnimation(characterInstanceId, anim::STAND_WOUND)) {
                animId = anim::STAND_WOUND;
            } else if (equippedRangedType_ == RangedWeaponType::BOW) {
                animId = pickFirstAvailable(
                    {anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedRangedType_ == RangedWeaponType::GUN) {
                animId = pickFirstAvailable(
                    {anim::READY_RIFLE, anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedRangedType_ == RangedWeaponType::CROSSBOW) {
                animId = pickFirstAvailable(
                    {anim::READY_CROSSBOW, anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedRangedType_ == RangedWeaponType::THROWN) {
                animId = pickFirstAvailable(
                    {anim::READY_THROWN, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedIs2HLoose_) {
                animId = pickFirstAvailable(
                    {anim::READY_2H_LOOSE, anim::READY_2H, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedWeaponInvType_ == game::InvType::TWO_HAND) {
                animId = pickFirstAvailable(
                    {anim::READY_2H, anim::READY_2H_LOOSE, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedIsFist_) {
                animId = pickFirstAvailable(
                    {anim::READY_FIST_1H, anim::READY_FIST, anim::READY_1H, anim::READY_UNARMED},
                    anim::STAND);
            } else if (equippedWeaponInvType_ == game::InvType::NON_EQUIP) {
                animId = pickFirstAvailable(
                    {anim::READY_UNARMED, anim::READY_1H, anim::READY_FIST},
                    anim::STAND);
            } else {
                // 1H (inventoryType 13, 21, etc.)
                animId = pickFirstAvailable(
                    {anim::READY_1H, anim::READY_2H, anim::READY_UNARMED},
                    anim::STAND);
            }
            loop = true;
            break;
        case CharAnimState::CHARGE:
            animId = anim::RUN;
            loop = true;
            break;
        case CharAnimState::UNSHEATHE:
            animId = anim::UNSHEATHE;
            loop = false;
            break;
        case CharAnimState::SHEATHE:
            animId = pickFirstAvailable({anim::SHEATHE, anim::HIP_SHEATHE}, anim::SHEATHE);
            loop = false;
            break;
        case CharAnimState::SPELL_PRECAST:
            animId = spellPrecastAnimId_ ? spellPrecastAnimId_ : anim::SPELL_PRECAST;
            loop = false;  // One-shot wind-up
            break;
        case CharAnimState::SPELL_CASTING:
            animId = spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL;
            loop = spellCastLoop_;
            break;
        case CharAnimState::SPELL_FINALIZE:
            // Play finalization anim if set, otherwise let the cast anim finish as one-shot
            animId = spellFinalizeAnimId_ ? spellFinalizeAnimId_
                   : (spellCastAnimId_ ? spellCastAnimId_ : anim::SPELL);
            loop = false;  // One-shot release
            break;
        case CharAnimState::HIT_REACTION:
            animId = hitReactionAnimId_ ? hitReactionAnimId_ : anim::COMBAT_WOUND;
            loop = false;
            break;
        case CharAnimState::STUNNED:
            animId = anim::STUN;
            loop = true;
            break;
    }

    // Stealth animation substitution: override idle/walk/run with stealth variants
    if (stealthed_) {
        if (charAnimState_ == CharAnimState::IDLE || charAnimState_ == CharAnimState::COMBAT_IDLE) {
            animId = pickFirstAvailable({anim::STEALTH_STAND}, animId);
        } else if (charAnimState_ == CharAnimState::WALK) {
            animId = pickFirstAvailable({anim::STEALTH_WALK}, animId);
        } else if (charAnimState_ == CharAnimState::RUN) {
            animId = pickFirstAvailable({anim::STEALTH_RUN, anim::STEALTH_WALK}, animId);
        }
    }

    uint32_t currentAnimId = 0;
    float currentAnimTimeMs = 0.0f;
    float currentAnimDurationMs = 0.0f;
    bool haveState = characterRenderer->getAnimationState(characterInstanceId, currentAnimId, currentAnimTimeMs, currentAnimDurationMs);
    const bool requestChanged = (lastPlayerAnimRequest_ != animId) || (lastPlayerAnimLoopRequest_ != loop);
    // requestChanged alone is sufficient: covers both anim ID changes AND loop-mode
    // changes on the same anim (e.g. spell cast loop → finalization one-shot).
    // The currentAnimId check handles engine drift (fallback anim playing instead).
    const bool shouldPlay = requestChanged || (haveState && currentAnimId != animId);
    if (shouldPlay) {
        characterRenderer->playAnimation(characterInstanceId, animId, loop);
        lastPlayerAnimRequest_ = animId;
        lastPlayerAnimLoopRequest_ = loop;
    }
}

// ── Footstep event detection ─────────────────────────────────────────────────

bool AnimationController::shouldTriggerFootstepEvent(uint32_t animationId, float animationTimeMs, float animationDurationMs) {
    if (animationDurationMs <= 1.0f) {
        footstepNormInitialized_ = false;
        return false;
    }

    float wrappedTime = animationTimeMs;
    while (wrappedTime >= animationDurationMs) {
        wrappedTime -= animationDurationMs;
    }
    if (wrappedTime < 0.0f) wrappedTime += animationDurationMs;
    float norm = wrappedTime / animationDurationMs;

    if (animationId != footstepLastAnimationId_) {
        footstepLastAnimationId_ = animationId;
        footstepLastNormTime_ = norm;
        footstepNormInitialized_ = true;
        return false;
    }

    if (!footstepNormInitialized_) {
        footstepNormInitialized_ = true;
        footstepLastNormTime_ = norm;
        return false;
    }

    auto crossed = [&](float eventNorm) {
        if (footstepLastNormTime_ <= norm) {
            return footstepLastNormTime_ < eventNorm && eventNorm <= norm;
        }
        return footstepLastNormTime_ < eventNorm || eventNorm <= norm;
    };

    bool trigger = crossed(0.22f) || crossed(0.72f);
    footstepLastNormTime_ = norm;
    return trigger;
}

audio::FootstepSurface AnimationController::resolveFootstepSurface() const {
    auto* cameraController = renderer_->getCameraController();
    if (!cameraController || !cameraController->isThirdPerson()) {
        return audio::FootstepSurface::STONE;
    }

    const glm::vec3& p = renderer_->getCharacterPosition();

    float distSq = glm::dot(p - cachedFootstepPosition_, p - cachedFootstepPosition_);
    if (distSq < 2.25f && cachedFootstepUpdateTimer_ < 0.5f) {
        return cachedFootstepSurface_;
    }

    cachedFootstepPosition_ = p;
    cachedFootstepUpdateTimer_ = 0.0f;

    if (cameraController->isSwimming()) {
        cachedFootstepSurface_ = audio::FootstepSurface::WATER;
        return audio::FootstepSurface::WATER;
    }

    auto* waterRenderer = renderer_->getWaterRenderer();
    if (waterRenderer) {
        auto waterH = waterRenderer->getWaterHeightAt(p.x, p.y);
        if (waterH && p.z < (*waterH + 0.25f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::WATER;
            return audio::FootstepSurface::WATER;
        }
    }

    auto* wmoRenderer = renderer_->getWMORenderer();
    auto* terrainManager = renderer_->getTerrainManager();
    if (wmoRenderer) {
        auto wmoFloor = wmoRenderer->getFloorHeight(p.x, p.y, p.z + 1.5f);
        auto terrainFloor = terrainManager ? terrainManager->getHeightAt(p.x, p.y) : std::nullopt;
        if (wmoFloor && (!terrainFloor || *wmoFloor >= *terrainFloor - 0.1f)) {
            cachedFootstepSurface_ = audio::FootstepSurface::STONE;
            return audio::FootstepSurface::STONE;
        }
    }

    audio::FootstepSurface surface = audio::FootstepSurface::STONE;

    if (terrainManager) {
        auto texture = terrainManager->getDominantTextureAt(p.x, p.y);
        if (texture) {
            std::string t = *texture;
            for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t.find("snow") != std::string::npos || t.find("ice") != std::string::npos) surface = audio::FootstepSurface::SNOW;
            else if (t.find("grass") != std::string::npos || t.find("moss") != std::string::npos || t.find("leaf") != std::string::npos) surface = audio::FootstepSurface::GRASS;
            else if (t.find("sand") != std::string::npos || t.find("dirt") != std::string::npos || t.find("mud") != std::string::npos) surface = audio::FootstepSurface::DIRT;
            else if (t.find("wood") != std::string::npos || t.find("timber") != std::string::npos) surface = audio::FootstepSurface::WOOD;
            else if (t.find("metal") != std::string::npos || t.find("iron") != std::string::npos) surface = audio::FootstepSurface::METAL;
            else if (t.find("stone") != std::string::npos || t.find("rock") != std::string::npos || t.find("cobble") != std::string::npos || t.find("brick") != std::string::npos) surface = audio::FootstepSurface::STONE;
        }
    }

    cachedFootstepSurface_ = surface;
    return surface;
}

// ── Footstep update (called from Renderer::update) ──────────────────────────

void AnimationController::updateFootsteps(float deltaTime) {
    auto* footstepManager = renderer_->getAudioCoordinator()->getFootstepManager();
    if (!footstepManager) return;

    auto* characterRenderer = renderer_->getCharacterRenderer();
    auto* cameraController = renderer_->getCameraController();
    uint32_t characterInstanceId = renderer_->getCharacterInstanceId();

    footstepManager->update(deltaTime);
    cachedFootstepUpdateTimer_ += deltaTime;

    bool canPlayFootsteps = characterRenderer && characterInstanceId > 0 &&
        cameraController && cameraController->isThirdPerson() &&
        cameraController->isGrounded() && !cameraController->isSwimming();

    if (canPlayFootsteps && isMounted() && mountInstanceId_ > 0 && !taxiFlight_) {
        // Mount footsteps: use mount's animation for timing
        uint32_t animId = 0;
        float animTimeMs = 0.0f, animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(mountInstanceId_, animId, animTimeMs, animDurationMs) &&
            animDurationMs > 1.0f && cameraController->isMoving()) {
            float wrappedTime = animTimeMs;
            while (wrappedTime >= animDurationMs) {
                wrappedTime -= animDurationMs;
            }
            if (wrappedTime < 0.0f) wrappedTime += animDurationMs;
            float norm = wrappedTime / animDurationMs;

            if (animId != mountFootstepLastAnimId_) {
                mountFootstepLastAnimId_ = animId;
                mountFootstepLastNormTime_ = norm;
                mountFootstepNormInitialized_ = true;
            } else if (!mountFootstepNormInitialized_) {
                mountFootstepNormInitialized_ = true;
                mountFootstepLastNormTime_ = norm;
            } else {
                auto crossed = [&](float eventNorm) {
                    if (mountFootstepLastNormTime_ <= norm) {
                        return mountFootstepLastNormTime_ < eventNorm && eventNorm <= norm;
                    }
                    return mountFootstepLastNormTime_ < eventNorm || eventNorm <= norm;
                };
                if (crossed(0.25f) || crossed(0.75f)) {
                    footstepManager->playFootstep(resolveFootstepSurface(), true);
                }
                mountFootstepLastNormTime_ = norm;
            }
        } else {
            mountFootstepNormInitialized_ = false;
        }
        footstepNormInitialized_ = false;
    } else if (canPlayFootsteps && isFootstepAnimationState()) {
        uint32_t animId = 0;
        float animTimeMs = 0.0f;
        float animDurationMs = 0.0f;
        if (characterRenderer->getAnimationState(characterInstanceId, animId, animTimeMs, animDurationMs) &&
            shouldTriggerFootstepEvent(animId, animTimeMs, animDurationMs)) {
            auto surface = resolveFootstepSurface();
            footstepManager->playFootstep(surface, cameraController->isSprinting());
            if (surface == audio::FootstepSurface::WATER) {
                if (renderer_->getAudioCoordinator()->getMovementSoundManager()) {
                    renderer_->getAudioCoordinator()->getMovementSoundManager()->playWaterFootstep(audio::MovementSoundManager::CharacterSize::MEDIUM);
                }
                auto* swimEffects = renderer_->getSwimEffects();
                auto* waterRenderer = renderer_->getWaterRenderer();
                if (swimEffects && waterRenderer) {
                    const glm::vec3& characterPosition = renderer_->getCharacterPosition();
                    auto wh = waterRenderer->getWaterHeightAt(characterPosition.x, characterPosition.y);
                    if (wh) {
                        swimEffects->spawnFootSplash(characterPosition, *wh);
                    }
                }
            }
        }
        mountFootstepNormInitialized_ = false;
    } else {
        footstepNormInitialized_ = false;
        mountFootstepNormInitialized_ = false;
    }
}

// ── Activity SFX state tracking ──────────────────────────────────────────────

void AnimationController::updateSfxState(float deltaTime) {
    auto* activitySoundManager = renderer_->getAudioCoordinator()->getActivitySoundManager();
    if (!activitySoundManager) return;

    auto* cameraController = renderer_->getCameraController();

    activitySoundManager->update(deltaTime);
    if (cameraController && cameraController->isThirdPerson()) {
        bool grounded = cameraController->isGrounded();
        bool jumping = cameraController->isJumping();
        bool falling = cameraController->isFalling();
        bool swimming = cameraController->isSwimming();
        bool moving = cameraController->isMoving();

        if (!sfxStateInitialized_) {
            sfxPrevGrounded_ = grounded;
            sfxPrevJumping_ = jumping;
            sfxPrevFalling_ = falling;
            sfxPrevSwimming_ = swimming;
            sfxStateInitialized_ = true;
        }

        if (jumping && !sfxPrevJumping_ && !swimming) {
            activitySoundManager->playJump();
        }

        if (grounded && !sfxPrevGrounded_) {
            bool hardLanding = sfxPrevFalling_;
            activitySoundManager->playLanding(resolveFootstepSurface(), hardLanding);
        }

        if (swimming && !sfxPrevSwimming_) {
            activitySoundManager->playWaterEnter();
        } else if (!swimming && sfxPrevSwimming_) {
            activitySoundManager->playWaterExit();
        }

        activitySoundManager->setSwimmingState(swimming, moving);

        if (renderer_->getAudioCoordinator()->getMusicManager()) {
            renderer_->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(swimming);
        }

        sfxPrevGrounded_ = grounded;
        sfxPrevJumping_ = jumping;
        sfxPrevFalling_ = falling;
        sfxPrevSwimming_ = swimming;
    } else {
        activitySoundManager->setSwimmingState(false, false);
        if (renderer_->getAudioCoordinator()->getMusicManager()) {
            renderer_->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(false);
        }
        sfxStateInitialized_ = false;
    }

    // Mount ambient sounds
    if (renderer_->getAudioCoordinator()->getMountSoundManager()) {
        renderer_->getAudioCoordinator()->getMountSoundManager()->update(deltaTime);
        if (cameraController && isMounted()) {
            bool isMoving = cameraController->isMoving();
            bool flying = taxiFlight_ || !cameraController->isGrounded();
            renderer_->getAudioCoordinator()->getMountSoundManager()->setMoving(isMoving);
            renderer_->getAudioCoordinator()->getMountSoundManager()->setFlying(flying);
        }
    }
}

} // namespace rendering
} // namespace wowee
