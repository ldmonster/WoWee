#include "rendering/amd_fsr3_runtime.hpp"

#include "rendering/amd_fsr3_wrapper_abi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/logger.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#endif

namespace wowee::rendering {

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
struct AmdFsr3Runtime::RuntimeFns {
    uint32_t (*wrapperGetAbiVersion)() = nullptr;
    const char* (*wrapperGetName)() = nullptr;
    int32_t (*wrapperInitialize)(const WoweeFsr3WrapperInitDesc*, WoweeFsr3WrapperContext*, char*, uint32_t) = nullptr;
    int32_t (*wrapperDispatchUpscale)(WoweeFsr3WrapperContext, const WoweeFsr3WrapperDispatchDesc*) = nullptr;
    int32_t (*wrapperDispatchFramegen)(WoweeFsr3WrapperContext, const WoweeFsr3WrapperDispatchDesc*) = nullptr;
    void (*wrapperShutdown)(WoweeFsr3WrapperContext) = nullptr;

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
};
#else
struct AmdFsr3Runtime::RuntimeFns {};
#endif

AmdFsr3Runtime::AmdFsr3Runtime() = default;

AmdFsr3Runtime::~AmdFsr3Runtime() {
    shutdown();
}

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
namespace {
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
}  // namespace
#endif

bool AmdFsr3Runtime::initialize(const AmdFsr3RuntimeInitDesc& desc) {
    shutdown();
    lastError_.clear();
    loadPathKind_ = LoadPathKind::None;
    backend_ = RuntimeBackend::None;

#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)desc;
    lastError_ = "FSR3 runtime support not compiled in";
    return false;
#else
    if (!desc.physicalDevice || !desc.device || !desc.getDeviceProcAddr ||
        desc.maxRenderWidth == 0 || desc.maxRenderHeight == 0 ||
        desc.displayWidth == 0 || desc.displayHeight == 0 ||
        desc.colorFormat == VK_FORMAT_UNDEFINED) {
        LOG_WARNING("FSR3 runtime: invalid initialization descriptors.");
        lastError_ = "invalid initialization descriptors";
        return false;
    }

    struct Candidate {
        std::string path;
        LoadPathKind kind = LoadPathKind::Official;
    };
    std::vector<Candidate> candidates;
    if (const char* envPath = std::getenv("WOWEE_FFX_SDK_RUNTIME_LIB")) {
        if (*envPath) candidates.push_back({envPath, LoadPathKind::Official});
    }
    if (const char* wrapperEnv = std::getenv("WOWEE_FFX_SDK_RUNTIME_WRAPPER_LIB")) {
        if (*wrapperEnv) candidates.push_back({wrapperEnv, LoadPathKind::Wrapper});
    }
#if defined(_WIN32)
    candidates.push_back({"ffx_fsr3_vk.dll", LoadPathKind::Official});
    candidates.push_back({"ffx_fsr3.dll", LoadPathKind::Official});
    candidates.push_back({"ffx_fsr3_vk_wrapper.dll", LoadPathKind::Wrapper});
    candidates.push_back({"ffx_fsr3_bridge.dll", LoadPathKind::Wrapper});
#elif defined(__APPLE__)
    candidates.push_back({"libffx_fsr3_vk.dylib", LoadPathKind::Official});
    candidates.push_back({"libffx_fsr3.dylib", LoadPathKind::Official});
    candidates.push_back({"libffx_fsr3_vk_wrapper.dylib", LoadPathKind::Wrapper});
    candidates.push_back({"libffx_fsr3_bridge.dylib", LoadPathKind::Wrapper});
#else
    candidates.push_back({"./libffx_fsr3_vk.so", LoadPathKind::Official});
    candidates.push_back({"libffx_fsr3_vk.so", LoadPathKind::Official});
    candidates.push_back({"libffx_fsr3.so", LoadPathKind::Official});
    candidates.push_back({"./libffx_fsr3_vk_wrapper.so", LoadPathKind::Wrapper});
    candidates.push_back({"libffx_fsr3_vk_wrapper.so", LoadPathKind::Wrapper});
    candidates.push_back({"libffx_fsr3_bridge.so", LoadPathKind::Wrapper});
#endif

    for (const Candidate& candidate : candidates) {
#if defined(_WIN32)
        HMODULE h = LoadLibraryA(candidate.path.c_str());
        if (!h) continue;
        libHandle_ = reinterpret_cast<void*>(h);
#else
        void* h = dlopen(candidate.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        libHandle_ = h;
#endif
        loadedLibraryPath_ = candidate.path;
        loadPathKind_ = candidate.kind;
        break;
    }
    if (!libHandle_) {
        lastError_ = "no official runtime (Path A) or wrapper runtime (Path B) found";
        return false;
    }

    auto resolveSym = [&](const char* name) -> void* {
#if defined(_WIN32)
        return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(libHandle_), name));
#else
        return dlsym(libHandle_, name);
#endif
    };

    fns_ = new RuntimeFns{};
    if (loadPathKind_ == LoadPathKind::Wrapper) {
        fns_->wrapperGetAbiVersion = reinterpret_cast<decltype(fns_->wrapperGetAbiVersion)>(resolveSym("wowee_fsr3_wrapper_get_abi_version"));
        fns_->wrapperGetName = reinterpret_cast<decltype(fns_->wrapperGetName)>(resolveSym("wowee_fsr3_wrapper_get_name"));
        fns_->wrapperInitialize = reinterpret_cast<decltype(fns_->wrapperInitialize)>(resolveSym("wowee_fsr3_wrapper_initialize"));
        fns_->wrapperDispatchUpscale = reinterpret_cast<decltype(fns_->wrapperDispatchUpscale)>(resolveSym("wowee_fsr3_wrapper_dispatch_upscale"));
        fns_->wrapperDispatchFramegen = reinterpret_cast<decltype(fns_->wrapperDispatchFramegen)>(resolveSym("wowee_fsr3_wrapper_dispatch_framegen"));
        fns_->wrapperShutdown = reinterpret_cast<decltype(fns_->wrapperShutdown)>(resolveSym("wowee_fsr3_wrapper_shutdown"));

        if (!fns_->wrapperGetAbiVersion || !fns_->wrapperInitialize ||
            !fns_->wrapperDispatchUpscale || !fns_->wrapperShutdown) {
            LOG_WARNING("FSR3 runtime: required wrapper ABI symbols not found in ", loadedLibraryPath_);
            lastError_ = "missing required wowee_fsr3_wrapper_* symbols";
            shutdown();
            return false;
        }

        const uint32_t abiVersion = fns_->wrapperGetAbiVersion();
        if (abiVersion != WOWEE_FSR3_WRAPPER_ABI_VERSION) {
            LOG_WARNING("FSR3 runtime: wrapper ABI mismatch. expected=", WOWEE_FSR3_WRAPPER_ABI_VERSION,
                        " got=", abiVersion);
            lastError_ = "wrapper ABI version mismatch";
            shutdown();
            return false;
        }
        if (desc.enableFrameGeneration && !fns_->wrapperDispatchFramegen) {
            LOG_WARNING("FSR3 runtime: wrapper runtime missing framegen dispatch symbol.");
            lastError_ = "wrapper missing frame generation entry points";
            shutdown();
            return false;
        }

        WoweeFsr3WrapperInitDesc wrapperInit{};
        wrapperInit.structSize = sizeof(wrapperInit);
        wrapperInit.abiVersion = WOWEE_FSR3_WRAPPER_ABI_VERSION;
        wrapperInit.physicalDevice = desc.physicalDevice;
        wrapperInit.device = desc.device;
        wrapperInit.getDeviceProcAddr = desc.getDeviceProcAddr;
        wrapperInit.maxRenderWidth = desc.maxRenderWidth;
        wrapperInit.maxRenderHeight = desc.maxRenderHeight;
        wrapperInit.displayWidth = desc.displayWidth;
        wrapperInit.displayHeight = desc.displayHeight;
        wrapperInit.colorFormat = desc.colorFormat;
        wrapperInit.enableFlags = 0;
        if (desc.hdrInput) wrapperInit.enableFlags |= WOWEE_FSR3_WRAPPER_ENABLE_HDR_INPUT;
        if (desc.depthInverted) wrapperInit.enableFlags |= WOWEE_FSR3_WRAPPER_ENABLE_DEPTH_INVERTED;
        if (desc.enableFrameGeneration) wrapperInit.enableFlags |= WOWEE_FSR3_WRAPPER_ENABLE_FRAME_GENERATION;

        char errorText[256] = {};
        WoweeFsr3WrapperContext wrapperCtx = nullptr;
        if (fns_->wrapperInitialize(&wrapperInit, &wrapperCtx, errorText, static_cast<uint32_t>(sizeof(errorText))) != 0 || !wrapperCtx) {
            LOG_WARNING("FSR3 runtime: wrapper initialization failed: ", errorText[0] ? errorText : "unknown error");
            lastError_ = errorText[0] ? errorText : "wrapper initialization failed";
            shutdown();
            return false;
        }

        wrapperContext_ = wrapperCtx;
        frameGenerationReady_ = desc.enableFrameGeneration;
        ready_ = true;
        backend_ = RuntimeBackend::Wrapper;
        if (fns_->wrapperGetName) {
            const char* wrapperName = fns_->wrapperGetName();
            if (wrapperName && *wrapperName) {
                LOG_INFO("FSR3 runtime: wrapper active: ", wrapperName);
            }
        }
        return true;
    }

    fns_->getScratchMemorySizeVK = reinterpret_cast<decltype(fns_->getScratchMemorySizeVK)>(resolveSym("ffxGetScratchMemorySizeVK"));
    fns_->getDeviceVK = reinterpret_cast<decltype(fns_->getDeviceVK)>(resolveSym("ffxGetDeviceVK"));
    fns_->getInterfaceVK = reinterpret_cast<decltype(fns_->getInterfaceVK)>(resolveSym("ffxGetInterfaceVK"));
    fns_->getCommandListVK = reinterpret_cast<decltype(fns_->getCommandListVK)>(resolveSym("ffxGetCommandListVK"));
    fns_->getResourceVK = reinterpret_cast<decltype(fns_->getResourceVK)>(resolveSym("ffxGetResourceVK"));
    fns_->fsr3ContextCreate = reinterpret_cast<decltype(fns_->fsr3ContextCreate)>(resolveSym("ffxFsr3ContextCreate"));
    fns_->fsr3ContextDispatchUpscale = reinterpret_cast<decltype(fns_->fsr3ContextDispatchUpscale)>(resolveSym("ffxFsr3ContextDispatchUpscale"));
    fns_->fsr3ConfigureFrameGeneration = reinterpret_cast<decltype(fns_->fsr3ConfigureFrameGeneration)>(resolveSym("ffxFsr3ConfigureFrameGeneration"));
    fns_->fsr3DispatchFrameGeneration = reinterpret_cast<decltype(fns_->fsr3DispatchFrameGeneration)>(resolveSym("ffxFsr3DispatchFrameGeneration"));
    fns_->fsr3ContextDestroy = reinterpret_cast<decltype(fns_->fsr3ContextDestroy)>(resolveSym("ffxFsr3ContextDestroy"));

    if (!fns_->getScratchMemorySizeVK || !fns_->getDeviceVK || !fns_->getInterfaceVK ||
        !fns_->getCommandListVK || !fns_->getResourceVK || !fns_->fsr3ContextCreate || !fns_->fsr3ContextDispatchUpscale ||
        !fns_->fsr3ContextDestroy) {
        LOG_WARNING("FSR3 runtime: required symbols not found in ", loadedLibraryPath_);
        lastError_ = "missing required Vulkan FSR3 symbols in runtime library";
        shutdown();
        return false;
    }

    scratchBufferSize_ = fns_->getScratchMemorySizeVK(desc.physicalDevice, FFX_FSR3_CONTEXT_COUNT);
    if (scratchBufferSize_ == 0) {
        LOG_WARNING("FSR3 runtime: scratch buffer size query returned 0.");
        lastError_ = "scratch buffer size query returned 0";
        shutdown();
        return false;
    }
    scratchBuffer_ = std::malloc(scratchBufferSize_);
    if (!scratchBuffer_) {
        LOG_WARNING("FSR3 runtime: failed to allocate scratch buffer.");
        lastError_ = "failed to allocate scratch buffer";
        shutdown();
        return false;
    }

    VkDeviceContext vkDevCtx{};
    vkDevCtx.vkDevice = desc.device;
    vkDevCtx.vkPhysicalDevice = desc.physicalDevice;
    vkDevCtx.vkDeviceProcAddr = desc.getDeviceProcAddr;

    FfxDevice ffxDevice = fns_->getDeviceVK(&vkDevCtx);
    FfxInterface backendShared{};
    FfxErrorCode ifaceErr = fns_->getInterfaceVK(&backendShared, ffxDevice, scratchBuffer_, scratchBufferSize_, FFX_FSR3_CONTEXT_COUNT);
    if (ifaceErr != FFX_OK) {
        LOG_WARNING("FSR3 runtime: ffxGetInterfaceVK failed (", static_cast<int>(ifaceErr), ").");
        lastError_ = "ffxGetInterfaceVK failed";
        shutdown();
        return false;
    }

    FfxFsr3ContextDescription ctxDesc{};
    ctxDesc.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE |
                    FFX_FSR3_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (!desc.enableFrameGeneration) {
        ctxDesc.flags |= FFX_FSR3_ENABLE_UPSCALING_ONLY;
    }
    if (desc.hdrInput) ctxDesc.flags |= FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE;
    if (desc.depthInverted) ctxDesc.flags |= FFX_FSR3_ENABLE_DEPTH_INVERTED;
    ctxDesc.maxRenderSize.width = desc.maxRenderWidth;
    ctxDesc.maxRenderSize.height = desc.maxRenderHeight;
    setUpscaleOutputSizeIfPresent(ctxDesc, desc.displayWidth, desc.displayHeight);
    ctxDesc.displaySize.width = desc.displayWidth;
    ctxDesc.displaySize.height = desc.displayHeight;
    if (!backendShared.fpSwapChainConfigureFrameGeneration) {
        backendShared.fpSwapChainConfigureFrameGeneration = vkSwapchainConfigureNoop;
    }
    ctxDesc.backendInterfaceSharedResources = backendShared;
    ctxDesc.backendInterfaceUpscaling = backendShared;
    ctxDesc.backendInterfaceFrameInterpolation = backendShared;
    ctxDesc.fpMessage = nullptr;
    ctxDesc.backBufferFormat = mapVkFormatToFfxSurfaceFormat(desc.colorFormat, false);

    contextStorage_ = std::malloc(sizeof(FfxFsr3Context));
    if (!contextStorage_) {
        LOG_WARNING("FSR3 runtime: failed to allocate context storage.");
        lastError_ = "failed to allocate context storage";
        shutdown();
        return false;
    }
    std::memset(contextStorage_, 0, sizeof(FfxFsr3Context));

    FfxErrorCode createErr = fns_->fsr3ContextCreate(reinterpret_cast<FfxFsr3Context*>(contextStorage_), &ctxDesc);
    if (createErr != FFX_OK) {
        LOG_WARNING("FSR3 runtime: ffxFsr3ContextCreate failed (", static_cast<int>(createErr), ").");
        lastError_ = "ffxFsr3ContextCreate failed";
        shutdown();
        return false;
    }

    if (desc.enableFrameGeneration) {
        if (!fns_->fsr3ConfigureFrameGeneration || !fns_->fsr3DispatchFrameGeneration) {
            LOG_WARNING("FSR3 runtime: frame generation symbols unavailable in ", loadedLibraryPath_);
            lastError_ = "frame generation entry points unavailable";
            shutdown();
            return false;
        }
        FfxFrameGenerationConfig fgCfg{};
        fgCfg.frameGenerationEnabled = true;
        fgCfg.allowAsyncWorkloads = false;
        fgCfg.flags = 0;
        fgCfg.onlyPresentInterpolated = false;
        FfxErrorCode cfgErr = fns_->fsr3ConfigureFrameGeneration(
            reinterpret_cast<FfxFsr3Context*>(contextStorage_), &fgCfg);
        if (cfgErr != FFX_OK) {
            LOG_WARNING("FSR3 runtime: ffxFsr3ConfigureFrameGeneration failed (", static_cast<int>(cfgErr), ").");
            lastError_ = "ffxFsr3ConfigureFrameGeneration failed";
            shutdown();
            return false;
        }
        frameGenerationReady_ = true;
    }

    ready_ = true;
    backend_ = RuntimeBackend::Official;
    return true;
#endif
}

bool AmdFsr3Runtime::dispatchUpscale(const AmdFsr3RuntimeDispatchDesc& desc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)desc;
    return false;
#else
    if (!ready_ || !fns_) return false;
    if (!desc.commandBuffer || !desc.colorImage || !desc.depthImage || !desc.motionVectorImage || !desc.outputImage) return false;
    if (backend_ == RuntimeBackend::Wrapper) {
        if (!wrapperContext_ || !fns_->wrapperDispatchUpscale) return false;
        WoweeFsr3WrapperDispatchDesc wrapperDesc{};
        wrapperDesc.structSize = sizeof(wrapperDesc);
        wrapperDesc.commandBuffer = desc.commandBuffer;
        wrapperDesc.colorImage = desc.colorImage;
        wrapperDesc.depthImage = desc.depthImage;
        wrapperDesc.motionVectorImage = desc.motionVectorImage;
        wrapperDesc.outputImage = desc.outputImage;
        wrapperDesc.frameGenOutputImage = desc.frameGenOutputImage;
        wrapperDesc.renderWidth = desc.renderWidth;
        wrapperDesc.renderHeight = desc.renderHeight;
        wrapperDesc.outputWidth = desc.outputWidth;
        wrapperDesc.outputHeight = desc.outputHeight;
        wrapperDesc.colorFormat = desc.colorFormat;
        wrapperDesc.depthFormat = desc.depthFormat;
        wrapperDesc.motionVectorFormat = desc.motionVectorFormat;
        wrapperDesc.outputFormat = desc.outputFormat;
        wrapperDesc.jitterX = desc.jitterX;
        wrapperDesc.jitterY = desc.jitterY;
        wrapperDesc.motionScaleX = desc.motionScaleX;
        wrapperDesc.motionScaleY = desc.motionScaleY;
        wrapperDesc.frameTimeDeltaMs = desc.frameTimeDeltaMs;
        wrapperDesc.cameraNear = desc.cameraNear;
        wrapperDesc.cameraFar = desc.cameraFar;
        wrapperDesc.cameraFovYRadians = desc.cameraFovYRadians;
        wrapperDesc.reset = desc.reset ? 1u : 0u;
        return fns_->wrapperDispatchUpscale(static_cast<WoweeFsr3WrapperContext>(wrapperContext_), &wrapperDesc) == 0;
    }

    if (!contextStorage_ || !fns_->fsr3ContextDispatchUpscale) return false;

    FfxResourceDescription colorDesc = makeResourceDescription(
        desc.colorFormat, desc.renderWidth, desc.renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc = makeResourceDescription(
        desc.depthFormat, desc.renderWidth, desc.renderHeight, FFX_RESOURCE_USAGE_DEPTHTARGET, true);
    FfxResourceDescription mvDesc = makeResourceDescription(
        desc.motionVectorFormat, desc.renderWidth, desc.renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc = makeResourceDescription(
        desc.outputFormat, desc.outputWidth, desc.outputHeight, FFX_RESOURCE_USAGE_UAV);

    FfxFsr3DispatchUpscaleDescription dispatch{};
    dispatch.commandList = fns_->getCommandListVK(desc.commandBuffer);
    static wchar_t kColorName[] = L"FSR3_Color";
    static wchar_t kDepthName[] = L"FSR3_Depth";
    static wchar_t kMotionName[] = L"FSR3_MotionVectors";
    static wchar_t kOutputName[] = L"FSR3_Output";
    dispatch.color = fns_->getResourceVK(reinterpret_cast<void*>(desc.colorImage), colorDesc, kColorName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = fns_->getResourceVK(reinterpret_cast<void*>(desc.depthImage), depthDesc, kDepthName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = fns_->getResourceVK(reinterpret_cast<void*>(desc.motionVectorImage), mvDesc, kMotionName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = FfxResource{};
    dispatch.reactive = FfxResource{};
    dispatch.transparencyAndComposition = FfxResource{};
    dispatch.upscaleOutput = fns_->getResourceVK(reinterpret_cast<void*>(desc.outputImage), outDesc, kOutputName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset.x = desc.jitterX;
    dispatch.jitterOffset.y = desc.jitterY;
    dispatch.motionVectorScale.x = desc.motionScaleX;
    dispatch.motionVectorScale.y = desc.motionScaleY;
    dispatch.renderSize.width = desc.renderWidth;
    dispatch.renderSize.height = desc.renderHeight;
    dispatch.enableSharpening = false;
    dispatch.sharpness = 0.0f;
    dispatch.frameTimeDelta = std::max(0.001f, desc.frameTimeDeltaMs);
    dispatch.preExposure = 1.0f;
    dispatch.reset = desc.reset;
    dispatch.cameraNear = desc.cameraNear;
    dispatch.cameraFar = desc.cameraFar;
    dispatch.cameraFovAngleVertical = desc.cameraFovYRadians;
    dispatch.viewSpaceToMetersFactor = 1.0f;

    FfxErrorCode err = fns_->fsr3ContextDispatchUpscale(
        reinterpret_cast<FfxFsr3Context*>(contextStorage_), &dispatch);
    return err == FFX_OK;
#endif
}

bool AmdFsr3Runtime::dispatchFrameGeneration(const AmdFsr3RuntimeDispatchDesc& desc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)desc;
    return false;
#else
    if (!ready_ || !frameGenerationReady_ || !fns_) return false;
    if (!desc.commandBuffer || !desc.outputImage || !desc.frameGenOutputImage ||
        desc.outputWidth == 0 || desc.outputHeight == 0 || desc.outputFormat == VK_FORMAT_UNDEFINED) return false;
    if (backend_ == RuntimeBackend::Wrapper) {
        if (!wrapperContext_ || !fns_->wrapperDispatchFramegen) return false;
        WoweeFsr3WrapperDispatchDesc wrapperDesc{};
        wrapperDesc.structSize = sizeof(wrapperDesc);
        wrapperDesc.commandBuffer = desc.commandBuffer;
        wrapperDesc.colorImage = desc.colorImage;
        wrapperDesc.depthImage = desc.depthImage;
        wrapperDesc.motionVectorImage = desc.motionVectorImage;
        wrapperDesc.outputImage = desc.outputImage;
        wrapperDesc.frameGenOutputImage = desc.frameGenOutputImage;
        wrapperDesc.renderWidth = desc.renderWidth;
        wrapperDesc.renderHeight = desc.renderHeight;
        wrapperDesc.outputWidth = desc.outputWidth;
        wrapperDesc.outputHeight = desc.outputHeight;
        wrapperDesc.colorFormat = desc.colorFormat;
        wrapperDesc.depthFormat = desc.depthFormat;
        wrapperDesc.motionVectorFormat = desc.motionVectorFormat;
        wrapperDesc.outputFormat = desc.outputFormat;
        wrapperDesc.jitterX = desc.jitterX;
        wrapperDesc.jitterY = desc.jitterY;
        wrapperDesc.motionScaleX = desc.motionScaleX;
        wrapperDesc.motionScaleY = desc.motionScaleY;
        wrapperDesc.frameTimeDeltaMs = desc.frameTimeDeltaMs;
        wrapperDesc.cameraNear = desc.cameraNear;
        wrapperDesc.cameraFar = desc.cameraFar;
        wrapperDesc.cameraFovYRadians = desc.cameraFovYRadians;
        wrapperDesc.reset = desc.reset ? 1u : 0u;
        return fns_->wrapperDispatchFramegen(static_cast<WoweeFsr3WrapperContext>(wrapperContext_), &wrapperDesc) == 0;
    }

    if (!contextStorage_ || !fns_->fsr3DispatchFrameGeneration) return false;

    FfxResourceDescription presentDesc = makeResourceDescription(
        desc.outputFormat, desc.outputWidth, desc.outputHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription fgOutDesc = makeResourceDescription(
        desc.outputFormat, desc.outputWidth, desc.outputHeight, FFX_RESOURCE_USAGE_UAV);

    static wchar_t kPresentName[] = L"FSR3_PresentColor";
    static wchar_t kInterpolatedName[] = L"FSR3_InterpolatedOutput";
    FfxFrameGenerationDispatchDescription fgDispatch{};
    fgDispatch.commandList = fns_->getCommandListVK(desc.commandBuffer);
    fgDispatch.presentColor = fns_->getResourceVK(
        reinterpret_cast<void*>(desc.outputImage), presentDesc, kPresentName, FFX_RESOURCE_STATE_COMPUTE_READ);
    fgDispatch.outputs[0] = fns_->getResourceVK(
        reinterpret_cast<void*>(desc.frameGenOutputImage), fgOutDesc, kInterpolatedName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fgDispatch.numInterpolatedFrames = 1;
    fgDispatch.reset = desc.reset;
    fgDispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fgDispatch.minMaxLuminance[0] = 0.0f;
    fgDispatch.minMaxLuminance[1] = 1.0f;

    FfxErrorCode err = fns_->fsr3DispatchFrameGeneration(&fgDispatch);
    return err == FFX_OK;
#endif
}

void AmdFsr3Runtime::shutdown() {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
    if (wrapperContext_ && fns_ && fns_->wrapperShutdown) {
        fns_->wrapperShutdown(static_cast<WoweeFsr3WrapperContext>(wrapperContext_));
    }
    wrapperContext_ = nullptr;
    if (contextStorage_ && fns_ && fns_->fsr3ContextDestroy) {
        fns_->fsr3ContextDestroy(reinterpret_cast<FfxFsr3Context*>(contextStorage_));
    }
#endif
    if (contextStorage_) {
        std::free(contextStorage_);
        contextStorage_ = nullptr;
    }
    if (scratchBuffer_) {
        std::free(scratchBuffer_);
        scratchBuffer_ = nullptr;
    }
    scratchBufferSize_ = 0;
    ready_ = false;
    frameGenerationReady_ = false;
    if (fns_) {
        delete fns_;
        fns_ = nullptr;
    }
#if defined(_WIN32)
    if (libHandle_) FreeLibrary(reinterpret_cast<HMODULE>(libHandle_));
#else
    if (libHandle_) dlclose(libHandle_);
#endif
    libHandle_ = nullptr;
    loadedLibraryPath_.clear();
    loadPathKind_ = LoadPathKind::None;
    backend_ = RuntimeBackend::None;
}

}  // namespace wowee::rendering
