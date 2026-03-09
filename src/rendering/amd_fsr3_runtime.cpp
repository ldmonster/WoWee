#include "rendering/amd_fsr3_runtime.hpp"

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
#include "third_party/ffx_fsr3_legacy_compat.h"
#include <ffx_vk.h>
#endif

namespace wowee::rendering {

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
struct AmdFsr3Runtime::RuntimeFns {
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

    std::vector<std::string> candidates;
    if (const char* envPath = std::getenv("WOWEE_FFX_SDK_RUNTIME_LIB")) {
        if (*envPath) candidates.emplace_back(envPath);
    }
#if defined(_WIN32)
    candidates.emplace_back("amd_fidelityfx_vk.dll");
    candidates.emplace_back("libamd_fidelityfx_vk.dll");
    candidates.emplace_back("ffx_fsr3_vk.dll");
    candidates.emplace_back("ffx_fsr3.dll");
#elif defined(__APPLE__)
    candidates.emplace_back("libamd_fidelityfx_vk.dylib");
    candidates.emplace_back("libffx_fsr3_vk.dylib");
    candidates.emplace_back("libffx_fsr3.dylib");
#else
    candidates.emplace_back("./libamd_fidelityfx_vk.so");
    candidates.emplace_back("libamd_fidelityfx_vk.so");
    candidates.emplace_back("./libffx_fsr3_vk.so");
    candidates.emplace_back("libffx_fsr3_vk.so");
    candidates.emplace_back("libffx_fsr3.so");
#endif

    for (const std::string& path : candidates) {
#if defined(_WIN32)
        HMODULE h = LoadLibraryA(path.c_str());
        if (!h) continue;
        libHandle_ = reinterpret_cast<void*>(h);
#else
        void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        libHandle_ = h;
#endif
        loadedLibraryPath_ = path;
        loadPathKind_ = LoadPathKind::Official;
        break;
    }

    if (!libHandle_) {
        lastError_ = "no official runtime (Path A) found";
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

    scratchBufferSize_ = fns_->getScratchMemorySizeVK(FFX_FSR3_CONTEXT_COUNT);
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

    FfxDevice ffxDevice = fns_->getDeviceVK(desc.device);

    FfxInterface backendShared{};
    FfxErrorCode ifaceErr = fns_->getInterfaceVK(
        &backendShared, ffxDevice, desc.physicalDevice, scratchBuffer_, scratchBufferSize_, FFX_FSR3_CONTEXT_COUNT);
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
    LOG_INFO("FSR3 runtime: loaded official library ", loadedLibraryPath_,
             " framegenReady=", frameGenerationReady_ ? "yes" : "no");
    return true;
#endif
}

bool AmdFsr3Runtime::dispatchUpscale(const AmdFsr3RuntimeDispatchDesc& desc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)desc;
    lastError_ = "FSR3 runtime support not compiled in";
    return false;
#else
    if (!ready_ || !fns_) {
        lastError_ = "runtime not initialized";
        return false;
    }
    if (!desc.commandBuffer || !desc.colorImage || !desc.depthImage || !desc.motionVectorImage || !desc.outputImage) {
        lastError_ = "invalid upscale dispatch resources";
        return false;
    }
    if (!contextStorage_ || !fns_->fsr3ContextDispatchUpscale) {
        lastError_ = "official runtime upscale context unavailable";
        return false;
    }

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
    dispatch.color = fns_->getResourceVK(desc.colorImage, colorDesc, kColorName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = fns_->getResourceVK(desc.depthImage, depthDesc, kDepthName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = fns_->getResourceVK(desc.motionVectorImage, mvDesc, kMotionName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = FfxResource{};
    dispatch.reactive = FfxResource{};
    dispatch.transparencyAndComposition = FfxResource{};
    dispatch.upscaleOutput = fns_->getResourceVK(desc.outputImage, outDesc, kOutputName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
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
    if (err != FFX_OK) {
        lastError_ = "ffxFsr3ContextDispatchUpscale failed";
        return false;
    }
    lastError_.clear();
    return true;
#endif
}

bool AmdFsr3Runtime::dispatchFrameGeneration(const AmdFsr3RuntimeDispatchDesc& desc) {
#if !WOWEE_HAS_AMD_FSR3_FRAMEGEN
    (void)desc;
    lastError_ = "FSR3 runtime support not compiled in";
    return false;
#else
    if (!ready_ || !frameGenerationReady_ || !fns_) {
        lastError_ = "frame generation is not ready";
        return false;
    }
    if (!desc.commandBuffer || !desc.outputImage || !desc.frameGenOutputImage ||
        desc.outputWidth == 0 || desc.outputHeight == 0 || desc.outputFormat == VK_FORMAT_UNDEFINED) {
        lastError_ = "invalid frame generation dispatch resources";
        return false;
    }
    if (!contextStorage_ || !fns_->fsr3DispatchFrameGeneration) {
        lastError_ = "official runtime frame generation context unavailable";
        return false;
    }

    FfxResourceDescription presentDesc = makeResourceDescription(
        desc.outputFormat, desc.outputWidth, desc.outputHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription fgOutDesc = makeResourceDescription(
        desc.outputFormat, desc.outputWidth, desc.outputHeight, FFX_RESOURCE_USAGE_UAV);

    static wchar_t kPresentName[] = L"FSR3_PresentColor";
    static wchar_t kInterpolatedName[] = L"FSR3_InterpolatedOutput";
    FfxFrameGenerationDispatchDescription fgDispatch{};
    fgDispatch.commandList = fns_->getCommandListVK(desc.commandBuffer);
    fgDispatch.presentColor = fns_->getResourceVK(
        desc.outputImage, presentDesc, kPresentName, FFX_RESOURCE_STATE_COMPUTE_READ);
    fgDispatch.outputs[0] = fns_->getResourceVK(
        desc.frameGenOutputImage, fgOutDesc, kInterpolatedName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fgDispatch.numInterpolatedFrames = 1;
    fgDispatch.reset = desc.reset;
    fgDispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fgDispatch.minMaxLuminance[0] = 0.0f;
    fgDispatch.minMaxLuminance[1] = 1.0f;

    FfxErrorCode err = fns_->fsr3DispatchFrameGeneration(&fgDispatch);
    if (err != FFX_OK) {
        lastError_ = "ffxFsr3DispatchFrameGeneration failed";
        return false;
    }
    lastError_.clear();
    return true;
#endif
}

void AmdFsr3Runtime::shutdown() {
#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
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
}

}  // namespace wowee::rendering
