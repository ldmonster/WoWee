// world_map_facade.cpp — Public API for the world map system.
// Composes all extracted components and orchestrates the world map (Phase 10).
#include "rendering/world_map/world_map_facade.hpp"
#include "rendering/world_map/data_repository.hpp"
#include "rendering/world_map/view_state_machine.hpp"
#include "rendering/world_map/composite_renderer.hpp"
#include "rendering/world_map/exploration_state.hpp"
#include "rendering/world_map/zone_metadata.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "rendering/world_map/map_resolver.hpp"
#include "rendering/world_map/overlay_renderer.hpp"
#include "rendering/world_map/input_handler.hpp"
#include "rendering/world_map/layers/player_marker_layer.hpp"
#include "rendering/world_map/layers/party_dot_layer.hpp"
#include "rendering/world_map/layers/taxi_node_layer.hpp"
#include "rendering/world_map/layers/poi_marker_layer.hpp"
#include "rendering/world_map/layers/quest_poi_layer.hpp"
#include "rendering/world_map/layers/corpse_marker_layer.hpp"
#include "rendering/world_map/layers/zone_highlight_layer.hpp"
#include "rendering/world_map/layers/coordinate_display.hpp"
#include "rendering/world_map/layers/subzone_tooltip_layer.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/asset_manager.hpp"
#include "ui/ui_colors.hpp"
#include "game/game_utils.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace wowee {
namespace rendering {
namespace world_map {

// Find the zone index for the WORLD view background.
// Find the best continent root zone for displaying a map in CONTINENT view.
// Skips synthetic zones (Cosmic, World) and prefers a zone matching mapName.
static int findContinentRootIdx(const std::vector<Zone>& zones,
                                 int cosmicIdx,
                                 int worldIdx,
                                 const std::string& mapName) {
    LOG_INFO("findContinentRootIdx: searching ", zones.size(), " zones, mapName='", mapName, "'");
    // 1) Exact areaName match for the map name (e.g. "Azeroth", "Kalimdor")
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (i == cosmicIdx || i == worldIdx) continue;
        if (zones[i].areaID == 0 && zones[i].areaName == mapName) {
            LOG_INFO("findContinentRootIdx: matched mapName '", mapName, "' at zone[", i, "]");
            return i;
        }
    }
    // 2) Root continent (parent of leaf continents)
    int firstContinent = -1;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (i == cosmicIdx || i == worldIdx) continue;
        if (zones[i].areaID == 0) {
            if (firstContinent < 0) firstContinent = i;
            if (isRootContinent(zones, i)) return i;
        }
    }
    // 3) First continent entry
    return firstContinent;
}

// Find the best zone for the WORLD view (prefers synthetic "World" zone).
// Used only as a fallback when data.worldIdx() is not available.
static int findWorldViewContinentIdx(const std::vector<Zone>& zones,
                                      int cosmicIdx,
                                      const std::string& mapName) {
    LOG_INFO("findWorldViewContinentIdx: searching ", zones.size(), " zones, cosmicIdx=", cosmicIdx, " mapName='", mapName, "'");
    // 1) Exact areaName match for "World" folder
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (i == cosmicIdx) continue;
        if (zones[i].areaID == 0 && zones[i].areaName == "World") {
            LOG_INFO("findWorldViewContinentIdx: matched 'World' at zone[", i, "]");
            return i;
        }
    }
    // 2) Exact areaName match for the map name (e.g. "Azeroth")
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (i == cosmicIdx) continue;
        if (zones[i].areaID == 0 && zones[i].areaName == mapName) {
            LOG_INFO("findWorldViewContinentIdx: matched mapName '", mapName, "' at zone[", i, "]");
            return i;
        }
    }
    // 3) Root continent (parent of leaf continents)
    int firstContinent = -1;
    for (int i = 0; i < static_cast<int>(zones.size()); i++) {
        if (i == cosmicIdx) continue;
        if (zones[i].areaID == 0) {
            if (firstContinent < 0) firstContinent = i;
            if (isRootContinent(zones, i)) return i;
        }
    }
    // 4) First continent entry
    return firstContinent;
}

// ── PIMPL Implementation ─────────────────────────────────────

struct WorldMapFacade::Impl {
    VkContext* vkCtx = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    bool initialized = false;
    bool open = false;
    std::string mapName = "Azeroth";
    std::string pendingMapName;   // stored by external setMapName while in world/cosmic view
    bool userMapOverride = false; // true when user manually navigated to world/cosmic view

    DataRepository data;
    ViewStateMachine viewState;
    CompositeRenderer compositor;
    ExplorationState exploration;
    ZoneMetadata zoneMetadata;
    InputHandler input;
    OverlayRenderer overlay;

    // Typed layer pointers for setters (non-owning references into overlay)
    PartyDotLayer* partyDotLayer = nullptr;
    TaxiNodeLayer* taxiNodeLayer = nullptr;
    POIMarkerLayer* poiMarkerLayer = nullptr;
    QuestPOILayer* questPOILayer = nullptr;
    CorpseMarkerLayer* corpseMarkerLayer = nullptr;
    ZoneHighlightLayer* zoneHighlightLayer = nullptr;

    // Data set each frame from the UI layer
    std::vector<PartyDot> partyDots;
    std::vector<TaxiNode> taxiNodes;
    std::vector<QuestPOI> questPois;

    float lastFrameTime = 0.0f;

    void initOverlayLayers();
    void switchToMap(const std::string& newMapName);
    void switchToWorldView();
    void renderImGuiOverlay(const glm::vec3& playerRenderPos,
                            int screenWidth, int screenHeight,
                            float playerYawDeg,
                            bool rightClickConsumed);
};

void WorldMapFacade::Impl::switchToMap(const std::string& newMapName) {
    if (mapName == newMapName && !data.zones().empty()) return;
    userMapOverride = true;
    pendingMapName.clear();
    if (zoneHighlightLayer) zoneHighlightLayer->clearTextures();
    compositor.detachZoneTextures();
    data.clear();
    compositor.invalidateComposite();

    mapName = newMapName;
    data.loadZones(mapName, *assetManager);
    zoneMetadata.initialize();
    viewState.setCosmicEnabled(data.cosmicEnabled());

    // Find the continent root zone and display it (skip synthetic World/Cosmic)
    int rootIdx = findContinentRootIdx(data.zones(), data.cosmicIdx(), data.worldIdx(), mapName);
    if (rootIdx < 0) rootIdx = 0;
    viewState.setContinentIdx(rootIdx);
    compositor.loadZoneTextures(rootIdx, data.zones(), mapName);
    compositor.requestComposite(rootIdx);
    viewState.setCurrentZoneIdx(rootIdx);
    viewState.setLevel(ViewLevel::CONTINENT);
}

void WorldMapFacade::Impl::switchToWorldView() {
    LOG_INFO("switchToWorldView: mapName='", mapName, "'");

    // Determine whether the current map is an Azeroth continent (EK, Kalimdor,
    // Northrend) or a separate world (Outland).  Azeroth continents go to the
    // world view; other worlds go to the cosmic view.
    bool isAzerothContinent = (mapName == "Azeroth");
    if (!isAzerothContinent) {
        int curMapId = folderToMapId(mapName);
        for (const auto& region : data.azerothRegions()) {
            if (static_cast<int>(region.mapId) == curMapId) {
                isAzerothContinent = true;
                break;
            }
        }
    }

    // If on a different map, switch back to Azeroth first.
    if (mapName != "Azeroth") {
        if (zoneHighlightLayer) zoneHighlightLayer->clearTextures();
        compositor.detachZoneTextures();
        data.clear();
        compositor.invalidateComposite();
        mapName = "Azeroth";
        data.loadZones(mapName, *assetManager);
        zoneMetadata.initialize();
        viewState.setCosmicEnabled(data.cosmicEnabled());
    }
    userMapOverride = true;

    // Non-Azeroth worlds (e.g. Outland) go to cosmic view.
    if (!isAzerothContinent && viewState.cosmicEnabled() && data.cosmicIdx() >= 0) {
        viewState.enterCosmicView();
        compositor.loadZoneTextures(data.cosmicIdx(), data.zones(), mapName);
        compositor.requestComposite(data.cosmicIdx());
        viewState.setCurrentZoneIdx(data.cosmicIdx());
        return;
    }

    viewState.enterWorldView();

    // Use the dedicated synthetic "World" zone — its tiles (world1-12.blp)
    // are cached independently from zone[0] (Azeroth), avoiding stale-tile
    // conflicts when transitioning between WORLD and CONTINENT views.
    int worldIdx = data.worldIdx();
    LOG_INFO("switchToWorldView: worldIdx=", worldIdx);
    if (worldIdx >= 0) {
        compositor.loadZoneTextures(worldIdx, data.zones(), mapName);
        if (compositor.hasAnyTile(worldIdx)) {
            compositor.invalidateComposite();
            compositor.requestComposite(worldIdx);
            viewState.setCurrentZoneIdx(worldIdx);
            return;
        }
    }

    // Fallback: try the root continent zone
    int rootIdx = findWorldViewContinentIdx(data.zones(), data.cosmicIdx(), mapName);
    LOG_INFO("switchToWorldView: fallback rootIdx=", rootIdx);
    if (rootIdx >= 0) {
        compositor.loadZoneTextures(rootIdx, data.zones(), mapName);
        if (compositor.hasAnyTile(rootIdx)) {
            compositor.invalidateComposite();
            compositor.requestComposite(rootIdx);
            viewState.setCurrentZoneIdx(rootIdx);
        }
    }
}

void WorldMapFacade::Impl::initOverlayLayers() {
    // Order matters: later layers draw on top of earlier ones

    // Zone highlights (continent view)
    auto zhLayer = std::make_unique<ZoneHighlightLayer>();
    zhLayer->setMetadata(&zoneMetadata);
    zoneHighlightLayer = zhLayer.get();
    overlay.addLayer(std::move(zhLayer));

    // Player marker
    overlay.addLayer(std::make_unique<PlayerMarkerLayer>());

    // Party dots
    auto pdLayer = std::make_unique<PartyDotLayer>();
    partyDotLayer = pdLayer.get();
    overlay.addLayer(std::move(pdLayer));

    // Taxi nodes
    auto tnLayer = std::make_unique<TaxiNodeLayer>();
    taxiNodeLayer = tnLayer.get();
    overlay.addLayer(std::move(tnLayer));

    // // POI markers
    // auto poiLayer = std::make_unique<POIMarkerLayer>();
    // poiMarkerLayer = poiLayer.get();
    // overlay.addLayer(std::move(poiLayer));

    // Quest POI markers
    auto qpLayer = std::make_unique<QuestPOILayer>();
    questPOILayer = qpLayer.get();
    overlay.addLayer(std::move(qpLayer));

    // Corpse marker
    auto cmLayer = std::make_unique<CorpseMarkerLayer>();
    corpseMarkerLayer = cmLayer.get();
    overlay.addLayer(std::move(cmLayer));

    // Coordinate display
    overlay.addLayer(std::make_unique<CoordinateDisplay>());

    // Subzone tooltip
    overlay.addLayer(std::make_unique<SubzoneTooltipLayer>());
}

// ── WorldMapFacade Public Methods ────────────────────────────

WorldMapFacade::WorldMapFacade() : impl_(std::make_unique<Impl>()) {
    impl_->zoneMetadata.initialize();
    impl_->initOverlayLayers();
}

WorldMapFacade::~WorldMapFacade() {
    shutdown();
}

bool WorldMapFacade::initialize(VkContext* ctx, pipeline::AssetManager* am) {
    impl_->vkCtx = ctx;
    impl_->assetManager = am;
    if (!impl_->compositor.initialize(ctx, am)) return false;
    if (impl_->zoneHighlightLayer)
        impl_->zoneHighlightLayer->initialize(ctx, am);
    impl_->initialized = true;
    return true;
}

void WorldMapFacade::shutdown() {
    if (!impl_) return;
    if (impl_->zoneHighlightLayer)
        impl_->zoneHighlightLayer->clearTextures();
    impl_->compositor.shutdown();
    impl_->data.clear();
    impl_->initialized = false;
}

void WorldMapFacade::compositePass(VkCommandBuffer cmd) {
    impl_->compositor.flushStaleTextures();
    impl_->compositor.compositePass(cmd,
                                     impl_->data.zones(),
                                     impl_->exploration.exploredOverlays(),
                                     impl_->exploration.hasServerMask());
}

void WorldMapFacade::render(const glm::vec3& playerRenderPos,
                             int screenWidth, int screenHeight,
                             float playerYawDeg) {
    auto& d = *impl_;
    if (!d.initialized || !d.assetManager) return;

    // Update transition animation
    float now = static_cast<float>(ImGui::GetTime());
    float dt = now - d.lastFrameTime;
    d.lastFrameTime = now;
    d.viewState.updateTransition(dt);

    // Update exploration state
    if (!d.data.zones().empty()) {
        d.exploration.update(d.data.zones(), playerRenderPos,
                             d.viewState.currentZoneIdx(),
                             d.data.exploreFlagByAreaId());
        if (d.exploration.overlaysChanged() && d.viewState.currentZoneIdx() >= 0) {
            d.compositor.invalidateComposite();
            d.compositor.requestComposite(d.viewState.currentZoneIdx());
        }
    }

    // First-time open or zones lost after map change
    if (!d.open || d.data.zones().empty()) {
        d.open = true;
        if (d.data.zones().empty()) {
            d.data.loadZones(d.mapName, *d.assetManager);
            d.zoneMetadata.initialize();
            d.viewState.setCosmicEnabled(d.data.cosmicEnabled());
        }

        int bestContinent = findBestContinentForPlayer(d.data.zones(), playerRenderPos);
        if (bestContinent >= 0 && bestContinent != d.viewState.continentIdx()) {
            d.viewState.setContinentIdx(bestContinent);
            d.compositor.invalidateComposite();
        }

        int playerZone = findZoneForPlayer(d.data.zones(), playerRenderPos);
        if (playerZone >= 0 && d.viewState.continentIdx() >= 0 &&
            zoneBelongsToContinent(d.data.zones(), playerZone, d.viewState.continentIdx())) {
            d.compositor.loadZoneTextures(playerZone, d.data.zones(), d.mapName);
            d.compositor.loadOverlayTextures(playerZone, d.data.zones());
            d.viewState.setCurrentZoneIdx(playerZone);
            d.viewState.setLevel(ViewLevel::ZONE);
            d.exploration.update(d.data.zones(), playerRenderPos, playerZone,
                                 d.data.exploreFlagByAreaId());
            d.compositor.requestComposite(playerZone);
        } else if (d.viewState.continentIdx() >= 0) {
            d.compositor.loadZoneTextures(d.viewState.continentIdx(), d.data.zones(), d.mapName);
            d.compositor.requestComposite(d.viewState.continentIdx());
            d.viewState.setCurrentZoneIdx(d.viewState.continentIdx());
            d.viewState.setLevel(ViewLevel::CONTINENT);
        }
    }

    // Process input
    int hoveredZone = d.zoneHighlightLayer ? d.zoneHighlightLayer->hoveredZone() : -1;
    InputResult inputResult = d.input.process(d.viewState.currentLevel(),
                                               hoveredZone,
                                               d.viewState.cosmicEnabled());

    switch (inputResult.action) {
        case InputAction::CLOSE:
            d.open = false;
            d.userMapOverride = false;
            if (!d.pendingMapName.empty()) {
                d.mapName = d.pendingMapName;
                d.pendingMapName.clear();
            }
            return;

        case InputAction::ZOOM_IN: {
            int playerZone = findZoneForPlayer(d.data.zones(), playerRenderPos);
            // For continent→zone, verify the zone belongs to the current continent
            int candidateZone = hoveredZone >= 0 ? hoveredZone : playerZone;
            if (d.viewState.currentLevel() == ViewLevel::CONTINENT &&
                candidateZone >= 0 &&
                !zoneBelongsToContinent(d.data.zones(), candidateZone, d.viewState.continentIdx())) {
                candidateZone = -1;
            }
            // Bug fix: also validate playerZone against the continent so the
            // fallback inside zoomIn() doesn't navigate to the wrong continent.
            int validPlayerZone = playerZone;
            if (d.viewState.currentLevel() == ViewLevel::CONTINENT &&
                validPlayerZone >= 0 &&
                !zoneBelongsToContinent(d.data.zones(), validPlayerZone, d.viewState.continentIdx())) {
                validPlayerZone = -1;
            }
            auto zr = d.viewState.zoomIn(candidateZone, validPlayerZone);
            if (zr.changed && zr.targetIdx >= 0) {
                d.compositor.loadZoneTextures(zr.targetIdx, d.data.zones(), d.mapName);
                if (zr.newLevel == ViewLevel::ZONE) {
                    d.compositor.loadOverlayTextures(zr.targetIdx, d.data.zones());
                }
                d.compositor.requestComposite(zr.targetIdx);
            } else if (zr.changed && zr.newLevel == ViewLevel::WORLD) {
                d.switchToWorldView();
            }
            break;
        }

        case InputAction::ZOOM_OUT: {
            auto zr = d.viewState.zoomOut();
            if (zr.changed && zr.targetIdx >= 0) {
                d.compositor.loadZoneTextures(zr.targetIdx, d.data.zones(), d.mapName);
                d.compositor.requestComposite(zr.targetIdx);
            } else if (zr.changed && zr.newLevel == ViewLevel::WORLD) {
                d.switchToWorldView();
            } else if (zr.changed && zr.newLevel == ViewLevel::COSMIC) {
                if (d.data.cosmicIdx() >= 0) {
                    d.compositor.loadZoneTextures(d.data.cosmicIdx(), d.data.zones(), d.mapName);
                    d.compositor.requestComposite(d.data.cosmicIdx());
                    d.viewState.setCurrentZoneIdx(d.data.cosmicIdx());
                }
            }
            break;
        }

        case InputAction::CLICK_ZONE: {
            int hz = inputResult.targetIdx;
            if (hz >= 0) {
                // Use centralized resolver to handle cross-map zone navigation
                auto zoneResult = resolveZoneClick(hz, d.data.zones(), d.data.currentMapId());
                switch (zoneResult.action) {
                    case MapResolveAction::LOAD_MAP:
                        d.switchToMap(zoneResult.targetMapName);
                        break;
                    case MapResolveAction::ENTER_ZONE:
                        d.compositor.loadZoneTextures(hz, d.data.zones(), d.mapName);
                        d.compositor.loadOverlayTextures(hz, d.data.zones());
                        d.compositor.requestComposite(hz);
                        d.viewState.enterZone(hz);
                        break;
                    default:
                        break;
                }
            }
            break;
        }

        case InputAction::RIGHT_CLICK_BACK: {
            // Only process right-click if we're at zone or continent level
            if (d.viewState.currentLevel() == ViewLevel::ZONE &&
                d.viewState.continentIdx() >= 0) {
                d.compositor.loadZoneTextures(d.viewState.continentIdx(), d.data.zones(), d.mapName);
                d.compositor.requestComposite(d.viewState.continentIdx());
                d.viewState.setCurrentZoneIdx(d.viewState.continentIdx());
                d.viewState.setLevel(ViewLevel::CONTINENT);
            } else if (d.viewState.currentLevel() == ViewLevel::CONTINENT) {
                d.switchToWorldView();
            } else if (d.viewState.currentLevel() == ViewLevel::WORLD &&
                       d.viewState.cosmicEnabled()) {
                d.viewState.enterCosmicView();
                if (d.data.cosmicIdx() >= 0) {
                    d.compositor.loadZoneTextures(d.data.cosmicIdx(), d.data.zones(), d.mapName);
                    d.compositor.requestComposite(d.data.cosmicIdx());
                    d.viewState.setCurrentZoneIdx(d.data.cosmicIdx());
                }
            }
            break;
        }

        default:
            break;
    }

    if (!d.open) return;
    bool rightClickConsumed = (inputResult.action == InputAction::RIGHT_CLICK_BACK);
    d.renderImGuiOverlay(playerRenderPos, screenWidth, screenHeight, playerYawDeg, rightClickConsumed);
}

void WorldMapFacade::setMapName(const std::string& name) {
    auto& d = *impl_;
    // While the user has manually navigated to the world/cosmic overview,
    // remember the game's desired map but don't reset the view.
    if (d.userMapOverride) {
        d.pendingMapName = name;
        return;
    }
    if (d.mapName == name && !d.data.zones().empty()) return;
    d.mapName = name;

    if (d.zoneHighlightLayer)
        d.zoneHighlightLayer->clearTextures();
    d.compositor.detachZoneTextures();
    d.data.clear();
    d.viewState.setContinentIdx(-1);
    d.viewState.setCurrentZoneIdx(-1);
    d.compositor.invalidateComposite();
    d.viewState.setLevel(ViewLevel::WORLD);
    d.open = false;
}

void WorldMapFacade::setServerExplorationMask(const std::vector<uint32_t>& masks, bool hasData) {
    impl_->exploration.setServerMask(masks, hasData);
}

void WorldMapFacade::setPartyDots(std::vector<PartyDot> dots) {
    impl_->partyDots = std::move(dots);
}

void WorldMapFacade::setTaxiNodes(std::vector<TaxiNode> nodes) {
    impl_->taxiNodes = std::move(nodes);
}

void WorldMapFacade::setQuestPois(std::vector<QuestPOI> pois) {
    impl_->questPois = std::move(pois);
}

void WorldMapFacade::setCorpsePos(bool hasCorpse, glm::vec3 renderPos) {
    if (impl_->corpseMarkerLayer)
        impl_->corpseMarkerLayer->setCorpse(hasCorpse, renderPos);
}

bool WorldMapFacade::isOpen() const { return impl_->open; }
void WorldMapFacade::close() {
    impl_->open = false;
    impl_->userMapOverride = false;
    // Apply any map name that was deferred while in world/cosmic view
    if (!impl_->pendingMapName.empty()) {
        impl_->mapName = impl_->pendingMapName;
        impl_->pendingMapName.clear();
    }
}

// ── ImGui Overlay ────────────────────────────────────────────

void WorldMapFacade::Impl::renderImGuiOverlay(const glm::vec3& playerRenderPos,
                                                int screenWidth, int screenHeight,
                                                float playerYawDeg,
                                                bool rightClickConsumed) {
    float sw = static_cast<float>(screenWidth);
    float sh = static_cast<float>(screenHeight);

    // Use the full FBO (1024×768) for aspect ratio — all coordinate math
    // (kVOffset, zone DBC projection, ZMP grid) is calibrated for the full
    // tile grid, not the cropped 1002×668 content area.
    float mapAspect = static_cast<float>(CompositeRenderer::FBO_W) /
                      static_cast<float>(CompositeRenderer::FBO_H);
    float availW = sw * 0.70f;
    float availH = sh * 0.70f;
    float displayW, displayH;
    if (availW / availH > mapAspect) {
        displayH = availH;
        displayW = availH * mapAspect;
    } else {
        displayW = availW;
        displayH = availW / mapAspect;
    }

    // Floor to pixel boundary
    displayW = std::floor(displayW);
    displayH = std::floor(displayH);
    float mapX = std::floor((sw - displayW) / 2.0f);
    float mapY = std::floor((sh - displayH) / 2.0f);

    // Map window — styled like the character selection window
    ImGui::SetNextWindowPos(ImVec2(mapX, mapY), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(displayW, displayH), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    // Bug fix: pass nullptr instead of &open so ImGui's X-button doesn't
    // set open=false directly — that bypasses cleanup (userMapOverride,
    // pendingMapName) and causes immediate re-open on next render() call.
    // Close is handled by ESC / InputAction::CLOSE instead.
    if (ImGui::Begin("World Map", nullptr, flags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // imgMin/imgMax = the content area (after title bar)
        ImVec2 contentPos = ImGui::GetCursorScreenPos();
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        ImVec2 imgMin = contentPos;
        ImVec2 imgMax(contentPos.x + contentSize.x, contentPos.y + contentSize.y);
        displayW = contentSize.x;
        displayH = contentSize.y;
        // Show the full 1024×768 FBO — coordinate math (kVOffset, ZMP grid,
        // DBC zone projection) is all calibrated for the full tile grid.
        ImGui::Image(
            reinterpret_cast<ImTextureID>(compositor.displayDescriptorSet()),
            ImVec2(displayW, displayH),
            ImVec2(0, 0), ImVec2(1, 1));

        // Transition fade overlay
        const auto& trans = viewState.transition();
        if (trans.active) {
            float alpha = std::max(0.0f, 1.0f - trans.progress);
            if (alpha > 0.01f) {
                uint8_t fadeAlpha = static_cast<uint8_t>(alpha * 180.0f);
                drawList->AddRectFilled(imgMin, imgMax,
                                        IM_COL32(0, 0, 0, fadeAlpha));
            }
        }



        // Build continent index list (expansion-aware filtering, excludes cosmic)
        std::vector<int> continentIndices;
        int cosmicZoneIdx = data.cosmicIdx();
        bool hasLeafContinents = false;
        for (int i = 0; i < static_cast<int>(data.zones().size()); i++) {
            if (i == cosmicZoneIdx) continue;
            if (isLeafContinent(data.zones(), i)) { hasLeafContinents = true; break; }
        }
        for (int i = 0; i < static_cast<int>(data.zones().size()); i++) {
            if (i == cosmicZoneIdx) continue;
            if (data.zones()[i].areaID != 0) continue;
            if (hasLeafContinents) {
                if (isLeafContinent(data.zones(), i)) continentIndices.push_back(i);
            } else if (!isRootContinent(data.zones(), i)) {
                continentIndices.push_back(i);
            }
        }
        if (continentIndices.size() > 1) {
            std::vector<int> filtered;
            filtered.reserve(continentIndices.size());
            for (int idx : continentIndices) {
                if (data.zones()[idx].areaName == mapName) continue;
                filtered.push_back(idx);
            }
            if (!filtered.empty()) continentIndices = std::move(filtered);
        }
        if (continentIndices.empty()) {
            for (int i = 0; i < static_cast<int>(data.zones().size()); i++) {
                if (i == cosmicZoneIdx) continue;
                if (data.zones()[i].areaID == 0) continentIndices.push_back(i);
            }
        }

        // Expansion filtering
        {
            std::vector<int> expFiltered;
            expFiltered.reserve(continentIndices.size());
            for (int ci : continentIndices) {
                uint32_t mapId = data.zones()[ci].displayMapID;
                if (mapId == 530 && game::isPreWotlk() && !game::isActiveExpansion("tbc")) continue;
                if (mapId == 571 && game::isPreWotlk()) continue;
                expFiltered.push_back(ci);
            }
            if (!expFiltered.empty()) continentIndices = std::move(expFiltered);
        }

        // Update layer data pointers
        if (partyDotLayer) partyDotLayer->setDots(partyDots);
        if (taxiNodeLayer) taxiNodeLayer->setNodes(taxiNodes);
        if (poiMarkerLayer) poiMarkerLayer->setMarkers(data.poiMarkers());
        if (questPOILayer) questPOILayer->setPois(questPois);

        // Build layer context
        LayerContext layerCtx;
        layerCtx.drawList = drawList;
        layerCtx.imgMin = imgMin;
        layerCtx.displayW = displayW;
        layerCtx.displayH = displayH;
        layerCtx.playerRenderPos = playerRenderPos;
        layerCtx.playerYawDeg = playerYawDeg;
        layerCtx.currentZoneIdx = viewState.currentZoneIdx();
        layerCtx.continentIdx = viewState.continentIdx();
        layerCtx.currentMapId = data.currentMapId();
        layerCtx.viewLevel = viewState.currentLevel();
        layerCtx.zones = &data.zones();
        layerCtx.exploredZones = &exploration.exploredZones();
        layerCtx.exploredOverlays = &exploration.exploredOverlays();
        layerCtx.areaNameByAreaId = &data.areaNameByAreaId();
        layerCtx.fboW = CompositeRenderer::FBO_W;
        layerCtx.fboH = CompositeRenderer::FBO_H;

        // ZMP pixel map for continent-view hover
        if (data.hasZmpData()) {
            layerCtx.zmpGrid = &data.zmpGrid();
            layerCtx.hasZmpData = true;
            layerCtx.zmpResolveZoneIdx = [](const void* repo, uint32_t areaId) -> int {
                return static_cast<const DataRepository*>(repo)->zoneIndexForAreaId(areaId);
            };
            layerCtx.zmpRepoPtr = &data;
            layerCtx.zmpZoneBounds = &data.zmpZoneBounds();
        }

        // World-level: Azeroth map with clickable continent regions
        ViewLevel vl = viewState.currentLevel();
        if (vl == ViewLevel::WORLD) {
            bool goCosmic = false;
            if (viewState.cosmicEnabled() && !rightClickConsumed) {
                goCosmic = ImGui::GetIO().MouseClicked[1];
            }

            // "< Cosmic" back button (only if cosmic view is available for this expansion)
            if (viewState.cosmicEnabled()) {
                ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
                if (ImGui::Button("< Cosmic")) goCosmic = true;
                ImGui::PopStyleColor(3);
            }

            if (goCosmic) {
                viewState.enterCosmicView();
                if (data.cosmicIdx() >= 0) {
                    compositor.loadZoneTextures(data.cosmicIdx(), data.zones(), mapName);
                    compositor.requestComposite(data.cosmicIdx());
                    viewState.setCurrentZoneIdx(data.cosmicIdx());
                }
            }

            // Title
            ImVec2 titleSz = ImGui::CalcTextSize("World");
            float titleX = imgMin.x + (displayW - titleSz.x) * 0.5f;
            float titleY = imgMin.y - titleSz.y - 8.0f;
            if (titleY > 0.0f) {
                drawList->AddText(ImVec2(titleX + 1.0f, titleY + 1.0f),
                                  IM_COL32(0, 0, 0, 220), "World");
                drawList->AddText(ImVec2(titleX, titleY),
                                  IM_COL32(255, 215, 0, 255), "World");
            }

            // Clickable continent regions on the Azeroth map
            ImVec2 mp2 = ImGui::GetMousePos();
            auto& io = ImGui::GetIO();
            for (const auto& region : data.azerothRegions()) {
                float rx0 = imgMin.x + region.uvLeft * displayW;
                float ry0 = imgMin.y + region.uvTop * displayH;
                float rx1 = imgMin.x + region.uvRight * displayW;
                float ry1 = imgMin.y + region.uvBottom * displayH;

                bool hovered = (mp2.x >= rx0 && mp2.x <= rx1 &&
                                mp2.y >= ry0 && mp2.y <= ry1);

                if (hovered) {
                    // Map region mapId to the highlight texture folder name
                    std::string regionFolder = mapIdToFolder(region.mapId);

                    // Draw highlight texture covering the full map area
                    if (zoneHighlightLayer && !regionFolder.empty()) {
                        ImTextureID hlTex = zoneHighlightLayer->getHighlightTexture(regionFolder);
                        if (hlTex) {
                            drawList->AddImage(hlTex,
                                ImVec2(imgMin.x, imgMin.y),
                                ImVec2(imgMin.x + displayW, imgMin.y + displayH),
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32(255, 255, 255, 180));
                        } else {
                            drawList->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                                    IM_COL32(255, 215, 0, 25));
                        }
                    } else {
                        drawList->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                                IM_COL32(255, 215, 0, 25));
                    }
                    drawList->AddRect(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                      IM_COL32(255, 215, 0, 100), 0, 0, 1.5f);

                    ImFont* font = ImGui::GetFont();
                    ImVec2 labelSz = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f,
                                                          region.label.c_str());
                    float lx = (rx0 + rx1 - labelSz.x) * 0.5f;
                    float ly = ry0 - labelSz.y - 4.0f;
                    if (ly < imgMin.y) ly = ry0 + 4.0f;
                    drawList->AddText(ImVec2(lx + 1.0f, ly + 1.0f),
                                      IM_COL32(0, 0, 0, 200), region.label.c_str());
                    drawList->AddText(ImVec2(lx, ly),
                                      IM_COL32(255, 230, 100, 255), region.label.c_str());

                    if (io.MouseClicked[0]) {
                        // Use centralized map resolver to determine navigation action
                        auto resolveResult = resolveWorldRegionClick(
                            region.mapId, data.zones(), data.currentMapId(), data.cosmicIdx());
                        switch (resolveResult.action) {
                            case MapResolveAction::NAVIGATE_CONTINENT:
                                // Same map — just switch to the continent view
                                viewState.setContinentIdx(resolveResult.targetZoneIdx);
                                compositor.loadZoneTextures(resolveResult.targetZoneIdx, data.zones(), mapName);
                                compositor.requestComposite(resolveResult.targetZoneIdx);
                                viewState.setCurrentZoneIdx(resolveResult.targetZoneIdx);
                                viewState.setLevel(ViewLevel::CONTINENT);
                                break;
                            case MapResolveAction::LOAD_MAP:
                                switchToMap(resolveResult.targetMapName);
                                break;
                            default:
                                break;
                        }
                        break;
                    }
                }
            }
        } else if (vl == ViewLevel::CONTINENT && continentIndices.size() > 1) {
            ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
            for (size_t i = 0; i < continentIndices.size(); i++) {
                int ci = continentIndices[i];
                if (i > 0) ImGui::SameLine();
                const bool selected = (ci == viewState.continentIdx());
                if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.25f, 0.05f, 0.9f));
                std::string rawName = data.zones()[ci].areaName.empty() ? "Continent" : data.zones()[ci].areaName;
                if (rawName == "Azeroth") rawName = mapDisplayName(0);
                std::string label = rawName + "##" + std::to_string(ci);
                if (ImGui::Button(label.c_str())) {
                    viewState.setContinentIdx(ci);
                    compositor.loadZoneTextures(ci, data.zones(), mapName);
                    compositor.requestComposite(ci);
                    viewState.setCurrentZoneIdx(ci);
                }
                if (selected) ImGui::PopStyleColor();
            }
        }

        // Render all overlay layers
        overlay.render(layerCtx);

        // Zone view: back to continent + zone name
        if (vl == ViewLevel::ZONE && viewState.continentIdx() >= 0) {
            ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
            if (ImGui::Button("< Back")) {
                compositor.loadZoneTextures(viewState.continentIdx(), data.zones(), mapName);
                compositor.requestComposite(viewState.continentIdx());
                viewState.setCurrentZoneIdx(viewState.continentIdx());
                viewState.setLevel(ViewLevel::CONTINENT);
            }
            ImGui::PopStyleColor(3);

            int curIdx = viewState.currentZoneIdx();
            if (curIdx >= 0 && curIdx < static_cast<int>(data.zones().size())) {
                const char* zoneName = data.zones()[curIdx].areaName.c_str();
                ImVec2 nameSize = ImGui::CalcTextSize(zoneName);
                float nameY = mapY - nameSize.y - 8.0f;
                if (nameY > 0.0f) {
                    ImGui::SetCursorPos(ImVec2((sw - nameSize.x) / 2.0f, nameY));
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 0.9f), "%s", zoneName);
                }
            }
        }

        // Continent view: back to world + hovered zone name
        if (vl == ViewLevel::CONTINENT) {
            float localBtnY = (continentIndices.size() > 1 ? 40.0f : 8.0f);
            ImGui::SetCursorPos(ImVec2(8.0f, localBtnY));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
            if (ImGui::Button("< Azeroth")) {
                switchToWorldView();
            }
            ImGui::PopStyleColor(3);

            // Show hovered zone name above the map
            int hovZone = zoneHighlightLayer ? zoneHighlightLayer->hoveredZone() : -1;
            if (hovZone >= 0 && hovZone < static_cast<int>(data.zones().size())) {
                const std::string& rawName = data.zones()[hovZone].areaName;
                if (!rawName.empty()) {
                    const ZoneMeta* meta = zoneMetadata.find(rawName);
                    std::string hoverLabel = ZoneMetadata::formatHoverLabel(rawName, meta);

                    ImVec2 hoverSz = ImGui::CalcTextSize(hoverLabel.c_str());
                    float hx = imgMin.x + (displayW - hoverSz.x) * 0.5f;
                    float hy = imgMin.y - hoverSz.y - 8.0f;
                    if (hy > 0.0f) {
                        drawList->AddText(ImVec2(hx + 1.0f, hy + 1.0f),
                                          IM_COL32(0, 0, 0, 220), hoverLabel.c_str());
                        ImU32 hoverColor = IM_COL32(255, 215, 0, 255);
                        if (meta) {
                            switch (meta->faction) {
                                case ZoneFaction::Alliance: hoverColor = IM_COL32(100, 160, 255, 255); break;
                                case ZoneFaction::Horde:    hoverColor = IM_COL32(255, 80, 80, 255); break;
                                default: break;
                            }
                        }
                        drawList->AddText(ImVec2(hx, hy), hoverColor, hoverLabel.c_str());
                    }
                }
            }
        }

        // Cosmic view: title + clickable landmass regions
        if (vl == ViewLevel::COSMIC) {
            ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.1f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::colors::kBrightGold);
            if (ImGui::Button("< Azeroth")) {
                switchToWorldView();
            }
            ImGui::PopStyleColor(3);

            ImVec2 titleSz = ImGui::CalcTextSize("Cosmic");
            float titleX = imgMin.x + (displayW - titleSz.x) * 0.5f;
            float titleY = imgMin.y - titleSz.y - 8.0f;
            if (titleY > 0.0f) {
                drawList->AddText(ImVec2(titleX + 1.0f, titleY + 1.0f),
                                  IM_COL32(0, 0, 0, 220), "Cosmic");
                drawList->AddText(ImVec2(titleX, titleY),
                                  IM_COL32(255, 215, 0, 255), "Cosmic");
            }

            ImVec2 mp2 = ImGui::GetMousePos();
            auto& io = ImGui::GetIO();

            for (const auto& entry : data.cosmicMaps()) {
                float rx0 = imgMin.x + entry.uvLeft * displayW;
                float ry0 = imgMin.y + entry.uvTop * displayH;
                float rx1 = imgMin.x + entry.uvRight * displayW;
                float ry1 = imgMin.y + entry.uvBottom * displayH;

                bool hovered = (mp2.x >= rx0 && mp2.x <= rx1 &&
                                mp2.y >= ry0 && mp2.y <= ry1);

                if (hovered) {
                    // Cosmic highlight files: cosmic-{label}-highlight.blp
                    std::string cosmicLabel = entry.label;
                    std::transform(cosmicLabel.begin(), cosmicLabel.end(), cosmicLabel.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::string cosmicKey = "cosmic-" + cosmicLabel;
                    std::string cosmicPath = "Interface\\WorldMap\\Cosmic\\cosmic-" + cosmicLabel + "-highlight.blp";

                    // ─── Cosmic Highlight Rendering Logic ───────────────────
                    //
                    // SOURCE TEXTURES:
                    //   cosmic-azeroth-highlight.blp  → 512×512 px (DXT3, has alpha)
                    //   cosmic-outland-highlight.blp  → 512×512 px (DXT3, has alpha)
                    //   The glow is baked into the alpha channel:
                    //     - Azeroth highlight: glow sits in the RIGHT-CENTER of the texture
                    //     - Outland highlight: glow sits in the LEFT-CENTER of the texture
                    //
                    // DISPLAY AREA:
                    //   The map on screen is displayW × displayH pixels.
                    //   displayW/displayH ≈ 1002/668 ≈ 1.5:1 (wider than tall).
                    //   imgMin = top-left corner,  imgMax = bottom-right corner.
                    //
                    // THE PROBLEM:
                    //   512×512 is square, but the display area is 1.5× wider than tall.
                    //   If we stretch the texture to fill the full display area
                    //   (imgMin → imgMax), the circular glow becomes an ellipse
                    //   (horizontally stretched ~50%).
                    //   If we render it as a square (side = displayH), it has the
                    //   correct aspect but only covers 2/3 of the map width.
                    //
                    // CURRENT APPROACH:
                    //   Render as a square (side = displayH), anchored:
                    //     Azeroth → flush to the RIGHT edge of the map (glow lands bottom-right)
                    //     Outland → flush to the LEFT edge of the map  (glow lands top-left)
                    //   This preserves the 1:1 aspect ratio of the glow shape.
                    //
                    // TO ADJUST:
                    //   • Make glow wider:  increase hlW (e.g. displayH * 1.2f)
                    //   • Make glow taller: increase hlH (e.g. displayH * 1.1f)
                    //   • Full stretch (like WoW original): hlW = displayW, hlH = displayH
                    //   • Shift glow position: adjust hlX offset
                    //
                    float hlW = displayW;       // width  of highlight rect (= square)
                    float hlH = displayH;       // height of highlight rect (= square)
                    float hlX, hlY;
                    if (cosmicLabel == "azeroth") {
                        hlX = imgMax.x - hlW;   // flush right
                        hlY = imgMax.y - hlH;   // flush bottom
                    } else {
                        hlX = imgMin.x;          // flush left
                        hlY = imgMin.y;          // flush top
                    }

                    if (zoneHighlightLayer) {
                        ImTextureID hlTex = zoneHighlightLayer->getHighlightTexture(cosmicKey, cosmicPath);
                        if (hlTex) {
                            drawList->AddImage(hlTex,
                                ImVec2(hlX, hlY),
                                ImVec2(hlX + hlW, hlY + hlH),
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32(255, 255, 255, 180));
                        } else {
                            drawList->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                                    IM_COL32(255, 215, 0, 25));
                        }
                    } else {
                        drawList->AddRectFilled(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                                IM_COL32(255, 215, 0, 25));
                    }
                    drawList->AddRect(ImVec2(rx0, ry0), ImVec2(rx1, ry1),
                                      IM_COL32(255, 215, 0, 100), 0, 0, 1.5f);

                    ImFont* font = ImGui::GetFont();
                    ImVec2 labelSz = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f,
                                                          entry.label.c_str());
                    float lx = (rx0 + rx1 - labelSz.x) * 0.5f;
                    float ly = ry0 - labelSz.y - 4.0f;
                    if (ly < imgMin.y) ly = ry0 + 4.0f;
                    drawList->AddText(ImVec2(lx + 1.0f, ly + 1.0f),
                                      IM_COL32(0, 0, 0, 200), entry.label.c_str());
                    drawList->AddText(ImVec2(lx, ly),
                                      IM_COL32(255, 230, 100, 255), entry.label.c_str());

                    if (io.MouseClicked[0]) {
                        if (entry.label == "Outland") {
                            switchToMap("Expansion01");
                        } else {
                            viewState.enterWorldView();
                            int wIdx = data.worldIdx();
                            if (wIdx >= 0) {
                                compositor.loadZoneTextures(wIdx, data.zones(), mapName);
                                compositor.invalidateComposite();
                                compositor.requestComposite(wIdx);
                                viewState.setCurrentZoneIdx(wIdx);
                            }
                        }
                        break;
                    }
                }
            }
        }

        // Help text
        const char* helpText;
        if (vl == ViewLevel::ZONE)
            helpText = "Right-click to zoom out | M or Escape to close";
        else if (vl == ViewLevel::COSMIC)
            helpText = "Scroll in or click to zoom in | M or Escape to close";
        else if (vl == ViewLevel::WORLD && viewState.cosmicEnabled())
            helpText = "Click a continent | Right-click for Cosmic view | M or Escape to close";
        else if (vl == ViewLevel::WORLD)
            helpText = "Click a continent | M or Escape to close";
        else
            helpText = "Click zone to open | Right-click to zoom out | M or Escape to close";

        ImVec2 textSize = ImGui::CalcTextSize(helpText);
        float textX = mapX + (displayW - textSize.x) / 2.0f;
        float textY = mapY + displayH - textSize.y - 4.0f;
        ImGui::SetCursorScreenPos(ImVec2(textX, textY));
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 0.8f), "%s", helpText);
    }
    ImGui::End();

    ImGui::PopStyleVar(2);  // WindowPadding + ItemSpacing
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
