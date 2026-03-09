#include "rendering/amd_fsr3_wrapper_abi.h"

#if WOWEE_HAS_AMD_FSR3_FRAMEGEN
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
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
};

struct WrapperContext {
    void* backendLibHandle = nullptr;
    RuntimeFns fns{};
    void* scratchBuffer = nullptr;
    size_t scratchBufferSize = 0;
    void* fsr3ContextStorage = nullptr;
    bool frameGenerationReady = false;
};

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

void destroyContext(WrapperContext* ctx) {
    if (!ctx) return;
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
#endif

}  // namespace

WOWEE_FSR3_WRAPPER_EXPORT uint32_t wowee_fsr3_wrapper_get_abi_version(void) {
    return WOWEE_FSR3_WRAPPER_ABI_VERSION;
}

WOWEE_FSR3_WRAPPER_EXPORT const char* wowee_fsr3_wrapper_get_name(void) {
    return "WoWee FSR3 DX12/VK Wrapper";
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

    std::vector<std::string> candidates;
    if (const char* backendEnv = std::getenv("WOWEE_FSR3_WRAPPER_BACKEND_LIB")) {
        if (*backendEnv) candidates.emplace_back(backendEnv);
    }
    if (const char* runtimeEnv = std::getenv("WOWEE_FFX_SDK_RUNTIME_LIB")) {
        if (*runtimeEnv) candidates.emplace_back(runtimeEnv);
    }
#if defined(_WIN32)
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

    WrapperContext* ctx = new WrapperContext{};
    for (const std::string& path : candidates) {
        ctx->backendLibHandle = openLibrary(path.c_str());
        if (ctx->backendLibHandle) break;
    }
    if (!ctx->backendLibHandle) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "no FSR3 backend runtime found for wrapper");
        return -1;
    }

    ctx->fns.getScratchMemorySizeVK = reinterpret_cast<decltype(ctx->fns.getScratchMemorySizeVK)>(resolveSymbol(ctx->backendLibHandle, "ffxGetScratchMemorySizeVK"));
    ctx->fns.getDeviceVK = reinterpret_cast<decltype(ctx->fns.getDeviceVK)>(resolveSymbol(ctx->backendLibHandle, "ffxGetDeviceVK"));
    ctx->fns.getInterfaceVK = reinterpret_cast<decltype(ctx->fns.getInterfaceVK)>(resolveSymbol(ctx->backendLibHandle, "ffxGetInterfaceVK"));
    ctx->fns.getCommandListVK = reinterpret_cast<decltype(ctx->fns.getCommandListVK)>(resolveSymbol(ctx->backendLibHandle, "ffxGetCommandListVK"));
    ctx->fns.getResourceVK = reinterpret_cast<decltype(ctx->fns.getResourceVK)>(resolveSymbol(ctx->backendLibHandle, "ffxGetResourceVK"));
    ctx->fns.fsr3ContextCreate = reinterpret_cast<decltype(ctx->fns.fsr3ContextCreate)>(resolveSymbol(ctx->backendLibHandle, "ffxFsr3ContextCreate"));
    ctx->fns.fsr3ContextDispatchUpscale = reinterpret_cast<decltype(ctx->fns.fsr3ContextDispatchUpscale)>(resolveSymbol(ctx->backendLibHandle, "ffxFsr3ContextDispatchUpscale"));
    ctx->fns.fsr3ConfigureFrameGeneration = reinterpret_cast<decltype(ctx->fns.fsr3ConfigureFrameGeneration)>(resolveSymbol(ctx->backendLibHandle, "ffxFsr3ConfigureFrameGeneration"));
    ctx->fns.fsr3DispatchFrameGeneration = reinterpret_cast<decltype(ctx->fns.fsr3DispatchFrameGeneration)>(resolveSymbol(ctx->backendLibHandle, "ffxFsr3DispatchFrameGeneration"));
    ctx->fns.fsr3ContextDestroy = reinterpret_cast<decltype(ctx->fns.fsr3ContextDestroy)>(resolveSymbol(ctx->backendLibHandle, "ffxFsr3ContextDestroy"));

    if (!ctx->fns.getScratchMemorySizeVK || !ctx->fns.getDeviceVK || !ctx->fns.getInterfaceVK ||
        !ctx->fns.getCommandListVK || !ctx->fns.getResourceVK || !ctx->fns.fsr3ContextCreate ||
        !ctx->fns.fsr3ContextDispatchUpscale || !ctx->fns.fsr3ContextDestroy) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "backend missing required ffx symbols");
        return -1;
    }

    const bool enableFrameGeneration = (initDesc->enableFlags & WOWEE_FSR3_WRAPPER_ENABLE_FRAME_GENERATION) != 0u;
    if (enableFrameGeneration && (!ctx->fns.fsr3ConfigureFrameGeneration || !ctx->fns.fsr3DispatchFrameGeneration)) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "backend missing frame generation symbols");
        return -1;
    }

    ctx->scratchBufferSize = ctx->fns.getScratchMemorySizeVK(initDesc->physicalDevice, FFX_FSR3_CONTEXT_COUNT);
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

    VkDeviceContext vkDevCtx{};
    vkDevCtx.vkDevice = initDesc->device;
    vkDevCtx.vkPhysicalDevice = initDesc->physicalDevice;
    vkDevCtx.vkDeviceProcAddr = initDesc->getDeviceProcAddr;

    FfxDevice ffxDevice = ctx->fns.getDeviceVK(&vkDevCtx);
    FfxInterface backendShared{};
    FfxErrorCode ifaceErr = ctx->fns.getInterfaceVK(
        &backendShared, ffxDevice, ctx->scratchBuffer, ctx->scratchBufferSize, FFX_FSR3_CONTEXT_COUNT);
    if (ifaceErr != FFX_OK) {
        destroyContext(ctx);
        writeError(outErrorText, outErrorTextCapacity, "ffxGetInterfaceVK failed");
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
        !dispatchDesc->motionVectorImage || !dispatchDesc->outputImage) return -1;
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);

    FfxResourceDescription colorDesc = makeResourceDescription(
        dispatchDesc->colorFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription depthDesc = makeResourceDescription(
        dispatchDesc->depthFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_DEPTHTARGET, true);
    FfxResourceDescription mvDesc = makeResourceDescription(
        dispatchDesc->motionVectorFormat, dispatchDesc->renderWidth, dispatchDesc->renderHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription outDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_UAV);

    static wchar_t kColorName[] = L"FSR3_Color";
    static wchar_t kDepthName[] = L"FSR3_Depth";
    static wchar_t kMotionName[] = L"FSR3_MotionVectors";
    static wchar_t kOutputName[] = L"FSR3_Output";
    FfxFsr3DispatchUpscaleDescription dispatch{};
    dispatch.commandList = ctx->fns.getCommandListVK(dispatchDesc->commandBuffer);
    dispatch.color = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->colorImage), colorDesc, kColorName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->depthImage), depthDesc, kDepthName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->motionVectorImage), mvDesc, kMotionName, FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = FfxResource{};
    dispatch.reactive = FfxResource{};
    dispatch.transparencyAndComposition = FfxResource{};
    dispatch.upscaleOutput = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->outputImage), outDesc, kOutputName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
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

    return (ctx->fns.fsr3ContextDispatchUpscale(reinterpret_cast<FfxFsr3Context*>(ctx->fsr3ContextStorage), &dispatch) == FFX_OK) ? 0 : -1;
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
    if (!dispatchDesc->commandBuffer || !dispatchDesc->outputImage || !dispatchDesc->frameGenOutputImage) return -1;
    WrapperContext* ctx = reinterpret_cast<WrapperContext*>(context);
    if (!ctx->frameGenerationReady || !ctx->fns.fsr3DispatchFrameGeneration) return -1;

    FfxResourceDescription presentDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResourceDescription fgOutDesc = makeResourceDescription(
        dispatchDesc->outputFormat, dispatchDesc->outputWidth, dispatchDesc->outputHeight, FFX_RESOURCE_USAGE_UAV);

    static wchar_t kPresentName[] = L"FSR3_PresentColor";
    static wchar_t kInterpolatedName[] = L"FSR3_InterpolatedOutput";
    FfxFrameGenerationDispatchDescription fgDispatch{};
    fgDispatch.commandList = ctx->fns.getCommandListVK(dispatchDesc->commandBuffer);
    fgDispatch.presentColor = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->outputImage), presentDesc, kPresentName, FFX_RESOURCE_STATE_COMPUTE_READ);
    fgDispatch.outputs[0] = ctx->fns.getResourceVK(reinterpret_cast<void*>(dispatchDesc->frameGenOutputImage), fgOutDesc, kInterpolatedName, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fgDispatch.numInterpolatedFrames = 1;
    fgDispatch.reset = (dispatchDesc->reset != 0u);
    fgDispatch.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fgDispatch.minMaxLuminance[0] = 0.0f;
    fgDispatch.minMaxLuminance[1] = 1.0f;

    return (ctx->fns.fsr3DispatchFrameGeneration(&fgDispatch) == FFX_OK) ? 0 : -1;
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
