#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>

// Forward declare StormLib handle
typedef void* HANDLE;

namespace wowee {
namespace pipeline {

/**
 * MPQManager - Manages MPQ archive loading and file reading
 *
 * WoW 3.3.5a stores all game assets in MPQ archives.
 * This manager loads multiple archives and provides unified file access.
 */
class MPQManager {
public:
    MPQManager();
    ~MPQManager();

    /**
     * Initialize the MPQ system
     * @param dataPath Path to WoW Data directory
     * @return true if initialization succeeded
     */
    bool initialize(const std::string& dataPath);

    /**
     * Shutdown and close all archives
     */
    void shutdown();

    /**
     * Load a single MPQ archive
     * @param path Full path to MPQ file
     * @param priority Priority for file resolution (higher = checked first)
     * @return true if archive loaded successfully
     */
    bool loadArchive(const std::string& path, int priority = 0);

    /**
     * Check if a file exists in any loaded archive
     * @param filename Virtual file path (e.g., "World\\Maps\\Azeroth\\Azeroth.wdt")
     * @return true if file exists
     */
    bool fileExists(const std::string& filename) const;

    /**
     * Read a file from MPQ archives
     * @param filename Virtual file path
     * @return File contents as byte vector (empty if not found)
     */
    std::vector<uint8_t> readFile(const std::string& filename) const;

    /**
     * Get file size without reading it
     * @param filename Virtual file path
     * @return File size in bytes (0 if not found)
     */
    uint32_t getFileSize(const std::string& filename) const;

    /**
     * Check if MPQ system is initialized
     */
    bool isInitialized() const { return initialized; }

    /**
     * Get list of loaded archives
     */
    const std::vector<std::string>& getLoadedArchives() const { return archiveNames; }

private:
    struct ArchiveEntry {
        HANDLE handle;
        std::string path;
        int priority;
    };

    bool initialized = false;
    std::string dataPath;
    std::vector<ArchiveEntry> archives;
    std::vector<std::string> archiveNames;

    /**
     * Find archive containing a file
     * @param filename File to search for
     * @return Archive handle or nullptr if not found
     */
    HANDLE findFileArchive(const std::string& filename) const;

    /**
     * Load patch archives (e.g., patch.MPQ, patch-2.MPQ, etc.)
     */
    bool loadPatchArchives();

    /**
     * Load locale-specific archives
     * @param locale Locale string (e.g., "enUS")
     */
    bool loadLocaleArchives(const std::string& locale);

    void logMissingFileOnce(const std::string& filename) const;

    // Cache for mapping "virtual filename" -> archive handle (or INVALID_HANDLE_VALUE for not found).
    // This avoids scanning every archive for repeated lookups, which can otherwise appear as a hang
    // on screens that trigger many asset probes (character select, character preview, etc.).
    //
    // Important: caching misses can blow up memory if the game probes many unique non-existent filenames.
    // Miss caching is disabled by default and must be explicitly enabled.
    mutable std::shared_mutex fileArchiveCacheMutex_;
    mutable std::unordered_map<std::string, HANDLE> fileArchiveCache_;
    size_t fileArchiveCacheMaxEntries_ = 500000;
    bool fileArchiveCacheMisses_ = false;

    mutable std::mutex missingFileMutex_;
    mutable std::unordered_set<std::string> missingFileWarnings_;
};

} // namespace pipeline
} // namespace wowee
