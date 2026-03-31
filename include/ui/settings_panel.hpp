#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <functional>
#include <cstdint>

namespace wowee {
namespace rendering { class Renderer; }
namespace ui {

class InventoryScreen;
class ChatPanel;

/**
 * Settings panel (extracted from GameScreen)
 *
 * Owns all settings UI rendering, settings state variables, and
 * graphics preset logic.  Save/load remains in GameScreen since
 * it serialises cross-cutting state (chat, quest tracker, etc.).
 */
class SettingsPanel {
public:
    // ---- Settings UI visibility flags (written by EscapeMenu / Escape key) ----
    bool showEscapeSettingsNotice = false;
    bool showSettingsWindow = false;
    bool settingsInit = false;

    // ---- Pending video / graphics settings ----
    bool pendingFullscreen = false;
    bool pendingVsync = false;
    int pendingResIndex = 0;
    bool pendingShadows = true;
    float pendingShadowDistance = 300.0f;
    bool pendingWaterRefraction = true;
    int pendingBrightness = 50; // 0-100, maps to 0.0-2.0 (50 = 1.0 default)

    // ---- Pending audio settings ----
    int pendingMasterVolume = 100;
    int pendingMusicVolume = 30;
    int pendingAmbientVolume = 100;
    int pendingUiVolume = 100;
    int pendingCombatVolume = 100;
    int pendingSpellVolume = 100;
    int pendingMovementVolume = 100;
    int pendingFootstepVolume = 100;
    int pendingNpcVoiceVolume = 100;
    int pendingMountVolume = 100;
    int pendingActivityVolume = 100;

    // ---- Pending camera / controls ----
    float pendingMouseSensitivity = 0.2f;
    bool pendingInvertMouse = false;
    bool pendingExtendedZoom = false;
    float pendingCameraStiffness = 30.0f;  // Camera smooth speed (higher = tighter, less sway)
    float pendingPivotHeight = 1.6f;       // Camera pivot height above feet (lower = less detached feel)
    float pendingFov = 70.0f;  // degrees, default matches WoW's ~70° horizontal FOV

    // ---- Pending UI / interface ----
    int pendingUiOpacity = 65;
    bool pendingMinimapRotate = false;
    bool pendingMinimapSquare = false;
    bool pendingMinimapNpcDots = false;
    bool pendingShowLatencyMeter = true;
    bool pendingSeparateBags = true;
    bool pendingShowKeyring = true;

    // ---- Pending gameplay ----
    bool pendingAutoLoot = false;
    bool pendingAutoSellGrey = false;
    bool pendingAutoRepair = false;

    // ---- Pending soundtrack ----
    bool pendingUseOriginalSoundtrack = true;

    // ---- Pending action bar layout ----
    bool pendingShowActionBar2 = true;   // Show second action bar above main bar
    float pendingActionBarScale = 1.0f;  // Multiplier for action bar slot size (0.5–1.5)
    float pendingActionBar2OffsetX = 0.0f;  // Horizontal offset from default center position
    float pendingActionBar2OffsetY = 0.0f;  // Vertical offset from default (above bar 1)
    bool pendingShowRightBar = false;   // Right-edge vertical action bar (bar 3, slots 24-35)
    bool pendingShowLeftBar  = false;   // Left-edge vertical action bar (bar 4, slots 36-47)
    float pendingRightBarOffsetY = 0.0f;  // Vertical offset from screen center
    float pendingLeftBarOffsetY  = 0.0f;  // Vertical offset from screen center

    // ---- Pending graphics quality ----
    int pendingGroundClutterDensity = 100;
    int pendingAntiAliasing = 0;  // 0=Off, 1=2x, 2=4x, 3=8x
    bool pendingFXAA = false;     // FXAA post-process (combinable with MSAA)
    bool pendingNormalMapping = true;   // on by default
    float pendingNormalMapStrength = 0.8f;  // 0.0-2.0
    bool pendingPOM = true;             // on by default
    int pendingPOMQuality = 1;          // 0=Low(16), 1=Medium(32), 2=High(64)
    bool pendingFSR = false;
    int pendingUpscalingMode = 0;       // 0=Off, 1=FSR1, 2=FSR3
    int pendingFSRQuality = 3;          // 0=UltraQuality, 1=Quality, 2=Balanced, 3=Native(100%)
    float pendingFSRSharpness = 1.6f;
    float pendingFSR2JitterSign = 0.38f;
    float pendingFSR2MotionVecScaleX = 1.0f;
    float pendingFSR2MotionVecScaleY = 1.0f;
    bool pendingAMDFramegen = false;

    // ---- Graphics quality presets ----
    enum class GraphicsPreset : int {
        CUSTOM = 0,
        LOW = 1,
        MEDIUM = 2,
        HIGH = 3,
        ULTRA = 4
    };
    GraphicsPreset currentGraphicsPreset = GraphicsPreset::CUSTOM;
    GraphicsPreset pendingGraphicsPreset = GraphicsPreset::CUSTOM;

    // ---- Applied-once flags (used by GameScreen::render() one-time-apply blocks) ----
    bool fsrSettingsApplied_ = false;
    float uiOpacity_ = 0.65f;  // UI element transparency (0.0 = fully transparent, 1.0 = fully opaque)
    bool minimapRotate_ = false;
    bool minimapSquare_ = false;
    bool minimapNpcDots_ = false;
    bool showLatencyMeter_ = true;           // Show server latency indicator
    bool minimapSettingsApplied_ = false;
    bool volumeSettingsApplied_ = false;  // True once saved volume settings applied to audio managers
    bool msaaSettingsApplied_ = false;   // True once saved MSAA setting applied to renderer
    bool fxaaSettingsApplied_ = false;   // True once saved FXAA setting applied to renderer
    bool waterRefractionApplied_ = false;
    bool normalMapSettingsApplied_ = false;  // True once saved normal map/POM settings applied

    // ---- Mute state: mute bypasses master volume without touching slider values ----
    bool soundMuted_ = false;
    float preMuteVolume_ = 1.0f;  // AudioEngine master volume before muting

    // ---- Config toggles (read by GameScreen rendering, edited by Interface tab) ----
    float nameplateScale_ = 1.0f; // Scale multiplier for nameplate bar dimensions
    bool showFriendlyNameplates_ = true;  // Shift+V toggles friendly player nameplates
    bool showDPSMeter_ = false;
    bool showCooldownTracker_ = false;
    bool damageFlashEnabled_ = true;
    bool lowHealthVignetteEnabled_ = true; // Persistent pulsing red vignette below 20% HP

    // ---- Public methods ----

    /// Render the settings window (call from GameScreen::render)
    void renderSettingsWindow(InventoryScreen& inventoryScreen, ChatPanel& chatPanel,
                              std::function<void()> saveCallback);

    /// Apply audio volume levels to all renderer sound managers
    void applyAudioVolumes(rendering::Renderer* renderer);

    /// Return the platform-specific settings file path
    static std::string getSettingsPath();

private:
    // Keybinding customization (private — only used in Controls tab)
    int pendingRebindAction_ = -1;  // -1 = not rebinding, otherwise action index
    bool awaitingKeyPress_ = false;

    // Settings tab rendering
    void renderSettingsInterfaceTab(std::function<void()> saveCallback);
    void renderSettingsGameplayTab(InventoryScreen& inventoryScreen,
                                   std::function<void()> saveCallback);
    void renderSettingsControlsTab(std::function<void()> saveCallback);
    void renderSettingsAudioTab(std::function<void()> saveCallback);
    void renderSettingsAboutTab();
    void applyGraphicsPreset(GraphicsPreset preset);
    void updateGraphicsPresetFromCurrentSettings();
};

} // namespace ui
} // namespace wowee
