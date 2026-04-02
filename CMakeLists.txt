cmake_minimum_required(VERSION 3.15)
project(wowee VERSION 1.0.0 LANGUAGES C CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Explicitly tag optimized configs so runtime defaults can enforce low-noise logging.
add_compile_definitions(
    $<$<CONFIG:Release>:WOWEE_RELEASE_LOGGING>
    $<$<CONFIG:RelWithDebInfo>:WOWEE_RELEASE_LOGGING>
    $<$<CONFIG:MinSizeRel>:WOWEE_RELEASE_LOGGING>
)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(WIN32)
    # Needed for Vulkan Win32 external memory/semaphore handle types used by FSR3 interop.
    add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR)
endif()

# Options
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(WOWEE_BUILD_TESTS "Build tests" OFF)
option(WOWEE_ENABLE_ASAN "Enable AddressSanitizer (Debug builds)" OFF)
option(WOWEE_ENABLE_AMD_FSR2 "Enable AMD FidelityFX FSR2 backend when SDK is present" ON)
option(WOWEE_ENABLE_AMD_FSR3_FRAMEGEN "Enable AMD FidelityFX SDK FSR3 frame generation interface probe when SDK is present" ON)
option(WOWEE_BUILD_AMD_FSR3_RUNTIME "Build native AMD FidelityFX VK runtime (Path A) from extern/FidelityFX-SDK/Kits" ON)

# AMD FidelityFX FSR2 SDK detection (drop-in under extern/FidelityFX-FSR2)
set(WOWEE_AMD_FSR2_DIR ${CMAKE_SOURCE_DIR}/extern/FidelityFX-FSR2)
set(WOWEE_AMD_FSR2_HEADER ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/ffx_fsr2.h)
set(WOWEE_AMD_FSR2_VK_PERM_HEADER ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/shaders/ffx_fsr2_accumulate_pass_permutations.h)
set(WOWEE_AMD_FSR2_VK_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/fsr2_vk_permutations)

# Upstream SDK checkouts may not ship generated Vulkan permutation headers.
# If we have a vendored snapshot, copy it into the SDK tree before detection.
if(WOWEE_ENABLE_AMD_FSR2 AND EXISTS ${WOWEE_AMD_FSR2_HEADER} AND NOT EXISTS ${WOWEE_AMD_FSR2_VK_PERM_HEADER} AND EXISTS ${WOWEE_AMD_FSR2_VK_VENDOR_DIR})
    file(GLOB WOWEE_AMD_FSR2_VK_VENDOR_HEADERS "${WOWEE_AMD_FSR2_VK_VENDOR_DIR}/ffx_fsr2_*pass*.h")
    list(LENGTH WOWEE_AMD_FSR2_VK_VENDOR_HEADERS WOWEE_AMD_FSR2_VK_VENDOR_COUNT)
    if(WOWEE_AMD_FSR2_VK_VENDOR_COUNT GREATER 0)
        file(MAKE_DIRECTORY ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/shaders)
        file(COPY ${WOWEE_AMD_FSR2_VK_VENDOR_HEADERS} DESTINATION ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/shaders)
        message(STATUS "AMD FSR2: bootstrapped ${WOWEE_AMD_FSR2_VK_VENDOR_COUNT} vendored Vulkan permutation headers")
    endif()
endif()

if(WOWEE_ENABLE_AMD_FSR2 AND EXISTS ${WOWEE_AMD_FSR2_HEADER} AND EXISTS ${WOWEE_AMD_FSR2_VK_PERM_HEADER})
    message(STATUS "AMD FSR2 SDK detected at ${WOWEE_AMD_FSR2_DIR}")
    add_compile_definitions(WOWEE_HAS_AMD_FSR2=1)
    add_compile_definitions(FFX_GCC=1)

    # AMD FSR2 Vulkan backend sources (official SDK implementation)
    set(WOWEE_AMD_FSR2_SOURCES
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/ffx_assert.cpp
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/ffx_fsr2.cpp
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/ffx_fsr2_vk.cpp
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/shaders/ffx_fsr2_shaders_vk.cpp
    )
    add_library(wowee_fsr2_amd_vk STATIC ${WOWEE_AMD_FSR2_SOURCES})
    set_target_properties(wowee_fsr2_amd_vk PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(wowee_fsr2_amd_vk PUBLIC
        ${WOWEE_AMD_FSR2_DIR}/src
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk
        ${WOWEE_AMD_FSR2_DIR}/src/ffx-fsr2-api/vk/shaders
    )
    set(WOWEE_FFX_COMPAT_HEADER ${CMAKE_SOURCE_DIR}/include/third_party/ffx_fsr2_compat.h)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(wowee_fsr2_amd_vk PRIVATE
            "-include${WOWEE_FFX_COMPAT_HEADER}"
        )
    elseif(MSVC)
        target_compile_options(wowee_fsr2_amd_vk PRIVATE
            "/FI${WOWEE_FFX_COMPAT_HEADER}"
        )
    endif()
    target_link_libraries(wowee_fsr2_amd_vk PUBLIC Vulkan::Vulkan)
else()
    add_compile_definitions(WOWEE_HAS_AMD_FSR2=0)
    if(WOWEE_ENABLE_AMD_FSR2)
        if(NOT EXISTS ${WOWEE_AMD_FSR2_HEADER})
            message(WARNING "AMD FSR2 SDK not found at ${WOWEE_AMD_FSR2_DIR}; using internal fallback implementation.")
        elseif(NOT EXISTS ${WOWEE_AMD_FSR2_VK_PERM_HEADER})
            message(WARNING "AMD FSR2 SDK found, but generated Vulkan permutation headers are missing (e.g. ${WOWEE_AMD_FSR2_VK_PERM_HEADER}); using internal fallback implementation.")
        endif()
    endif()
endif()

# AMD FidelityFX SDK (FSR3 frame generation interfaces) detection under extern/FidelityFX-SDK
set(WOWEE_AMD_FFX_SDK_KITS_DIR ${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/Kits/FidelityFX)
set(WOWEE_AMD_FFX_SDK_KITS_FG_HEADER ${WOWEE_AMD_FFX_SDK_KITS_DIR}/framegeneration/include/ffx_framegeneration.h)

set(WOWEE_AMD_FFX_SDK_KITS_READY FALSE)
if(EXISTS ${WOWEE_AMD_FFX_SDK_KITS_DIR}/upscalers/fsr3/include/ffx_fsr3upscaler.h
   AND EXISTS ${WOWEE_AMD_FFX_SDK_KITS_DIR}/framegeneration/fsr3/include/ffx_frameinterpolation.h
   AND EXISTS ${WOWEE_AMD_FFX_SDK_KITS_DIR}/framegeneration/fsr3/include/ffx_opticalflow.h
   AND EXISTS ${WOWEE_AMD_FFX_SDK_KITS_DIR}/backend/vk/ffx_vk.h)
    set(WOWEE_AMD_FFX_SDK_KITS_READY TRUE)
endif()

if(WOWEE_ENABLE_AMD_FSR3_FRAMEGEN AND WOWEE_AMD_FFX_SDK_KITS_READY)
    message(STATUS "AMD FidelityFX-SDK framegen headers detected at ${WOWEE_AMD_FFX_SDK_KITS_DIR} (Kits layout)")
    add_compile_definitions(WOWEE_AMD_FFX_SDK_KITS=1)
    add_compile_definitions(WOWEE_HAS_AMD_FSR3_FRAMEGEN=1)
    add_library(wowee_fsr3_framegen_amd_vk_probe STATIC
        src/rendering/amd_fsr3_framegen_probe.cpp
    )
    set_target_properties(wowee_fsr3_framegen_amd_vk_probe PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(wowee_fsr3_framegen_amd_vk_probe PUBLIC
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/upscalers/include
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/upscalers/fsr3/include
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/framegeneration/fsr3/include
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/framegeneration/include
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/backend/vk
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/api/internal
        ${WOWEE_AMD_FFX_SDK_KITS_DIR}/api/include
    )
    target_link_libraries(wowee_fsr3_framegen_amd_vk_probe PUBLIC Vulkan::Vulkan)

    if(WOWEE_BUILD_AMD_FSR3_RUNTIME)
        message(STATUS "AMD FSR3 Path A runtime target enabled: build with target 'wowee_fsr3_official_runtime_copy'")
        set(WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR ${CMAKE_BINARY_DIR}/ffx_sdk_runtime_build)
        if(WIN32)
            set(WOWEE_AMD_FSR3_RUNTIME_NAME amd_fidelityfx_vk.dll)
            if(CMAKE_CONFIGURATION_TYPES)
                set(WOWEE_AMD_FSR3_RUNTIME_SRC ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}/$<CONFIG>/amd_fidelityfx_vk.dll)
            else()
                set(WOWEE_AMD_FSR3_RUNTIME_SRC ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}/amd_fidelityfx_vk.dll)
            endif()
        elseif(APPLE)
            set(WOWEE_AMD_FSR3_RUNTIME_NAME libamd_fidelityfx_vk.dylib)
            if(CMAKE_CONFIGURATION_TYPES)
                set(WOWEE_AMD_FSR3_RUNTIME_SRC ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}/$<CONFIG>/libamd_fidelityfx_vk.dylib)
            else()
                set(WOWEE_AMD_FSR3_RUNTIME_SRC ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}/libamd_fidelityfx_vk.dylib)
            endif()
        else()
            set(WOWEE_AMD_FSR3_RUNTIME_NAME libamd_fidelityfx_vk.so)
            set(WOWEE_AMD_FSR3_RUNTIME_SRC ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}/libamd_fidelityfx_vk.so)
        endif()

        if(CMAKE_BUILD_TYPE)
            set(WOWEE_AMD_FSR3_RUNTIME_BUILD_TYPE ${CMAKE_BUILD_TYPE})
        else()
            set(WOWEE_AMD_FSR3_RUNTIME_BUILD_TYPE Release)
        endif()

        # Locate bash at configure time so the build-time COMMAND works on Windows
        # (cmake custom commands run via cmd.exe on Windows, so bare 'bash' is not found).
        find_program(BASH_EXECUTABLE bash
            HINTS
                /usr/bin
                /bin
                "${MSYS2_PATH}/usr/bin"
                "$ENV{MSYS2_PATH}/usr/bin"
                "C:/msys64/usr/bin"
                "D:/msys64/usr/bin"
        )
        if(BASH_EXECUTABLE)
            add_custom_target(wowee_fsr3_official_runtime_build
                COMMAND ${CMAKE_COMMAND}
                    -S ${WOWEE_AMD_FFX_SDK_KITS_DIR}
                    -B ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}
                    -DCMAKE_BUILD_TYPE=${WOWEE_AMD_FSR3_RUNTIME_BUILD_TYPE}
                    -DFFX_BUILD_VK=ON
                    -DFFX_BUILD_FRAMEGENERATION=ON
                    -DFFX_BUILD_UPSCALER=ON
                COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generate_ffx_sdk_vk_permutations.sh
                    ${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK
                COMMAND ${CMAKE_COMMAND}
                    --build ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}
                    --config $<CONFIG>
                    --parallel
                COMMENT "Building native AMD FSR3 runtime (Path A) from FidelityFX-SDK Kits"
                VERBATIM
            )
        else()
            message(STATUS "bash not found; VK permutation headers will not be auto-generated")
            add_custom_target(wowee_fsr3_official_runtime_build
                COMMAND ${CMAKE_COMMAND}
                    -S ${WOWEE_AMD_FFX_SDK_KITS_DIR}
                    -B ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}
                    -DCMAKE_BUILD_TYPE=${WOWEE_AMD_FSR3_RUNTIME_BUILD_TYPE}
                    -DFFX_BUILD_VK=ON
                    -DFFX_BUILD_FRAMEGENERATION=ON
                    -DFFX_BUILD_UPSCALER=ON
                COMMAND ${CMAKE_COMMAND}
                    --build ${WOWEE_AMD_FSR3_RUNTIME_BUILD_DIR}
                    --config $<CONFIG>
                    --parallel
                COMMENT "Building native AMD FSR3 runtime (Path A) from FidelityFX-SDK Kits (no permutation bootstrap)"
                VERBATIM
            )
        endif()

        add_custom_target(wowee_fsr3_official_runtime_copy
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${WOWEE_AMD_FSR3_RUNTIME_SRC}
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${WOWEE_AMD_FSR3_RUNTIME_NAME}
            DEPENDS wowee_fsr3_official_runtime_build
            COMMENT "Copying native AMD FSR3 runtime to ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            VERBATIM
        )
    endif()

else()
    add_compile_definitions(WOWEE_HAS_AMD_FSR3_FRAMEGEN=0)
    add_compile_definitions(WOWEE_AMD_FFX_SDK_KITS=0)
    if(WOWEE_ENABLE_AMD_FSR3_FRAMEGEN)
        if(EXISTS ${WOWEE_AMD_FFX_SDK_KITS_FG_HEADER})
            message(STATUS "FidelityFX-SDK Kits layout detected at ${WOWEE_AMD_FFX_SDK_KITS_DIR}, but required FSR3 headers are incomplete. FSR3 framegen interface probe disabled.")
        else()
            message(WARNING "AMD FidelityFX-SDK Kits headers not found at ${WOWEE_AMD_FFX_SDK_KITS_DIR}; FSR3 framegen interface probe disabled.")
        endif()
    endif()
endif()

# Opcode registry generation/validation
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(opcodes-generate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_opcode_registry.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating opcode registry include fragments"
    )

    add_custom_target(opcodes-validate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_opcode_maps.py --root ${CMAKE_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS opcodes-generate
        COMMENT "Validating canonical opcode registry and expansion maps"
    )
endif()

# Find required packages
find_package(SDL2 REQUIRED)
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    # For Windows cross-compilation the host pkg-config finds the Linux libvulkan-dev
    # and injects /usr/include as an INTERFACE_INCLUDE_DIRECTORY, which causes
    # MinGW clang to pull in glibc headers (bits/libc-header-start.h) instead of
    # the MinGW sysroot headers.  Skip the host pkg-config path entirely and instead
    # locate Vulkan via vcpkg-installed vulkan-headers or the MinGW toolchain.
    if(CMAKE_CROSSCOMPILING AND WIN32)
        # The cross-compile build script generates a Vulkan import library
        # (libvulkan-1.a) in ${CMAKE_BINARY_DIR}/vulkan-import from the headers.
        set(_VULKAN_IMPORT_DIR "${CMAKE_BINARY_DIR}/vulkan-import")

        find_package(VulkanHeaders CONFIG QUIET)
        if(VulkanHeaders_FOUND)
            if(NOT TARGET Vulkan::Vulkan)
                add_library(Vulkan::Vulkan INTERFACE IMPORTED)
            endif()
            # Vulkan::Headers is provided by vcpkg's vulkan-headers port and carries
            # the correct MinGW include path — no Linux system headers involved.
            set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES Vulkan::Headers)
            # Link against the Vulkan loader import library (vulkan-1.dll).
            if(EXISTS "${_VULKAN_IMPORT_DIR}/libvulkan-1.a")
                set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                    INTERFACE_LINK_DIRECTORIES "${_VULKAN_IMPORT_DIR}")
                set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                    INTERFACE_LINK_LIBRARIES vulkan-1)
            endif()
            set(Vulkan_FOUND TRUE)
            message(STATUS "Found Vulkan headers for Windows cross-compile via vcpkg VulkanHeaders")
        else()
            # Last-resort: check the LLVM-MinGW toolchain sysroot directly.
            find_path(_VULKAN_MINGW_INCLUDE NAMES vulkan/vulkan.h
                PATHS /opt/llvm-mingw/x86_64-w64-mingw32/include NO_DEFAULT_PATH)
            if(_VULKAN_MINGW_INCLUDE)
                if(NOT TARGET Vulkan::Vulkan)
                    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
                endif()
                set_target_properties(Vulkan::Vulkan PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${_VULKAN_MINGW_INCLUDE}")
                # Link against the Vulkan loader import library (vulkan-1.dll).
                if(EXISTS "${_VULKAN_IMPORT_DIR}/libvulkan-1.a")
                    set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                        INTERFACE_LINK_DIRECTORIES "${_VULKAN_IMPORT_DIR}")
                    set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                        INTERFACE_LINK_LIBRARIES vulkan-1)
                endif()
                set(Vulkan_FOUND TRUE)
                message(STATUS "Found Vulkan headers in LLVM-MinGW sysroot: ${_VULKAN_MINGW_INCLUDE}")
            endif()
        endif()
    elseif(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        # macOS cross-compilation: use vcpkg-installed vulkan-headers.
        # The host pkg-config would find Linux libvulkan-dev headers which the
        # macOS cross-compiler cannot use (different sysroot).
        find_package(VulkanHeaders CONFIG QUIET)
        if(VulkanHeaders_FOUND)
            if(NOT TARGET Vulkan::Vulkan)
                add_library(Vulkan::Vulkan INTERFACE IMPORTED)
            endif()
            set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                INTERFACE_LINK_LIBRARIES Vulkan::Headers)
            set(Vulkan_FOUND TRUE)
            message(STATUS "Found Vulkan headers for macOS cross-compile via vcpkg VulkanHeaders")
        endif()
    else()
        # Fallback: some distros / CMake versions need pkg-config to locate Vulkan.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(VULKAN_PKG vulkan)
            if(VULKAN_PKG_FOUND)
                add_library(Vulkan::Vulkan INTERFACE IMPORTED)
                set_target_properties(Vulkan::Vulkan PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${VULKAN_PKG_INCLUDE_DIRS}"
                    INTERFACE_LINK_LIBRARIES "${VULKAN_PKG_LIBRARIES}"
                )
                if(VULKAN_PKG_LIBRARY_DIRS)
                    set_property(TARGET Vulkan::Vulkan APPEND PROPERTY
                        INTERFACE_LINK_DIRECTORIES "${VULKAN_PKG_LIBRARY_DIRS}")
                endif()
                set(Vulkan_FOUND TRUE)
                message(STATUS "Found Vulkan via pkg-config: ${VULKAN_PKG_LIBRARIES}")
            endif()
        endif()
    endif()
    if(NOT Vulkan_FOUND)
        message(FATAL_ERROR "Could not find Vulkan. Install libvulkan-dev (Linux), vulkan-loader (macOS), or the Vulkan SDK (Windows).")
    endif()
endif()
# macOS cross-compilation: the Vulkan loader (MoltenVK) is not available at link
# time. Allow unresolved Vulkan symbols — they are resolved at runtime.
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(WOWEE_MACOS_CROSS_COMPILE TRUE)
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
if(WIN32)
    find_package(PkgConfig QUIET)
else()
    find_package(PkgConfig REQUIRED)
endif()
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG libavformat libavcodec libswscale libavutil)
else()
    # Fallback for MSVC/vcpkg — find FFmpeg libraries manually
    find_path(FFMPEG_INCLUDE_DIRS libavformat/avformat.h)
    find_library(AVFORMAT_LIB NAMES avformat)
    find_library(AVCODEC_LIB  NAMES avcodec)
    find_library(AVUTIL_LIB   NAMES avutil)
    find_library(SWSCALE_LIB  NAMES swscale)
    set(FFMPEG_LIBRARIES ${AVFORMAT_LIB} ${AVCODEC_LIB} ${AVUTIL_LIB} ${SWSCALE_LIB})
endif()
if(FFMPEG_INCLUDE_DIRS AND AVFORMAT_LIB)
    set(HAVE_FFMPEG TRUE)
    message(STATUS "Found FFmpeg: ${AVFORMAT_LIB}")
elseif(FFMPEG_FOUND)
    set(HAVE_FFMPEG TRUE)
else()
    set(HAVE_FFMPEG FALSE)
    message(WARNING "FFmpeg not found — video_player will be disabled. Install via vcpkg: ffmpeg:x64-windows")
endif()

# Unicorn Engine (x86 emulator for cross-platform Warden module execution)
find_library(UNICORN_LIBRARY NAMES unicorn)
find_path(UNICORN_INCLUDE_DIR unicorn/unicorn.h)
if(NOT UNICORN_LIBRARY OR NOT UNICORN_INCLUDE_DIR)
    message(WARNING "Unicorn Engine not found. Install with: sudo apt-get install libunicorn-dev")
    message(WARNING "Warden emulation will be disabled")
    set(HAVE_UNICORN FALSE)
else()
    message(STATUS "Found Unicorn Engine: ${UNICORN_LIBRARY}")
    set(HAVE_UNICORN TRUE)
endif()

# GLM (header-only math library)
find_package(glm QUIET)
if(NOT glm_FOUND)
    message(STATUS "GLM not found, will use system includes or download")
endif()
# GLM GTX extensions (quaternion, norm, etc.) require this flag on newer GLM versions
add_compile_definitions(GLM_ENABLE_EXPERIMENTAL GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(WIN32)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# SPIR-V shader compilation via glslc
find_program(GLSLC glslc HINTS ${Vulkan_GLSLC_EXECUTABLE} "$ENV{VULKAN_SDK}/bin")
if(GLSLC)
    message(STATUS "Found glslc: ${GLSLC}")
else()
    message(WARNING "glslc not found. Install the Vulkan SDK or vulkan-tools package.")
    message(WARNING "Shaders will not be compiled to SPIR-V.")
endif()

# Function to compile GLSL shaders to SPIR-V
function(compile_shaders TARGET_NAME)
    set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)
    set(SPV_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/shaders)
    file(MAKE_DIRECTORY ${SPV_DIR})

    file(GLOB GLSL_SOURCES "${SHADER_DIR}/*.glsl")
    set(SPV_OUTPUTS)

    foreach(GLSL_FILE ${GLSL_SOURCES})
        get_filename_component(FILE_NAME ${GLSL_FILE} NAME)
        # e.g. skybox.vert.glsl -> skybox.vert.spv
        string(REGEX REPLACE "\\.glsl$" ".spv" SPV_NAME ${FILE_NAME})
        set(SPV_FILE ${SPV_DIR}/${SPV_NAME})

        # Determine shader stage from filename
        if(FILE_NAME MATCHES "\\.vert\\.glsl$")
            set(SHADER_STAGE vertex)
        elseif(FILE_NAME MATCHES "\\.frag\\.glsl$")
            set(SHADER_STAGE fragment)
        elseif(FILE_NAME MATCHES "\\.comp\\.glsl$")
            set(SHADER_STAGE compute)
        elseif(FILE_NAME MATCHES "\\.geom\\.glsl$")
            set(SHADER_STAGE geometry)
        else()
            message(WARNING "Cannot determine shader stage for: ${FILE_NAME}")
            continue()
        endif()

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND ${GLSLC} -fshader-stage=${SHADER_STAGE} -O ${GLSL_FILE} -o ${SPV_FILE}
            DEPENDS ${GLSL_FILE}
            COMMENT "Compiling SPIR-V: ${FILE_NAME} -> ${SPV_NAME}"
            VERBATIM
        )
        list(APPEND SPV_OUTPUTS ${SPV_FILE})
    endforeach()

    add_custom_target(${TARGET_NAME}_shaders ALL DEPENDS ${SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shaders)
endfunction()

# StormLib for MPQ extraction tool (not needed for main executable)
find_library(STORMLIB_LIBRARY NAMES StormLib stormlib storm)
find_path(STORMLIB_INCLUDE_DIR StormLib.h PATH_SUFFIXES StormLib)

# Include ImGui as a static library (Vulkan backend)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui)
if(EXISTS ${IMGUI_DIR})
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
    target_include_directories(imgui PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC SDL2::SDL2 Vulkan::Vulkan ${CMAKE_DL_LIBS})
else()
    message(WARNING "ImGui not found in extern/imgui. Clone it with:")
    message(WARNING "  git clone https://github.com/ocornut/imgui.git extern/imgui")
endif()

# vk-bootstrap (Vulkan device/instance setup)
set(VK_BOOTSTRAP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/vk-bootstrap)
if(EXISTS ${VK_BOOTSTRAP_DIR})
    add_library(vk-bootstrap STATIC
        ${VK_BOOTSTRAP_DIR}/src/VkBootstrap.cpp
    )
    target_include_directories(vk-bootstrap PUBLIC ${VK_BOOTSTRAP_DIR}/src)
    target_link_libraries(vk-bootstrap PUBLIC Vulkan::Vulkan)
else()
    message(FATAL_ERROR "vk-bootstrap not found in extern/vk-bootstrap")
endif()

# Source files
set(WOWEE_SOURCES
    # Core
    src/core/application.cpp
    src/core/entity_spawner.cpp
    src/core/appearance_composer.cpp
    src/core/world_loader.cpp
    src/core/window.cpp
    src/core/input.cpp
    src/core/logger.cpp
    src/core/memory_monitor.cpp

    # Network
    src/network/socket.cpp
    src/network/packet.cpp
    src/network/tcp_socket.cpp
    src/network/world_socket.cpp

    # Auth
    src/auth/auth_handler.cpp
    src/auth/auth_opcodes.cpp
    src/auth/auth_packets.cpp
    src/auth/pin_auth.cpp
    src/auth/integrity.cpp
    src/auth/srp.cpp
    src/auth/big_num.cpp
    src/auth/crypto.cpp
    src/auth/rc4.cpp
    src/auth/vanilla_crypt.cpp

    # Game
    src/game/expansion_profile.cpp
    src/game/opcode_table.cpp
    src/game/update_field_table.cpp
    src/game/game_handler.cpp
    src/game/chat_handler.cpp
    src/game/movement_handler.cpp
    src/game/combat_handler.cpp
    src/game/spell_handler.cpp
    src/game/inventory_handler.cpp
    src/game/social_handler.cpp
    src/game/quest_handler.cpp
    src/game/entity_controller.cpp
    src/game/warden_handler.cpp
    src/game/warden_crypto.cpp
    src/game/warden_module.cpp
    src/game/warden_emulator.cpp
    src/game/warden_memory.cpp
    src/game/transport_manager.cpp
    src/game/world.cpp
    src/game/player.cpp
    src/game/entity.cpp
    src/game/opcodes.cpp
    src/game/world_packets.cpp
    src/game/packet_parsers_tbc.cpp
    src/game/packet_parsers_classic.cpp
    src/game/character.cpp
    src/game/zone_manager.cpp
    src/game/inventory.cpp

    # Audio
    src/audio/audio_engine.cpp
    src/audio/audio_coordinator.cpp
    src/audio/music_manager.cpp
    src/audio/footstep_manager.cpp
    src/audio/activity_sound_manager.cpp
    src/audio/mount_sound_manager.cpp
    src/audio/npc_voice_manager.cpp
    src/audio/ambient_sound_manager.cpp
    src/audio/ui_sound_manager.cpp
    src/audio/combat_sound_manager.cpp
    src/audio/spell_sound_manager.cpp
    src/audio/movement_sound_manager.cpp

    # Pipeline (asset loaders)
    src/pipeline/blp_loader.cpp
    src/pipeline/dbc_loader.cpp
    src/pipeline/asset_manager.cpp
    src/pipeline/asset_manifest.cpp
    src/pipeline/loose_file_reader.cpp
    src/pipeline/m2_loader.cpp
    src/pipeline/wmo_loader.cpp
    src/pipeline/adt_loader.cpp
    src/pipeline/wdt_loader.cpp
    src/pipeline/dbc_layout.cpp

    src/pipeline/terrain_mesh.cpp

    # Rendering (Vulkan infrastructure)
    src/rendering/vk_context.cpp
    src/rendering/vk_utils.cpp
    src/rendering/vk_shader.cpp
    src/rendering/vk_texture.cpp
    src/rendering/vk_buffer.cpp
    src/rendering/vk_pipeline.cpp
    src/rendering/vk_render_target.cpp

    # Rendering
    src/rendering/renderer.cpp
    src/rendering/amd_fsr3_runtime.cpp
    src/rendering/camera.cpp
    src/rendering/camera_controller.cpp
    src/rendering/material.cpp
    src/rendering/terrain_renderer.cpp
    src/rendering/terrain_manager.cpp
    src/rendering/frustum.cpp
    src/rendering/performance_hud.cpp
    src/rendering/water_renderer.cpp
    src/rendering/skybox.cpp
    src/rendering/celestial.cpp
    src/rendering/starfield.cpp
    src/rendering/clouds.cpp
    src/rendering/lens_flare.cpp
    src/rendering/weather.cpp
    src/rendering/lightning.cpp
    src/rendering/lighting_manager.cpp
    src/rendering/sky_system.cpp
    src/rendering/character_renderer.cpp
    src/rendering/character_preview.cpp
    src/rendering/wmo_renderer.cpp
    src/rendering/m2_renderer.cpp
    src/rendering/m2_model_classifier.cpp
    src/rendering/quest_marker_renderer.cpp
    src/rendering/minimap.cpp
    src/rendering/world_map.cpp
    src/rendering/swim_effects.cpp
    src/rendering/mount_dust.cpp
    src/rendering/levelup_effect.cpp
    src/rendering/charge_effect.cpp
    src/rendering/loading_screen.cpp

    # UI
    src/ui/ui_manager.cpp
    src/ui/auth_screen.cpp
    src/ui/realm_screen.cpp
    src/ui/character_create_screen.cpp
    src/ui/character_screen.cpp
    src/ui/game_screen.cpp
    src/ui/chat_panel.cpp
    src/ui/toast_manager.cpp
    src/ui/dialog_manager.cpp
    src/ui/settings_panel.cpp
    src/ui/combat_ui.cpp
    src/ui/social_panel.cpp
    src/ui/action_bar_panel.cpp
    src/ui/window_manager.cpp
    src/ui/inventory_screen.cpp
    src/ui/quest_log_screen.cpp
    src/ui/spellbook_screen.cpp
    src/ui/talent_screen.cpp
    src/ui/keybinding_manager.cpp

    # Addons
    src/addons/addon_manager.cpp
    src/addons/lua_engine.cpp
    src/addons/toc_parser.cpp

    # Main
    src/main.cpp
)

set(WOWEE_HEADERS
    include/core/application.hpp
    include/core/window.hpp
    include/core/input.hpp
    include/core/logger.hpp

    include/network/socket.hpp
    include/network/packet.hpp
    include/network/tcp_socket.hpp
    include/network/world_socket.hpp
    include/network/net_platform.hpp

    include/platform/process.hpp

    include/auth/auth_handler.hpp
    include/auth/auth_opcodes.hpp
    include/auth/auth_packets.hpp
    include/auth/srp.hpp
    include/auth/big_num.hpp
    include/auth/crypto.hpp

    include/game/game_handler.hpp
    include/game/world.hpp
    include/game/player.hpp
    include/game/entity.hpp
    include/game/opcodes.hpp
    include/game/zone_manager.hpp
    include/game/inventory.hpp
    include/game/spell_defines.hpp
    include/game/group_defines.hpp
    include/game/world_packets.hpp
    include/game/character.hpp

    include/audio/audio_engine.hpp
    include/audio/music_manager.hpp
    include/audio/footstep_manager.hpp
    include/audio/activity_sound_manager.hpp
    include/audio/mount_sound_manager.hpp
    include/audio/npc_voice_manager.hpp
    include/audio/ambient_sound_manager.hpp
    include/audio/ui_sound_manager.hpp
    include/audio/combat_sound_manager.hpp
    include/audio/spell_sound_manager.hpp
    include/audio/movement_sound_manager.hpp

    include/pipeline/blp_loader.hpp
    include/pipeline/asset_manifest.hpp
    include/pipeline/loose_file_reader.hpp
    include/pipeline/m2_loader.hpp
    include/pipeline/wmo_loader.hpp
    include/pipeline/adt_loader.hpp
    include/pipeline/wdt_loader.hpp
    include/pipeline/dbc_loader.hpp
    include/pipeline/terrain_mesh.hpp

    include/rendering/vk_context.hpp
    include/rendering/vk_utils.hpp
    include/rendering/vk_shader.hpp
    include/rendering/vk_texture.hpp
    include/rendering/vk_buffer.hpp
    include/rendering/vk_pipeline.hpp
    include/rendering/vk_render_target.hpp
    include/rendering/renderer.hpp
    include/rendering/camera.hpp
    include/rendering/camera_controller.hpp
    include/rendering/material.hpp
    include/rendering/terrain_renderer.hpp
    include/rendering/terrain_manager.hpp
    include/rendering/frustum.hpp
    include/rendering/performance_hud.hpp
    include/rendering/water_renderer.hpp
    include/rendering/skybox.hpp
    include/rendering/celestial.hpp
    include/rendering/starfield.hpp
    include/rendering/clouds.hpp
    include/rendering/lens_flare.hpp
    include/rendering/weather.hpp
    include/rendering/lightning.hpp
    include/rendering/swim_effects.hpp
    include/rendering/world_map.hpp
    include/rendering/character_renderer.hpp
    include/rendering/character_preview.hpp
    include/rendering/wmo_renderer.hpp
    include/rendering/loading_screen.hpp

    include/ui/ui_manager.hpp
    include/ui/auth_screen.hpp
    include/ui/realm_screen.hpp
    include/ui/character_create_screen.hpp
    include/ui/character_screen.hpp
    include/ui/game_screen.hpp
    include/ui/inventory_screen.hpp
    include/ui/spellbook_screen.hpp
    include/ui/talent_screen.hpp
    include/ui/keybinding_manager.hpp
)

set(WOWEE_PLATFORM_SOURCES)
if(WIN32)
    # Copy icon into build tree so windres can find it via the relative path
    # in wowee.rc ("assets\\wowee.ico").  Tell the RC compiler to also search
    # the build directory — GNU windres uses cwd (already the build dir) but
    # llvm-windres resolves relative to the .rc file, so it needs the hint.
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/Wowee.ico
        ${CMAKE_CURRENT_BINARY_DIR}/assets/wowee.ico
        COPYONLY
    )
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I ${CMAKE_CURRENT_BINARY_DIR} -I ${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND WOWEE_PLATFORM_SOURCES resources/wowee.rc)
endif()

# ---- Lua 5.1.5 (vendored, static library) ----
set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/lua-5.1.5/src)
set(LUA_SOURCES
    ${LUA_DIR}/lapi.c    ${LUA_DIR}/lcode.c   ${LUA_DIR}/ldebug.c
    ${LUA_DIR}/ldo.c     ${LUA_DIR}/ldump.c   ${LUA_DIR}/lfunc.c
    ${LUA_DIR}/lgc.c     ${LUA_DIR}/llex.c    ${LUA_DIR}/lmem.c
    ${LUA_DIR}/lobject.c ${LUA_DIR}/lopcodes.c ${LUA_DIR}/lparser.c
    ${LUA_DIR}/lstate.c  ${LUA_DIR}/lstring.c ${LUA_DIR}/ltable.c
    ${LUA_DIR}/ltm.c     ${LUA_DIR}/lundump.c ${LUA_DIR}/lvm.c
    ${LUA_DIR}/lzio.c    ${LUA_DIR}/lauxlib.c ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/ldblib.c  ${LUA_DIR}/liolib.c  ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/loslib.c  ${LUA_DIR}/ltablib.c ${LUA_DIR}/lstrlib.c
    ${LUA_DIR}/linit.c
)
add_library(lua51 STATIC ${LUA_SOURCES})
set_target_properties(lua51 PROPERTIES LINKER_LANGUAGE C C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
target_include_directories(lua51 PUBLIC ${LUA_DIR})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lua51 PRIVATE -w)
endif()

# Create executable
add_executable(wowee ${WOWEE_SOURCES} ${WOWEE_HEADERS} ${WOWEE_PLATFORM_SOURCES})
if(TARGET opcodes-generate)
    add_dependencies(wowee opcodes-generate)
endif()
# macOS cross-compilation: MoltenVK is not available at link time.
# Allow unresolved Vulkan symbols — resolved at runtime. Scoped to wowee only.
if(WOWEE_MACOS_CROSS_COMPILE)
    target_link_options(wowee PRIVATE "-undefined" "dynamic_lookup")
endif()

# FidelityFX-SDK headers can trigger compiler-specific pragma/unused-static noise
# when included through the runtime bridge; keep suppression scoped to that TU.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/rendering/amd_fsr3_runtime.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-unknown-pragmas;-Wno-unused-variable"
    )
endif()

# Compile GLSL shaders to SPIR-V
if(GLSLC)
    compile_shaders(wowee)
endif()

# Include directories
target_include_directories(wowee PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# Vendored headers as SYSTEM to suppress third-party warnings
target_include_directories(wowee SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/vk-bootstrap/src
)
if(HAVE_FFMPEG)
    target_include_directories(wowee PRIVATE ${FFMPEG_INCLUDE_DIRS})
endif()

# Link libraries
target_link_libraries(wowee PRIVATE
    SDL2::SDL2
    Vulkan::Vulkan
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ZLIB::ZLIB
    lua51
    ${CMAKE_DL_LIBS}
)

if(HAVE_FFMPEG)
    target_compile_definitions(wowee PRIVATE HAVE_FFMPEG)
    target_link_libraries(wowee PRIVATE ${FFMPEG_LIBRARIES})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(wowee PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
endif()

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(wowee PRIVATE X11)
endif()
if(WIN32)
    target_link_libraries(wowee PRIVATE ws2_32)
    # SDL2main provides WinMain entry point on Windows
    if(TARGET SDL2::SDL2main)
        target_link_libraries(wowee PRIVATE SDL2::SDL2main)
    endif()
endif()

# Link ImGui if available
if(TARGET imgui)
    target_link_libraries(wowee PRIVATE imgui)
endif()

# Link vk-bootstrap
if(TARGET vk-bootstrap)
    target_link_libraries(wowee PRIVATE vk-bootstrap)
endif()

if(TARGET wowee_fsr2_amd_vk)
    target_link_libraries(wowee PRIVATE wowee_fsr2_amd_vk)
endif()
if(TARGET wowee_fsr3_framegen_amd_vk_probe)
    target_link_libraries(wowee PRIVATE wowee_fsr3_framegen_amd_vk_probe)
endif()
if(TARGET wowee_fsr3_official_runtime_copy)
    # FSR3 Path A runtime is an opt-in artifact; build explicitly with:
    #   cmake --build build --target wowee_fsr3_official_runtime_copy
    # Do NOT add as a hard dependency of wowee — it would break arm64 and Windows CI
    # (no DXC available on arm64; bash context issues on MSYS2 Windows).
endif()

# Link Unicorn if available
if(HAVE_UNICORN)
    target_link_libraries(wowee PRIVATE ${UNICORN_LIBRARY})
    target_include_directories(wowee PRIVATE ${UNICORN_INCLUDE_DIR})
    target_compile_definitions(wowee PRIVATE HAVE_UNICORN)
endif()

# Link GLM if found
if(TARGET glm::glm)
    target_link_libraries(wowee PRIVATE glm::glm)
elseif(glm_FOUND)
    target_include_directories(wowee PRIVATE ${GLM_INCLUDE_DIRS})
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(wowee PRIVATE /W4)
else()
    target_compile_options(wowee PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    # GCC LTO emits -Wodr for FFX enum-name mismatches across SDK generations.
    # We intentionally keep FSR2+FSR3 integrations in separate TUs and suppress
    # this linker-time diagnostic to avoid CI noise.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(wowee PRIVATE -Wno-odr)
        target_link_options(wowee PRIVATE -Wno-odr)
    endif()
endif()

# Debug build flags
if(MSVC)
    # /ZI  — Edit-and-Continue debug info (works with hot-reload in VS 2022)
    # /RTC1 — stack-frame and uninitialised-variable runtime checks (Debug only)
    # /sdl  — additional SDL security checks
    # /Od   — disable optimisation so stepping matches source lines exactly
    target_compile_options(wowee PRIVATE
        $<$<CONFIG:Debug>:/ZI /RTC1 /sdl /Od>
    )
    # Ensure the linker emits a .pdb alongside the .exe for every config
    target_link_options(wowee PRIVATE
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<CONFIG:RelWithDebInfo>:/DEBUG:FASTLINK>
    )
else()
    # -g3          — maximum DWARF debug info (includes macro definitions)
    # -Og          — optimise for debugging (better than -O0, keeps most frames)
    # -fno-omit-frame-pointer — preserve frame pointers so stack traces are clean
    target_compile_options(wowee PRIVATE
        $<$<CONFIG:Debug>:-g3 -Og -fno-omit-frame-pointer>
        $<$<CONFIG:RelWithDebInfo>:-g -fno-omit-frame-pointer>
    )
endif()

# AddressSanitizer — catch buffer overflows, use-after-free, etc.
# Enable with:  cmake ... -DWOWEE_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
if(WOWEE_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(wowee PRIVATE /fsanitize=address)
        # ASAN on MSVC requires the dynamic CRT (/MD or /MDd)
        target_compile_options(wowee PRIVATE
            $<$<CONFIG:Debug>:/MDd>
            $<$<CONFIG:Release>:/MD>
        )
    else()
        target_compile_options(wowee PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(wowee PRIVATE -fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer: ENABLED")
endif()

# Release build optimizations
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
if(_ipo_supported)
    set_property(TARGET wowee PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
if(NOT MSVC)
    # -O3: more aggressive inlining and auto-vectorization vs CMake's default -O2
    target_compile_options(wowee PRIVATE $<$<CONFIG:Release>:-O3>)
    # -fvisibility=hidden: keeps all symbols internal by default, shrinks binary
    # and gives the linker and optimizer more freedom to dead-strip and inline
    target_compile_options(wowee PRIVATE $<$<CONFIG:Release>:-fvisibility=hidden -fvisibility-inlines-hidden>)
endif()

# Copy assets next to the executable (runs every build, not just configure).
# Uses $<TARGET_FILE_DIR:wowee> so MSVC multi-config generators place assets
# in bin/Debug/ or bin/Release/ alongside the exe, not the common bin/ parent.
add_custom_command(TARGET wowee POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:wowee>/assets
    COMMENT "Syncing assets to $<TARGET_FILE_DIR:wowee>/assets"
)

# Symlink Data/ next to the executable so expansion profiles, opcode tables,
# and other runtime data files are found when running from the build directory.
if(NOT WIN32)
    add_custom_command(TARGET wowee POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/Data
            $<TARGET_FILE_DIR:wowee>/Data
        COMMENT "Symlinking Data to $<TARGET_FILE_DIR:wowee>/Data"
    )
endif()

# On Windows, SDL 2.28+ uses LoadLibraryExW with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
# which does NOT include System32. Copy vulkan-1.dll into the output directory so
# SDL_Vulkan_LoadLibrary can locate it without needing a full system PATH search.
if(WIN32)
    find_file(VULKAN_DLL vulkan-1.dll
        PATHS "$ENV{SystemRoot}/System32" "$ENV{VULKAN_SDK}/Bin"
        NO_DEFAULT_PATH)
    if(VULKAN_DLL)
        add_custom_command(TARGET wowee POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VULKAN_DLL}" "$<TARGET_FILE_DIR:wowee>/vulkan-1.dll"
            COMMENT "Copying vulkan-1.dll to output directory"
        )
    else()
        message(STATUS "  vulkan-1.dll not found — skipping copy (MSYS2 provides it via PATH)")
    endif()
endif()

# Install targets
install(TARGETS wowee
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Note: tool install rules are placed next to each target definition below.

# Install built-in assets (exclude proprietary music)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets
    DESTINATION bin
    PATTERN "Original Music" EXCLUDE)

# On Windows, install any DLLs that were bundled into the build output dir
# (populated by the CI workflow's DLL-bundling step before cpack runs)
if(WIN32)
    install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll")
endif()

# Linux desktop integration (launcher + icon)
if(UNIX AND NOT APPLE)
    set(WOWEE_LINUX_ICON_PATH "${CMAKE_INSTALL_FULL_DATAROOTDIR}/icons/hicolor/256x256/apps/wowee.png")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/wowee.desktop.in
        ${CMAKE_CURRENT_BINARY_DIR}/wowee.desktop
        @ONLY
    )

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wowee.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/assets/Wowee.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME wowee.png)
endif()

# ---- Tool: asset_extract (MPQ → loose files) ----
if(STORMLIB_LIBRARY AND STORMLIB_INCLUDE_DIR)
    add_executable(asset_extract
        tools/asset_extract/main.cpp
        tools/asset_extract/extractor.cpp
        tools/asset_extract/path_mapper.cpp
        tools/asset_extract/manifest_writer.cpp
        src/pipeline/dbc_loader.cpp
        src/core/logger.cpp
    )
    target_include_directories(asset_extract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset_extract
        ${STORMLIB_INCLUDE_DIR}
    )
    target_link_libraries(asset_extract PRIVATE
        ${STORMLIB_LIBRARY}
        ZLIB::ZLIB
        Threads::Threads
    )
    if(WIN32)
        find_library(WININET_LIB wininet)
        find_library(BZ2_LIB bz2)
        if(WININET_LIB)
            target_link_libraries(asset_extract PRIVATE ${WININET_LIB})
        endif()
        if(BZ2_LIB)
            target_link_libraries(asset_extract PRIVATE ${BZ2_LIB})
        endif()
    endif()
    set_target_properties(asset_extract PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    install(TARGETS asset_extract RUNTIME DESTINATION bin)
    message(STATUS "  asset_extract tool: ENABLED")
else()
    message(STATUS "  asset_extract tool: DISABLED (requires StormLib)")
endif()

# ---- Tool: dbc_to_csv (DBC → CSV text) ----
add_executable(dbc_to_csv
    tools/dbc_to_csv/main.cpp
    src/pipeline/dbc_loader.cpp
    src/core/logger.cpp
)
target_include_directories(dbc_to_csv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dbc_to_csv PRIVATE Threads::Threads)
set_target_properties(dbc_to_csv PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
install(TARGETS dbc_to_csv RUNTIME DESTINATION bin)

# ---- Tool: auth_probe (LOGON_CHALLENGE probe) ----
add_executable(auth_probe
    tools/auth_probe/main.cpp
    src/auth/auth_packets.cpp
    src/auth/auth_opcodes.cpp
    src/auth/crypto.cpp
    src/network/packet.cpp
    src/network/socket.cpp
    src/network/tcp_socket.cpp
    src/core/logger.cpp
)
target_include_directories(auth_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(auth_probe PRIVATE Threads::Threads OpenSSL::Crypto
    $<$<BOOL:${WIN32}>:ws2_32>)
set_target_properties(auth_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
install(TARGETS auth_probe RUNTIME DESTINATION bin)

# ---- Tool: auth_login_probe (challenge + proof probe) ----
add_executable(auth_login_probe
    tools/auth_login_probe/main.cpp
    src/auth/auth_packets.cpp
    src/auth/auth_opcodes.cpp
    src/auth/crypto.cpp
    src/auth/integrity.cpp
    src/auth/big_num.cpp
    src/auth/srp.cpp
    src/network/packet.cpp
    src/network/socket.cpp
    src/network/tcp_socket.cpp
    src/core/logger.cpp
)
target_include_directories(auth_login_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(auth_login_probe PRIVATE Threads::Threads OpenSSL::Crypto
    $<$<BOOL:${WIN32}>:ws2_32>)
set_target_properties(auth_login_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
install(TARGETS auth_login_probe RUNTIME DESTINATION bin)

# ---- Tool: blp_convert (BLP ↔ PNG) ----
add_executable(blp_convert
    tools/blp_convert/main.cpp
    src/pipeline/blp_loader.cpp
    src/core/logger.cpp
)
target_include_directories(blp_convert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern
)
target_link_libraries(blp_convert PRIVATE Threads::Threads)
set_target_properties(blp_convert PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
install(TARGETS blp_convert RUNTIME DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "Wowee Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  SDL2: ${SDL2_VERSION}")
message(STATUS "  OpenSSL: ${OPENSSL_VERSION}")
message(STATUS "  ImGui: ${IMGUI_DIR}")
message(STATUS "  ASAN: ${WOWEE_ENABLE_ASAN}")
message(STATUS "")

# ---- CPack packaging ----
set(CPACK_PACKAGE_NAME "wowee")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "World of Warcraft client emulator")
set(CPACK_PACKAGE_VENDOR "Wowee")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Wowee")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "Wowee")
    set(CPACK_NSIS_PACKAGE_NAME "Wowee")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    # Run wowee from bin/ so that ./assets/ resolves correctly.
    # SetOutPath sets the shortcut's working directory in NSIS.
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "SetOutPath '$INSTDIR\\\\bin'\nCreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Wowee.lnk' '$INSTDIR\\\\bin\\\\wowee.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Wowee.lnk'")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    # Linux — generate postinst/prerm wrapper scripts
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/packaging)
    # postinst: write a wrapper script at /usr/local/bin/wowee that cd's to
    # the install dir so ./assets/ resolves correctly.
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/packaging/postinst
[[#!/bin/sh
cat > /usr/local/bin/wowee << 'WOWEE_WRAPPER'
#!/bin/sh
cd /opt/wowee/bin
exec ./wowee "$@"
WOWEE_WRAPPER
chmod +x /usr/local/bin/wowee
]])
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/packaging/prerm
"#!/bin/sh\nrm -f /usr/local/bin/wowee\n")
    file(CHMOD
        ${CMAKE_CURRENT_BINARY_DIR}/packaging/postinst
        ${CMAKE_CURRENT_BINARY_DIR}/packaging/prerm
        PERMISSIONS
            OWNER_EXECUTE OWNER_WRITE OWNER_READ
            GROUP_EXECUTE GROUP_READ
            WORLD_EXECUTE WORLD_READ)

    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/wowee")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Wowee")
    set(CPACK_DEBIAN_PACKAGE_SECTION "games")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libsdl2-2.0-0, libvulkan1, libssl3, zlib1g")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/postinst;${CMAKE_CURRENT_BINARY_DIR}/packaging/prerm")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
        set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
    endif()
endif()

include(CPack)
