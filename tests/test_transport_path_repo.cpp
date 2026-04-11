// Tests for TransportPathRepository (Phase 3 of spline refactoring).
// Verifies PathEntry wrapping, path operations, and CatmullRomSpline integration.
#include <catch2/catch_amalgamated.hpp>
#include "game/transport_path_repository.hpp"
#include "pipeline/asset_manager.hpp"
#include "math/spline.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

// ── Minimal stubs for pipeline symbols referenced by DBC loading functions ──
// The test never calls loadTransportAnimationDBC / loadTaxiPathNodeDBC,
// but the linker still needs these symbols from the compiled translation unit.
namespace wowee::pipeline {
AssetManager::AssetManager() {}
AssetManager::~AssetManager() {}
std::vector<uint8_t> AssetManager::readFile(const std::string&) const { return {}; }
std::vector<uint8_t> AssetManager::readFileOptional(const std::string&) const { return {}; }
} // namespace wowee::pipeline

using namespace wowee;

// ── Helpers ────────────────────────────────────────────────────

static constexpr float kEps = 0.001f;

static void requireVec3Near(const glm::vec3& v, float x, float y, float z,
                             float eps = kEps) {
    REQUIRE(std::abs(v.x - x) < eps);
    REQUIRE(std::abs(v.y - y) < eps);
    REQUIRE(std::abs(v.z - z) < eps);
}

// ── PathEntry construction ─────────────────────────────────────

TEST_CASE("PathEntry wraps CatmullRomSpline with metadata", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {1000, {10.0f, 0.0f, 0.0f}},
        {2000, {20.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 42, false, true, false);

    REQUIRE(entry.pathId == 42);
    REQUIRE(entry.fromDBC == true);
    REQUIRE(entry.zOnly == false);
    REQUIRE(entry.worldCoords == false);
    REQUIRE(entry.spline.keyCount() == 3);
    REQUIRE(entry.spline.durationMs() == 2000);
}

TEST_CASE("PathEntry is move-constructible", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {500, {5.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 99, true, false, true);

    game::PathEntry moved(std::move(entry));
    REQUIRE(moved.pathId == 99);
    REQUIRE(moved.zOnly == true);
    REQUIRE(moved.worldCoords == true);
    REQUIRE(moved.spline.keyCount() == 2);
}

// ── Repository: loadPathFromNodes ──────────────────────────────

TEST_CASE("loadPathFromNodes stores a retrievable path", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    std::vector<glm::vec3> waypoints = {
        {0.0f, 0.0f, 0.0f},
        {100.0f, 0.0f, 0.0f},
        {200.0f, 0.0f, 0.0f},
    };

    repo.loadPathFromNodes(1001, waypoints, true, 10.0f);
    auto* entry = repo.findPath(1001);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->pathId == 1001);
    REQUIRE(entry->fromDBC == false);
    REQUIRE(entry->zOnly == false);
    // 3 waypoints + 1 wrap point = 4 keys
    REQUIRE(entry->spline.keyCount() == 4);
    REQUIRE(entry->spline.durationMs() > 0);
}

TEST_CASE("loadPathFromNodes single waypoint creates stationary path", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    repo.loadPathFromNodes(2001, {{5.0f, 6.0f, 7.0f}}, false, 10.0f);

    auto* entry = repo.findPath(2001);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->spline.keyCount() == 1);
    // Stationary: duration is the last key's time (0ms for single key)
    requireVec3Near(entry->spline.evaluatePosition(0), 5.0f, 6.0f, 7.0f);
}

TEST_CASE("loadPathFromNodes non-looping omits wrap point", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    std::vector<glm::vec3> waypoints = {
        {0.0f, 0.0f, 0.0f},
        {50.0f, 0.0f, 0.0f},
    };

    repo.loadPathFromNodes(3001, waypoints, false, 10.0f);
    auto* entry = repo.findPath(3001);
    REQUIRE(entry != nullptr);
    // Non-looping: 2 waypoints, no wrap point
    REQUIRE(entry->spline.keyCount() == 2);
}

TEST_CASE("loadPathFromNodes looping adds wrap point", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    std::vector<glm::vec3> waypoints = {
        {0.0f, 0.0f, 0.0f},
        {50.0f, 0.0f, 0.0f},
    };

    repo.loadPathFromNodes(3002, waypoints, true, 10.0f);
    auto* entry = repo.findPath(3002);
    REQUIRE(entry != nullptr);
    // Looping: 2 waypoints + 1 wrap point = 3 keys
    REQUIRE(entry->spline.keyCount() == 3);
    // Wrap point should match first waypoint
    const auto& keys = entry->spline.keys();
    requireVec3Near(keys.back().position, 0.0f, 0.0f, 0.0f);
}

// ── Repository: findPath / storePath / hasPathForEntry ──────────

TEST_CASE("findPath returns nullptr for missing paths", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    REQUIRE(repo.findPath(999) == nullptr);
}

TEST_CASE("storePath overwrites existing paths", "[transport_path_repo]") {
    game::TransportPathRepository repo;

    // Store first path
    std::vector<math::SplineKey> keys1 = {{0, {0, 0, 0}}, {1000, {10, 0, 0}}};
    math::CatmullRomSpline spline1(std::move(keys1), false);
    repo.storePath(500, game::PathEntry(std::move(spline1), 500, false, true, false));

    auto* e1 = repo.findPath(500);
    REQUIRE(e1 != nullptr);
    REQUIRE(e1->spline.keyCount() == 2);

    // Overwrite with different path
    std::vector<math::SplineKey> keys2 = {{0, {0, 0, 0}}, {500, {5, 0, 0}}, {1000, {10, 5, 0}}};
    math::CatmullRomSpline spline2(std::move(keys2), false);
    repo.storePath(500, game::PathEntry(std::move(spline2), 500, true, false, true));

    auto* e2 = repo.findPath(500);
    REQUIRE(e2 != nullptr);
    REQUIRE(e2->spline.keyCount() == 3);
    REQUIRE(e2->zOnly == true);
    REQUIRE(e2->worldCoords == true);
}

TEST_CASE("hasPathForEntry checks fromDBC flag", "[transport_path_repo]") {
    game::TransportPathRepository repo;

    // Non-DBC path
    repo.loadPathFromNodes(700, {{0, 0, 0}, {10, 0, 0}}, true, 10.0f);
    REQUIRE(repo.hasPathForEntry(700) == false);  // fromDBC=false

    // DBC path (via storePath)
    std::vector<math::SplineKey> keys = {{0, {0, 0, 0}}, {1000, {10, 0, 0}}};
    math::CatmullRomSpline spline(std::move(keys), false);
    repo.storePath(701, game::PathEntry(std::move(spline), 701, false, true, false));
    REQUIRE(repo.hasPathForEntry(701) == true);
}

// ── Repository: hasUsableMovingPathForEntry ─────────────────────

TEST_CASE("hasUsableMovingPathForEntry rejects stationary/z-only", "[transport_path_repo]") {
    game::TransportPathRepository repo;

    // Single-point path (stationary)
    std::vector<math::SplineKey> keys1 = {{0, {0, 0, 0}}};
    math::CatmullRomSpline sp1(std::move(keys1), false);
    repo.storePath(800, game::PathEntry(std::move(sp1), 800, false, true, false));
    REQUIRE(repo.hasUsableMovingPathForEntry(800) == false);

    // Z-only path (flagged)
    std::vector<math::SplineKey> keys2 = {{0, {0, 0, 0}}, {1000, {0, 0, 5}}};
    math::CatmullRomSpline sp2(std::move(keys2), false);
    repo.storePath(801, game::PathEntry(std::move(sp2), 801, true, true, false));
    REQUIRE(repo.hasUsableMovingPathForEntry(801) == false);

    // Moving XY path
    std::vector<math::SplineKey> keys3 = {{0, {0, 0, 0}}, {1000, {100, 0, 0}}};
    math::CatmullRomSpline sp3(std::move(keys3), false);
    repo.storePath(802, game::PathEntry(std::move(sp3), 802, false, true, false));
    REQUIRE(repo.hasUsableMovingPathForEntry(802) == true);
}

// ── Repository: inferDbcPathForSpawn ────────────────────────────

TEST_CASE("inferDbcPathForSpawn finds nearest DBC path", "[transport_path_repo]") {
    game::TransportPathRepository repo;

    // Path A at (100, 0, 0)
    std::vector<math::SplineKey> keysA = {{0, {100, 0, 0}}, {1000, {200, 0, 0}}};
    math::CatmullRomSpline spA(std::move(keysA), false);
    repo.storePath(10, game::PathEntry(std::move(spA), 10, false, true, false));

    // Path B at (500, 0, 0)
    std::vector<math::SplineKey> keysB = {{0, {500, 0, 0}}, {1000, {600, 0, 0}}};
    math::CatmullRomSpline spB(std::move(keysB), false);
    repo.storePath(20, game::PathEntry(std::move(spB), 20, false, true, false));

    // Spawn near Path A
    uint32_t result = repo.inferDbcPathForSpawn({105.0f, 0.0f, 0.0f}, 200.0f, true);
    REQUIRE(result == 10);

    // Spawn near Path B
    result = repo.inferDbcPathForSpawn({510.0f, 0.0f, 0.0f}, 200.0f, true);
    REQUIRE(result == 20);

    // Spawn too far from both
    result = repo.inferDbcPathForSpawn({9999.0f, 0.0f, 0.0f}, 200.0f, true);
    REQUIRE(result == 0);
}

TEST_CASE("inferMovingPathForSpawn skips z-only paths", "[transport_path_repo]") {
    game::TransportPathRepository repo;

    // Z-only path near spawn
    std::vector<math::SplineKey> keys = {{0, {10, 0, 0}}, {1000, {10, 0, 5}}};
    math::CatmullRomSpline sp(std::move(keys), false);
    repo.storePath(30, game::PathEntry(std::move(sp), 30, true, true, false));

    // inferMovingPathForSpawn passes allowZOnly=false
    uint32_t result = repo.inferMovingPathForSpawn({10.0f, 0.0f, 0.0f}, 200.0f);
    REQUIRE(result == 0);
}

// ── Repository: taxi paths ─────────────────────────────────────

TEST_CASE("hasTaxiPath and findTaxiPath", "[transport_path_repo]") {
    game::TransportPathRepository repo;
    REQUIRE(repo.hasTaxiPath(100) == false);
    REQUIRE(repo.findTaxiPath(100) == nullptr);

    // loadTaxiPathNodeDBC would populate this, but we can't test it without AssetManager.
    // This just verifies the API works with empty data.
}

// ── Spline evaluation through PathEntry (Phase 3 integration) ──

TEST_CASE("PathEntry spline evaluates position at midpoint", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {1000, {100.0f, 0.0f, 0.0f}},
        {2000, {200.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 1, false, true, false);

    // At t=1000ms, should be near (100, 0, 0) — exactly at key 1
    glm::vec3 pos = entry.spline.evaluatePosition(1000);
    requireVec3Near(pos, 100.0f, 0.0f, 0.0f);
}

TEST_CASE("PathEntry spline evaluates position at interpolated time", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {1000, {100.0f, 0.0f, 0.0f}},
        {2000, {200.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 1, false, true, false);

    // At t=500ms, should be approximately (50, 0, 0)
    glm::vec3 pos = entry.spline.evaluatePosition(500);
    REQUIRE(pos.x > 40.0f);
    REQUIRE(pos.x < 60.0f);
    REQUIRE(std::abs(pos.y) < 1.0f);
}

TEST_CASE("PathEntry spline evaluate returns tangent for orientation", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {1000, {100.0f, 0.0f, 0.0f}},
        {2000, {200.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 1, false, true, false);

    // Tangent at midpoint should point roughly in +X direction
    auto result = entry.spline.evaluate(1000);
    REQUIRE(result.tangent.x > 0.0f);
    REQUIRE(std::abs(result.tangent.y) < 1.0f);
}

TEST_CASE("PathEntry findNearestKey finds closest waypoint", "[transport_path_repo]") {
    std::vector<math::SplineKey> keys = {
        {0, {0.0f, 0.0f, 0.0f}},
        {1000, {100.0f, 0.0f, 0.0f}},
        {2000, {200.0f, 0.0f, 0.0f}},
    };
    math::CatmullRomSpline spline(std::move(keys), false);
    game::PathEntry entry(std::move(spline), 1, false, true, false);

    // Point near key 1 (100, 0, 0)
    size_t nearest = entry.spline.findNearestKey({105.0f, 0.0f, 0.0f});
    REQUIRE(nearest == 1);

    // Point near key 2 (200, 0, 0)
    nearest = entry.spline.findNearestKey({195.0f, 0.0f, 0.0f});
    REQUIRE(nearest == 2);
}
