#include "core/application.hpp"
#include "core/logger.hpp"
#include <exception>
#include <csignal>
#include <cstdlib>
#include <cctype>
#include <string>
#include <SDL2/SDL.h>
#ifdef __linux__
#include <X11/Xlib.h>
#include <execinfo.h>
#include <unistd.h>

// Keep a persistent X11 connection for emergency mouse release in signal handlers.
// XOpenDisplay inside a signal handler is unreliable, so we open it once at startup.
static Display* g_emergencyDisplay = nullptr;

static void releaseMouseGrab() {
    if (g_emergencyDisplay) {
        XUngrabPointer(g_emergencyDisplay, CurrentTime);
        XUngrabKeyboard(g_emergencyDisplay, CurrentTime);
        XFlush(g_emergencyDisplay);
    }
}
#else
static void releaseMouseGrab() {}
#endif

static void crashHandler(int sig) {
    releaseMouseGrab();
#ifdef __linux__
    // Dump backtrace to debug log
    {
        void* frames[64];
        int n = backtrace(frames, 64);
        const char* sigName = (sig == SIGSEGV) ? "SIGSEGV" :
                              (sig == SIGABRT) ? "SIGABRT" :
                              (sig == SIGFPE)  ? "SIGFPE"  : "UNKNOWN";
        // Write to stderr and to the debug log file
        fprintf(stderr, "\n=== CRASH: signal %s (%d) ===\n", sigName, sig);
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
        FILE* f = fopen("/tmp/wowee_debug.log", "a");
        if (f) {
            fprintf(f, "\n=== CRASH: signal %s (%d) ===\n", sigName, sig);
            fflush(f);
            // Also write backtrace to the log file fd
            backtrace_symbols_fd(frames, n, fileno(f));
            fclose(f);
        }
    }
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

static wowee::core::LogLevel readLogLevelFromEnv() {
    const char* raw = std::getenv("WOWEE_LOG_LEVEL");
    if (!raw || !*raw) return wowee::core::LogLevel::WARNING;
    std::string level(raw);
    for (char& c : level) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (level == "debug") return wowee::core::LogLevel::DEBUG;
    if (level == "info") return wowee::core::LogLevel::INFO;
    if (level == "warn" || level == "warning") return wowee::core::LogLevel::WARNING;
    if (level == "error") return wowee::core::kLogLevelError;
    if (level == "fatal") return wowee::core::LogLevel::FATAL;
    return wowee::core::LogLevel::WARNING;
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
#ifdef __linux__
    g_emergencyDisplay = XOpenDisplay(nullptr);
#endif
    std::signal(SIGSEGV, crashHandler);
    std::signal(SIGABRT, crashHandler);
    std::signal(SIGFPE,  crashHandler);
    std::signal(SIGTERM, crashHandler);
    std::signal(SIGINT,  crashHandler);
    try {
        wowee::core::Logger::getInstance().setLogLevel(readLogLevelFromEnv());
        LOG_INFO("=== Wowee Native Client ===");
        LOG_INFO("Starting application...");

        wowee::core::Application app;

        if (!app.initialize()) {
            LOG_FATAL("Failed to initialize application");
            return 1;
        }

        app.run();
        app.shutdown();

        LOG_INFO("Application exited successfully");
#ifdef __linux__
        if (g_emergencyDisplay) { XCloseDisplay(g_emergencyDisplay); g_emergencyDisplay = nullptr; }
#endif
        return 0;
    }
    catch (const std::exception& e) {
        releaseMouseGrab();
        LOG_FATAL("Unhandled exception: ", e.what());
        return 1;
    }
    catch (...) {
        releaseMouseGrab();
        LOG_FATAL("Unknown exception occurred");
        return 1;
    }
}
