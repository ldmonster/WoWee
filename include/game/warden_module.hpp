#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>

namespace wowee {
namespace game {

// Forward declarations
class WardenEmulator;

/**
 * Represents Warden callback functions exported by loaded module
 *
 * Real modules expose these 4 functions after loading.
 * For now, these are stubs for future native code execution.
 */
struct WardenFuncList {
    using GenerateRC4KeysFunc = std::function<void(uint8_t* packet)>;
    using UnloadFunc = std::function<void(uint8_t* rc4Keys)>;
    using PacketHandlerFunc = std::function<void(uint8_t* data, size_t length)>;
    using TickFunc = std::function<uint32_t(uint32_t deltaMs)>;

    GenerateRC4KeysFunc generateRC4Keys;  // Triggered by 0x05 packets (re-keying)
    UnloadFunc unload;                     // Cleanup, save RC4 state
    PacketHandlerFunc packetHandler;       // Process check requests (0x02, 0x04, etc.)
    TickFunc tick;                         // Periodic execution
};

/**
 * Warden module loader and executor
 *
 * IMPLEMENTATION STATUS:
 * ✅ Module metadata parsing
 * ✅ Basic validation framework
 * ⏳ RC4 decryption (uses existing WardenCrypto)
 * ❌ RSA signature verification (TODO - requires OpenSSL RSA)
 * ❌ zlib decompression (TODO - requires zlib library)
 * ❌ Custom executable format parsing (TODO - major reverse engineering)
 * ❌ Address relocation (TODO - x86 address fixups)
 * ❌ API binding (TODO - kernel32/user32 function resolution)
 * ❌ Native code execution (TODO - execute loaded x86 code)
 *
 * For strict servers like Warmane, ALL TODOs must be implemented.
 * For permissive servers, fake responses in GameHandler work.
 */
class WardenModule {
public:
    WardenModule();
    ~WardenModule();

    /**
     * Load module from encrypted module data
     *
     * Steps:
     * 1. Verify MD5 hash against expected identifier
     * 2. RC4 decrypt using session key
     * 3. Verify RSA signature
     * 4. zlib decompress
     * 5. Parse custom executable format
     * 6. Apply relocations
     * 7. Bind API functions
     * 8. Initialize module and get WardenFuncList
     *
     * @param moduleData Encrypted module bytes from SMSG_WARDEN_DATA
     * @param md5Hash Expected MD5 hash (module identifier)
     * @param rc4Key RC4 decryption key from seed
     * @return true if module loaded successfully
     */
    bool load(const std::vector<uint8_t>& moduleData,
              const std::vector<uint8_t>& md5Hash,
              const std::vector<uint8_t>& rc4Key);

    /**
     * Check if module is loaded and ready
     */
    bool isLoaded() const { return loaded_; }

    /**
     * Get module MD5 identifier
     */
    const std::vector<uint8_t>& getMD5Hash() const { return md5Hash_; }

    /**
     * Process check request packet via module's PacketHandler
     *
     * This would call the loaded module's native code to:
     * - Parse check opcodes (0xF3, 0xB2, 0x98, etc.)
     * - Perform actual memory scans
     * - Compute file checksums
     * - Generate REAL response data
     *
     * For now, returns false (not implemented).
     *
     * @param checkData Decrypted check request payload
     * @param responseOut Response data to send back
     * @return true if processed successfully
     */
    bool processCheckRequest(const std::vector<uint8_t>& checkData,
                            std::vector<uint8_t>& responseOut);

    /**
     * Periodic tick for module state updates
     *
     * @param deltaMs Milliseconds since last tick
     * @return Next tick interval in ms (0 = no more ticks needed)
     */
    uint32_t tick(uint32_t deltaMs);

    /**
     * Generate new RC4 keys (triggered by server opcode 0x05)
     */
    void generateRC4Keys(uint8_t* packet);

    /**
     * Unload module and cleanup
     */
    void unload();

    const void* getModuleMemory() const { return moduleMemory_; }
    size_t getModuleSize() const { return moduleSize_; }
    const std::vector<uint8_t>& getDecompressedData() const { return decompressedData_; }

private:
    bool loaded_;                          // Module successfully loaded
    std::vector<uint8_t> md5Hash_;         // Module identifier
    std::vector<uint8_t> moduleData_;      // Raw encrypted data
    std::vector<uint8_t> decryptedData_;   // RC4 decrypted data
    std::vector<uint8_t> decompressedData_; // zlib decompressed data

    // Module execution context
    void* moduleMemory_;                   // Allocated executable memory region
    size_t moduleSize_;                    // Size of loaded code
    uint32_t moduleBase_;                  // Module base address (for emulator)
    size_t relocDataOffset_ = 0;           // Offset into decompressedData_ where relocation data starts
    WardenFuncList funcList_;              // Callback functions
    std::unique_ptr<WardenEmulator> emulator_; // Cross-platform x86 emulator
    uint32_t emulatedPacketHandlerAddr_ = 0;   // Raw emulated VA for 4-arg PacketHandler call

    // Validation and loading steps
    bool verifyMD5(const std::vector<uint8_t>& data,
                   const std::vector<uint8_t>& expectedHash);
    bool decryptRC4(const std::vector<uint8_t>& encrypted,
                    const std::vector<uint8_t>& key,
                    std::vector<uint8_t>& decryptedOut);
    bool verifyRSASignature(const std::vector<uint8_t>& data);
    bool decompressZlib(const std::vector<uint8_t>& compressed,
                        std::vector<uint8_t>& decompressedOut);
    bool parseExecutableFormat(const std::vector<uint8_t>& exeData);
    bool applyRelocations();
    bool bindAPIs();
    bool initializeModule();
};

/**
 * Warden module manager
 *
 * Handles multiple module downloads and lifecycle.
 * Servers can send different modules per session.
 */
class WardenModuleManager {
public:
    WardenModuleManager();
    ~WardenModuleManager();

    /**
     * Check if we have module cached locally
     *
     * @param md5Hash Module identifier
     * @return true if module is cached
     */
    bool hasModule(const std::vector<uint8_t>& md5Hash);

    /**
     * Get or create module instance
     *
     * @param md5Hash Module identifier
     * @return Module instance (may not be loaded yet)
     */
    std::shared_ptr<WardenModule> getModule(const std::vector<uint8_t>& md5Hash);

    /**
     * Receive module data chunk from server
     *
     * Modules may be sent in multiple SMSG_WARDEN_DATA packets.
     * This accumulates chunks until complete.
     *
     * @param md5Hash Module identifier
     * @param chunkData Data chunk
     * @param isComplete true if this is the last chunk
     * @return true if chunk accepted
     */
    bool receiveModuleChunk(const std::vector<uint8_t>& md5Hash,
                           const std::vector<uint8_t>& chunkData,
                           bool isComplete);

    /**
     * Save module to disk cache
     *
     * Cached modules skip re-download on reconnect.
     * Cache directory: ~/.local/share/wowee/warden_cache/
     */
    bool cacheModule(const std::vector<uint8_t>& md5Hash,
                     const std::vector<uint8_t>& moduleData);

    /**
     * Load module from disk cache
     */
    bool loadCachedModule(const std::vector<uint8_t>& md5Hash,
                         std::vector<uint8_t>& moduleDataOut);

private:
    std::map<std::vector<uint8_t>, std::shared_ptr<WardenModule>> modules_;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> downloadBuffer_; // Partial downloads
    std::string cacheDirectory_;

    std::string getCachePath(const std::vector<uint8_t>& md5Hash);
};

} // namespace game
} // namespace wowee
