#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
}

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;
    spellVisualDbcLoaded_ = true; // Set early to prevent re-entry on failure

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    if (!cachedAssetManager_) return;

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svPrecastKitField = svLayout  ? (*svLayout)["PrecastKit"]    : 1;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;

    // Kit effect fields to probe, in priority order.
    // SpecialEffect0 > BaseEffect > LeftHand > RightHand > Chest > Head > Breath
    struct KitField { const char* name; uint32_t fallback; };
    static constexpr KitField kitFieldDefs[] = {
        {"SpecialEffect0",  11}, {"BaseEffect",       5},
        {"LeftHandEffect",   6}, {"RightHandEffect",  7},
        {"ChestEffect",      4}, {"HeadEffect",       3},
        {"BreathEffect",     8}, {"SpecialEffect1",  12},
        {"SpecialEffect2",  13},
    };
    constexpr size_t numKitFields = sizeof(kitFieldDefs) / sizeof(kitFieldDefs[0]);
    uint32_t kitFields[numKitFields];
    for (size_t k = 0; k < numKitFields; ++k)
        kitFields[k] = kitLayout ? kitLayout->field(kitFieldDefs[k].name) : kitFieldDefs[k].fallback;

    // Load SpellVisualEffectName.dbc — ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) {
            // DBC stores old-format extensions (.mdx, .mdl) but extracted assets are .m2
            if (p.size() > 4) {
                std::string ext = p.substr(p.size() - 4);
                // Case-insensitive extension check
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (ext == ".mdx" || ext == ".mdl") {
                    p = p.substr(0, p.size() - 4) + ".m2";
                }
            }
            effectPaths[id] = p;
        }
    }

    // Load SpellVisualKit.dbc — kitId → best SpellVisualEffectName ID
    // Probes all effect slots in priority order and keeps the first valid hit.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    std::unordered_map<uint32_t, uint32_t> kitToEffectName; // kitId → effectNameId
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            uint32_t eff = 0;
            for (size_t k = 0; k < numKitFields && !eff; ++k) {
                if (kitFields[k] < fc)
                    eff = kitDbc->getUInt32(i, kitFields[k]);
            }
            if (eff) kitToEffectName[kitId] = eff;
        }
    }

    // Helper: resolve path for a given kit ID
    auto kitPath = [&](uint32_t kitId) -> std::string {
        if (!kitId) return {};
        auto kitIt = kitToEffectName.find(kitId);
        if (kitIt == kitToEffectName.end()) return {};
        auto fxIt = effectPaths.find(kitIt->second);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };
    auto missilePath = [&](uint32_t effId) -> std::string {
        if (!effId) return {};
        auto fxIt = effectPaths.find(effId);
        return (fxIt != effectPaths.end()) ? fxIt->second : std::string{};
    };

    // Load SpellVisual.dbc — visualId → cast/impact M2 paths via kit chain
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    uint32_t loadedPrecast = 0, loadedCast = 0, loadedImpact = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;

        // Precast path: PrecastKit → SpecialEffect0/BaseEffect
        {
            std::string path;
            if (svPrecastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svPrecastKitField));
            if (!path.empty()) { spellVisualPrecastPath_[vid] = path; ++loadedPrecast; }
        }
        // Cast path: CastKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svCastKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svCastKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualCastPath_[vid] = path; ++loadedCast; }
        }
        // Impact path: ImpactKit → SpecialEffect0/BaseEffect, fallback to MissileModel
        {
            std::string path;
            if (svImpactKitField < svFc)
                path = kitPath(svDbc->getUInt32(i, svImpactKitField));
            if (path.empty() && svMissileField < svFc)
                path = missilePath(svDbc->getUInt32(i, svMissileField));
            if (!path.empty()) { spellVisualImpactPath_[vid] = path; ++loadedImpact; }
        }
    }
    LOG_INFO("SpellVisual: loaded precast=", loadedPrecast, " cast=", loadedCast, " impact=", loadedImpact,
             " visual\u2192M2 mappings (of ", svDbc->getRecordCount(), " records)");
}

// ---------------------------------------------------------------------------
// Classify model path to a character attachment point for bone tracking
// ---------------------------------------------------------------------------
uint32_t SpellVisualSystem::classifyAttachmentId(const std::string& modelPath) {
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects track the right hand (attachment 1)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos)
        return 1; // RightHand
    // "chest" effects track chest/torso (attachment 5 in M2 spec)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos)
        return 5; // Chest
    // "head" effects track head (attachment 11)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos)
        return 11; // Head
    return 0; // No bone tracking (static position or base effect)
}

// ---------------------------------------------------------------------------
// Height offset for spell effect placement (fallback when no bone tracking)
// ---------------------------------------------------------------------------
glm::vec3 SpellVisualSystem::applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath) {
    // Lowercase the path for case-insensitive matching
    std::string lower = modelPath;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // "hand" effects go at hand height (~0.8m above feet)
    if (lower.find("_hand") != std::string::npos || lower.find("hand_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 0.8f);
    }
    // "chest" effects go at chest height (~1.0m above feet)
    if (lower.find("_chest") != std::string::npos || lower.find("chest_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.0f);
    }
    // "head" effects go at head height (~1.6m above feet)
    if (lower.find("_head") != std::string::npos || lower.find("head_") != std::string::npos) {
        return basePos + glm::vec3(0.0f, 0.0f, 1.6f);
    }
    // "base" / "feet" / ground effects stay at ground level
    return basePos;
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t castTimeMs) {
    LOG_INFO("SpellVisual: playSpellVisualPrecast visualId=", visualId,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z,
             ") castTimeMs=", castTimeMs);
    if (!m2Renderer_ || visualId == 0) {
        LOG_WARNING("SpellVisual: playSpellVisualPrecast early-out: m2Renderer_=", (m2Renderer_ ? "yes" : "null"),
                    " visualId=", visualId);
        return;
    }

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) { LOG_WARNING("SpellVisual: no AssetManager"); return; }

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Try precast path first, fall back to cast path
    auto pathIt = spellVisualPrecastPath_.find(visualId);
    if (pathIt == spellVisualPrecastPath_.end()) {
        // No precast kit — fall back to playing cast kit
        playSpellVisual(visualId, worldPosition, false);
        return;
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: precast path resolved to: ", modelPath);

    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: precast model in failed-cache, skipping: ", modelPath);
        return;
    }

    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            // Fall back to cast kit
            playSpellVisual(visualId, worldPosition, false);
            return;
        }
        LOG_INFO("SpellVisual: precast M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        LOG_INFO("SpellVisual: precast M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size(),
                 " globalSeqs=", model.globalSequenceDurations.size(),
                 " sequences=", model.sequences.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty precast model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false);
            return;
        }
        if (model.version >= 264) {
            std::string skinPath = modelPath.substr(0, modelPath.rfind('.')) + "00.skin";
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) {
                pipeline::M2Loader::loadSkin(skinData, model);
                LOG_INFO("SpellVisual: loaded skin, indices=", model.indices.size());
            }
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load precast model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            playSpellVisual(visualId, worldPosition, false);
            return;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        LOG_INFO("SpellVisual: loaded precast model id=", modelId, " path=", modelPath);
    }

    // Determine attachment point for bone tracking (hand/chest/head → follow character bones)
    uint32_t attachId = classifyAttachmentId(modelPath);
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        uint32_t charInstId = renderer_->getCharacterInstanceId();
        if (charRenderer && charInstId != 0) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(charInstId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: createInstance returned 0 for precast model=", modelPath);
        return;
    }

    // Duration: prefer server cast time if available (long casts like Hearthstone=10s),
    // otherwise fall back to M2 animation duration, then default.
    float duration;
    if (castTimeMs >= 500) {
        // Server cast time available — precast should last the full cast duration
        duration = std::clamp(static_cast<float>(castTimeMs) / 1000.0f, 0.5f, 30.0f);
    } else {
        float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
        duration = (animDurMs > 100.0f)
            ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
            : SPELL_VISUAL_DEFAULT_DURATION;
    }
    activeSpellVisuals_.push_back({instanceId, 0.0f, duration, true, attachId});
    LOG_INFO("SpellVisual: spawned precast visualId=", visualId, " instanceId=", instanceId,
             " duration=", duration, "s castTimeMs=", castTimeMs, " attach=", attachId,
             " model=", modelPath,
             " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            uint32_t ci = renderer_->getCharacterInstanceId();
            if (cr && ci != 0) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(ci, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            activeSpellVisuals_.push_back({leftId, 0.0f, duration, true, 2 /* LeftHand */});
        }
    }
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit) {
    LOG_INFO("SpellVisual: playSpellVisual visualId=", visualId, " impact=", useImpactKit,
             " pos=(", worldPosition.x, ",", worldPosition.y, ",", worldPosition.z, ")");
    if (!m2Renderer_ || visualId == 0) return;

    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    // Select cast or impact path map
    auto& pathMap = useImpactKit ? spellVisualImpactPath_ : spellVisualCastPath_;
    auto pathIt = pathMap.find(visualId);
    if (pathIt == pathMap.end()) {
        LOG_WARNING("SpellVisual: no ", (useImpactKit ? "impact" : "cast"), " path for visualId=", visualId);
        return;
    }

    const std::string& modelPath = pathIt->second;
    LOG_INFO("SpellVisual: ", (useImpactKit ? "impact" : "cast"), " path resolved to: ", modelPath);

    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_WARNING("SpellVisual: model in failed-cache, skipping: ", modelPath);
        return;
    }

    // Load the M2 model if not already loaded
    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        LOG_INFO("SpellVisual: cast/impact M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        LOG_INFO("SpellVisual: M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        // Load skin file for WotLK-format M2s
        if (model.version >= 264) {
            std::string skinPath = modelPath.substr(0, modelPath.rfind('.')) + "00.skin";
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    }

    // Determine attachment point for bone tracking on cast effects at caster
    uint32_t attachId = 0;
    if (!useImpactKit) {
        attachId = classifyAttachmentId(modelPath);
    }
    glm::vec3 spawnPos = worldPosition;
    if (attachId != 0 && renderer_) {
        auto* charRenderer = renderer_->getCharacterRenderer();
        uint32_t charInstId = renderer_->getCharacterInstanceId();
        if (charRenderer && charInstId != 0) {
            glm::mat4 attachMat;
            if (charRenderer->getAttachmentTransform(charInstId, attachId, attachMat)) {
                spawnPos = glm::vec3(attachMat[3]);
            } else {
                spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
                attachId = 0;
            }
        } else {
            spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
            attachId = 0;
        }
    } else {
        spawnPos = applyEffectHeightOffset(worldPosition, modelPath);
    }

    // Spawn instance at world position
    uint32_t instanceId = m2Renderer_->createInstance(modelId,
                                                       spawnPos,
                                                       glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create instance for visualId=", visualId);
        return;
    }
    // Determine lifetime from M2 animation duration (clamp to reasonable range)
    float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    float duration = (animDurMs > 100.0f)
        ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
        : SPELL_VISUAL_DEFAULT_DURATION;
    activeSpellVisuals_.push_back({instanceId, 0.0f, duration, false, attachId});
    LOG_INFO("SpellVisual: spawned ", (useImpactKit ? "impact" : "cast"), " visualId=", visualId,
             " instanceId=", instanceId, " duration=", duration, "s animDurMs=", animDurMs,
             " attach=", attachId, " model=", modelPath, " active=", activeSpellVisuals_.size());

    // Hand effects: spawn a mirror copy on the left hand (attachment 2)
    if (attachId == 1 /* RightHand */) {
        glm::vec3 leftPos = worldPosition;
        if (renderer_) {
            auto* cr = renderer_->getCharacterRenderer();
            uint32_t ci = renderer_->getCharacterInstanceId();
            if (cr && ci != 0) {
                glm::mat4 lm;
                if (cr->getAttachmentTransform(ci, 2, lm))
                    leftPos = glm::vec3(lm[3]);
            }
        }
        uint32_t leftId = m2Renderer_->createInstance(modelId, leftPos, glm::vec3(0.0f), 1.0f);
        if (leftId != 0) {
            activeSpellVisuals_.push_back({leftId, 0.0f, duration, false, 2 /* LeftHand */});
        }
    }
}

void SpellVisualSystem::update(float deltaTime) {
    if (activeSpellVisuals_.empty() || !m2Renderer_) return;

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    uint32_t charInstId = renderer_ ? renderer_->getCharacterInstanceId() : 0;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // Update position for bone-tracked effects (follow character hands/chest/head)
            if (it->attachmentId != 0 && charRenderer && charInstId != 0) {
                glm::mat4 attachMat;
                if (charRenderer->getAttachmentTransform(charInstId, it->attachmentId, attachMat)) {
                    glm::vec3 bonePos = glm::vec3(attachMat[3]);
                    m2Renderer_->setInstancePosition(it->instanceId, bonePos);
                }
            }
            ++it;
        }
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
