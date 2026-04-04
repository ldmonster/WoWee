#pragma once

#include "ui/ui_services.hpp"
#include "auth/auth_handler.hpp"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <functional>

namespace wowee { namespace rendering { class VkContext; } }

namespace wowee { namespace ui {

/**
 * Authentication screen UI
 *
 * Allows user to enter credentials and connect to auth server
 */
class AuthScreen {
public:
    AuthScreen();

    /**
     * Render the UI
     * @param authHandler Reference to auth handler
     */
    void render(auth::AuthHandler& authHandler);

    /**
     * Set callback for successful authentication
     */
    void setOnSuccess(std::function<void()> callback) { onSuccess = callback; }

    /// Set services (dependency injection)
    void setServices(const UIServices& services) { services_ = services; }


    /**
     * Check if authentication is in progress
     */
    bool isAuthenticating() const { return authenticating; }

    void stopLoginMusic();

    /**
     * Get status message
     */
    const std::string& getStatusMessage() const { return statusMessage; }

private:
    UIServices services_;  // Injected service references

    struct ServerProfile {
        std::string hostname;
        int port = 3724;
        std::string username;
        std::string passwordHash;  // SHA1 hex (UPPER(user):UPPER(pass))
        std::string expansionId;   // "wotlk", "tbc", "classic", "turtle", ...
    };

    // UI state
    char hostname[256] = "localhost";
    char username[256] = "";
    char password[256] = "";
    char pinCode[32] = "";
    int port = 3724;
    int expansionIndex = 0;     // Index into expansion registry profiles
    bool authenticating = false;
    bool showPassword = false;
    bool pinAutoSubmitted_ = false;

    // Status
    std::string statusMessage;
    bool statusIsError = false;
    std::string failureReason;    // Specific reason from auth handler
    float authTimer = 0.0f;       // Timeout tracker
    static constexpr float AUTH_TIMEOUT = 10.0f;

    // Saved password hash (SHA1(UPPER(user):UPPER(pass)) as hex)
    std::string savedPasswordHash;
    bool usingStoredHash = false;
    static constexpr const char* PASSWORD_PLACEHOLDER = "\x01\x01\x01\x01\x01\x01\x01\x01";

    // Saved server-specific profiles
    std::vector<ServerProfile> servers_;
    int selectedServerIndex_ = -1;  // -1 = custom/unlisted

    // Callbacks
    std::function<void()> onSuccess;

    /**
     * Attempt authentication
     */
    void attemptAuth(auth::AuthHandler& authHandler);

    /**
     * Update status message
     */
    void setStatus(const std::string& message, bool isError = false);

    /**
     * Persist/restore login fields
     */
    void saveLoginInfo(bool includePasswordHash);
    void loadLoginInfo();
    static std::string getConfigPath();
    bool loginInfoLoaded = false;

    static std::string makeServerKey(const std::string& host, int port);
    void selectServerProfile(int index);
    void upsertCurrentServerProfile(bool includePasswordHash);
    std::string currentExpansionId() const;

    // Background image (Vulkan)
    bool bgInitAttempted = false;
    bool loadBackgroundImage();
    void destroyBackgroundImage();
    rendering::VkContext* bgVkCtx = nullptr;
    VkImage bgImage = VK_NULL_HANDLE;
    VkDeviceMemory bgMemory = VK_NULL_HANDLE;
    VkImageView bgImageView = VK_NULL_HANDLE;
    VkSampler bgSampler = VK_NULL_HANDLE;
    VkDescriptorSet bgDescriptorSet = VK_NULL_HANDLE;
    int bgWidth = 0;
    int bgHeight = 0;

    bool musicInitAttempted = false;
    bool musicPlaying = false;
    bool missingIntroTracksLogged_ = false;
    bool introTracksScanned_ = false;
    std::vector<std::string> introTracks_;
    bool loginMusicVolumeAdjusted_ = false;
    int savedMusicVolume_ = 30;

    // ----- Login-screen graphics settings popup -----
    bool showLoginSettings_ = false;

    // Local copies of the settings keys we expose in the login popup.
    // Loaded on first open; saved on Apply.
    struct LoginGraphicsState {
        int  preset          = 2;   // 0=Custom 1=Low 2=Medium 3=High 4=Ultra
        bool shadows         = true;
        float shadowDistance = 300.0f;
        int  antiAliasing    = 0;   // 0=Off 1=2x 2=4x 3=8x
        bool fxaa            = false;
        bool normalMapping   = true;
        bool pom             = true;
        int  pomQuality      = 1;   // 0=Low 1=Medium 2=High
        int  upscalingMode   = 0;   // 0=Off 1=FSR1 2=FSR3
        bool waterRefraction = true;
        int  groundClutter   = 100; // 0-150
        int  brightness      = 50;  // 0-100
        bool vsync           = false;
        bool fullscreen      = false;
    };
    LoginGraphicsState loginGfx_;
    bool loginGfxLoaded_ = false;

    void renderLoginSettingsWindow();
    void loadLoginGraphicsState();
    void saveLoginGraphicsState();
    static void applyPresetToState(LoginGraphicsState& s, int preset);
};

}} // namespace wowee::ui
