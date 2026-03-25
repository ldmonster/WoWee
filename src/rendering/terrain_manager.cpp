#include "rendering/terrain_manager.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/camera.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "core/coordinates.hpp"
#include "core/memory_monitor.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/wmo_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <unordered_set>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace wowee {
namespace rendering {

namespace {

int computeTerrainWorkerCount() {
    const char* raw = std::getenv("WOWEE_TERRAIN_WORKERS");
    if (raw && *raw) {
        char* end = nullptr;
        unsigned long long forced = std::strtoull(raw, &end, 10);
        if (end != raw && forced > 0) {
            return static_cast<int>(forced);
        }
    }

    unsigned hc = std::thread::hardware_concurrency();
    if (hc > 0) {
        // Keep terrain workers conservative by default. Over-subscribing loader
        // threads can starve main-thread networking/render updates on large-core CPUs.
        const unsigned reserved = (hc >= 16u) ? 4u : ((hc >= 8u) ? 2u : 1u);
        const unsigned maxDefaultWorkers = 8u;
        const unsigned targetWorkers = std::max(4u, std::min(maxDefaultWorkers, hc - reserved));
        return static_cast<int>(targetWorkers);
    }
    return 4;  // Fallback
}

bool decodeLayerAlpha(const pipeline::MapChunk& chunk, size_t layerIdx, std::vector<uint8_t>& outAlpha) {
    if (layerIdx >= chunk.layers.size()) return false;
    const auto& layer = chunk.layers[layerIdx];
    if (!layer.useAlpha() || layer.offsetMCAL >= chunk.alphaMap.size()) return false;

    size_t offset = layer.offsetMCAL;
    size_t layerSize = chunk.alphaMap.size() - offset;
    for (size_t j = layerIdx + 1; j < chunk.layers.size(); j++) {
        if (chunk.layers[j].useAlpha()) {
            layerSize = chunk.layers[j].offsetMCAL - offset;
            break;
        }
    }

    outAlpha.assign(4096, 255);

    if (layer.compressedAlpha()) {
        size_t readPos = offset;
        size_t writePos = 0;
        while (writePos < 4096 && readPos < chunk.alphaMap.size()) {
            uint8_t cmd = chunk.alphaMap[readPos++];
            bool fill = (cmd & 0x80) != 0;
            int count = (cmd & 0x7F) + 1;

            if (fill) {
                if (readPos >= chunk.alphaMap.size()) break;
                uint8_t val = chunk.alphaMap[readPos++];
                for (int i = 0; i < count && writePos < 4096; i++) {
                    outAlpha[writePos++] = val;
                }
            } else {
                for (int i = 0; i < count && writePos < 4096 && readPos < chunk.alphaMap.size(); i++) {
                    outAlpha[writePos++] = chunk.alphaMap[readPos++];
                }
            }
        }
        return true;
    }

    if (layerSize >= 4096) {
        std::copy(chunk.alphaMap.begin() + offset, chunk.alphaMap.begin() + offset + 4096, outAlpha.begin());
        return true;
    }

    if (layerSize >= 2048) {
        for (size_t i = 0; i < 2048; i++) {
            uint8_t v = chunk.alphaMap[offset + i];
            outAlpha[i * 2] = (v & 0x0F) * 17;
            outAlpha[i * 2 + 1] = (v >> 4) * 17;
        }
        return true;
    }

    return false;
}

std::string toLowerCopy(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

} // namespace

TerrainManager::TerrainManager() {
}

TerrainManager::~TerrainManager() {
    stopWorkers();
}

bool TerrainManager::initialize(pipeline::AssetManager* assets, TerrainRenderer* renderer) {
    assetManager = assets;
    terrainRenderer = renderer;

    if (!assetManager) {
        LOG_ERROR("Asset manager is null");
        return false;
    }

    if (!terrainRenderer) {
        LOG_ERROR("Terrain renderer is null");
        return false;
    }

    // Set dynamic tile cache budget.
    // Keep this lower so decompressed MPQ file cache can stay very aggressive.
    auto& memMonitor = core::MemoryMonitor::getInstance();
    tileCacheBudgetBytes_ = memMonitor.getRecommendedCacheBudget() / 4;
    LOG_INFO("Terrain tile cache budget: ", tileCacheBudgetBytes_ / (1024 * 1024), " MB (dynamic)");

    // Start background worker pool (dynamic: scales with available cores)
    // Keep defaults moderate; env override can increase if streaming is bottlenecked.
    workerRunning.store(true);
    workerCount = computeTerrainWorkerCount();
    workerThreads.reserve(workerCount);
    for (int i = 0; i < workerCount; i++) {
        workerThreads.emplace_back(&TerrainManager::workerLoop, this);
    }

    LOG_INFO("Terrain manager initialized (async loading enabled)");
    LOG_INFO("  Map: ", mapName);
    LOG_INFO("  Load radius: ", loadRadius, " tiles");
    LOG_INFO("  Unload radius: ", unloadRadius, " tiles");
    LOG_INFO("  Workers: ", workerCount);

    return true;
}

void TerrainManager::update(const Camera& camera, float deltaTime) {
    if (!streamingEnabled || !assetManager || !terrainRenderer) {
        return;
    }

    // Always process ready tiles each frame (GPU uploads from background thread)
    // Time-budgeted internally to prevent frame spikes.
    processReadyTiles();

    timeSinceLastUpdate += deltaTime;

    // Only update streaming periodically (not every frame)
    if (timeSinceLastUpdate < updateInterval) {
        return;
    }

    timeSinceLastUpdate = 0.0f;

    // Get current tile from camera position.
    glm::vec3 camPos = camera.getPosition();
    TileCoord newTile = worldToTile(camPos.x, camPos.y);

    // Check if we've moved to a different tile
    if (newTile.x != currentTile.x || newTile.y != currentTile.y) {
        LOG_DEBUG("Camera moved to tile [", newTile.x, ",", newTile.y, "]");
        currentTile = newTile;
    }

    // Stream tiles when player crosses a tile boundary
    if (newTile.x != lastStreamTile.x || newTile.y != lastStreamTile.y) {
        LOG_DEBUG("Streaming: cam=(", camPos.x, ",", camPos.y, ",", camPos.z,
                 ") tile=[", newTile.x, ",", newTile.y,
                 "] loaded=", loadedTiles.size());
        streamTiles();
        lastStreamTile = newTile;
    } else {
        // Proactive loading: when workers are idle, periodically re-check for
        // unloaded tiles within range. Throttled to avoid hitching right after
        // world load when many tiles finalize simultaneously.
        proactiveStreamTimer_ += deltaTime;
        if (proactiveStreamTimer_ >= 2.0f) {
            proactiveStreamTimer_ = 0.0f;
            bool workersIdle;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                workersIdle = loadQueue.empty();
            }
            if (workersIdle) {
                streamTiles();
            }
        }
    }
}

// Synchronous fallback for initial tile loading (before worker thread is useful)
bool TerrainManager::loadTile(int x, int y) {
    TileCoord coord = {x, y};

    // Check if already loaded
    if (loadedTiles.find(coord) != loadedTiles.end()) {
        return true;
    }

    // Don't retry tiles that already failed
    if (failedTiles.find(coord) != failedTiles.end()) {
        return false;
    }

    LOG_INFO("Loading terrain tile [", x, ",", y, "] (synchronous)");

    auto pending = prepareTile(x, y);
    if (!pending) {
        failedTiles[coord] = true;
        return false;
    }

    VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;
    if (vkCtx) vkCtx->beginUploadBatch();

    FinalizingTile ft;
    ft.pending = std::move(pending);
    while (!advanceFinalization(ft)) {}

    if (vkCtx) vkCtx->endUploadBatchSync();  // Sync — caller expects tile ready
    return true;
}

bool TerrainManager::enqueueTile(int x, int y) {
    TileCoord coord = {x, y};
    if (loadedTiles.find(coord) != loadedTiles.end()) {
        return true;
    }
    if (pendingTiles.find(coord) != pendingTiles.end()) {
        return true;
    }
    if (failedTiles.find(coord) != failedTiles.end()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        loadQueue.push_back(coord);
        pendingTiles[coord] = true;
    }
    queueCV.notify_all();
    return true;
}

std::shared_ptr<PendingTile> TerrainManager::prepareTile(int x, int y) {
    TileCoord coord = {x, y};
    if (auto cached = getCachedTile(coord)) {
        LOG_DEBUG("Using cached tile [", x, ",", y, "]");
        return cached;
    }

    LOG_DEBUG("Preparing tile [", x, ",", y, "] (CPU work)");

    // Early-exit check — worker should bail fast during shutdown
    if (!workerRunning.load()) return nullptr;

    // Load ADT file
    std::string adtPath = getADTPath(coord);
    auto adtData = assetManager->readFile(adtPath);

    if (adtData.empty()) {
        logMissingAdtOnce(adtPath);
        return nullptr;
    }

    // Parse ADT — allocate on heap to avoid stack overflow on macOS
    // (ADTTerrain contains std::array<MapChunk,256> ≈ 280 KB; macOS worker
    //  threads default to 512 KB stack, so two on-stack copies would overflow)
    auto terrainPtr = std::make_unique<pipeline::ADTTerrain>(pipeline::ADTLoader::load(adtData));
    if (!terrainPtr->isLoaded()) {
        LOG_ERROR("Failed to parse ADT terrain: ", adtPath);
        return nullptr;
    }

    if (!workerRunning.load()) return nullptr;

    // WotLK split ADTs can store placements in *_obj0.adt.
    // Merge object chunks so doodads/WMOs (including ground clutter) are available.
    std::string objPath = "World\\Maps\\" + mapName + "\\" + mapName + "_" +
                          std::to_string(coord.x) + "_" + std::to_string(coord.y) + "_obj0.adt";
    auto objData = assetManager->readFile(objPath);
    if (!objData.empty()) {
        auto objTerrain = std::make_unique<pipeline::ADTTerrain>(pipeline::ADTLoader::load(objData));
        if (objTerrain->isLoaded()) {
            const uint32_t doodadNameBase = static_cast<uint32_t>(terrainPtr->doodadNames.size());
            const uint32_t wmoNameBase = static_cast<uint32_t>(terrainPtr->wmoNames.size());

            terrainPtr->doodadNames.insert(terrainPtr->doodadNames.end(),
                                       objTerrain->doodadNames.begin(), objTerrain->doodadNames.end());
            terrainPtr->wmoNames.insert(terrainPtr->wmoNames.end(),
                                    objTerrain->wmoNames.begin(), objTerrain->wmoNames.end());

            std::unordered_set<uint32_t> existingDoodadUniqueIds;
            existingDoodadUniqueIds.reserve(terrainPtr->doodadPlacements.size());
            for (const auto& p : terrainPtr->doodadPlacements) {
                if (p.uniqueId != 0) existingDoodadUniqueIds.insert(p.uniqueId);
            }

            size_t mergedDoodads = 0;
            for (auto placement : objTerrain->doodadPlacements) {
                if (placement.nameId >= objTerrain->doodadNames.size()) continue;
                placement.nameId += doodadNameBase;
                if (placement.uniqueId != 0 && !existingDoodadUniqueIds.insert(placement.uniqueId).second) {
                    continue;
                }
                terrainPtr->doodadPlacements.push_back(placement);
                mergedDoodads++;
            }

            std::unordered_set<uint32_t> existingWmoUniqueIds;
            existingWmoUniqueIds.reserve(terrainPtr->wmoPlacements.size());
            for (const auto& p : terrainPtr->wmoPlacements) {
                if (p.uniqueId != 0) existingWmoUniqueIds.insert(p.uniqueId);
            }

            size_t mergedWmos = 0;
            for (auto placement : objTerrain->wmoPlacements) {
                if (placement.nameId >= objTerrain->wmoNames.size()) continue;
                placement.nameId += wmoNameBase;
                if (placement.uniqueId != 0 && !existingWmoUniqueIds.insert(placement.uniqueId).second) {
                    continue;
                }
                terrainPtr->wmoPlacements.push_back(placement);
                mergedWmos++;
            }

            if (mergedDoodads > 0 || mergedWmos > 0) {
                LOG_DEBUG("Merged obj0 tile [", x, ",", y, "]: +", mergedDoodads,
                          " doodads, +", mergedWmos, " WMOs");
            }
        }
    }

    // Set tile coordinates so mesh knows where to position this tile in world
    terrainPtr->coord.x = x;
    terrainPtr->coord.y = y;

    // Generate mesh
    pipeline::TerrainMesh mesh = pipeline::TerrainMeshGenerator::generate(*terrainPtr);
    if (mesh.validChunkCount == 0) {
        LOG_ERROR("Failed to generate terrain mesh: ", adtPath);
        return nullptr;
    }

    if (!workerRunning.load()) return nullptr;

    auto pending = std::make_shared<PendingTile>();
    pending->coord = coord;
    pending->terrain = std::move(*terrainPtr);
    pending->mesh = std::move(mesh);

    std::unordered_set<uint32_t> preparedModelIds;
    auto ensureModelPrepared = [&](const std::string& m2Path,
                                   uint32_t modelId,
                                   int& skippedFileNotFound,
                                   int& skippedInvalid,
                                   int& skippedSkinNotFound) -> bool {
        if (preparedModelIds.find(modelId) != preparedModelIds.end()) return true;

        // Skip file I/O + parsing for models already uploaded to GPU from previous tiles
        {
            std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
            if (uploadedM2Ids_.count(modelId)) {
                preparedModelIds.insert(modelId);
                return true;
            }
        }

        std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            skippedFileNotFound++;
            LOG_WARNING("M2 file not found: ", m2Path);
            return false;
        }

        pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
        if (m2Model.name.empty()) {
            m2Model.name = m2Path;
        }
        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
        std::vector<uint8_t> skinData = assetManager->readFileOptional(skinPath);
        if (!skinData.empty() && m2Model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, m2Model);
        } else if (skinData.empty() && m2Model.version >= 264) {
            skippedSkinNotFound++;
        }

        if (!m2Model.isValid()) {
            skippedInvalid++;
            LOG_DEBUG("M2 model invalid (no verts/indices): ", m2Path);
            return false;
        }

        // Pre-decode M2 model textures on background thread
        for (const auto& tex : m2Model.textures) {
            if (tex.filename.empty()) continue;
            std::string texKey = tex.filename;
            std::replace(texKey.begin(), texKey.end(), '/', '\\');
            std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (pending->preloadedM2Textures.find(texKey) != pending->preloadedM2Textures.end()) continue;
            auto blp = assetManager->loadTexture(texKey);
            if (blp.isValid()) {
                pending->preloadedM2Textures[texKey] = std::move(blp);
            }
        }

        PendingTile::M2Ready ready;
        ready.modelId = modelId;
        ready.model = std::move(m2Model);
        ready.path = m2Path;
        pending->m2Models.push_back(std::move(ready));
        preparedModelIds.insert(modelId);
        return true;
    };

    // Pre-load M2 doodads (CPU: read files, parse models)
    int skippedNameId = 0, skippedFileNotFound = 0, skippedInvalid = 0, skippedSkinNotFound = 0;
    for (const auto& placement : pending->terrain.doodadPlacements) {
        if (!workerRunning.load()) return nullptr;
        if (placement.nameId >= pending->terrain.doodadNames.size()) {
            skippedNameId++;
            continue;
        }

        std::string m2Path = pending->terrain.doodadNames[placement.nameId];
        if (m2Path.size() > 4) {
            std::string ext = toLowerCopy(m2Path.substr(m2Path.size() - 4));
            if (ext == ".mdx") {
                m2Path = m2Path.substr(0, m2Path.size() - 4) + ".m2";
            }
        }

        uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(m2Path));
        if (!ensureModelPrepared(m2Path, modelId, skippedFileNotFound, skippedInvalid, skippedSkinNotFound)) {
            continue;
        }

        float wowX = placement.position[0];
        float wowY = placement.position[1];
        float wowZ = placement.position[2];
        glm::vec3 glPos = core::coords::adtToWorld(wowX, wowY, wowZ);

        PendingTile::M2Placement p;
        p.modelId = modelId;
        p.uniqueId = placement.uniqueId;
        p.position = glPos;
        p.rotation = glm::vec3(
            -placement.rotation[2] * 3.14159f / 180.0f,
            -placement.rotation[0] * 3.14159f / 180.0f,
            (placement.rotation[1] + 180.0f) * 3.14159f / 180.0f
        );
        p.scale = placement.scale / 1024.0f;
        pending->m2Placements.push_back(p);
    }

    if (skippedNameId > 0 || skippedFileNotFound > 0 || skippedInvalid > 0 || skippedSkinNotFound > 0) {
        LOG_DEBUG("Tile [", x, ",", y, "] doodad issues: ",
                  skippedNameId, " bad nameId, ",
                  skippedFileNotFound, " file not found, ",
                  skippedInvalid, " invalid model, ",
                  skippedSkinNotFound, " skin not found");
    }

    // Procedural ground clutter from terrain layer effectId -> GroundEffectTexture/Doodad DBCs.
    ensureGroundEffectTablesLoaded();
    generateGroundClutterPlacements(pending, preparedModelIds);

    if (!workerRunning.load()) return nullptr;

    // Pre-load WMOs (CPU: read files, parse models and groups)
    if (!pending->terrain.wmoPlacements.empty()) {
        for (const auto& placement : pending->terrain.wmoPlacements) {
            if (!workerRunning.load()) return nullptr;
            if (placement.nameId >= pending->terrain.wmoNames.size()) continue;

            const std::string& wmoPath = pending->terrain.wmoNames[placement.nameId];
            std::vector<uint8_t> wmoData = assetManager->readFile(wmoPath);
            if (wmoData.empty()) continue;

            pipeline::WMOModel wmoModel = pipeline::WMOLoader::load(wmoData);
            if (wmoModel.nGroups > 0) {
                std::string basePath = wmoPath;
                std::string extension;
                if (basePath.size() > 4) {
                    extension = basePath.substr(basePath.size() - 4);
                    std::string extLower = extension;
                    for (char& c : extLower) c = std::tolower(c);
                    if (extLower == ".wmo") {
                        basePath = basePath.substr(0, basePath.size() - 4);
                    }
                }

                for (uint32_t gi = 0; gi < wmoModel.nGroups; gi++) {
                    char groupSuffix[16];
                    snprintf(groupSuffix, sizeof(groupSuffix), "_%03u%s", gi, extension.c_str());
                    std::string groupPath = basePath + groupSuffix;
                    std::vector<uint8_t> groupData = assetManager->readFile(groupPath);
                    if (groupData.empty()) {
                        snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.wmo", gi);
                        groupData = assetManager->readFile(basePath + groupSuffix);
                    }
                    if (groupData.empty()) {
                        snprintf(groupSuffix, sizeof(groupSuffix), "_%03u.WMO", gi);
                        groupData = assetManager->readFile(basePath + groupSuffix);
                    }
                    if (!groupData.empty()) {
                        pipeline::WMOLoader::loadGroup(groupData, wmoModel, gi);
                    }
                }
            }

            if (!wmoModel.groups.empty()) {
                glm::vec3 pos = core::coords::adtToWorld(placement.position[0],
                                                       placement.position[1],
                                                       placement.position[2]);

                glm::vec3 rot(
                    -placement.rotation[2] * 3.14159f / 180.0f,
                    -placement.rotation[0] * 3.14159f / 180.0f,
                    (placement.rotation[1] + 180.0f) * 3.14159f / 180.0f
                );

                // Pre-load WMO doodads (M2 models inside WMO)
                if (!workerRunning.load()) return nullptr;

                // Skip WMO doodads if this placement was already prepared by another tile's worker.
                // This prevents 15+ copies of Stormwind's ~6000 doodads from being parsed
                // simultaneously, which was the primary cause of OOM during world load.
                bool wmoAlreadyPrepared = false;
                if (placement.uniqueId != 0) {
                    std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
                    wmoAlreadyPrepared = !preparedWmoUniqueIds_.insert(placement.uniqueId).second;
                }

                if (!wmoAlreadyPrepared && !wmoModel.doodadSets.empty() && !wmoModel.doodads.empty()) {
                    glm::mat4 wmoMatrix(1.0f);
                    wmoMatrix = glm::translate(wmoMatrix, pos);
                    wmoMatrix = glm::rotate(wmoMatrix, rot.z, glm::vec3(0, 0, 1));
                    wmoMatrix = glm::rotate(wmoMatrix, rot.y, glm::vec3(0, 1, 0));
                    wmoMatrix = glm::rotate(wmoMatrix, rot.x, glm::vec3(1, 0, 0));

                    // Load doodads from set 0 (global) + placement-specific set
                    std::vector<uint32_t> setsToLoad = {0};
                    if (placement.doodadSet > 0 && placement.doodadSet < wmoModel.doodadSets.size()) {
                        setsToLoad.push_back(placement.doodadSet);
                    }
                    std::unordered_set<uint32_t> loadedDoodadIndices;
                    std::unordered_set<uint32_t> wmoPreparedModelIds;  // within-WMO model dedup
                    for (uint32_t setIdx : setsToLoad) {
                        const auto& doodadSet = wmoModel.doodadSets[setIdx];
                    for (uint32_t di = 0; di < doodadSet.count; di++) {
                        uint32_t doodadIdx = doodadSet.startIndex + di;
                        if (doodadIdx >= wmoModel.doodads.size()) break;
                        if (!loadedDoodadIndices.insert(doodadIdx).second) continue;

                        const auto& doodad = wmoModel.doodads[doodadIdx];
                        auto nameIt = wmoModel.doodadNames.find(doodad.nameIndex);
                        if (nameIt == wmoModel.doodadNames.end()) continue;

                        std::string m2Path = nameIt->second;
                        if (m2Path.empty()) continue;

                        if (m2Path.size() > 4) {
                            std::string ext = m2Path.substr(m2Path.size() - 4);
                            for (char& c : ext) c = std::tolower(c);
                            if (ext == ".mdx" || ext == ".mdl") {
                                m2Path = m2Path.substr(0, m2Path.size() - 4) + ".m2";
                            }
                        }

                        uint32_t doodadModelId = static_cast<uint32_t>(std::hash<std::string>{}(m2Path));

                        // Skip file I/O if model already uploaded or already prepared within this WMO
                        bool modelAlreadyUploaded = false;
                        {
                            std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                            modelAlreadyUploaded = uploadedM2Ids_.count(doodadModelId) > 0;
                        }
                        bool modelAlreadyPreparedInWmo = !wmoPreparedModelIds.insert(doodadModelId).second;

                        pipeline::M2Model m2Model;
                        if (!modelAlreadyUploaded && !modelAlreadyPreparedInWmo) {
                            std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
                            if (m2Data.empty()) continue;

                            m2Model = pipeline::M2Loader::load(m2Data);
                            if (m2Model.name.empty()) {
                                m2Model.name = m2Path;
                            }
                            std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
                            std::vector<uint8_t> skinData = assetManager->readFile(skinPath);
                            if (!skinData.empty() && m2Model.version >= 264) {
                                pipeline::M2Loader::loadSkin(skinData, m2Model);
                            }
                            if (!m2Model.isValid()) continue;

                            // Pre-decode doodad M2 textures on background thread
                            for (const auto& tex : m2Model.textures) {
                                if (tex.filename.empty()) continue;
                                std::string texKey = tex.filename;
                                std::replace(texKey.begin(), texKey.end(), '/', '\\');
                                std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                if (pending->preloadedM2Textures.find(texKey) != pending->preloadedM2Textures.end()) continue;
                                auto blp = assetManager->loadTexture(texKey);
                                if (blp.isValid()) {
                                    pending->preloadedM2Textures[texKey] = std::move(blp);
                                }
                            }
                        }

                        // Build doodad's local transform (WoW coordinates)
                        // WMO doodads use quaternion rotation
                        glm::quat fixedRotation(doodad.rotation.w, doodad.rotation.x, doodad.rotation.y, doodad.rotation.z);

                        glm::mat4 doodadLocal(1.0f);
                        doodadLocal = glm::translate(doodadLocal, doodad.position);
                        doodadLocal *= glm::mat4_cast(fixedRotation);
                        doodadLocal = glm::scale(doodadLocal, glm::vec3(doodad.scale));

                        // Full world transform = WMO world transform * doodad local transform
                        glm::mat4 worldMatrix = wmoMatrix * doodadLocal;

                        // Extract world position for frustum culling
                        glm::vec3 worldPos = glm::vec3(worldMatrix[3]);

                        // Detect ambient sound emitters from doodad model path
                        std::string m2PathLower = m2Path;
                        std::transform(m2PathLower.begin(), m2PathLower.end(), m2PathLower.begin(), ::tolower);

                        // Debug: Log all doodad paths to help identify fire-related models
                        static int doodadLogCount = 0;
                        if (doodadLogCount < 50) {  // Limit logging to first 50 doodads
                            LOG_DEBUG("WMO doodad: ", m2Path);
                            doodadLogCount++;
                        }

                        if (m2PathLower.find("fire") != std::string::npos ||
                            m2PathLower.find("brazier") != std::string::npos ||
                            m2PathLower.find("campfire") != std::string::npos) {
                            // Fireplace/brazier emitter
                            PendingTile::AmbientEmitter emitter;
                            emitter.position = worldPos;
                            if (m2PathLower.find("small") != std::string::npos || m2PathLower.find("campfire") != std::string::npos) {
                                emitter.type = 0;  // FIREPLACE_SMALL
                            } else {
                                emitter.type = 1;  // FIREPLACE_LARGE
                            }
                            pending->ambientEmitters.push_back(emitter);
                        } else if (m2PathLower.find("torch") != std::string::npos) {
                            // Torch emitter
                            PendingTile::AmbientEmitter emitter;
                            emitter.position = worldPos;
                            emitter.type = 2;  // TORCH
                            pending->ambientEmitters.push_back(emitter);
                        } else if (m2PathLower.find("fountain") != std::string::npos) {
                            // Fountain emitter
                            PendingTile::AmbientEmitter emitter;
                            emitter.position = worldPos;
                            emitter.type = 3;  // FOUNTAIN
                            pending->ambientEmitters.push_back(emitter);
                        } else if (m2PathLower.find("waterfall") != std::string::npos) {
                            // Waterfall emitter
                            PendingTile::AmbientEmitter emitter;
                            emitter.position = worldPos;
                            emitter.type = 6;  // WATERFALL
                            pending->ambientEmitters.push_back(emitter);
                        }

                        PendingTile::WMODoodadReady doodadReady;
                        doodadReady.modelId = doodadModelId;
                        doodadReady.model = std::move(m2Model);
                        doodadReady.worldPosition = worldPos;
                        doodadReady.modelMatrix = worldMatrix;
                        pending->wmoDoodads.push_back(std::move(doodadReady));
                    }
                    }
                }

                // Pre-decode WMO textures on background thread
                for (const auto& texPath : wmoModel.textures) {
                    if (texPath.empty()) continue;
                    std::string texKey = texPath;
                    // Truncate at NUL (WMO paths can have stray bytes)
                    size_t nul = texKey.find('\0');
                    if (nul != std::string::npos) texKey.resize(nul);
                    std::replace(texKey.begin(), texKey.end(), '/', '\\');
                    std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (texKey.empty()) continue;
                    if (pending->preloadedWMOTextures.find(texKey) != pending->preloadedWMOTextures.end()) continue;
                    // Try .blp variant
                    std::string blpKey = texKey;
                    if (blpKey.size() >= 4) {
                        std::string ext = blpKey.substr(blpKey.size() - 4);
                        if (ext == ".tga" || ext == ".dds") {
                            blpKey = blpKey.substr(0, blpKey.size() - 4) + ".blp";
                        }
                    }
                    auto blp = assetManager->loadTexture(blpKey);
                    if (blp.isValid()) {
                        pending->preloadedWMOTextures[blpKey] = std::move(blp);
                    }
                }

                PendingTile::WMOReady ready;
                // Cache WMO model uploads by path; placement dedup uses uniqueId separately.
                ready.modelId = static_cast<uint32_t>(std::hash<std::string>{}(wmoPath));
                if (ready.modelId == 0) ready.modelId = 1;
                ready.uniqueId = placement.uniqueId;
                ready.model = std::move(wmoModel);
                ready.position = pos;
                ready.rotation = rot;
                pending->wmoModels.push_back(std::move(ready));
            }
        }
    }

    if (!workerRunning.load()) return nullptr;

    // Pre-load terrain texture BLP data on background thread so finalizeTile
    // doesn't block the main thread with file I/O.
    for (const auto& texPath : pending->terrain.textures) {
        if (pending->preloadedTextures.find(texPath) != pending->preloadedTextures.end()) continue;
        pending->preloadedTextures[texPath] = assetManager->loadTexture(texPath);
    }

    LOG_DEBUG("Prepared tile [", x, ",", y, "]: ",
             pending->m2Models.size(), " M2 models, ",
             pending->m2Placements.size(), " M2 placements, ",
             pending->wmoModels.size(), " WMOs, ",
             pending->wmoDoodads.size(), " WMO doodads, ",
             pending->preloadedTextures.size(), " textures");

    return pending;
}

void TerrainManager::logMissingAdtOnce(const std::string& adtPath) {
    std::string normalized = adtPath;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::lock_guard<std::mutex> lock(missingAdtWarningsMutex_);
    if (missingAdtWarnings_.insert(normalized).second) {
        LOG_WARNING("Failed to load ADT file: ", adtPath);
    }
}

bool TerrainManager::advanceFinalization(FinalizingTile& ft) {
    auto& pending = ft.pending;
    int x = pending->coord.x;
    int y = pending->coord.y;
    TileCoord coord = pending->coord;

    switch (ft.phase) {

    case FinalizationPhase::TERRAIN: {
        // Check if tile was already loaded or failed
        if (loadedTiles.find(coord) != loadedTiles.end() || failedTiles.find(coord) != failedTiles.end()) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                pendingTiles.erase(coord);
            }
            ft.phase = FinalizationPhase::DONE;
            return true;
        }

        // Upload pre-loaded textures (once)
        if (!ft.terrainPreloaded) {
            LOG_DEBUG("Finalizing tile [", x, ",", y, "] (incremental)");
            if (!pending->preloadedTextures.empty()) {
                terrainRenderer->uploadPreloadedTextures(pending->preloadedTextures);
            }
            ft.terrainPreloaded = true;
            // Yield after preload to give time budget a chance to interrupt
            return false;
        }

        // Upload terrain chunks incrementally (16 per call to spread across frames)
        if (!ft.terrainMeshDone) {
            if (pending->mesh.validChunkCount == 0) {
                LOG_ERROR("Failed to upload terrain to GPU for tile [", x, ",", y, "]");
                failedTiles[coord] = true;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    pendingTiles.erase(coord);
                }
                ft.phase = FinalizationPhase::DONE;
                return true;
            }
            bool allDone = terrainRenderer->loadTerrainIncremental(
                pending->mesh, pending->terrain.textures, x, y,
                ft.terrainChunkNext, 16);
            if (!allDone) {
                return false; // More chunks remain — yield to time budget
            }
            ft.terrainMeshDone = true;
        }

        // Load water after all terrain chunks are uploaded
        if (waterRenderer) {
            size_t beforeSurfaces = waterRenderer->getSurfaceCount();
            waterRenderer->loadFromTerrain(pending->terrain, true, x, y);
            size_t afterSurfaces = waterRenderer->getSurfaceCount();
            if (afterSurfaces > beforeSurfaces) {
                LOG_INFO("Water: tile [", x, ",", y, "] added ", afterSurfaces - beforeSurfaces,
                         " surfaces (total: ", afterSurfaces, ")");
            }
        } else {
            LOG_WARNING("Water: waterRenderer is null during tile [", x, ",", y, "] finalization!");
        }

        // Ensure M2 renderer has asset manager
        if (m2Renderer && assetManager) {
            m2Renderer->initialize(nullptr, VK_NULL_HANDLE, assetManager);
        }

        ft.phase = FinalizationPhase::M2_MODELS;
        return false;
    }

    case FinalizationPhase::M2_MODELS: {
        // Upload multiple M2 models per call (batched GPU uploads).
        // When no more tiles are queued for background parsing, increase the
        // per-frame budget so idle workers don't waste time waiting for the
        // main thread to trickle-upload models.
        if (m2Renderer && ft.m2ModelIndex < pending->m2Models.size()) {
            // Set pre-decoded BLP cache so loadTexture() skips main-thread BLP decode
            m2Renderer->setPredecodedBLPCache(&pending->preloadedM2Textures);
            bool workersIdle;
            {
                std::lock_guard<std::mutex> lk(queueMutex);
                workersIdle = loadQueue.empty() && readyQueue.empty();
            }
            const size_t kModelsPerStep = workersIdle ? 6 : 4;
            size_t uploaded = 0;
            while (ft.m2ModelIndex < pending->m2Models.size() && uploaded < kModelsPerStep) {
                auto& m2Ready = pending->m2Models[ft.m2ModelIndex];
                if (m2Renderer->loadModel(m2Ready.model, m2Ready.modelId)) {
                    ft.uploadedM2ModelIds.insert(m2Ready.modelId);
                    // Track uploaded model IDs so background threads can skip re-reading
                    std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                    uploadedM2Ids_.insert(m2Ready.modelId);
                }
                ft.m2ModelIndex++;
                uploaded++;
            }
            m2Renderer->setPredecodedBLPCache(nullptr);
            // Stay in this phase until all models uploaded
            if (ft.m2ModelIndex < pending->m2Models.size()) {
                return false;
            }
        }
        if (!ft.uploadedM2ModelIds.empty()) {
            LOG_DEBUG("  Uploaded ", ft.uploadedM2ModelIds.size(), " M2 models for tile [", x, ",", y, "]");
        }
        ft.phase = FinalizationPhase::M2_INSTANCES;
        return false;
    }

    case FinalizationPhase::M2_INSTANCES: {
        // Create M2 instances incrementally to avoid main-thread stalls.
        // createInstance includes an O(n) bone-sibling scan that becomes expensive
        // on dense tiles with many placements and a large existing instance list.
        if (m2Renderer && ft.m2InstanceIndex < pending->m2Placements.size()) {
            constexpr size_t kInstancesPerStep = 32;
            size_t created = 0;
            while (ft.m2InstanceIndex < pending->m2Placements.size() && created < kInstancesPerStep) {
                const auto& p = pending->m2Placements[ft.m2InstanceIndex++];
                if (p.uniqueId != 0 && placedDoodadIds.count(p.uniqueId)) {
                    continue;
                }
                if (!m2Renderer->hasModel(p.modelId)) {
                    continue;
                }
                uint32_t instId = m2Renderer->createInstance(p.modelId, p.position, p.rotation, p.scale);
                if (instId) {
                    ft.m2InstanceIds.push_back(instId);
                    if (p.uniqueId != 0) {
                        placedDoodadIds.insert(p.uniqueId);
                        ft.tileUniqueIds.push_back(p.uniqueId);
                    }
                    created++;
                }
            }
            if (ft.m2InstanceIndex < pending->m2Placements.size()) {
                return false; // More instances to create — yield
            }
            LOG_DEBUG("  Loaded doodads for tile [", x, ",", y, "]: ",
                     ft.m2InstanceIds.size(), " instances (", ft.uploadedM2ModelIds.size(), " new models)");
        }
        ft.phase = FinalizationPhase::WMO_MODELS;
        return false;
    }

    case FinalizationPhase::WMO_MODELS: {
        // Upload multiple WMO models per call (batched GPU uploads)
        if (wmoRenderer && assetManager) {
            wmoRenderer->initialize(nullptr, VK_NULL_HANDLE, assetManager);
            // Set pre-decoded BLP cache and defer normal maps during streaming
            wmoRenderer->setPredecodedBLPCache(&pending->preloadedWMOTextures);
            wmoRenderer->setDeferNormalMaps(true);

            bool wmoWorkersIdle;
            {
                std::lock_guard<std::mutex> lk(queueMutex);
                wmoWorkersIdle = loadQueue.empty() && readyQueue.empty();
            }
            const size_t kWmosPerStep = wmoWorkersIdle ? 2 : 1;
            size_t uploaded = 0;
            while (ft.wmoModelIndex < pending->wmoModels.size() && uploaded < kWmosPerStep) {
                auto& wmoReady = pending->wmoModels[ft.wmoModelIndex];
                if (wmoReady.uniqueId != 0 && placedWmoIds.count(wmoReady.uniqueId)) {
                    ft.wmoModelIndex++;
                } else {
                    wmoRenderer->loadModel(wmoReady.model, wmoReady.modelId);
                    ft.wmoModelIndex++;
                    uploaded++;
                }
            }
            wmoRenderer->setDeferNormalMaps(false);
            wmoRenderer->setPredecodedBLPCache(nullptr);
            if (ft.wmoModelIndex < pending->wmoModels.size()) return false;
            // All WMO models loaded — backfill normal/height maps that were skipped during streaming
            wmoRenderer->backfillNormalMaps();
        }
        ft.phase = FinalizationPhase::WMO_INSTANCES;
        return false;
    }

    case FinalizationPhase::WMO_INSTANCES: {
        // Create WMO instances incrementally to avoid stalls on tiles with many WMOs.
        if (wmoRenderer && ft.wmoInstanceIndex < pending->wmoModels.size()) {
            constexpr size_t kWmoInstancesPerStep = 4;
            size_t created = 0;
            while (ft.wmoInstanceIndex < pending->wmoModels.size() && created < kWmoInstancesPerStep) {
                auto& wmoReady = pending->wmoModels[ft.wmoInstanceIndex++];
                if (wmoReady.uniqueId != 0 && placedWmoIds.count(wmoReady.uniqueId)) {
                    continue;
                }
                if (!wmoRenderer->isModelLoaded(wmoReady.modelId)) {
                    continue;
                }
                uint32_t wmoInstId = wmoRenderer->createInstance(wmoReady.modelId, wmoReady.position, wmoReady.rotation);
                if (wmoInstId) {
                    ft.wmoInstanceIds.push_back(wmoInstId);
                    if (wmoReady.uniqueId != 0) {
                        placedWmoIds.insert(wmoReady.uniqueId);
                        ft.tileWmoUniqueIds.push_back(wmoReady.uniqueId);
                    }
                    // Load WMO liquids (canals, pools, etc.)
                    if (waterRenderer) {
                        glm::mat4 modelMatrix = glm::mat4(1.0f);
                        modelMatrix = glm::translate(modelMatrix, wmoReady.position);
                        modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
                        modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
                        modelMatrix = glm::rotate(modelMatrix, wmoReady.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
                        for (const auto& group : wmoReady.model.groups) {
                            if (!group.liquid.hasLiquid()) continue;
                            if (group.flags & 0x2000) {
                                uint16_t lt = group.liquid.materialId;
                                uint8_t basicType = (lt == 0) ? 0 : ((lt - 1) % 4);
                                if (basicType < 2) continue;
                            }
                            waterRenderer->loadFromWMO(group.liquid, modelMatrix, wmoInstId);
                        }
                    }
                    created++;
                }
            }
            if (ft.wmoInstanceIndex < pending->wmoModels.size()) {
                return false; // More WMO instances to create — yield
            }
            LOG_DEBUG("  Loaded WMOs for tile [", x, ",", y, "]: ", ft.wmoInstanceIds.size(), " instances");
        }
        ft.phase = FinalizationPhase::WMO_DOODADS;
        return false;
    }

    case FinalizationPhase::WMO_DOODADS: {
        // Upload multiple WMO doodad M2s per call (batched GPU uploads)
        if (m2Renderer && ft.wmoDoodadIndex < pending->wmoDoodads.size()) {
            // Set pre-decoded BLP cache for doodad M2 textures
            m2Renderer->setPredecodedBLPCache(&pending->preloadedM2Textures);
            constexpr size_t kDoodadsPerStep = 4;
            size_t uploaded = 0;
            while (ft.wmoDoodadIndex < pending->wmoDoodads.size() && uploaded < kDoodadsPerStep) {
                auto& doodad = pending->wmoDoodads[ft.wmoDoodadIndex];
                if (m2Renderer->loadModel(doodad.model, doodad.modelId)) {
                    std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
                    uploadedM2Ids_.insert(doodad.modelId);
                }
                uint32_t wmoDoodadInstId = m2Renderer->createInstanceWithMatrix(
                    doodad.modelId, doodad.modelMatrix, doodad.worldPosition);
                if (wmoDoodadInstId) {
                    m2Renderer->setSkipCollision(wmoDoodadInstId, true);
                    ft.m2InstanceIds.push_back(wmoDoodadInstId);
                }
                ft.wmoDoodadIndex++;
                uploaded++;
            }
            m2Renderer->setPredecodedBLPCache(nullptr);
            if (ft.wmoDoodadIndex < pending->wmoDoodads.size()) return false;
        }
        ft.phase = FinalizationPhase::WATER;
        return false;
    }

    case FinalizationPhase::WATER: {
        // Terrain water was already loaded in TERRAIN phase.
        // Generate water ambient emitters here.
        if (ambientSoundManager) {
            for (size_t chunkIdx = 0; chunkIdx < pending->terrain.waterData.size(); chunkIdx++) {
                const auto& chunkWater = pending->terrain.waterData[chunkIdx];
                if (!chunkWater.hasWater()) continue;

                int chunkX = chunkIdx % 16;
                int chunkY = chunkIdx / 16;
                float tileOriginX = (32.0f - x) * 533.33333f;
                float tileOriginY = (32.0f - y) * 533.33333f;
                float chunkCenterX = tileOriginX + (chunkX + 0.5f) * 33.333333f;
                float chunkCenterY = tileOriginY + (chunkY + 0.5f) * 33.333333f;

                if (!chunkWater.layers.empty()) {
                    const auto& layer = chunkWater.layers[0];
                    float waterHeight = layer.minHeight;
                    if (layer.liquidType == 0 && chunkIdx % 32 == 0) {
                        PendingTile::AmbientEmitter emitter;
                        emitter.position = glm::vec3(chunkCenterX, chunkCenterY, waterHeight);
                        emitter.type = 4;
                        pending->ambientEmitters.push_back(emitter);
                    } else if (layer.liquidType == 1 && chunkIdx % 64 == 0) {
                        PendingTile::AmbientEmitter emitter;
                        emitter.position = glm::vec3(chunkCenterX, chunkCenterY, waterHeight);
                        emitter.type = 4;
                        pending->ambientEmitters.push_back(emitter);
                    }
                }
            }
        }

        ft.phase = FinalizationPhase::AMBIENT;
        return false;
    }

    case FinalizationPhase::AMBIENT: {
        // Register ambient sound emitters
        if (ambientSoundManager && !pending->ambientEmitters.empty()) {
            for (const auto& emitter : pending->ambientEmitters) {
                auto type = static_cast<audio::AmbientSoundManager::AmbientType>(emitter.type);
                ambientSoundManager->addEmitter(emitter.position, type);
            }
        }

        // Commit tile to loadedTiles
        auto tile = std::make_unique<TerrainTile>();
        tile->coord = coord;
        tile->terrain = std::move(pending->terrain);
        tile->mesh = std::move(pending->mesh);
        tile->loaded = true;
        tile->m2InstanceIds = std::move(ft.m2InstanceIds);
        tile->wmoInstanceIds = std::move(ft.wmoInstanceIds);
        tile->wmoUniqueIds = std::move(ft.tileWmoUniqueIds);
        tile->doodadUniqueIds = std::move(ft.tileUniqueIds);
        getTileBounds(coord, tile->minX, tile->minY, tile->maxX, tile->maxY);
        loadedTiles[coord] = std::move(tile);
        // NOTE: Don't cache pending here — std::move above empties terrain/mesh,
        // so the cached tile would have 0 valid chunks on reuse.  Tiles are
        // re-parsed from ADT files (file-cache hit) when they re-enter range.

        // Now safe to remove from pendingTiles (tile is in loadedTiles)
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pendingTiles.erase(coord);
        }

        LOG_DEBUG("  Finalized tile [", x, ",", y, "]");

        ft.phase = FinalizationPhase::DONE;
        return true;
    }

    case FinalizationPhase::DONE:
        return true;
    }
    return true;
}

void TerrainManager::workerLoop() {
    // Keep worker threads off core 0 (reserved for main thread)
    {
        int numCores = static_cast<int>(std::thread::hardware_concurrency());
        if (numCores >= 2) {
#ifdef __linux__
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            for (int i = 1; i < numCores; i++) {
                CPU_SET(i, &cpuset);
            }
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#elif defined(_WIN32)
            DWORD_PTR mask = 0;
            for (int i = 1; i < numCores && i < 64; i++) {
                mask |= (static_cast<DWORD_PTR>(1) << i);
            }
            SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__APPLE__)
            // Use affinity tag 2 for workers (separate from main thread tag 1)
            thread_affinity_policy_data_t policy = { 2 };
            thread_policy_set(
                pthread_mach_thread_np(pthread_self()),
                THREAD_AFFINITY_POLICY,
                reinterpret_cast<thread_policy_t>(&policy),
                THREAD_AFFINITY_POLICY_COUNT);
#endif
        }
    }
    LOG_INFO("Terrain worker thread started");

    while (workerRunning.load()) {
        TileCoord coord;
        bool hasWork = false;

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this]() {
                return !loadQueue.empty() || !workerRunning.load();
            });

            if (!workerRunning.load()) {
                break;
            }

            if (!loadQueue.empty()) {
                coord = loadQueue.front();
                loadQueue.pop_front();
                hasWork = true;
            }
        }

        if (hasWork) {
            auto pending = prepareTile(coord.x, coord.y);

            std::lock_guard<std::mutex> lock(queueMutex);
            if (pending) {
                readyQueue.push(pending);
            } else {
                // Mark as failed so we don't re-enqueue
                // We'll set failedTiles on the main thread in processReadyTiles
                // For now, just remove from pending tracking
                pendingTiles.erase(coord);
            }
        }
    }

    LOG_INFO("Terrain worker thread stopped");
}

void TerrainManager::processReadyTiles() {
    // Move newly ready tiles into the finalizing deque.
    // Keep them in pendingTiles so streamTiles() won't re-enqueue them.
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!readyQueue.empty()) {
            auto pending = readyQueue.front();
            readyQueue.pop();
            if (pending) {
                FinalizingTile ft;
                ft.pending = std::move(pending);
                finalizingTiles_.push_back(std::move(ft));
            }
        }
    }

    VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;

    // Reclaim completed async uploads from previous frames (non-blocking)
    if (vkCtx) vkCtx->pollUploadBatches();

    // Nothing to finalize — done.
    if (finalizingTiles_.empty()) return;

    // Async upload batch: record GPU copies into a command buffer, submit with
    // a fence, but DON'T wait.  The fence is polled on subsequent frames.
    // This eliminates the main-thread stall from vkWaitForFences entirely.
    //
    // Time-budgeted: yield after 8ms to prevent main-loop stalls. Each
    // advanceFinalization step is designed to be small, but texture uploads
    // and M2 model loads can occasionally spike. The budget ensures we
    // spread heavy tiles across multiple frames instead of blocking.
    const auto budgetStart = std::chrono::steady_clock::now();
    const float budgetMs = taxiStreamingMode_ ? 16.0f : 8.0f;

    if (vkCtx) vkCtx->beginUploadBatch();

    while (!finalizingTiles_.empty()) {
        auto& ft = finalizingTiles_.front();
        bool done = advanceFinalization(ft);
        if (done) {
            finalizingTiles_.pop_front();
        }
        float elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - budgetStart).count();
        if (elapsed >= budgetMs) break;
    }

    if (vkCtx) vkCtx->endUploadBatch();  // Async — submits but doesn't wait
}

void TerrainManager::processAllReadyTiles() {
    // Move all ready tiles into finalizing deque
    // Keep in pendingTiles until committed (same as processReadyTiles)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!readyQueue.empty()) {
            auto pending = readyQueue.front();
            readyQueue.pop();
            if (pending) {
                FinalizingTile ft;
                ft.pending = std::move(pending);
                finalizingTiles_.push_back(std::move(ft));
            }
        }
    }

    // Batch all GPU uploads across all tiles into a single submission
    VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;
    if (vkCtx) vkCtx->beginUploadBatch();

    // Finalize all tiles completely (no time budget — used for loading screens)
    while (!finalizingTiles_.empty()) {
        auto& ft = finalizingTiles_.front();
        while (!advanceFinalization(ft)) {}
        finalizingTiles_.pop_front();
    }

    if (vkCtx) vkCtx->endUploadBatchSync();  // Sync — load screen needs data ready
}

void TerrainManager::processOneReadyTile() {
    // Move ready tiles into finalizing deque
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!readyQueue.empty()) {
            auto pending = readyQueue.front();
            readyQueue.pop();
            if (pending) {
                FinalizingTile ft;
                ft.pending = std::move(pending);
                finalizingTiles_.push_back(std::move(ft));
            }
        }
    }
    // Finalize ONE tile completely, then return so caller can update the screen
    if (!finalizingTiles_.empty()) {
        VkContext* vkCtx = terrainRenderer ? terrainRenderer->getVkContext() : nullptr;
        if (vkCtx) vkCtx->beginUploadBatch();

        auto& ft = finalizingTiles_.front();
        while (!advanceFinalization(ft)) {}
        finalizingTiles_.pop_front();

        if (vkCtx) vkCtx->endUploadBatchSync();  // Sync — load screen needs data ready
    }
}

std::shared_ptr<PendingTile> TerrainManager::getCachedTile(const TileCoord& coord) {
    std::lock_guard<std::mutex> lock(tileCacheMutex_);
    auto it = tileCache_.find(coord);
    if (it == tileCache_.end()) return nullptr;
    tileCacheLru_.erase(it->second.lruIt);
    tileCacheLru_.push_front(coord);
    it->second.lruIt = tileCacheLru_.begin();
    return it->second.tile;
}

void TerrainManager::putCachedTile(const std::shared_ptr<PendingTile>& tile) {
    if (!tile) return;
    std::lock_guard<std::mutex> lock(tileCacheMutex_);
    TileCoord coord = tile->coord;

    auto it = tileCache_.find(coord);
    if (it != tileCache_.end()) {
        tileCacheLru_.erase(it->second.lruIt);
        tileCacheBytes_ -= it->second.bytes;
        tileCache_.erase(it);
    }

    size_t bytes = estimatePendingTileBytes(*tile);
    tileCacheLru_.push_front(coord);
    tileCache_[coord] = CachedTile{tile, bytes, tileCacheLru_.begin()};
    tileCacheBytes_ += bytes;

    // Evict least-recently used tiles until under budget
    while (tileCacheBytes_ > tileCacheBudgetBytes_ && !tileCacheLru_.empty()) {
        TileCoord evictCoord = tileCacheLru_.back();
        auto eit = tileCache_.find(evictCoord);
        if (eit != tileCache_.end()) {
            tileCacheBytes_ -= eit->second.bytes;
            tileCache_.erase(eit);
        }
        tileCacheLru_.pop_back();
    }
}

size_t TerrainManager::estimatePendingTileBytes(const PendingTile& tile) const {
    size_t bytes = 0;
    bytes += sizeof(PendingTile);
    bytes += tile.terrain.textures.size() * 64;
    bytes += tile.terrain.doodadNames.size() * 64;
    bytes += tile.terrain.wmoNames.size() * 64;
    bytes += tile.terrain.doodadPlacements.size() * sizeof(pipeline::ADTTerrain::DoodadPlacement);
    bytes += tile.terrain.wmoPlacements.size() * sizeof(pipeline::ADTTerrain::WMOPlacement);

    for (const auto& chunk : tile.terrain.chunks) {
        bytes += sizeof(chunk);
        bytes += chunk.layers.size() * sizeof(pipeline::TextureLayer);
        bytes += chunk.alphaMap.size();
    }

    for (const auto& cm : tile.mesh.chunks) {
        bytes += cm.vertices.size() * sizeof(pipeline::TerrainVertex);
        bytes += cm.indices.size() * sizeof(pipeline::TerrainIndex);
        for (const auto& layer : cm.layers) {
            bytes += layer.alphaData.size();
        }
    }

    for (const auto& ready : tile.m2Models) {
        bytes += ready.model.vertices.size() * sizeof(pipeline::M2Vertex);
        bytes += ready.model.indices.size() * sizeof(uint16_t);
        bytes += ready.model.textures.size() * sizeof(pipeline::M2Texture);
    }
    bytes += tile.m2Placements.size() * sizeof(PendingTile::M2Placement);

    for (const auto& ready : tile.wmoModels) {
        for (const auto& group : ready.model.groups) {
            bytes += group.vertices.size() * sizeof(pipeline::WMOVertex);
            bytes += group.indices.size() * sizeof(uint16_t);
            bytes += group.batches.size() * sizeof(pipeline::WMOBatch);
            bytes += group.portalVertices.size() * sizeof(glm::vec3);
            bytes += group.portals.size() * sizeof(pipeline::WMOPortal);
            bytes += group.bspNodes.size();
        }
    }
    bytes += tile.wmoDoodads.size() * sizeof(PendingTile::WMODoodadReady);

    for (const auto& [_, img] : tile.preloadedTextures) {
        bytes += img.data.size();
    }
    return bytes;
}

void TerrainManager::unloadTile(int x, int y) {
    TileCoord coord = {x, y};

    // Also remove from pending if it was queued but not yet loaded
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        pendingTiles.erase(coord);
    }

    // Remove from finalizingTiles_ if it's being incrementally finalized.
    // Water may have already been loaded in TERRAIN phase, so clean it up.
    for (auto fit = finalizingTiles_.begin(); fit != finalizingTiles_.end(); ++fit) {
        if (fit->pending && fit->pending->coord == coord) {
            // If terrain chunks were already uploaded, free their descriptor sets
            if (fit->terrainMeshDone && terrainRenderer) {
                terrainRenderer->removeTile(x, y);
            }
            // If past TERRAIN phase, water was already loaded — remove it
            if (fit->phase != FinalizationPhase::TERRAIN && waterRenderer) {
                waterRenderer->removeTile(x, y);
            }
            // Clean up any M2/WMO instances that were already created
            if (m2Renderer && !fit->m2InstanceIds.empty()) {
                m2Renderer->removeInstances(fit->m2InstanceIds);
            }
            if (wmoRenderer && !fit->wmoInstanceIds.empty()) {
                for (uint32_t id : fit->wmoInstanceIds) {
                    if (waterRenderer) waterRenderer->removeWMO(id);
                }
                wmoRenderer->removeInstances(fit->wmoInstanceIds);
            }
            for (uint32_t uid : fit->tileUniqueIds) placedDoodadIds.erase(uid);
            for (uint32_t uid : fit->tileWmoUniqueIds) {
                placedWmoIds.erase(uid);
                std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
                preparedWmoUniqueIds_.erase(uid);
            }
            finalizingTiles_.erase(fit);
            return;
        }
    }

    auto it = loadedTiles.find(coord);
    if (it == loadedTiles.end()) {
        return;
    }

    LOG_INFO("Unloading terrain tile [", x, ",", y, "]");

    const auto& tile = it->second;

    // Remove doodad unique IDs from dedup set
    for (uint32_t uid : tile->doodadUniqueIds) {
        placedDoodadIds.erase(uid);
    }
    for (uint32_t uid : tile->wmoUniqueIds) {
        placedWmoIds.erase(uid);
        std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
        preparedWmoUniqueIds_.erase(uid);
    }

    // Remove M2 doodad instances
    if (m2Renderer) {
        m2Renderer->removeInstances(tile->m2InstanceIds);
        LOG_DEBUG("  Removed ", tile->m2InstanceIds.size(), " M2 instances");
    }

    // Remove WMO instances and their liquids
    if (wmoRenderer) {
        for (uint32_t id : tile->wmoInstanceIds) {
            // Remove WMO liquids associated with this instance
            if (waterRenderer) {
                waterRenderer->removeWMO(id);
            }
        }
        wmoRenderer->removeInstances(tile->wmoInstanceIds);
        LOG_DEBUG("  Removed ", tile->wmoInstanceIds.size(), " WMO instances");
    }

    // Remove terrain chunks for this tile
    if (terrainRenderer) {
        terrainRenderer->removeTile(x, y);
    }

    // Remove water surfaces for this tile
    if (waterRenderer) {
        waterRenderer->removeTile(x, y);
    }

    loadedTiles.erase(it);
}

void TerrainManager::stopWorkers() {
    if (!workerRunning.load()) {
        LOG_WARNING("stopWorkers: already stopped");
        return;
    }
    LOG_WARNING("stopWorkers: signaling ", workerThreads.size(), " workers to stop...");
    workerRunning.store(false);
    queueCV.notify_all();

    // Workers check workerRunning at each I/O point in prepareTile() and bail
    // out quickly.  Use plain join() which is safe with std::thread — no
    // pthread_timedjoin_np (which silently joins the pthread but leaves the
    // std::thread object thinking it's still joinable → std::terminate on dtor).
    for (size_t i = 0; i < workerThreads.size(); i++) {
        if (workerThreads[i].joinable()) {
            LOG_WARNING("stopWorkers: joining worker ", i, "...");
            workerThreads[i].join();
        }
    }
    workerThreads.clear();
    LOG_WARNING("stopWorkers: done");
}

void TerrainManager::unloadAll() {
    // Signal worker threads to stop and wait briefly for them to finish.
    // Workers may be mid-prepareTile (reading MPQ / parsing ADT) which can
    // take seconds, so use a short deadline and detach any stragglers.
    if (workerRunning.load()) {
        workerRunning.store(false);
        queueCV.notify_all();

        for (auto& t : workerThreads) {
            if (t.joinable()) t.join();
        }
        workerThreads.clear();
    }

    // Clear queues
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!loadQueue.empty()) loadQueue.pop_front();
        while (!readyQueue.empty()) readyQueue.pop();
    }
    pendingTiles.clear();
    finalizingTiles_.clear();
    placedDoodadIds.clear();
    placedWmoIds.clear();
    {
        std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
        uploadedM2Ids_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
        preparedWmoUniqueIds_.clear();
    }

    LOG_INFO("Unloading all terrain tiles");
    loadedTiles.clear();
    failedTiles.clear();

    // Reset tile tracking so streaming re-triggers at the new location
    currentTile = {-1, -1};
    lastStreamTile = {-1, -1};

    // Clear terrain renderer
    if (terrainRenderer) {
        terrainRenderer->clear();
    }

    // Clear water
    if (waterRenderer) {
        waterRenderer->clear();
    }

    // Clear WMO and M2 renderers so old-location geometry doesn't persist
    if (wmoRenderer) {
        wmoRenderer->clearInstances();
    }
    if (m2Renderer) {
        m2Renderer->clear();
    }

    // Restart worker threads so streaming can resume (dynamic: scales with available cores)
    // Use 75% of logical cores for decompression, leaving headroom for render/OS
    workerRunning.store(true);
    workerCount = computeTerrainWorkerCount();
    workerThreads.reserve(workerCount);
    for (int i = 0; i < workerCount; i++) {
        workerThreads.emplace_back(&TerrainManager::workerLoop, this);
    }
}

void TerrainManager::softReset() {
    // Clear queues (workers may still be running — they'll find empty queues)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        loadQueue.clear();
        while (!readyQueue.empty()) readyQueue.pop();
    }
    pendingTiles.clear();
    finalizingTiles_.clear();
    placedDoodadIds.clear();
    placedWmoIds.clear();
    {
        std::lock_guard<std::mutex> lock(uploadedM2IdsMutex_);
        uploadedM2Ids_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(preparedWmoUniqueIdsMutex_);
        preparedWmoUniqueIds_.clear();
    }

    // Clear tile cache — keys are (x,y) without map name, so stale entries from
    // a different map with overlapping coordinates would produce wrong geometry.
    {
        std::lock_guard<std::mutex> lock(tileCacheMutex_);
        tileCache_.clear();
        tileCacheLru_.clear();
        tileCacheBytes_ = 0;
    }

    LOG_INFO("Soft-resetting terrain (clearing tiles + water + cache, workers stay alive)");
    loadedTiles.clear();
    failedTiles.clear();

    currentTile = {-1, -1};
    lastStreamTile = {-1, -1};

    if (terrainRenderer) {
        terrainRenderer->clear();
    }
    if (waterRenderer) {
        waterRenderer->clear();
    }
}

TileCoord TerrainManager::worldToTile(float glX, float glY) const {
    auto [tileX, tileY] = core::coords::worldToTile(glX, glY);
    return {tileX, tileY};
}

void TerrainManager::getTileBounds(const TileCoord& coord, float& minX, float& minY,
                                    float& maxX, float& maxY) const {
    // Calculate world bounds for this tile
    // Tile (32, 32) is at origin
    float offsetX = (32 - coord.x) * TILE_SIZE;
    float offsetY = (32 - coord.y) * TILE_SIZE;

    minX = offsetX - TILE_SIZE;
    minY = offsetY - TILE_SIZE;
    maxX = offsetX;
    maxY = offsetY;
}

std::string TerrainManager::getADTPath(const TileCoord& coord) const {
    // Format: World\Maps\{MapName}\{MapName}_{X}_{Y}.adt
    return "World\\Maps\\" + mapName + "\\" + mapName + "_" +
           std::to_string(coord.x) + "_" + std::to_string(coord.y) + ".adt";
}

void TerrainManager::ensureGroundEffectTablesLoaded() {
    if (groundEffectsLoaded_ || !assetManager) return;
    groundEffectsLoaded_ = true;

    auto groundEffectTex = assetManager->loadDBC("GroundEffectTexture.dbc");
    auto groundEffectDoodad = assetManager->loadDBC("GroundEffectDoodad.dbc");
    if (!groundEffectTex || !groundEffectDoodad) {
        LOG_WARNING("Ground clutter DBCs missing; skipping procedural ground effects");
        return;
    }

    // GroundEffectTexture: id + 4 doodad IDs + 4 weights + density + sound
    for (uint32_t i = 0; i < groundEffectTex->getRecordCount(); ++i) {
        uint32_t effectId = groundEffectTex->getUInt32(i, 0);
        if (effectId == 0) continue;

        GroundEffectEntry e;
        e.doodadIds[0] = groundEffectTex->getUInt32(i, 1);
        e.doodadIds[1] = groundEffectTex->getUInt32(i, 2);
        e.doodadIds[2] = groundEffectTex->getUInt32(i, 3);
        e.doodadIds[3] = groundEffectTex->getUInt32(i, 4);
        e.weights[0] = groundEffectTex->getUInt32(i, 5);
        e.weights[1] = groundEffectTex->getUInt32(i, 6);
        e.weights[2] = groundEffectTex->getUInt32(i, 7);
        e.weights[3] = groundEffectTex->getUInt32(i, 8);
        e.density = groundEffectTex->getUInt32(i, 9);
        groundEffectById_[effectId] = e;
    }

    // GroundEffectDoodad: id + modelName(offset) + flags
    for (uint32_t i = 0; i < groundEffectDoodad->getRecordCount(); ++i) {
        uint32_t doodadId = groundEffectDoodad->getUInt32(i, 0);
        std::string modelName = groundEffectDoodad->getString(i, 1);
        if (doodadId == 0 || modelName.empty()) continue;

        std::string lower = toLowerCopy(modelName);
        if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".mdl") {
            lower = lower.substr(0, lower.size() - 4) + ".m2";
        }
        if (lower.find('\\') != std::string::npos || lower.find('/') != std::string::npos) {
            groundDoodadModelById_[doodadId] = lower;
        } else {
            groundDoodadModelById_[doodadId] = "World\\NoDXT\\Detail\\" + lower;
        }
    }

    LOG_INFO("Ground clutter tables loaded: ", groundEffectById_.size(),
             " effects, ", groundDoodadModelById_.size(), " doodad models");
}

void TerrainManager::generateGroundClutterPlacements(std::shared_ptr<PendingTile>& pending,
                                                     std::unordered_set<uint32_t>& preparedModelIds) {
    if (taxiStreamingMode_) return;  // Skip clutter while on taxi flights.
    if (!pending || groundEffectById_.empty() || groundDoodadModelById_.empty()) return;

    static const std::string kGroundClutterProxyModel = "World\\NoDXT\\Detail\\ElwGra01.m2";
    static bool loggedProxy = false;
    if (!loggedProxy) {
        LOG_INFO("Ground clutter: forcing proxy model ", kGroundClutterProxyModel);
        loggedProxy = true;
    }

    size_t modelMissing = 0;
    size_t modelInvalid = 0;
    auto ensureModelPrepared = [&](const std::string& m2Path, uint32_t modelId) -> bool {
        if (preparedModelIds.count(modelId)) return true;

        std::vector<uint8_t> m2Data = assetManager->readFile(m2Path);
        if (m2Data.empty()) {
            modelMissing++;
            return false;
        }

        pipeline::M2Model m2Model = pipeline::M2Loader::load(m2Data);
        if (m2Model.name.empty()) {
            m2Model.name = m2Path;
        }
        std::string skinPath = m2Path.substr(0, m2Path.size() - 3) + "00.skin";
        std::vector<uint8_t> skinData = assetManager->readFileOptional(skinPath);
        if (!skinData.empty() && m2Model.version >= 264) {
            pipeline::M2Loader::loadSkin(skinData, m2Model);
        }
        if (!m2Model.isValid()) {
            modelInvalid++;
            return false;
        }

        PendingTile::M2Ready ready;
        ready.modelId = modelId;
        ready.model = std::move(m2Model);
        ready.path = m2Path;
        pending->m2Models.push_back(std::move(ready));
        preparedModelIds.insert(modelId);
        return true;
    };

    constexpr float unitSize = CHUNK_SIZE / 8.0f;
    constexpr float pi = 3.1415926535f;
    constexpr size_t kBaseMaxGroundClutterPerTile = 220;
    constexpr uint32_t kBaseMaxAttemptsPerLayer = 4;
    const float densityScaleRaw = glm::clamp(groundClutterDensityScale_, 0.0f, 1.5f);
    // Keep runtime density bounded to avoid large streaming spikes in dense tiles.
    const float densityScale = std::min(densityScaleRaw, 1.0f);
    const size_t kMaxGroundClutterPerTile = std::max<size_t>(
        0, static_cast<size_t>(std::lround(static_cast<float>(kBaseMaxGroundClutterPerTile) * densityScale)));
    const uint32_t kMaxAttemptsPerLayer = std::max<uint32_t>(
        1u, static_cast<uint32_t>(std::lround(static_cast<float>(kBaseMaxAttemptsPerLayer) * densityScale)));
    std::vector<uint8_t> alphaScratch;
    std::vector<uint8_t> alphaScratchTex;
    size_t added = 0;
    size_t attemptsTotal = 0;
    size_t alphaRejected = 0;
    size_t roadRejected = 0;
    size_t noEffectMatch = 0;
    size_t textureIdFallbackMatch = 0;
    size_t noDoodadModel = 0;
    std::array<uint16_t, 256> perChunkAdded{};

    auto isRoadLikeTexture = [](const std::string& texPath) -> bool {
        std::string t = toLowerCopy(texPath);
        return (t.find("road") != std::string::npos) ||
               (t.find("cobble") != std::string::npos) ||
               (t.find("path") != std::string::npos) ||
               (t.find("street") != std::string::npos) ||
               (t.find("pavement") != std::string::npos) ||
               (t.find("brick") != std::string::npos);
    };

    auto layerWeightAt = [&](const pipeline::MapChunk& chunk, size_t layerIdx, int alphaIndex) -> int {
        if (layerIdx >= chunk.layers.size()) return 0;
        if (layerIdx == 0) {
            int accum = 0;
            size_t numLayers = std::min(chunk.layers.size(), static_cast<size_t>(4));
            for (size_t i = 1; i < numLayers; ++i) {
                int a = 0;
                if (decodeLayerAlpha(chunk, i, alphaScratchTex) &&
                    alphaIndex >= 0 &&
                    alphaIndex < static_cast<int>(alphaScratchTex.size())) {
                    a = alphaScratchTex[alphaIndex];
                }
                accum += a;
            }
            return glm::clamp(255 - accum, 0, 255);
        }
        if (decodeLayerAlpha(chunk, layerIdx, alphaScratchTex) &&
            alphaIndex >= 0 &&
            alphaIndex < static_cast<int>(alphaScratchTex.size())) {
            return alphaScratchTex[alphaIndex];
        }
        return 0;
    };

    auto hasRoadLikeTextureAt = [&](const pipeline::MapChunk& chunk, float fracX, float fracY) -> bool {
        if (chunk.layers.empty()) return false;
        int alphaX = glm::clamp(static_cast<int>((fracX / 8.0f) * 63.0f), 0, 63);
        int alphaY = glm::clamp(static_cast<int>((fracY / 8.0f) * 63.0f), 0, 63);
        int alphaIndex = alphaY * 64 + alphaX;

        size_t numLayers = std::min(chunk.layers.size(), static_cast<size_t>(4));
        for (size_t layerIdx = 0; layerIdx < numLayers; ++layerIdx) {
            uint32_t texId = chunk.layers[layerIdx].textureId;
            if (texId >= pending->terrain.textures.size()) continue;
            const std::string& texPath = pending->terrain.textures[texId];
            if (!isRoadLikeTexture(texPath)) continue;
            // Treat meaningful blend contribution as road occupancy.
            int w = layerWeightAt(chunk, layerIdx, alphaIndex);
            if (w >= 24) return true;
        }
        return false;
    };

    for (int cy = 0; cy < 16; ++cy) {
        if (added >= kMaxGroundClutterPerTile) break;
        for (int cx = 0; cx < 16; ++cx) {
            if (added >= kMaxGroundClutterPerTile) break;
            const auto& chunk = pending->terrain.getChunk(cx, cy);
            if (!chunk.hasHeightMap() || chunk.layers.empty()) continue;

            for (size_t layerIdx = 0; layerIdx < chunk.layers.size(); ++layerIdx) {
                if (added >= kMaxGroundClutterPerTile) break;
                const auto& layer = chunk.layers[layerIdx];
                if (layer.effectId == 0) continue;

                auto geIt = groundEffectById_.find(layer.effectId);
                if (geIt == groundEffectById_.end() && layer.textureId != 0) {
                    geIt = groundEffectById_.find(layer.textureId);
                    if (geIt != groundEffectById_.end()) {
                        textureIdFallbackMatch++;
                    }
                }
                if (geIt == groundEffectById_.end()) {
                    noEffectMatch++;
                    continue;
                }
                const GroundEffectEntry& ge = geIt->second;

                uint32_t totalWeight = ge.weights[0] + ge.weights[1] + ge.weights[2] + ge.weights[3];
                if (totalWeight == 0) totalWeight = 4;

                uint32_t density = std::min<uint32_t>(ge.density, 16u);
                density = static_cast<uint32_t>(std::lround(static_cast<float>(density) * densityScale));
                if (density == 0) continue;
                uint32_t attempts = std::max<uint32_t>(3u, density * 2u);
                attempts = std::min<uint32_t>(attempts, kMaxAttemptsPerLayer);
                attemptsTotal += attempts;

                bool hasAlpha = decodeLayerAlpha(chunk, layerIdx, alphaScratch);
                uint32_t seed = static_cast<uint32_t>(
                    ((pending->coord.x & 0xFF) << 24) ^
                    ((pending->coord.y & 0xFF) << 16) ^
                    ((cx & 0x1F) << 8) ^
                    ((cy & 0x1F) << 3) ^
                    (layerIdx & 0x7));
                auto nextRand = [&seed]() -> uint32_t {
                    seed = seed * 1664525u + 1013904223u;
                    return seed;
                };

                for (uint32_t a = 0; a < attempts; ++a) {
                    float fracX = (nextRand() & 0xFFFFu) / 65535.0f * 8.0f;
                    float fracY = (nextRand() & 0xFFFFu) / 65535.0f * 8.0f;

                    if (hasAlpha && !alphaScratch.empty()) {
                        int alphaX = glm::clamp(static_cast<int>((fracX / 8.0f) * 63.0f), 0, 63);
                        int alphaY = glm::clamp(static_cast<int>((fracY / 8.0f) * 63.0f), 0, 63);
                        int alphaIndex = alphaY * 64 + alphaX;
                        if (alphaIndex < 0 || alphaIndex >= static_cast<int>(alphaScratch.size())) continue;
                        if (alphaScratch[alphaIndex] < 64) {
                            alphaRejected++;
                            continue;
                        }
                    }

                    if (hasRoadLikeTextureAt(chunk, fracX, fracY)) {
                        roadRejected++;
                        continue;
                    }

                    uint32_t roll = nextRand() % totalWeight;
                    int pick = 0;
                    uint32_t acc = 0;
                    for (int i = 0; i < 4; ++i) {
                        uint32_t w = ge.weights[i] > 0 ? ge.weights[i] : 1;
                        acc += w;
                        if (roll < acc) { pick = i; break; }
                    }
                    uint32_t doodadId = ge.doodadIds[pick];
                    if (doodadId == 0) continue;

                    auto doodadIt = groundDoodadModelById_.find(doodadId);
                    if (doodadIt == groundDoodadModelById_.end()) {
                        noDoodadModel++;
                        continue;
                    }
                    const std::string& doodadModelPath = doodadIt->second;
                    uint32_t modelId = static_cast<uint32_t>(std::hash<std::string>{}(doodadModelPath));
                    if (!ensureModelPrepared(doodadModelPath, modelId)) {
                        modelId = static_cast<uint32_t>(std::hash<std::string>{}(kGroundClutterProxyModel));
                        if (!ensureModelPrepared(kGroundClutterProxyModel, modelId)) {
                            continue;
                        }
                    }

                    float worldX = chunk.position[0] - fracY * unitSize;
                    float worldY = chunk.position[1] - fracX * unitSize;

                    int gx0 = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 8);
                    int gy0 = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 8);
                    int gx1 = std::min(gx0 + 1, 8);
                    int gy1 = std::min(gy0 + 1, 8);
                    float tx = fracX - static_cast<float>(gx0);
                    float ty = fracY - static_cast<float>(gy0);
                    float h00 = chunk.heightMap.getHeight(gx0, gy0);
                    float h10 = chunk.heightMap.getHeight(gx1, gy0);
                    float h01 = chunk.heightMap.getHeight(gx0, gy1);
                    float h11 = chunk.heightMap.getHeight(gx1, gy1);
                    float worldZ = chunk.position[2] +
                                 (h00 * (1 - tx) * (1 - ty) +
                                  h10 * tx * (1 - ty) +
                                  h01 * (1 - tx) * ty +
                                  h11 * tx * ty);

                    PendingTile::M2Placement p;
                    p.modelId = modelId;
                    p.uniqueId = 0;
                    // MCNK chunk.position is already in terrain/render world space.
                    // Do not convert via ADT placement mapping (that is for MDDF/MODF records).
                    p.rotation = glm::vec3(0.0f, 0.0f, (nextRand() & 0xFFFFu) / 65535.0f * (2.0f * pi));
                    p.scale = 0.80f + ((nextRand() & 0xFFFFu) / 65535.0f) * 0.35f;
                    // Snap directly to sampled terrain height.
                    p.position = glm::vec3(worldX, worldY, worldZ + 0.01f);
                    pending->m2Placements.push_back(p);
                    added++;
                    perChunkAdded[cy * 16 + cx]++;
                    if (added >= kMaxGroundClutterPerTile) break;
                }
            }
        }
    }

    size_t fallbackAdded = 0;
    const size_t kMinGroundClutterPerTile = static_cast<size_t>(std::lround(40.0f * densityScale));
    size_t fallbackNeeded = (added < kMinGroundClutterPerTile) ? (kMinGroundClutterPerTile - added) : 0;
    if (fallbackNeeded > 0) {
        const uint32_t proxyModelId = static_cast<uint32_t>(std::hash<std::string>{}(kGroundClutterProxyModel));
        if (ensureModelPrepared(kGroundClutterProxyModel, proxyModelId)) {
            constexpr uint32_t kFallbackPerChunk = 2;
            for (int cy = 0; cy < 16; ++cy) {
                for (int cx = 0; cx < 16; ++cx) {
                    if (fallbackAdded >= fallbackNeeded || added >= kMaxGroundClutterPerTile) break;
                    const auto& chunk = pending->terrain.getChunk(cx, cy);
                    if (!chunk.hasHeightMap()) continue;

                    for (uint32_t i = 0; i < kFallbackPerChunk; ++i) {
                        if (fallbackAdded >= fallbackNeeded || added >= kMaxGroundClutterPerTile) break;
                        // Deterministic scatter so the tile stays visually stable.
                        uint32_t seed = static_cast<uint32_t>(
                            ((pending->coord.x & 0xFF) << 24) ^
                            ((pending->coord.y & 0xFF) << 16) ^
                            ((cx & 0x1F) << 8) ^
                            ((cy & 0x1F) << 3) ^
                            (i & 0x7));
                        auto nextRand = [&seed]() -> uint32_t {
                            seed = seed * 1664525u + 1013904223u;
                            return seed;
                        };

                        float fracX = (nextRand() & 0xFFFFu) / 65535.0f * 8.0f;
                        float fracY = (nextRand() & 0xFFFFu) / 65535.0f * 8.0f;
                        if (hasRoadLikeTextureAt(chunk, fracX, fracY)) {
                            roadRejected++;
                            continue;
                        }
                        float worldX = chunk.position[0] - fracY * unitSize;
                        float worldY = chunk.position[1] - fracX * unitSize;

                        int gx0 = glm::clamp(static_cast<int>(std::floor(fracX)), 0, 8);
                        int gy0 = glm::clamp(static_cast<int>(std::floor(fracY)), 0, 8);
                        int gx1 = std::min(gx0 + 1, 8);
                        int gy1 = std::min(gy0 + 1, 8);
                        float tx = fracX - static_cast<float>(gx0);
                        float ty = fracY - static_cast<float>(gy0);
                        float h00 = chunk.heightMap.getHeight(gx0, gy0);
                        float h10 = chunk.heightMap.getHeight(gx1, gy0);
                        float h01 = chunk.heightMap.getHeight(gx0, gy1);
                        float h11 = chunk.heightMap.getHeight(gx1, gy1);
                        float worldZ = chunk.position[2] +
                                     (h00 * (1 - tx) * (1 - ty) +
                                      h10 * tx * (1 - ty) +
                                      h01 * (1 - tx) * ty +
                                      h11 * tx * ty);

                        PendingTile::M2Placement p;
                        p.modelId = proxyModelId;
                        p.uniqueId = 0;
                        p.rotation = glm::vec3(0.0f, 0.0f, (nextRand() & 0xFFFFu) / 65535.0f * (2.0f * pi));
                        p.scale = 0.75f + ((nextRand() & 0xFFFFu) / 65535.0f) * 0.40f;
                        p.position = glm::vec3(worldX, worldY, worldZ + 0.01f);
                        pending->m2Placements.push_back(p);
                        fallbackAdded++;
                        added++;
                        perChunkAdded[cy * 16 + cx]++;
                    }
                }
                if (fallbackAdded >= fallbackNeeded || added >= kMaxGroundClutterPerTile) break;
            }
        }
    }

    // Baseline pass disabled: one-per-chunk fill caused large instance spikes and hitches
    // when streaming tiles around the player.
    size_t baselineAdded = 0;

    if (added > 0) {
        static int clutterLogCount = 0;
        if (clutterLogCount < 12) {
            LOG_INFO("Ground clutter tile [", pending->coord.x, ",", pending->coord.y,
                     "] added=", added, " attempts=", attemptsTotal,
                     " fallbackAdded=", fallbackAdded,
                     " baselineAdded=", baselineAdded,
                     " roadRejected=", roadRejected);
            clutterLogCount++;
        }
    } else {
        static int noClutterLogCount = 0;
        if (noClutterLogCount < 8) {
            LOG_INFO("Ground clutter tile [", pending->coord.x, ",", pending->coord.y,
                     "] added=0 attempts=", attemptsTotal,
                     " alphaRejected=", alphaRejected,
                     " roadRejected=", roadRejected,
                     " noEffect=", noEffectMatch,
                     " textureFallback=", textureIdFallbackMatch,
                     " noDoodadModel=", noDoodadModel,
                     " modelMissing=", modelMissing,
                     " modelInvalid=", modelInvalid);
            noClutterLogCount++;
        }
    }
}

std::optional<float> TerrainManager::getHeightAt(float glX, float glY) const {
    // Terrain mesh vertices use chunk.position directly (WoW coordinates)
    // But camera is in GL coordinates. We query using the mesh coordinates directly
    // since terrain is rendered without model transformation.
    //
    // The terrain mesh generation puts vertices at:
    //   vertex.position[0] = chunk.position[0] - (offsetY * unitSize)
    //   vertex.position[1] = chunk.position[1] - (offsetX * unitSize)
    //   vertex.position[2] = chunk.position[2] + height
    //
    // So chunk spans:
    //   X: [chunk.position[0] - 8*unitSize, chunk.position[0]]
    //   Y: [chunk.position[1] - 8*unitSize, chunk.position[1]]

    const float unitSize = CHUNK_SIZE / 8.0f;

    auto sampleTileHeight = [&](const TerrainTile* tile) -> std::optional<float> {
        if (!tile || !tile->loaded) return std::nullopt;

        auto sampleChunk = [&](int cx, int cy) -> std::optional<float> {
            if (cx < 0 || cx >= 16 || cy < 0 || cy >= 16) return std::nullopt;
            const auto& chunk = tile->terrain.getChunk(cx, cy);
            if (!chunk.hasHeightMap()) return std::nullopt;

            float chunkMaxX = chunk.position[0];
            float chunkMinX = chunk.position[0] - 8.0f * unitSize;
            float chunkMaxY = chunk.position[1];
            float chunkMinY = chunk.position[1] - 8.0f * unitSize;

            if (glX < chunkMinX || glX > chunkMaxX ||
                glY < chunkMinY || glY > chunkMaxY) {
                return std::nullopt;
            }

            // Fractional position within chunk (0-8 range)
            float fracY = (chunk.position[0] - glX) / unitSize;  // maps to offsetY
            float fracX = (chunk.position[1] - glY) / unitSize;  // maps to offsetX

            fracX = glm::clamp(fracX, 0.0f, 8.0f);
            fracY = glm::clamp(fracY, 0.0f, 8.0f);

            // Bilinear interpolation on 9x9 outer grid
            int gx0 = static_cast<int>(std::floor(fracX));
            int gy0 = static_cast<int>(std::floor(fracY));
            int gx1 = std::min(gx0 + 1, 8);
            int gy1 = std::min(gy0 + 1, 8);

            float tx = fracX - gx0;
            float ty = fracY - gy0;

            float h00 = chunk.heightMap.heights[gy0 * 17 + gx0];
            float h10 = chunk.heightMap.heights[gy0 * 17 + gx1];
            float h01 = chunk.heightMap.heights[gy1 * 17 + gx0];
            float h11 = chunk.heightMap.heights[gy1 * 17 + gx1];

            float h = h00 * (1 - tx) * (1 - ty) +
                      h10 * tx * (1 - ty) +
                      h01 * (1 - tx) * ty +
                      h11 * tx * ty;

            return chunk.position[2] + h;
        };

        // Fast path: infer likely chunk index and probe 3x3 neighborhood.
        int guessCy = glm::clamp(static_cast<int>(std::floor((tile->maxX - glX) / CHUNK_SIZE)), 0, 15);
        int guessCx = glm::clamp(static_cast<int>(std::floor((tile->maxY - glY) / CHUNK_SIZE)), 0, 15);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                auto h = sampleChunk(guessCx + dx, guessCy + dy);
                if (h) return h;
            }
        }

        // Fallback full scan for robustness at seams/unusual coords.
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                auto h = sampleChunk(cx, cy);
                if (h) {
                    return h;
                }
            }
        }
        return std::nullopt;
    };

    // Fast path: sample the expected containing tile first.
    TileCoord tc = worldToTile(glX, glY);
    auto it = loadedTiles.find(tc);
    if (it != loadedTiles.end()) {
        auto h = sampleTileHeight(it->second.get());
        if (h) return h;
    }

    // Fallback: check all loaded tiles (handles seam/edge coordinate ambiguity).
    for (const auto& [coord, tile] : loadedTiles) {
        if (coord == tc) continue;
        auto h = sampleTileHeight(tile.get());
        if (h) return h;
    }

    return std::nullopt;
}

std::optional<std::string> TerrainManager::getDominantTextureAt(float glX, float glY) const {
    const float unitSize = CHUNK_SIZE / 8.0f;
    std::vector<uint8_t> alphaScratch;
    auto sampleTileTexture = [&](const TerrainTile* tile) -> std::optional<std::string> {
        if (!tile || !tile->loaded) return std::nullopt;

        auto sampleChunkTexture = [&](int cx, int cy) -> std::optional<std::string> {
            if (cx < 0 || cx >= 16 || cy < 0 || cy >= 16) return std::nullopt;
            const auto& chunk = tile->terrain.getChunk(cx, cy);
            if (!chunk.hasHeightMap() || chunk.layers.empty()) return std::nullopt;

            float chunkMaxX = chunk.position[0];
            float chunkMinX = chunk.position[0] - 8.0f * unitSize;
            float chunkMaxY = chunk.position[1];
            float chunkMinY = chunk.position[1] - 8.0f * unitSize;
            if (glX < chunkMinX || glX > chunkMaxX || glY < chunkMinY || glY > chunkMaxY) {
                return std::nullopt;
            }

            float fracY = (chunk.position[0] - glX) / unitSize;
            float fracX = (chunk.position[1] - glY) / unitSize;
            fracX = glm::clamp(fracX, 0.0f, 8.0f);
            fracY = glm::clamp(fracY, 0.0f, 8.0f);

            int alphaX = glm::clamp(static_cast<int>((fracX / 8.0f) * 63.0f), 0, 63);
            int alphaY = glm::clamp(static_cast<int>((fracY / 8.0f) * 63.0f), 0, 63);
            int alphaIndex = alphaY * 64 + alphaX;

            int weights[4] = {0, 0, 0, 0};
            size_t numLayers = std::min(chunk.layers.size(), static_cast<size_t>(4));
            int accum = 0;
            for (size_t layerIdx = 1; layerIdx < numLayers; layerIdx++) {
                int alpha = 0;
                if (decodeLayerAlpha(chunk, layerIdx, alphaScratch) && alphaIndex < static_cast<int>(alphaScratch.size())) {
                    alpha = alphaScratch[alphaIndex];
                }
                weights[layerIdx] = alpha;
                accum += alpha;
            }
            weights[0] = glm::clamp(255 - accum, 0, 255);

            size_t bestLayer = 0;
            int bestWeight = weights[0];
            for (size_t i = 1; i < numLayers; i++) {
                if (weights[i] > bestWeight) {
                    bestWeight = weights[i];
                    bestLayer = i;
                }
            }

            uint32_t texId = chunk.layers[bestLayer].textureId;
            if (texId < tile->terrain.textures.size()) {
                return tile->terrain.textures[texId];
            }
            return std::nullopt;
        };

        int guessCy = glm::clamp(static_cast<int>(std::floor((tile->maxX - glX) / CHUNK_SIZE)), 0, 15);
        int guessCx = glm::clamp(static_cast<int>(std::floor((tile->maxY - glY) / CHUNK_SIZE)), 0, 15);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                auto tex = sampleChunkTexture(guessCx + dx, guessCy + dy);
                if (tex) return tex;
            }
        }

        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++) {
                auto tex = sampleChunkTexture(cx, cy);
                if (tex) {
                    return tex;
                }
            }
        }
        return std::nullopt;
    };

    // Fast path: check expected containing tile first.
    TileCoord tc = worldToTile(glX, glY);
    auto it = loadedTiles.find(tc);
    if (it != loadedTiles.end()) {
        auto tex = sampleTileTexture(it->second.get());
        if (tex) return tex;
    }

    // Fallback: seam/edge case.
    for (const auto& [coord, tile] : loadedTiles) {
        if (coord == tc) continue;
        auto tex = sampleTileTexture(tile.get());
        if (tex) return tex;
    }

    return std::nullopt;
}

void TerrainManager::streamTiles() {
    auto shouldSkipMissingAdt = [this](const TileCoord& coord) -> bool {
        if (!assetManager) return false;
        if (failedTiles.find(coord) != failedTiles.end()) return true;
        const std::string adtPath = getADTPath(coord);
        if (!assetManager->fileExists(adtPath)) {
            // Mark permanently failed so future stream/precache passes do not retry.
            failedTiles[coord] = true;
            return true;
        }
        return false;
    };

    // Enqueue tiles in radius around current tile for async loading.
    // Collect all newly-needed tiles, then sort by distance so the closest
    // (most visible) tiles get loaded first.  This is critical during taxi
    // flight where new tiles enter the radius faster than they can load.
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        struct PendingEntry { TileCoord coord; int distSq; };
        std::vector<PendingEntry> newTiles;

        for (int dy = -loadRadius; dy <= loadRadius; dy++) {
            for (int dx = -loadRadius; dx <= loadRadius; dx++) {
                int tileX = currentTile.x + dx;
                int tileY = currentTile.y + dy;

                // Check valid range
                if (tileX < 0 || tileX > 63 || tileY < 0 || tileY > 63) {
                    continue;
                }

                // Circular pattern: skip corner tiles beyond radius (Euclidean distance)
                if (dx*dx + dy*dy > loadRadius*loadRadius) {
                    continue;
                }

                TileCoord coord = {tileX, tileY};

                // Skip if already loaded, pending, or failed
                if (loadedTiles.find(coord) != loadedTiles.end()) continue;
                if (pendingTiles.find(coord) != pendingTiles.end()) continue;
                if (failedTiles.find(coord) != failedTiles.end()) continue;
                if (shouldSkipMissingAdt(coord)) continue;

                newTiles.push_back({coord, dx*dx + dy*dy});
                pendingTiles[coord] = true;
            }
        }

        // Sort nearest tiles first so workers service the most visible tiles
        std::sort(newTiles.begin(), newTiles.end(),
                  [](const PendingEntry& a, const PendingEntry& b) { return a.distSq < b.distSq; });

        // Insert at front so new close tiles preempt any distant tiles already queued
        for (auto it = newTiles.rbegin(); it != newTiles.rend(); ++it) {
            loadQueue.push_front(it->coord);
        }
    }

    // Notify workers that there's work
    queueCV.notify_all();

    // Unload tiles beyond unload radius (well past the camera far clip)
    std::vector<TileCoord> tilesToUnload;

    for (const auto& pair : loadedTiles) {
        const TileCoord& coord = pair.first;

        int dx = coord.x - currentTile.x;
        int dy = coord.y - currentTile.y;

        // Circular pattern: unload beyond radius (Euclidean distance)
        if (dx*dx + dy*dy > unloadRadius*unloadRadius) {
            tilesToUnload.push_back(coord);
        }
    }

    for (const auto& coord : tilesToUnload) {
        unloadTile(coord.x, coord.y);
    }

    if (!tilesToUnload.empty()) {
        // Don't clean up models during streaming - keep them in VRAM for performance
        // Modern GPUs have 8-16GB VRAM, models are only ~hundreds of MB
        // Cleanup can be done manually when memory pressure is detected
        // NOTE: Disabled permanent model cleanup to leverage modern VRAM capacity
        // if (m2Renderer) {
        //     m2Renderer->cleanupUnusedModels();
        // }
        // if (wmoRenderer) {
        //     wmoRenderer->cleanupUnusedModels();
        // }

        LOG_INFO("Unloaded ", tilesToUnload.size(), " distant tiles, ",
                 loadedTiles.size(), " remain (models kept in VRAM)");
    }
}

void TerrainManager::precacheTiles(const std::vector<std::pair<int, int>>& tiles) {
    std::lock_guard<std::mutex> lock(queueMutex);

    for (const auto& [x, y] : tiles) {
        if (x < 0 || x > 63 || y < 0 || y > 63) continue;

        TileCoord coord = {x, y};

        // Skip if already loaded, pending, or failed
        if (loadedTiles.find(coord) != loadedTiles.end()) continue;
        if (pendingTiles.find(coord) != pendingTiles.end()) continue;
        if (failedTiles.find(coord) != failedTiles.end()) continue;
        if (assetManager && !assetManager->fileExists(getADTPath(coord))) {
            failedTiles[coord] = true;
            continue;
        }

        // Precache work is prioritized so taxi-route tiles are prepared before
        // opportunistic radius streaming tiles.
        loadQueue.push_front(coord);
        pendingTiles[coord] = true;
    }

    // Notify workers to start loading
    queueCV.notify_all();
}

} // namespace rendering
} // namespace wowee
