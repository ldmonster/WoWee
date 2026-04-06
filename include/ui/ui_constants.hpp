#pragma once

#include <cstdint>

// UI-layer constants: colors, layout sizes, effect timing.

namespace wowee {
namespace ui {

// ---------------------------------------------------------------------------
// Target selection-circle difficulty colours (WoW-canonical)
// ---------------------------------------------------------------------------
// Stored as {R, G, B} in [0,1].  Alpha is set by the caller.
struct Colour3f { float r, g, b; };

constexpr Colour3f SEL_COLOR_DEFAULT_YELLOW = {1.0f, 1.0f, 0.3f};
constexpr Colour3f SEL_COLOR_RED            = {1.0f, 0.1f, 0.1f};
constexpr Colour3f SEL_COLOR_ORANGE         = {1.0f, 0.5f, 0.1f};
constexpr Colour3f SEL_COLOR_YELLOW         = {1.0f, 1.0f, 0.1f};
constexpr Colour3f SEL_COLOR_GREEN          = {0.3f, 1.0f, 0.3f};
constexpr Colour3f SEL_COLOR_GREY           = {0.6f, 0.6f, 0.6f};
constexpr Colour3f SEL_COLOR_DEAD           = {0.5f, 0.5f, 0.5f};

// Level-diff thresholds that select colours above.
constexpr int MOB_LEVEL_DIFF_RED    = 10;   // ≥ 10 levels above player → red
constexpr int MOB_LEVEL_DIFF_ORANGE = 5;    // ≥ 5      → orange
constexpr int MOB_LEVEL_DIFF_YELLOW_FLOOR = -2; // ≥ -2 → yellow, else green

// Selection circle world-unit bounds
constexpr float SEL_CIRCLE_MIN_RADIUS = 0.8f;
constexpr float SEL_CIRCLE_MAX_RADIUS = 8.0f;

// ---------------------------------------------------------------------------
// Damage flash / vignette effect
// ---------------------------------------------------------------------------
constexpr float DAMAGE_FLASH_FADE_SPEED      = 2.0f;   // alpha units/sec
constexpr float DAMAGE_FLASH_ALPHA_SCALE     = 100.0f;  // multiplier
constexpr uint8_t DAMAGE_FLASH_RED_CHANNEL   = 200;     // IM_COL32 R channel
constexpr float DAMAGE_VIGNETTE_THICKNESS    = 0.12f;   // fraction of screen

// ---------------------------------------------------------------------------
// Low-health pulsing vignette
// ---------------------------------------------------------------------------
constexpr float LOW_HEALTH_THRESHOLD_PCT      = 0.20f;  // start at 20% HP
constexpr float LOW_HEALTH_PULSE_FREQUENCY    = 9.4f;   // angular speed (~1.5 Hz)
constexpr float LOW_HEALTH_MAX_ALPHA          = 90.0f;
constexpr float LOW_HEALTH_VIGNETTE_THICKNESS = 0.15f;

// ---------------------------------------------------------------------------
// Level-up flash overlay
// ---------------------------------------------------------------------------
constexpr float LEVELUP_FLASH_FADE_SPEED      = 1.0f;   // alpha units/sec
constexpr float LEVELUP_FLASH_ALPHA_SCALE     = 160.0f;
constexpr float LEVELUP_VIGNETTE_THICKNESS    = 0.18f;
constexpr float LEVELUP_TEXT_FONT_SIZE        = 28.0f;

// ---------------------------------------------------------------------------
// Ghost / death state
// ---------------------------------------------------------------------------
constexpr float GHOST_OPACITY = 0.5f;

// ---------------------------------------------------------------------------
// Click / interaction thresholds
// ---------------------------------------------------------------------------
constexpr float CLICK_THRESHOLD_PX = 5.0f;

} // namespace ui
} // namespace wowee
