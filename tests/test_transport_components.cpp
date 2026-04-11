// tests/test_transport_components.cpp
// Unit tests for TransportClockSync and TransportAnimator (Phase 3 extractions).
#include <catch_amalgamated.hpp>
#include "game/transport_clock_sync.hpp"
#include "game/transport_animator.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "math/spline.hpp"
#include <glm/gtc/constants.hpp>
#include <cmath>

using namespace wowee::game;
using namespace wowee::math;

// ── Helper: build a simple circular path ──────────────────────────
static PathEntry makeCirclePath() {
    // Circle-ish path with 4 points, 4000ms duration
    std::vector<SplineKey> keys = {
        {0,    glm::vec3(0.0f,  0.0f, 0.0f)},
        {1000, glm::vec3(10.0f, 0.0f, 0.0f)},
        {2000, glm::vec3(10.0f, 10.0f, 0.0f)},
        {3000, glm::vec3(0.0f,  10.0f, 0.0f)},
        {4000, glm::vec3(0.0f,  0.0f, 0.0f)},
    };
    CatmullRomSpline spline(std::move(keys), /*timeClosed=*/true);
    return PathEntry(std::move(spline), /*pathId=*/100, /*zOnly=*/false, /*fromDBC=*/true, /*worldCoords=*/false);
}

// ── Helper: create a fresh ActiveTransport ────────────────────────
static ActiveTransport makeTransport(uint64_t guid = 1, uint32_t pathId = 100) {
    ActiveTransport t{};
    t.guid = guid;
    t.pathId = pathId;
    t.basePosition = glm::vec3(100.0f, 200.0f, 0.0f);
    t.position = glm::vec3(100.0f, 200.0f, 0.0f);
    t.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    t.playerOnBoard = false;
    t.playerLocalOffset = glm::vec3(0);
    t.hasDeckBounds = false;
    t.localClockMs = 0;
    t.hasServerClock = false;
    t.serverClockOffsetMs = 0;
    t.useClientAnimation = true;
    t.clientAnimationReverse = false;
    t.serverYaw = 0.0f;
    t.hasServerYaw = false;
    t.serverYawFlipped180 = false;
    t.serverYawAlignmentScore = 0;
    t.lastServerUpdate = 0.0;
    t.serverUpdateCount = 0;
    t.serverLinearVelocity = glm::vec3(0);
    t.serverAngularVelocity = 0.0f;
    t.hasServerVelocity = false;
    t.allowBootstrapVelocity = true;
    t.isM2 = false;
    return t;
}

// ══════════════════════════════════════════════════════════════════
// TransportClockSync tests
// ══════════════════════════════════════════════════════════════════

TEST_CASE("ClockSync: client animation advances localClockMs", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.useClientAnimation = true;
    t.hasServerClock = false;

    uint32_t pathTimeMs = 0;
    bool result = sync.computePathTime(t, path.spline, 1.0, 0.016f, pathTimeMs);
    REQUIRE(result);
    REQUIRE(t.localClockMs > 0);  // Should have advanced
    REQUIRE(pathTimeMs == t.localClockMs % path.spline.durationMs());
}

TEST_CASE("ClockSync: server clock mode wraps correctly", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.hasServerClock = true;
    t.serverClockOffsetMs = 500;  // Server is 500ms ahead

    uint32_t pathTimeMs = 0;
    double elapsedTime = 3.7;  // 3700ms local → 4200ms server → 200ms wrapped (dur=4000)
    bool result = sync.computePathTime(t, path.spline, elapsedTime, 0.016f, pathTimeMs);
    REQUIRE(result);
    REQUIRE(pathTimeMs == 200);
}

TEST_CASE("ClockSync: strict server mode returns false", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.useClientAnimation = false;
    t.hasServerClock = false;

    uint32_t pathTimeMs = 0;
    bool result = sync.computePathTime(t, path.spline, 1.0, 0.016f, pathTimeMs);
    REQUIRE_FALSE(result);
}

TEST_CASE("ClockSync: reverse client animation decrements", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.useClientAnimation = true;
    t.clientAnimationReverse = true;
    t.localClockMs = 2000;

    uint32_t pathTimeMs = 0;
    bool result = sync.computePathTime(t, path.spline, 1.0, 0.5f, pathTimeMs);
    REQUIRE(result);
    // localClockMs should have decreased by ~500ms
    REQUIRE(t.localClockMs < 2000);
}

TEST_CASE("ClockSync: processServerUpdate sets yaw and rotation", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();

    glm::vec3 pos(105.0f, 205.0f, 1.0f);
    float yaw = 1.5f;
    sync.processServerUpdate(t, &path, pos, yaw, 10.0);

    REQUIRE(t.serverUpdateCount == 1);
    REQUIRE(t.hasServerYaw);
    REQUIRE(t.serverYaw == Catch::Approx(1.5f));
    REQUIRE(t.position == pos);
}

TEST_CASE("ClockSync: yaw flip detection after repeated misaligned updates", "[transport_clock_sync]") {
    TransportClockSync sync;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.useClientAnimation = false;

    // Simulate transport moving east (+X) but reporting yaw pointing west (pi)
    float westYaw = glm::pi<float>();
    glm::vec3 pos(100.0f, 200.0f, 0.0f);
    sync.processServerUpdate(t, &path, pos, westYaw, 1.0);

    // Send several updates moving east with west-facing yaw
    for (int i = 1; i <= 8; i++) {
        pos.x += 5.0f;
        sync.processServerUpdate(t, &path, pos, westYaw, 1.0 + i * 0.5);
    }

    // After enough misaligned updates, should have flipped
    REQUIRE(t.serverYawFlipped180);
}

// ══════════════════════════════════════════════════════════════════
// TransportAnimator tests
// ══════════════════════════════════════════════════════════════════

TEST_CASE("Animator: evaluateAndApply updates position from spline", "[transport_animator]") {
    TransportAnimator animator;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.hasServerYaw = false;

    animator.evaluateAndApply(t, path, 0);
    // At t=0, path offset is (0,0,0), so pos = base + (0,0,0) = (100,200,0)
    REQUIRE(t.position.x == Catch::Approx(100.0f));
    REQUIRE(t.position.y == Catch::Approx(200.0f));

    animator.evaluateAndApply(t, path, 1000);
    // At t=1000, path offset is (10,0,0), so pos = base + (10,0,0) = (110,200,0)
    REQUIRE(t.position.x == Catch::Approx(110.0f));
}

TEST_CASE("Animator: uses server yaw when available", "[transport_animator]") {
    TransportAnimator animator;
    auto path = makeCirclePath();
    auto t = makeTransport();
    t.hasServerYaw = true;
    t.serverYaw = 1.0f;
    t.serverYawFlipped180 = false;

    animator.evaluateAndApply(t, path, 500);
    // Rotation should be based on serverYaw=1.0, not spline tangent
    float expectedYaw = 1.0f;
    glm::quat expected = glm::angleAxis(expectedYaw, glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(t.rotation.w == Catch::Approx(expected.w).margin(0.01f));
    REQUIRE(t.rotation.z == Catch::Approx(expected.z).margin(0.01f));
}

TEST_CASE("Animator: Z clamping on non-world-coord client anim", "[transport_animator]") {
    TransportAnimator animator;

    // Build a path with a deep negative Z offset
    std::vector<SplineKey> keys = {
        {0,    glm::vec3(0.0f, 0.0f, 0.0f)},
        {1000, glm::vec3(5.0f, 0.0f, -50.0f)},  // Deep negative Z
        {2000, glm::vec3(10.0f, 0.0f, 0.0f)},
    };
    CatmullRomSpline spline(std::move(keys), false);
    PathEntry path(std::move(spline), 200, false, true, false);

    auto t = makeTransport();
    t.useClientAnimation = true;
    t.serverUpdateCount = 0;  // <= 1, so Z clamping applies

    animator.evaluateAndApply(t, path, 1000);
    // Z should be clamped to >= -2.0 (kMinFallbackZOffset)
    REQUIRE(t.position.z >= (t.basePosition.z - 2.0f));
}
