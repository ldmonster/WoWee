#include "rendering/amd_fsr3_wrapper_abi.h"

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#if defined(_WIN32)
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#endif
#include <FidelityFX/host/ffx_fsr3.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#else
#include <dlfcn.h>
#endif

#if defined(_WIN32)
#define WOWEE_FSR3_WRAPPER_EXPORT extern "C" __declspec(dllexport)
#else
#define WOWEE_FSR3_WRAPPER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

void writeError(char* outErrorText, uint32_t outErrorTextCapacity, const char* message) {
    if (!outErrorText || outErrorTextCapacity == 0) return;
    if (!message) message = "unknown error";
    std::strncpy(outErrorText, message, static_cast<size_t>(outErrorTextCapacity - 1));
    outErrorText[outErrorTextCapacity - 1] = '\0';
}

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
#if defined(_WIN32)
bool envEnabled(const char* key, bool defaultValue) {
    const char* val = std::getenv(key);
    if (!val || !*val) return defaultValue;
    std::string s(val);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !(s == "0" || s == "false" || s == "off" || s == "no");
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& ext : extensions) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

bool hasExternalImageInteropSupport(VkPhysicalDevice physicalDevice, VkFormat format) {
    if (!physicalDevice || format == VK_FORMAT_UNDEFINED) return false;

    VkPhysicalDeviceExternalImageFormatInfo externalInfo{};
    externalInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkPhysicalDeviceImageFormatInfo2 imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    imageInfo.format = format;
    imageInfo.type = VK_IMAGE_TYPE_2D;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.flags = 0;
    imageInfo.pNext = &externalInfo;

    VkExternalImageFormatProperties externalProps{};
    externalProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

    VkImageFormatProperties2 imageProps{};
    imageProps.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    imageProps.pNext = &externalProps;

    const VkResult res = vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &imageInfo, &imageProps);
    if (res != VK_SUCCESS) return false;

    const VkExternalMemoryFeatureFlags features = externalProps.externalMemoryProperties.externalMemoryFeatures;
    const bool exportable = (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    const bool importable = (features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    return exportable && importable;
}
#endif

#if defined(__linux__)
bool hasExtensionLinux(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& ext : extensions) {
        if (std::strcmp(ext.extensionName, name) == 0) return true;
    }
    return false;
}

bool runLinuxBridgePreflight(const WoweeFsr3WrapperInitDesc* initDesc, std::string& errorMessage) {
    std::vector<std::string> missing;
    if (!initDesc || !initDesc->device || !initDesc->getDeviceProcAddr || !initDesc->physicalDevice) {
        missing.emplace_back("valid Vulkan device/getDeviceProcAddr");
    } else {
        const char* requiredVkInteropFns[] = {
            "vkGetMemoryFdKHR",
            "vkGetSemaphoreFdKHR"
        };
        for (const char* fn : requiredVkInteropFns) {
            PFN_vkVoidFunction fp = initDesc->getDeviceProcAddr(initDesc->device, fn);
            if (!fp) missing.emplace_back(fn);
        }

        uint32_t extCount = 0;
        VkResult extErr = vkEnumerateDeviceExtensionProperties(initDesc->physicalDevice, nullptr, &extCount, nullptr);
        if (extErr != VK_SUCCESS || extCount == 0) {
            missing.emplace_back("vkEnumerateDeviceExtensionProperties");
        } else {
            std::vector<VkExtensionProperties> extensions(extCount);
            extErr = vkEnumerateDeviceExtensionProperties(
                initDesc->physicalDevice, nullptr, &extCount, extensions.data());
            if (extErr != VK_SUCCESS) {
                missing.emplace_back("vkEnumerateDeviceExtensionProperties(data)");
            } else {
                const char* requiredVkExtensions[] = {
                    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
                };
                for (const char* extName : requiredVkExtensions) {
                    if (!hasExtensionLinux(extensions, extName)) {
                        missing.emplace_back(extName);
                    }
                }
            }
        }
    }

    if (missing.empty()) return true;
    std::ostringstream oss;
    oss << "dx12_bridge preflight failed (linux), missing: ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) oss << ", ";
        oss << missing[i];
    }
    errorMessage = oss.str();
    return false;
}
#endif

enum class WrapperBackend {
    VulkanRuntime,
    Dx12Bridge
};

WrapperBackend selectBackend() {
    if (const char* envBackend = std::getenv("WOWEE_FSR3_WRAPPER_BACKEND")) {
        if (envBackend && *envBackend) {
            std::string mode(envBackend);
            std::transform(mode.begin(), mode.end(), mode.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "dx12" || mode == "dx12_bridge" || mode == "d3d12") {
                return WrapperBackend::Dx12Bridge;
            }
            if (mode == "vulkan" || mode == "vk" || mode == "vulkan_runtime") {
                return WrapperBackend::VulkanRuntime;
            }
        }
    }
    return WrapperBackend::VulkanRuntime;
}

const char* backendName(WrapperBackend backend) {
    switch (backend) {
        case WrapperBackend::Dx12Bridge:
            return "dx12_bridge";
        case WrapperBackend::VulkanRuntime:
            return "vulkan_runtime";
        default:
            return "unknown";
    }
}

FfxErrorCode vkSwapchainConfigureNoop(const FfxFrameGenerationConfig*) {
    return FFX_OK;
}

template <typename T, typename = void>
struct HasUpscaleOutputSize : std::false_type {};

template <typename T>
struct HasUpscaleOutputSize<T, std::void_t<decltype(std::declval<T&>().upscaleOutputSize)>> : std::true_type {};

template <typename T>
inline void setUpscaleOutputSizeIfPresent(T& ctxDesc, uint32_t width, uint32_t height) {
    if constexpr (HasUpscaleOutputSize<T>::value) {
        ctxDesc.upscaleOutputSize.width = width;
        ctxDesc.upscaleOutputSize.height = height;
    } else {
        (void)ctxDesc;
        (void)width;
        (void)height;
    }
}

FfxSurfaceFormat mapVkFormatToFfxSurfaceFormat(VkFormat format, bool isDepth) {
    if (isDepth) {
        switch (format) {
            case VK_FORMAT_D32_SFLOAT:
                return FFX_SURFACE_FORMAT_R32_FLOAT;
            case VK_FORMAT_D16_UNORM:
                return FFX_SURFACE_FORMAT_R16_UNORM;
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return FFX_SURFACE_FORMAT_R32_FLOAT;
            default:
                return FFX_SURFACE_FORMAT_R32_FLOAT;
        }
    }

    switch (format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
            return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return FFX_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return FFX_SURFACE_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return FFX_SURFACE_FORMAT_R11G11B10_FLOAT;
        case VK_FORMAT_R16G16_SFLOAT:
            return FFX_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R16G16_UINT:
            return FFX_SURFACE_FORMAT_R16G16_UINT;
        case VK_FORMAT_R16_SFLOAT:
            return FFX_SURFACE_FORMAT_R16_FLOAT;
        case VK_FORMAT_R16_UINT:
            return FFX_SURFACE_FORMAT_R16_UINT;
        case VK_FORMAT_R16_UNORM:
            return FFX_SURFACE_FORMAT_R16_UNORM;
        case VK_FORMAT_R16_SNORM:
            return FFX_SURFACE_FORMAT_R16_SNORM;
        case VK_FORMAT_R8_UNORM:
            return FFX_SURFACE_FORMAT_R8_UNORM;
        case VK_FORMAT_R8_UINT:
            return FFX_SURFACE_FORMAT_R8_UINT;
        case VK_FORMAT_R8G8_UNORM:
            return FFX_SURFACE_FORMAT_R8G8_UNORM;
        case VK_FORMAT_R32_SFLOAT:
            return FFX_SURFACE_FORMAT_R32_FLOAT;
        case VK_FORMAT_R32_UINT:
            return FFX_SURFACE_FORMAT_R32_UINT;
        default:
            return FFX_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxResourceDescription makeResourceDescription(VkFormat format,
                                               uint32_t width,
                                               uint32_t height,
                                               FfxResourceUsage usage,
                                               bool isDepth = false) {
    FfxResourceDescription description{};
    description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    description.format = mapVkFormatToFfxSurfaceFormat(format, isDepth);
    description.width = width;
    description.height = height;
    description.depth = 1;
    description.mipCount = 1;
    description.flags = FFX_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return description;
}

bool hasKnownSurfaceFormat(const FfxResourceDescription& description) {
    return description.format != FFX_SURFACE_FORMAT_UNKNOWN;
}

struct RuntimeFns {
    decltype(&ffxGetScratchMemorySizeVK) getScratchMemorySizeVK = nullptr;
    decltype(&ffxGetDeviceVK) getDeviceVK = nullptr;
    decltype(&ffxGetInterfaceVK) getInterfaceVK = nullptr;
    decltype(&ffxGetCommandListVK) getCommandListVK = nullptr;
    decltype(&ffxGetResourceVK) getResourceVK = nullptr;
    decltype(&ffxFsr3ContextCreate) fsr3ContextCreate = nullptr;
    decltype(&ffxFsr3ContextDispatchUpscale) fsr3ContextDispatchUpscale = nullptr;
    decltype(&ffxFsr3ConfigureFrameGeneration) fsr3ConfigureFrameGeneration = nullptr;
    decltype(&ffxFsr3DispatchFrameGeneration) fsr3DispatchFrameGeneration = nullptr;
    decltype(&ffxFsr3ContextDestroy) fsr3ContextDestroy = nullptr;
#if defined(_WIN32)
    decltype(&ffxGetScratchMemorySizeDX12) getScratchMemorySizeDX12 = nullptr;
    decltype(&ffxGetDeviceDX12) getDeviceDX12 = nullptr;
    decltype(&ffxGetInterfaceDX12) getInterfaceDX12 = nullptr;
    decltype(&ffxGetCommandListDX12) getCommandListDX12 = nullptr;
    decltype(&ffxGetResourceDX12) getResourceDX12 = nullptr;
#endif
};

struct WrapperContext {
    WrapperBackend backend = WrapperBackend::VulkanRuntime;
    void* backendLibHandle = nullptr;
    RuntimeFns fns{};
    void* scratchBuffer = nullptr;
    size_t scratchBufferSize = 0;
    void* fsr3ContextStorage = nullptr;
    bool frameGenerationReady = false;
    bool hdrInput = false;
    std::string lastError{};
#if defined(_WIN32)
    ID3D12Device* dx12Device = nullptr;
    ID3D12CommandQueue* dx12Queue = nullptr;
    ID3D12CommandAllocator* dx12CommandAllocator = nullptr;
    ID3D12GraphicsCommandList* dx12CommandList = nullptr;
    ID3D12Fence* dx12Fence = nullptr;
    HANDLE dx12FenceEvent = nullptr;
    uint64_t dx12FenceValue = 0;
#endif
};

void setContextError(WrapperContext* ctx, const char* message) {
    if (!ctx) return;
    ctx->lastError = (message && *message) ? message : "unknown wrapper error";
}

void closeLibrary(void* handle) {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

void* openLibrary(const char* path) {
    if (!path || !*path) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* resolveSymbol(void* handle, const char* symbol) {
    if (!handle || !symbol) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), symbol));
#else
    return dlsym(handle, symbol);
#endif
}

bool bindVulkanRuntimeFns(void* libHandle, RuntimeFns& outFns) {
    outFns = RuntimeFns{};
    outFns.getScratchMemorySizeVK = reinterpret_cast<decltype(outFns.getScratchMemorySizeVK)>(resolveSymbol(libHandle, "ffxGetScratchMemorySizeVK"));
    outFns.getDeviceVK = reinterpret_cast<decltype(outFns.getDeviceVK)>(resolveSymbol(libHandle, "ffxGetDeviceVK"));
    outFns.getInterfaceVK = reinterpret_cast<decltype(outFns.getInterfaceVK)>(resolveSymbol(libHandle, "ffxGetInterfaceVK"));
    outFns.getCommandListVK = reinterpret_cast<decltype(outFns.getCommandListVK)>(resolveSymbol(libHandle, "ffxGetCommandListVK"));
    outFns.getResourceVK = reinterpret_cast<decltype(outFns.getResourceVK)>(resolveSymbol(libHandle, "ffxGetResourceVK"));
    outFns.fsr3ContextCreate = reinterpret_cast<decltype(outFns.fsr3ContextCreate)>(resolveSymbol(libHandle, "ffxFsr3ContextCreate"));
    outFns.fsr3ContextDispatchUpscale = reinterpret_cast<decltype(outFns.fsr3ContextDispatchUpscale)>(resolveSymbol(libHandle, "ffxFsr3ContextDispatchUpscale"));
    outFns.fsr3ConfigureFrameGeneration = reinterpret_cast<decltype(outFns.fsr3ConfigureFrameGeneration)>(resolveSymbol(libHandle, "ffxFsr3ConfigureFrameGeneration"));
    outFns.fsr3DispatchFrameGeneration = reinterpret_cast<decltype(outFns.fsr3DispatchFrameGeneration)>(resolveSymbol(libHandle, "ffxFsr3DispatchFrameGeneration"));
    outFns.fsr3ContextDestroy = reinterpret_cast<decltype(outFns.fsr3ContextDestroy)>(resolveSymbol(libHandle, "ffxFsr3ContextDestroy"));

    return outFns.getScratchMemorySizeVK && outFns.getDeviceVK && outFns.getInterfaceVK &&
           outFns.getCommandListVK && outFns.getResourceVK && outFns.fsr3ContextCreate &&
           outFns.fsr3ContextDispatchUpscale && outFns.fsr3ContextDestroy;
}

#if defined(_WIN32)
bool bindDx12RuntimeFns(void* libHandle, RuntimeFns& outFns) {
    outFns = RuntimeFns{};
    outFns.getScratchMemorySizeDX12 = reinterpret_cast<decltype(outFns.getScratchMemorySizeDX12)>(resolveSymbol(libHandle, "ffxGetScratchMemorySizeDX12"));
    outFns.getDeviceDX12 = reinterpret_cast<decltype(outFns.getDeviceDX12)>(resolveSymbol(libHandle, "ffxGetDeviceDX12"));
    outFns.getInterfaceDX12 = reinterpret_cast<decltype(outFns.getInterfaceDX12)>(resolveSymbol(libHandle, "ffxGetInterfaceDX12"));
    outFns.getCommandListDX12 = reinterpret_cast<decltype(outFns.getCommandListDX12)>(resolveSymbol(libHandle, "ffxGetCommandListDX12"));
    outFns.getResourceDX12 = reinterpret_cast<decltype(outFns.getResourceDX12)>(resolveSymbol(libHandle, "ffxGetResourceDX12"));
    outFns.fsr3ContextCreate = reinterpret_cast<decltype(outFns.fsr3ContextCreate)>(resolveSymbol(libHandle, "ffxFsr3ContextCreate"));
    outFns.fsr3ContextDispatchUpscale = reinterpret_cast<decltype(outFns.fsr3ContextDispatchUpscale)>(resolveSymbol(libHandle, "ffxFsr3ContextDispatchUpscale"));
    outFns.fsr3ConfigureFrameGeneration = reinterpret_cast<decltype(outFns.fsr3ConfigureFrameGeneration)>(resolveSymbol(libHandle, "ffxFsr3ConfigureFrameGeneration"));
    outFns.fsr3DispatchFrameGeneration = reinterpret_cast<decltype(outFns.fsr3DispatchFrameGeneration)>(resolveSymbol(libHandle, "ffxFsr3DispatchFrameGeneration"));
    outFns.fsr3ContextDestroy = reinterpret_cast<decltype(outFns.fsr3ContextDestroy)>(resolveSymbol(libHandle, "ffxFsr3ContextDestroy"));
    return outFns.getScratchMemorySizeDX12 && outFns.getDeviceDX12 && outFns.getInterfaceDX12 &&
           outFns.getCommandListDX12 && outFns.getResourceDX12 && outFns.fsr3ContextCreate &&
           outFns.fsr3ContextDispatchUpscale && outFns.fsr3ContextDestroy;
}
#endif

void destroyContext(WrapperContext* ctx) {
    if (!ctx) return;
#if defined(_WIN32)
    if (ctx->dx12FenceEvent) {
        CloseHandle(ctx->dx12FenceEvent);
        ctx->dx12FenceEvent = nullptr;
    }
    if (ctx->dx12Fence) {
        ctx->dx12Fence->Release();
        ctx->dx12Fence = nullptr;
    }
    if (ctx->dx12CommandList) {
        ctx->dx12CommandList->Release();
        ctx->dx12CommandList = nullptr;
    }
    if (ctx->dx12CommandAllocator) {
        ctx->dx12CommandAllocator->Release();
        ctx->dx12CommandAllocator = nullptr;
    }
    if (ctx->dx12Queue) {
        ctx->dx12Queue->Release();
        ctx->dx12Queue = nullptr;
    }
    if (ctx->dx12Device) {
        ctx->dx12Device->Release();
        ctx->dx12Device = nullptr;
    }
#endif
    if (ctx->fsr3ContextStorage && ctx->fns.fsr3ContextDestroy) {
        ctx->fns.fsr3ContextDestroy(reinterpret_cast<FfxFsr3Context*>(ctx->fsr3ContextStorage));
    }
    if (ctx->fsr3ContextStorage) {
        std::free(ctx->fsr3ContextStorage);
        ctx->fsr3ContextStorage = nullptr;
    }
    if (ctx->scratchBuffer) {
        std::free(ctx->scratchBuffer);
        ctx->scratchBuffer = nullptr;
    }
    ctx->scratchBufferSize = 0;
    closeLibrary(ctx->backendLibHandle);
    ctx->backendLibHandle = nullptr;
    delete ctx;
}

#if defined(_WIN32)
bool openSharedResourceHandle(ID3D12Device* device, uint64_t handleValue, const char* name, std::string& outError) {
    if (!device || handleValue == 0) {
        outError = std::string("invalid shared resource handle for ") + (name ? name : "unknown");
        return false;
    }
    ID3D12Resource* res = nullptr;
    const HRESULT hr = device->OpenSharedHandle(reinterpret_cast<HANDLE>(handleValue), IID_PPV_ARGS(&res));
    if (FAILED(hr) || !res) {
        outError = std::string("failed to import shared resource handle for ") + (name ? name : "unknown");
        return false;
    }
    res->Release();
    return true;
}

bool openSharedFenceHandle(ID3D12Device* device, uint64_t handleValue, const char* name, std::string& outError) {
    if (!device || handleValue == 0) {
        outError = std::string("invalid shared fence handle for ") + (name ? name : "unknown");
        return false;
    }
    ID3D12Fence* fence = nullptr;
    const HRESULT hr = device->OpenSharedHandle(reinterpret_cast<HANDLE>(handleValue), IID_PPV_ARGS(&fence));
    if (FAILED(hr) || !fence) {
        outError = std::string("failed to import shared fence handle for ") + (name ? name : "unknown");
        return false;
    }
    fence->Release();
    return true;
}

template <typename T>
void safeRelease(T*& obj) {
    if (obj) {
        obj->Release();
        obj = nullptr;
    }
}

bool runDx12BridgePreflight(const WoweeFsr3WrapperInitDesc* initDesc, std::string& errorMessage) {
    std::vector<std::string> missing;

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) {
        missing.emplace_back("d3d12.dll");
    }

    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    if (!dxgi) {
        missing.emplace_back("dxgi.dll");
    }

    using PFN_D3D12CreateDeviceLocal = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using PFN_CreateDXGIFactory2Local = HRESULT(WINAPI*)(UINT, REFIID, void**);
    PFN_D3D12CreateDeviceLocal pD3D12CreateDevice = nullptr;
    PFN_CreateDXGIFactory2Local pCreateDXGIFactory2 = nullptr;
    if (d3d12) {
        pD3D12CreateDevice = reinterpret_cast<PFN_D3D12CreateDeviceLocal>(GetProcAddress(d3d12, "D3D12CreateDevice"));
        if (!pD3D12CreateDevice) {
            missing.emplace_back("D3D12CreateDevice");
        }
    }
    if (dxgi) {
        pCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2Local>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
        if (!pCreateDXGIFactory2) {
            missing.emplace_back("CreateDXGIFactory2");
        }
    }

    if (!initDesc || !initDesc->device || !initDesc->getDeviceProcAddr || !initDesc->physicalDevice) {
        missing.emplace_back("valid Vulkan device/getDeviceProcAddr");
    } else {
        const char* requiredVkInteropFns[] = {
            "vkGetMemoryWin32HandleKHR",
            "vkImportSemaphoreWin32HandleKHR",
            "vkGetSemaphoreWin32HandleKHR"
        };
        for (const char* fn : requiredVkInteropFns) {
            PFN_vkVoidFunction fp = initDesc->getDeviceProcAddr(initDesc->device, fn);
            if (!fp) {
                missing.emplace_back(fn);
            }
        }

        uint32_t extCount = 0;
        VkResult extErr = vkEnumerateDeviceExtensionProperties(initDesc->physicalDevice, nullptr, &extCount, nullptr);
        if (extErr != VK_SUCCESS || extCount == 0) {
            missing.emplace_back("vkEnumerateDeviceExtensionProperties");
        } else {
            std::vector<VkExtensionProperties> extensions(extCount);
            extErr = vkEnumerateDeviceExtensionProperties(
                initDesc->physicalDevice, nullptr, &extCount, extensions.data());
            if (extErr != VK_SUCCESS) {
                missing.emplace_back("vkEnumerateDeviceExtensionProperties(data)");
            } else {
                const char* requiredVkExtensions[] = {
                    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
                };
                for (const char* extName : requiredVkExtensions) {
                    if (!hasExtension(extensions, extName)) {
                        missing.emplace_back(extName);
                    }
                }
                if (!hasExternalImageInteropSupport(initDesc->physicalDevice, initDesc->colorFormat)) {
                    missing.emplace_back("external memory export/import support for swap/upscale format");
                }
            }
        }
    }

    std::vector<std::string> runtimeCandidates;
    if (const char* explicitRuntime = std::getenv("WOWEE_FSR3_DX12_RUNTIME_LIB")) {
        if (explicitRuntime && *explicitRuntime) runtimeCandidates.emplace_back(explicitRuntime);
    }
    runtimeCandidates.emplace_back("amd_fidelityfx_framegeneration_dx12.dll");
    runtimeCandidates.emplace_back("ffx_framegeneration_dx12.dll");

    bool foundRuntime = false;
    bool foundRequiredApiSymbols = false;
    for (const std::string& candidate : runtimeCandidates) {
        HMODULE runtime = LoadLibraryA(candidate.c_str());
        if (runtime) {
            const bool hasCreate = GetProcAddress(runtime, "ffxCreateContext") != nullptr;
            const bool hasDestroy = GetProcAddress(runtime, "ffxDestroyContext") != nullptr;
            const bool hasConfigure = GetProcAddress(runtime, "ffxConfigure") != nullptr;
            const bool hasDispatch = GetProcAddress(runtime, "ffxDispatch") != nullptr;
            if (hasCreate && hasDestroy && hasConfigure && hasDispatch) {
                foundRequiredApiSymbols = true;
            }
            FreeLibrary(runtime);
            foundRuntime = true;
            if (foundRequiredApiSymbols) break;
        }
    }
    if (!foundRuntime) {
        missing.emplace_back("amd_fidelityfx_framegeneration_dx12.dll");
    } else if (!foundRequiredApiSymbols) {
        missing.emplace_back("ffxCreateContext/ffxConfigure/ffxDispatch exports");
    }

    // Optional strict probe: attempt creating a DXGI factory and D3D12 device.
    if (missing.empty() && envEnabled("WOWEE_FSR3_WRAPPER_DX12_VALIDATE_DEVICE", true) &&
        pCreateDXGIFactory2 && pD3D12CreateDevice) {
        IDXGIFactory6* factory = nullptr;
        HRESULT hrFactory = pCreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(hrFactory) || !factory) {
            missing.emplace_back("DXGI factory creation");
        } else {
            IDXGIAdapter1* adapter = nullptr;
            bool hasHardwareAdapter = false;
            for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    hasHardwareAdapter = true;
                    break;
                }
                adapter->Release();
                adapter = nullptr;
            }
            if (!hasHardwareAdapter || !adapter) {
                missing.emplace_back("hardware DXGI adapter");
            } else {
                ID3D12Device* d3d12Device = nullptr;
                HRESULT hrDevice = pD3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device));
                if (FAILED(hrDevice) || !d3d12Device) {
                    missing.emplace_back("D3D12 device creation (feature level 12_0)");
                }
                if (d3d12Device) d3d12Device->Release();
                adapter->Release();
            }
            factory->Release();
        }
    }

    if (dxgi) FreeLibrary(dxgi);
    if (d3d12) FreeLibrary(d3d12);

    if (missing.empty()) return true;

    std::ostringstream oss;
    oss << "dx12_bridge preflight failed, missing: ";
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i) oss << ", ";
        oss << missing[i];
    }
    errorMessage = oss.str();
    return false;
}
#endif
#endif

}  // namespace

WOWEE_FSR3_WRAPPER_EXPORT uint32_t wowee_fsr3_wrapper_get_abi_version(void) {
    return WOWEE_FSR3_WRAPPER_ABI_VERSION;
}

WOWEE_FSR3_WRAPPER_EXPORT const char* wowee_fsr3_wrapper_get_name(void) {
    return "WoWee FSR3 Wrapper";
}

WOWEE_FSR3_WRAPPER_EXPORT const char* wowee_fsr3_wrapper_get_backend(WoweeFsr3WrapperContext context) {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    if (!ctx) return "invalid";
    return backendName(ctx->backend);
#else
    (void)context;
    return "unavailable";
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT uint32_t wowee_fsr3_wrapper_get_capabilities(WoweeFsr3WrapperContext context) {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    if (!ctx) return 0;
    uint32_t caps = WOWEE_FSR3_WRAPPER_CAP_UPSCALE;
    if (ctx->frameGenerationReady && ctx->fns.fsr3DispatchFrameGeneration) {
        caps |= WOWEE_FSR3_WRAPPER_CAP_FRAME_GENERATION;
    }
#if defined(_WIN32)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        caps |= WOWEE_FSR3_WRAPPER_CAP_EXTERNAL_INTEROP;
    }
#elif defined(__linux__)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        caps |= WOWEE_FSR3_WRAPPER_CAP_EXTERNAL_INTEROP;
    }
#endif
    return caps;
#else
    (void)context;
    return 0;
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT int32_t wowee_fsr3_wrapper_initialize(const WoweeFsr3WrapperInitDesc* initDesc,
                                                                WoweeFsr3WrapperContext* outContext,
                                                                char* outErrorText,
                                                                uint32_t outErrorTextCapacity) {
    if (outContext) *outContext = nullptr;
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)initDesc;
    writeError(outErrorText, outErrorTextCapacity, "wrapper built without WOWEE_HAS_AMD_FSR3_FRAMEGEN");
    return -1;
#else
    if (!initDesc || !outContext) {
        writeError(outErrorText, outErrorTextCapacity, "invalid init args");
        return -1;
    }
    if (initDesc->structSize < sizeof(WoweeFsr3WrapperInitDesc) ||
        initDesc->abiVersion != WOWEE_FSR3_WRAPPER_ABI_VERSION) {
        writeError(outErrorText, outErrorTextCapacity, "wrapper ABI/version mismatch");
        return -1;
    }
    if (!initDesc->physicalDevice || !initDesc->device || !initDesc->getDeviceProcAddr ||
        initDesc->maxRenderWidth == 0 || initDesc->maxRenderHeight == 0 ||
        initDesc->displayWidth == 0 || initDesc->displayHeight == 0 ||
        initDesc->colorFormat == VK_FORMAT_UNDEFINED) {
        writeError(outErrorText, outErrorTextCapacity, "invalid init descriptor values");
        return -1;
    }

    const char* backendEnvRaw = std::getenv("WOWEE_FSR3_WRAPPER_BACKEND");
    const bool backendExplicit = (backendEnvRaw && *backendEnvRaw);
    const WrapperBackend selectedBackend = selectBackend();
    if (selectedBackend == WrapperBackend::Dx12Bridge) {
#if defined(_WIN32)
        std::string preflightError;
        if (!runDx12BridgePreflight(initDesc, preflightError)) {
            writeError(outErrorText, outErrorTextCapacity, preflightError.c_str());
            return -1;
        }
#elif defined(__linux__)
        std::string preflightError;
        if (!runLinuxBridgePreflight(initDesc, preflightError)) {
            writeError(outErrorText, outErrorTextCapacity, preflightError.c_str());
            return -1;
        }
#endif
    }

    std::vector<std::string> baseCandidates;
    if (const char* backendEnv = std::getenv("WOWEE_FSR3_WRAPPER_BACKEND_LIB")) {
        if (*backendEnv) baseCandidates.emplace_back(backendEnv);
    }
    if (const char* runtimeEnv = std::getenv("WOWEE_FFX_SDK_RUNTIME_LIB")) {
        if (*runtimeEnv) baseCandidates.emplace_back(runtimeEnv);
    }

    WrapperContext* ctx = new WrapperContext{};
    ctx->backend = selectedBackend;
#if defined(_WIN32)
    auto initDx12BridgeState = [&](const WoweeFsr3WrapperInitDesc* desc) -> bool {
        if (ctx->dx12Device) return true;
        if (!runDx12BridgePreflight(desc, ctx->lastError)) return false;
        if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&ctx->dx12Device)) != S_OK || !ctx->dx12Device) {
            ctx->lastError = "dx12_bridge failed to create D3D12 device";
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (ctx->dx12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx->dx12Queue)) != S_OK || !ctx->dx12Queue) {
            ctx->lastError = "dx12_bridge failed to create command queue";
            return false;
        }
        if (ctx->dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx->dx12CommandAllocator)) != S_OK ||
            !ctx->dx12CommandAllocator) {
            ctx->lastError = "dx12_bridge failed to create command allocator";
            return false;
        }
        if (ctx->dx12Device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->dx12CommandAllocator, nullptr,
                IID_PPV_ARGS(&ctx->dx12CommandList)) != S_OK || !ctx->dx12CommandList) {
            ctx->lastError = "dx12_bridge failed to create command list";
            return false;
        }
        ctx->dx12CommandList->Close();
        if (ctx->dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx->dx12Fence)) != S_OK || !ctx->dx12Fence) {
            ctx->lastError = "dx12_bridge failed to create fence";
            return false;
        }
        ctx->dx12FenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!ctx->dx12FenceEvent) {
            ctx->lastError = "dx12_bridge failed to create fence event";
            return false;
        }
        ctx->dx12FenceValue = 1;
        return true;
    };
#endif

    auto tryLoadForBackend = [&](WrapperBackend backend) -> bool {
#if defined(_WIN32)
        if (backend == WrapperBackend::Dx12Bridge && !initDx12BridgeState(initDesc)) {
            return false;
        }
#endif
        std::vector<std::string> candidates = baseCandidates;
#if defined(_WIN32)
        if (backend == WrapperBackend::Dx12Bridge) {
            candidates.emplace_back("amd_fidelityfx_framegeneration_dx12.dll");
            candidates.emplace_back("ffx_framegeneration_dx12.dll");
        }
        candidates.emplace_back("ffx_fsr3_vk.dll");
        candidates.emplace_back("ffx_fsr3.dll");
        candidates.emplace_back("ffx_fsr3_bridge.dll");
#elif defined(__APPLE__)
        candidates.emplace_back("libffx_fsr3_vk.dylib");
        candidates.emplace_back("libffx_fsr3.dylib");
        candidates.emplace_back("libffx_fsr3_bridge.dylib");
#else
        candidates.emplace_back("./libffx_fsr3_vk.so");
        candidates.emplace_back("libffx_fsr3_vk.so");
        candidates.emplace_back("libffx_fsr3.so");
        candidates.emplace_back("libffx_fsr3_bridge.so");
#endif

        for (const std::string& path : candidates) {
            void* candidateHandle = openLibrary(path.c_str());
            if (!candidateHandle) continue;

            RuntimeFns candidateFns{};
#if defined(_WIN32)
            const bool bound = (backend == WrapperBackend::Dx12Bridge)
                ? bindDx12RuntimeFns(candidateHandle, candidateFns)
                : bindVulkanRuntimeFns(candidateHandle, candidateFns);
#else
            const bool bound = bindVulkanRuntimeFns(candidateHandle, candidateFns);
#endif
            if (!bound) {
                closeLibrary(candidateHandle);
                continue;
            }

            ctx->backend = backend;
            ctx->backendLibHandle = candidateHandle;
            ctx->fns = candidateFns;
            return true;
        }
        return false;
    };

    bool loaded = tryLoadForBackend(selectedBackend);
#if defined(_WIN32)
    if (!loaded && !backendExplicit && selectedBackend == WrapperBackend::VulkanRuntime) {
        loaded = tryLoadForBackend(WrapperBackend::Dx12Bridge);
    }
#endif
    if (!loaded) {
        if (ctx->backendLibHandle) {
            closeLibrary(ctx->backendLibHandle);
            ctx->backendLibHandle = nullptr;
        }
    }
    if (!ctx->backendLibHandle) {
        const bool attemptedDx12 =
            (selectedBackend == WrapperBackend::Dx12Bridge)
#if defined(_WIN32)
            || (!backendExplicit && selectedBackend == WrapperBackend::VulkanRuntime)
#endif
            ;
        const std::string err = ctx->lastError;
        destroyContext(ctx);
        if (attemptedDx12) {
            writeError(outErrorText, outErrorTextCapacity,
                       err.empty() ? "dx12_bridge requested, but no dispatch-capable runtime exports were found"
                                   : err.c_str());
        } else {
            writeError(outErrorText, outErrorTextCapacity, "no FSR3 backend runtime found for wrapper");
        }
        return -1;
    }

    const bool enableFrameGeneration = (initDesc->enableFlags & WOWEE_FSR3_WRAPPER_ENABLE_FRAME_GENERATION) != 0u;
    ctx->hdrInput = (initDesc->enableFlags & WOWEE_FSR3_WRAPPER_ENABLE_HDR_INPUT) != 0u;
    if (enableFrameGeneration && (!ctx->fns.fsr3ConfigureFrameGeneration || !ctx->fns.fsr3DispatchFrameGeneration)) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "backend missing frame generation symbols");
        return -1;
    }

    if (ctx->backend == WrapperBackend::Dx12Bridge) {
#if defined(_WIN32)
        ctx->scratchBufferSize = ctx->fns.getScratchMemorySizeDX12(FFX_FSR3_CONTEXT_COUNT);
#else
        ctx->scratchBufferSize = 0;
#endif
    } else {
        ctx->scratchBufferSize = ctx->fns.getScratchMemorySizeVK(initDesc->physicalDevice, FFX_FSR3_CONTEXT_COUNT);
    }
    if (ctx->scratchBufferSize == 0) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "scratch buffer size query returned 0");
        return -1;
    }
    ctx->scratchBuffer = std::malloc(ctx->scratchBufferSize);
    if (!ctx->scratchBuffer) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "scratch buffer allocation failed");
        return -1;
    }

    FfxInterface backendShared{};
    FfxErrorCode ifaceErr = FFX_ERROR_INVALID_ARGUMENT;
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
#if defined(_WIN32)
        FfxDevice ffxDevice = ctx->fns.getDeviceDX12(ctx->dx12Device);
        ifaceErr = ctx->fns.getInterfaceDX12(
            &backendShared, ffxDevice, ctx->scratchBuffer, ctx->scratchBufferSize, FFX_FSR3_CONTEXT_COUNT);
#endif
    } else {
        VkDeviceContext vkDevCtx{};
        vkDevCtx.vkDevice = initDesc->device;
        vkDevCtx.vkPhysicalDevice = initDesc->physicalDevice;
        vkDevCtx.vkDeviceProcAddr = initDesc->getDeviceProcAddr;
        FfxDevice ffxDevice = ctx->fns.getDeviceVK(&vkDevCtx);
        ifaceErr = ctx->fns.getInterfaceVK(
            &backendShared, ffxDevice, ctx->scratchBuffer, ctx->scratchBufferSize, FFX_FSR3_CONTEXT_COUNT);
    }
    if (ifaceErr != FFX_OK) {
        const bool wasDx12Backend = (ctx->backend == WrapperBackend::Dx12Bridge);
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity,
                   wasDx12Backend ? "ffxGetInterfaceDX12 failed"
                                  : "ffxGetInterfaceVK failed");
        return -1;
    }

    FfxFsr3ContextDescription fsr3Desc{};
    fsr3Desc.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE | FFX_FSR3_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (!enableFrameGeneration) fsr3Desc.flags |= FFX_FSR3_ENABLE_UPSCALING_ONLY;
    if (initDesc->enableFlags & WOWEE_FSR3_WRAPPER_ENABLE_HDR_INPUT) fsr3Desc.flags |= FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE;
    if (initDesc->enableFlags & WOWEE_FSR3_WRAPPER_ENABLE_DEPTH_INVERTED) fsr3Desc.flags |= FFX_FSR3_ENABLE_DEPTH_INVERTED;
    fsr3Desc.maxRenderSize.width = initDesc->maxRenderWidth;
    fsr3Desc.maxRenderSize.height = initDesc->maxRenderHeight;
    setUpscaleOutputSizeIfPresent(fsr3Desc, initDesc->displayWidth, initDesc->displayHeight);
    fsr3Desc.displaySize.width = initDesc->displayWidth;
    fsr3Desc.displaySize.height = initDesc->displayHeight;
    if (!backendShared.fpSwapChainConfigureFrameGeneration) {
        backendShared.fpSwapChainConfigureFrameGeneration = vkSwapchainConfigureNoop;
    }
    fsr3Desc.backendInterfaceSharedResources = backendShared;
    fsr3Desc.backendInterfaceUpscaling = backendShared;
    fsr3Desc.backendInterfaceFrameInterpolation = backendShared;
    fsr3Desc.backBufferFormat = mapVkFormatToFfxSurfaceFormat(initDesc->colorFormat, false);

    ctx->fsr3ContextStorage = std::malloc(sizeof(FfxFsr3Context));
    if (!ctx->fsr3ContextStorage) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "FSR3 context allocation failed");
        return -1;
    }
    std::memset(ctx->fsr3ContextStorage, 0, sizeof(FfxFsr3Context));

    FfxErrorCode createErr = ctx->fns.fsr3ContextCreate(reinterpret_cast<FfxFsr3Context*>(ctx->fsr3ContextStorage), &fsr3Desc);
    if (createErr != FFX_OK) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "ffxFsr3ContextCreate failed");
        return -1;
    }

    if (enableFrameGeneration) {
        FfxFrameGenerationConfig fgCfg{};
        fgCfg.frameGenerationEnabled = true;
        fgCfg.allowAsyncWorkloads = false;
        fgCfg.flags = 0;
        fgCfg.onlyPresentInterpolated = false;
        FfxErrorCode cfgErr = ctx->fns.fsr3ConfigureFrameGeneration(reinterpret_cast<FfxFsr3Context*>(ctx->fsr3ContextStorage), &fgCfg);
        if (cfgErr != FFX_OK) {
            destroyContext(ctx);
            writeError(outErrorText, outErrorTextCapacity, "ffxFsr3ConfigureFrameGeneration failed");
            return -1;
        }
        ctx->frameGenerationReady = true;
    }

    *outContext = reinterpret_cast<WoweeFsr3WrapperContext>(ctx);
    writeError(outErrorText, outErrorTextCapacity, "");
    return 0;
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT int32_t wowee_fsr3_wrapper_dispatch_upscale(WoweeFsr3WrapperContext context,
                                                                      const WoweeFsr3WrapperDispatchDesc* dispatchDesc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)context;
    (void)dispatchDesc;
    return -1;
#else
    if (!context || !dispatchDesc) return -1;
    if (dispatchDesc->structSize < sizeof(WoweeFsr3WrapperDispatchDesc)) return -1;
    if (!dispatchDesc->commandBuffer || !dispatchDesc->colorImage || !dispatchDesc->depthImage ||
        !dispatchDesc->motionVectorImage || !dispatchDesc->outputImage) {
        setContextError(reinterpret_cast<WrapperContext*>(context), "invalid dispatch resources for upscale");
        return -1;
    }
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
#if defined(_WIN32)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        const uint32_t requiredMask =
            WOWEE_FSR3_WRAPPER_EXTERNAL_COLOR_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_DEPTH_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_MOTION_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_ACQUIRE_SEMAPHORE |
            WOWEE_FSR3_WRAPPER_EXTERNAL_RELEASE_SEMAPHORE;
        if ((dispatchDesc->externalFlags & requiredMask) != requiredMask ||
            dispatchDesc->colorMemoryHandle == 0 || dispatchDesc->depthMemoryHandle == 0 ||
            dispatchDesc->motionVectorMemoryHandle == 0 || dispatchDesc->outputMemoryHandle == 0 ||
            dispatchDesc->acquireSemaphoreHandle == 0 || dispatchDesc->releaseSemaphoreHandle == 0 ||
            dispatchDesc->acquireSemaphoreValue == 0 || dispatchDesc->releaseSemaphoreValue == 0) {
            setContextError(ctx, "dx12_bridge dispatch missing required external handles for upscale");
            return -1;
        }
        if (!ctx->dx12Device) {
            setContextError(ctx, "dx12_bridge D3D12 device is unavailable");
            return -1;
        }
        std::string importError;
        if (!openSharedResourceHandle(ctx->dx12Device, dispatchDesc->colorMemoryHandle, "color", importError) ||
            !openSharedResourceHandle(ctx->dx12Device, dispatchDesc->depthMemoryHandle, "depth", importError) ||
            !openSharedResourceHandle(ctx->dx12Device, dispatchDesc->motionVectorMemoryHandle, "motion vectors", importError) ||
            !openSharedResourceHandle(ctx->dx12Device, dispatchDesc->outputMemoryHandle, "upscale output", importError) ||
            !openSharedFenceHandle(ctx->dx12Device, dispatchDesc->acquireSemaphoreHandle, "acquire semaphore", importError) ||
            !openSharedFenceHandle(ctx->dx12Device, dispatchDesc->releaseSemaphoreHandle, "release semaphore", importError)) {
            setContextError(ctx, importError.c_str());
            return -1;
        }
    }
#elif defined(__linux__)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        const uint32_t requiredMask =
            WOWEE_FSR3_WRAPPER_EXTERNAL_COLOR_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_DEPTH_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_MOTION_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_ACQUIRE_SEMAPHORE |
            WOWEE_FSR3_WRAPPER_EXTERNAL_RELEASE_SEMAPHORE;
        if ((dispatchDesc->externalFlags & requiredMask) != requiredMask ||
            dispatchDesc->colorMemoryHandle == 0 || dispatchDesc->depthMemoryHandle == 0 ||
            dispatchDesc->motionVectorMemoryHandle == 0 || dispatchDesc->outputMemoryHandle == 0 ||
            dispatchDesc->acquireSemaphoreHandle == 0 || dispatchDesc->releaseSemaphoreHandle == 0 ||
            dispatchDesc->acquireSemaphoreValue == 0 || dispatchDesc->releaseSemaphoreValue == 0) {
            setContextError(ctx, "dx12_bridge dispatch missing required external FDs for upscale");
            return -1;
        }
    }
#endif

    FfxResourceDescription colorDesc = makeResourceDescription(
        dispatchDesc->colorFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc = makeResourceDescription(
        dispatchDesc->depthFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_DEPTHTARGET, true);
    FfxResourceDescription mvDesc = makeResourceDescription(
        dispatchDesc->motionVectorFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_UAV);
    if (dispatchDesc->renderWidth == 0 || dispatchDesc->renderHeight == 0 ||
        dispatchDesc->outputWidth == 0 || dispatchDesc->outputHeight == 0) {
        setContextError(ctx, "invalid dispatch dimensions for upscale");
        return -1;
    }
    if (!hasKnownSurfaceFormat(colorDesc) || !hasKnownSurfaceFormat(depthDesc) ||
        !hasKnownSurfaceFormat(mvDesc) || !hasKnownSurfaceFormat(outDesc)) {
        setContextError(ctx, "unsupported image format in upscale dispatch");
        return -1;
    }

    static wchar_t kColorName[] = L"FSR3_Color";
    static wchar_t kDepthName[] = L"FSR3_Depth";
    static wchar_t kMotionName[] = L"FSR3_MotionVectors";
    static wchar_t kOutputName[] = L"FSR3_Output";
    FfxFsr3DispatchUpscaleDescription dispatch{};
    bool useDx12Interop = false;
#if defined(_WIN32)
    useDx12Interop = (ctx->backend == WrapperBackend::Dx12Bridge);
    ID3D12Resource* colorRes = nullptr;
    ID3D12Resource* depthRes = nullptr;
    ID3D12Resource* motionRes = nullptr;
    ID3D12Resource* outputRes = nullptr;
    ID3D12Fence* acquireFence = nullptr;
    ID3D12Fence* releaseFence = nullptr;
    auto cleanupDx12Imports = [&]() {
        safeRelease(colorRes);
        safeRelease(depthRes);
        safeRelease(motionRes);
        safeRelease(outputRes);
        safeRelease(acquireFence);
        safeRelease(releaseFence);
    };
#endif
    if (useDx12Interop) {
#if defined(_WIN32)
        if (ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->colorMemoryHandle), IID_PPV_ARGS(&colorRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->depthMemoryHandle), IID_PPV_ARGS(&depthRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->motionVectorMemoryHandle), IID_PPV_ARGS(&motionRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->outputMemoryHandle), IID_PPV_ARGS(&outputRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->acquireSemaphoreHandle), IID_PPV_ARGS(&acquireFence)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->releaseSemaphoreHandle), IID_PPV_ARGS(&releaseFence)) != S_OK ||
            !colorRes || !depthRes || !motionRes || !outputRes) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to import one or more upscale resources");
            return -1;
        }
        if (ctx->dx12CommandAllocator->Reset() != S_OK ||
            ctx->dx12CommandList->Reset(ctx->dx12CommandAllocator, nullptr) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to reset DX12 command objects for upscale dispatch");
            return -1;
        }
        dispatch.commandList = ctx->fns.getCommandListDX12(ctx->dx12CommandList);
        dispatch.color = ctx->fns.getResourceDX12(colorRes, colorDesc, kColorName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ctx->fns.getResourceDX12(depthRes, depthDesc, kDepthName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ctx->fns.getResourceDX12(motionRes, mvDesc, kMotionName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.upscaleOutput = ctx->fns.getResourceDX12(outputRes, outDesc, kOutputName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
#endif
    } else {
        dispatch.commandList = ctx->fns.getCommandListVK(dispatchDesc->commandBuffer);
        dispatch.color = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->colorImage), colorDesc, kColorName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->depthImage), depthDesc, kDepthName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->motionVectorImage), mvDesc, kMotionName, FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.upscaleOutput = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->outputImage), outDesc, kOutputName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    dispatch.exposure = FfxResource{};
    dispatch.reactive = FfxResource{};
    dispatch.transparencyAndComposition = FfxResource{};
    dispatch.jitterOffset.x = dispatchDesc->jitterX;
    dispatch.jitterOffset.y = dispatchDesc->jitterY;
    dispatch.motionVectorScale.x = dispatchDesc->motionScaleX;
    dispatch.motionVectorScale.y = dispatchDesc->motionScaleY;
    dispatch.renderSize.width = dispatchDesc->renderWidth;
    dispatch.renderSize.height = dispatchDesc->renderHeight;
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = std::max(0.001f, dispatchDesc->frameTimeDeltaMs);
    dispatch.preExposure = 1.0f;
    dispatch.reset = (dispatchDesc->reset != 0u);
    dispatch.cameraNear = dispatchDesc->cameraNear;
    dispatch.cameraFar = dispatchDesc->cameraFar;
    dispatch.cameraFovAngleVertical = dispatchDesc->cameraFovYRadians;
    dispatch.viewSpaceToMetersFactor = 1.0f;

    const bool ok = (ctx->fns.fsr3ContextDispatchUpscale(reinterpret_cast<FfxFsr3Context*>(ctx->fsr3ContextStorage), &dispatch) == FFX_OK);
#if defined(_WIN32)
    if (useDx12Interop) {
        if (ctx->dx12CommandList->Close() != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to close command list after upscale dispatch");
            return -1;
        }
        if (acquireFence && ctx->dx12Queue->Wait(acquireFence, dispatchDesc->acquireSemaphoreValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to wait on shared acquire fence before upscale dispatch");
            return -1;
        }
        ID3D12CommandList* lists[] = {ctx->dx12CommandList};
        ctx->dx12Queue->ExecuteCommandLists(1, lists);
        const uint64_t waitValue = ctx->dx12FenceValue++;
        if (ctx->dx12Queue->Signal(ctx->dx12Fence, waitValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to signal internal completion fence after upscale dispatch");
            return -1;
        }
        if (ctx->dx12Fence->GetCompletedValue() < waitValue) {
            ctx->dx12Fence->SetEventOnCompletion(waitValue, ctx->dx12FenceEvent);
            WaitForSingleObject(ctx->dx12FenceEvent, INFINITE);
        }
        if (releaseFence && ctx->dx12Queue->Signal(releaseFence, dispatchDesc->releaseSemaphoreValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to signal shared release fence after upscale dispatch");
            return -1;
        }
        cleanupDx12Imports();
    }
#endif
    if (!ok) {
        setContextError(ctx, "ffxFsr3ContextDispatchUpscale failed");
        return -1;
    }
    ctx->lastError.clear();
    return 0;
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT int32_t wowee_fsr3_wrapper_dispatch_framegen(WoweeFsr3WrapperContext context,
                                                                       const WoweeFsr3WrapperDispatchDesc* dispatchDesc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)context;
    (void)dispatchDesc;
    return -1;
#else
    if (!context || !dispatchDesc) return -1;
    if (dispatchDesc->structSize < sizeof(WoweeFsr3WrapperDispatchDesc)) return -1;
    if (!dispatchDesc->commandBuffer || !dispatchDesc->outputImage || !dispatchDesc->frameGenOutputImage) {
        setContextError(reinterpret_cast<WrapperContext*>(context), "invalid dispatch resources for frame generation");
        return -1;
    }
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    if (!ctx->frameGenerationReady || !ctx->fns.fsr3DispatchFrameGeneration) {
        setContextError(ctx, "frame generation backend is not ready");
        return -1;
    }
#if defined(_WIN32)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        const uint32_t requiredMask =
            WOWEE_FSR3_WRAPPER_EXTERNAL_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_FRAMEGEN_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_ACQUIRE_SEMAPHORE |
            WOWEE_FSR3_WRAPPER_EXTERNAL_RELEASE_SEMAPHORE;
        if ((dispatchDesc->externalFlags & requiredMask) != requiredMask ||
            dispatchDesc->outputMemoryHandle == 0 || dispatchDesc->frameGenOutputMemoryHandle == 0 ||
            dispatchDesc->acquireSemaphoreHandle == 0 || dispatchDesc->releaseSemaphoreHandle == 0 ||
            dispatchDesc->acquireSemaphoreValue == 0 || dispatchDesc->releaseSemaphoreValue == 0) {
            setContextError(ctx, "dx12_bridge dispatch missing required external handles for frame generation");
            return -1;
        }
        if (!ctx->dx12Device) {
            setContextError(ctx, "dx12_bridge D3D12 device is unavailable");
            return -1;
        }
        std::string importError;
        if (!openSharedResourceHandle(ctx->dx12Device, dispatchDesc->outputMemoryHandle, "present output", importError) ||
            !openSharedResourceHandle(ctx->dx12Device, dispatchDesc->frameGenOutputMemoryHandle, "framegen output", importError) ||
            !openSharedFenceHandle(ctx->dx12Device, dispatchDesc->acquireSemaphoreHandle, "acquire semaphore", importError) ||
            !openSharedFenceHandle(ctx->dx12Device, dispatchDesc->releaseSemaphoreHandle, "release semaphore", importError)) {
            setContextError(ctx, importError.c_str());
            return -1;
        }
    }
#elif defined(__linux__)
    if (ctx->backend == WrapperBackend::Dx12Bridge) {
        const uint32_t requiredMask =
            WOWEE_FSR3_WRAPPER_EXTERNAL_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_FRAMEGEN_OUTPUT_MEMORY |
            WOWEE_FSR3_WRAPPER_EXTERNAL_ACQUIRE_SEMAPHORE |
            WOWEE_FSR3_WRAPPER_EXTERNAL_RELEASE_SEMAPHORE;
        if ((dispatchDesc->externalFlags & requiredMask) != requiredMask ||
            dispatchDesc->outputMemoryHandle == 0 || dispatchDesc->frameGenOutputMemoryHandle == 0 ||
            dispatchDesc->acquireSemaphoreHandle == 0 || dispatchDesc->releaseSemaphoreHandle == 0 ||
            dispatchDesc->acquireSemaphoreValue == 0 || dispatchDesc->releaseSemaphoreValue == 0) {
            setContextError(ctx, "dx12_bridge dispatch missing required external FDs for frame generation");
            return -1;
        }
    }
#endif

    FfxResourceDescription presentDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription fgOutDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_UAV);
    if (dispatchDesc->outputWidth == 0 || dispatchDesc->outputHeight == 0) {
        setContextError(ctx, "invalid dispatch dimensions for frame generation");
        return -1;
    }
    if (!hasKnownSurfaceFormat(presentDesc) || !hasKnownSurfaceFormat(fgOutDesc)) {
        setContextError(ctx, "unsupported image format in frame generation dispatch");
        return -1;
    }

    static wchar_t kPresentName[] = L"FSR3_PresentColor";
    static wchar_t kInterpolatedName[] = L"FSR3_InterpolatedOutput";
    FfxFrameGenerationDispatchDescription fgDispatch{};
    bool useDx12Interop = false;
#if defined(_WIN32)
    useDx12Interop = (ctx->backend == WrapperBackend::Dx12Bridge);
    ID3D12Resource* presentRes = nullptr;
    ID3D12Resource* framegenRes = nullptr;
    ID3D12Fence* acquireFence = nullptr;
    ID3D12Fence* releaseFence = nullptr;
    auto cleanupDx12Imports = [&]() {
        safeRelease(presentRes);
        safeRelease(framegenRes);
        safeRelease(acquireFence);
        safeRelease(releaseFence);
    };
#endif
    if (useDx12Interop) {
#if defined(_WIN32)
        if (ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->outputMemoryHandle), IID_PPV_ARGS(&presentRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->frameGenOutputMemoryHandle), IID_PPV_ARGS(&framegenRes)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->acquireSemaphoreHandle), IID_PPV_ARGS(&acquireFence)) != S_OK ||
            ctx->dx12Device->OpenSharedHandle(reinterpret_cast<HANDLE>(dispatchDesc->releaseSemaphoreHandle), IID_PPV_ARGS(&releaseFence)) != S_OK ||
            !presentRes || !framegenRes) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to import frame generation resources");
            return -1;
        }
        if (ctx->dx12CommandAllocator->Reset() != S_OK ||
            ctx->dx12CommandList->Reset(ctx->dx12CommandAllocator, nullptr) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to reset DX12 command objects for frame generation dispatch");
            return -1;
        }
        fgDispatch.commandList = ctx->fns.getCommandListDX12(ctx->dx12CommandList);
        fgDispatch.presentColor = ctx->fns.getResourceDX12(presentRes, presentDesc, kPresentName, FFX_RESOURCE_STATE_COMPUTE_READ);
        fgDispatch.outputs[0] = ctx->fns.getResourceDX12(framegenRes, fgOutDesc, kInterpolatedName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
#endif
    } else {
        fgDispatch.commandList = ctx->fns.getCommandListVK(dispatchDesc->commandBuffer);
        fgDispatch.presentColor = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->outputImage), presentDesc, kPresentName, FFX_RESOURCE_STATE_COMPUTE_READ);
        fgDispatch.outputs[0] = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->frameGenOutputImage), fgOutDesc, kInterpolatedName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    fgDispatch.numInterpolatedFrames = 1;
    fgDispatch.reset = (dispatchDesc->reset != 0u);
    fgDispatch.backBufferTransferFunction = ctx->hdrInput
        ? FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB
        : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fgDispatch.minMaxLuminance[0] = 0.0f;
    fgDispatch.minMaxLuminance[1] = 1.0f;

    const bool ok = (ctx->fns.fsr3DispatchFrameGeneration(&fgDispatch) == FFX_OK);
#if defined(_WIN32)
    if (useDx12Interop) {
        if (ctx->dx12CommandList->Close() != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to close command list after frame generation dispatch");
            return -1;
        }
        if (acquireFence && ctx->dx12Queue->Wait(acquireFence, dispatchDesc->acquireSemaphoreValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to wait on shared acquire fence before frame generation dispatch");
            return -1;
        }
        ID3D12CommandList* lists[] = {ctx->dx12CommandList};
        ctx->dx12Queue->ExecuteCommandLists(1, lists);
        const uint64_t waitValue = ctx->dx12FenceValue++;
        if (ctx->dx12Queue->Signal(ctx->dx12Fence, waitValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to signal internal completion fence after frame generation dispatch");
            return -1;
        }
        if (ctx->dx12Fence->GetCompletedValue() < waitValue) {
            ctx->dx12Fence->SetEventOnCompletion(waitValue, ctx->dx12FenceEvent);
            WaitForSingleObject(ctx->dx12FenceEvent, INFINITE);
        }
        if (releaseFence && ctx->dx12Queue->Signal(releaseFence, dispatchDesc->releaseSemaphoreValue) != S_OK) {
            cleanupDx12Imports();
            setContextError(ctx, "dx12_bridge failed to signal shared release fence after frame generation dispatch");
            return -1;
        }
        cleanupDx12Imports();
    }
#endif
    if (!ok) {
        setContextError(ctx, "ffxFsr3DispatchFrameGeneration failed");
        return -1;
    }
    ctx->lastError.clear();
    return 0;
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT void wowee_fsr3_wrapper_shutdown(WoweeFsr3WrapperContext context) {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    destroyContext(ctx);
#else
    (void)context;
#endif
}

WOWEE_FSR3_WRAPPER_EXPORT const char* wowee_fsr3_wrapper_get_last_error(WoweeFsr3WrapperContext context) {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    if (!ctx) return "invalid wrapper context";
    return ctx->lastError.c_str();
#else
    (void)context;
    return "wrapper built without WOWEE_HAS_AMD_FSR3_FRAMEGEN";
#endif
}
