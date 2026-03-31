#include "core/window.hpp"
#include "core/logger.hpp"
#include "rendering/vk_context.hpp"
#include <SDL2/SDL_vulkan.h>
#ifdef _WIN32
#include <cstdlib>
#endif

namespace wowee {
namespace core {

Window::Window(const WindowConfig& config)
    : config(config)
    , width(config.width)
    , height(config.height)
    , windowedWidth(config.width)
    , windowedHeight(config.height)
    , fullscreen(config.fullscreen)
    , vsync(config.vsync) {
}

Window::~Window() {
    shutdown();
}

bool Window::initialize() {
    LOG_INFO("Initializing window: ", config.title);

    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        LOG_ERROR("Failed to initialize SDL: ", SDL_GetError());
        return false;
    }

    // Explicitly load the Vulkan library before creating the window.
    // SDL_CreateWindow with SDL_WINDOW_VULKAN fails on some platforms/drivers
    // if the Vulkan loader hasn't been located yet; calling this first gives a
    // clear error and avoids the misleading "not configured in SDL" message.
    // SDL 2.28+ uses LoadLibraryExW(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) which does
    // not search System32, so fall back to the explicit path on Windows if needed.
    //
    // On macOS, MoltenVK is a Vulkan "portability" driver.  The Vulkan loader
    // hides portability drivers (and their extensions like VK_KHR_surface) from
    // pre-instance enumeration unless told otherwise.  Setting this env var
    // makes the loader include portability ICDs so SDL's VK_KHR_surface check
    // succeeds.
#ifdef __APPLE__
    setenv("VK_LOADER_ENABLE_PORTABILITY_DRIVERS", "1", 0 /*don't overwrite*/);
#endif
    bool vulkanLoaded = (SDL_Vulkan_LoadLibrary(nullptr) == 0);
#ifdef _WIN32
    if (!vulkanLoaded) {
        const char* sysRoot = std::getenv("SystemRoot");
        if (sysRoot && *sysRoot) {
            std::string fallbackPath = std::string(sysRoot) + "\\System32\\vulkan-1.dll";
            vulkanLoaded = (SDL_Vulkan_LoadLibrary(fallbackPath.c_str()) == 0);
            if (vulkanLoaded) {
                LOG_INFO("Loaded Vulkan library via explicit path: ", fallbackPath);
            }
        }
    }
#endif
    if (!vulkanLoaded) {
        LOG_ERROR("Failed to load Vulkan library: ", SDL_GetError());
        LOG_ERROR("Ensure the Vulkan runtime (vulkan-1.dll) is installed. "
                  "Install the latest GPU drivers or the Vulkan Runtime from https://vulkan.lunarg.com/");
        SDL_Quit();
        return false;
    }

    // Create Vulkan window (no GL attributes needed)
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (config.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    if (config.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    window = SDL_CreateWindow(
        config.title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        flags
    );

    if (!window) {
        LOG_ERROR("Failed to create window: ", SDL_GetError());
        return false;
    }

    // Initialize Vulkan context
    vkContext = std::make_unique<rendering::VkContext>();
    vkContext->setVsync(vsync);
    if (!vkContext->initialize(window)) {
        LOG_ERROR("Failed to initialize Vulkan context");
        return false;
    }

    LOG_INFO("Window initialized successfully (Vulkan)");
    return true;
}

// Shutdown progress uses LOG_WARNING so these messages are always visible even at
// default log levels — useful for diagnosing hangs or crashes during teardown.
void Window::shutdown() {
    LOG_WARNING("Window::shutdown - vkContext...");
    if (vkContext) {
        vkContext->shutdown();
        vkContext.reset();
    }

    LOG_WARNING("Window::shutdown - SDL_DestroyWindow...");
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    LOG_WARNING("Window::shutdown - SDL_Quit...");
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    LOG_WARNING("Window shutdown complete");
}

void Window::pollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            shouldCloseFlag = true;
        }
        else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                width = event.window.data1;
                height = event.window.data2;
                if (vkContext) {
                    vkContext->markSwapchainDirty();
                }
                LOG_DEBUG("Window resized to ", width, "x", height);
            }
        }
    }
}

void Window::setFullscreen(bool enable) {
    if (!window) return;
    if (enable == fullscreen) return;
    if (enable) {
        windowedWidth = width;
        windowedHeight = height;
        if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
            LOG_WARNING("Failed to enter fullscreen: ", SDL_GetError());
            return;
        }
        fullscreen = true;
        SDL_GetWindowSize(window, &width, &height);
    } else {
        if (SDL_SetWindowFullscreen(window, 0) != 0) {
            LOG_WARNING("Failed to exit fullscreen: ", SDL_GetError());
            return;
        }
        fullscreen = false;
        SDL_SetWindowSize(window, windowedWidth, windowedHeight);
        width = windowedWidth;
        height = windowedHeight;
    }
    if (vkContext) {
        vkContext->markSwapchainDirty();
    }
}

void Window::setVsync(bool enable) {
    vsync = enable;
    if (vkContext) {
        vkContext->setVsync(enable);
        vkContext->markSwapchainDirty();
    }
    LOG_INFO("VSync ", enable ? "enabled" : "disabled");
}

void Window::applyResolution(int w, int h) {
    if (!window) return;
    if (w <= 0 || h <= 0) return;
    if (fullscreen) {
        windowedWidth = w;
        windowedHeight = h;
        return;
    }
    SDL_SetWindowSize(window, w, h);
    width = w;
    height = h;
    windowedWidth = w;
    windowedHeight = h;
    if (vkContext) {
        vkContext->markSwapchainDirty();
    }
}

} // namespace core
} // namespace wowee
