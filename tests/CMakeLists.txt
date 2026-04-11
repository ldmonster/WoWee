# Unit test infrastructure using Catch2 v3 (amalgamated)

# Catch2 amalgamated as a library target
add_library(catch2_main STATIC
    ${CMAKE_SOURCE_DIR}/extern/catch2/catch_amalgamated.cpp
)
target_include_directories(catch2_main PUBLIC
    ${CMAKE_SOURCE_DIR}/extern/catch2
)
# Catch2 v3 needs C++17 minimum
target_compile_features(catch2_main PUBLIC cxx_std_17)

# ── ASAN / UBSan propagation ────────────────────────────────
# Collect all test target names so we can apply sanitizer flags at the end.
set(ALL_TEST_TARGETS "")

# Helper: register a test target for ASAN/UBSan if enabled.
macro(register_test_target _target)
    list(APPEND ALL_TEST_TARGETS ${_target})
endmacro()

# Shared source files used across multiple tests
set(TEST_COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/logger.cpp
)

# Include directories matching the main target
set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
set(TEST_SYSTEM_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/extern
)

# ── test_packet ──────────────────────────────────────────────
add_executable(test_packet
    test_packet.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/network/packet.cpp
)
target_include_directories(test_packet PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_packet SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_packet PRIVATE catch2_main)
add_test(NAME packet COMMAND test_packet)
register_test_target(test_packet)

# ── test_srp ─────────────────────────────────────────────────
add_executable(test_srp
    test_srp.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/auth/srp.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/big_num.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/crypto.cpp
)
target_include_directories(test_srp PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_srp SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_srp PRIVATE catch2_main OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME srp COMMAND test_srp)
register_test_target(test_srp)

# ── test_spline ──────────────────────────────────────────────
add_executable(test_spline
    test_spline.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
)
target_include_directories(test_spline PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_spline SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_spline PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_spline PRIVATE glm::glm)
endif()
add_test(NAME spline COMMAND test_spline)
register_test_target(test_spline)

# ── test_transport_path_repo ─────────────────────────────────
add_executable(test_transport_path_repo
    test_transport_path_repo.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/transport_path_repository.cpp
    ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
)
target_include_directories(test_transport_path_repo PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_transport_path_repo SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_transport_path_repo PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_transport_path_repo PRIVATE glm::glm)
endif()
add_test(NAME transport_path_repo COMMAND test_transport_path_repo)
register_test_target(test_transport_path_repo)

# ── test_opcode_table ────────────────────────────────────────
add_executable(test_opcode_table
    test_opcode_table.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/opcode_table.cpp
)
target_include_directories(test_opcode_table PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_opcode_table SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_opcode_table PRIVATE catch2_main)
add_test(NAME opcode_table COMMAND test_opcode_table)
register_test_target(test_opcode_table)

# ── test_entity ──────────────────────────────────────────────
add_executable(test_entity
    test_entity.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/entity.cpp
    ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
)
target_include_directories(test_entity PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_entity SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_entity PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_entity PRIVATE glm::glm)
endif()
add_test(NAME entity COMMAND test_entity)
register_test_target(test_entity)

# ── test_dbc_loader ──────────────────────────────────────────
add_executable(test_dbc_loader
    test_dbc_loader.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
)
target_include_directories(test_dbc_loader PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_dbc_loader SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_dbc_loader PRIVATE catch2_main)
add_test(NAME dbc_loader COMMAND test_dbc_loader)
register_test_target(test_dbc_loader)

# ── test_m2_structs ──────────────────────────────────────────
# Header-only struct layout tests — no source files needed
add_executable(test_m2_structs
    test_m2_structs.cpp
)
target_include_directories(test_m2_structs PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_m2_structs SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_m2_structs PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_m2_structs PRIVATE glm::glm)
endif()
add_test(NAME m2_structs COMMAND test_m2_structs)
register_test_target(test_m2_structs)

# ── test_blp_loader ──────────────────────────────────────────
add_executable(test_blp_loader
    test_blp_loader.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/pipeline/blp_loader.cpp
)
target_include_directories(test_blp_loader PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_blp_loader SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_blp_loader PRIVATE catch2_main)
add_test(NAME blp_loader COMMAND test_blp_loader)
register_test_target(test_blp_loader)

# ── test_frustum ─────────────────────────────────────────────
add_executable(test_frustum
    test_frustum.cpp
    ${CMAKE_SOURCE_DIR}/src/rendering/frustum.cpp
)
target_include_directories(test_frustum PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_frustum SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_frustum PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_frustum PRIVATE glm::glm)
endif()
add_test(NAME frustum COMMAND test_frustum)
register_test_target(test_frustum)

# ── test_animation_ids ───────────────────────────────────────
add_executable(test_animation_ids
    test_animation_ids.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/animation_ids.cpp
    ${CMAKE_SOURCE_DIR}/src/pipeline/dbc_loader.cpp
)
target_include_directories(test_animation_ids PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_animation_ids SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_animation_ids PRIVATE catch2_main)
add_test(NAME animation_ids COMMAND test_animation_ids)
register_test_target(test_animation_ids)

# ── test_locomotion_fsm ──────────────────────────────────────
add_executable(test_locomotion_fsm
    test_locomotion_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/locomotion_fsm.cpp
)
target_include_directories(test_locomotion_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_locomotion_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_locomotion_fsm PRIVATE catch2_main)
add_test(NAME locomotion_fsm COMMAND test_locomotion_fsm)
register_test_target(test_locomotion_fsm)

# ── test_combat_fsm ──────────────────────────────────────────
add_executable(test_combat_fsm
    test_combat_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/combat_fsm.cpp
)
target_include_directories(test_combat_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_combat_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_combat_fsm PRIVATE catch2_main)
add_test(NAME combat_fsm COMMAND test_combat_fsm)
register_test_target(test_combat_fsm)

# ── test_activity_fsm ────────────────────────────────────────
add_executable(test_activity_fsm
    test_activity_fsm.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/rendering/animation/activity_fsm.cpp
)
target_include_directories(test_activity_fsm PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_activity_fsm SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_activity_fsm PRIVATE catch2_main)
add_test(NAME activity_fsm COMMAND test_activity_fsm)
register_test_target(test_activity_fsm)

# NPC animator tests removed — NpcAnimator replaced by generic CharacterAnimator

# ── test_anim_capability ─────────────────────────────────────
# Header-only struct tests — no source files needed
add_executable(test_anim_capability
    test_anim_capability.cpp
)
target_include_directories(test_anim_capability PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_anim_capability SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_anim_capability PRIVATE catch2_main)
add_test(NAME anim_capability COMMAND test_anim_capability)
register_test_target(test_anim_capability)

# ── test_indoor_shadows ──────────────────────────────────────
add_executable(test_indoor_shadows
    test_indoor_shadows.cpp
)
target_include_directories(test_indoor_shadows PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_indoor_shadows SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_indoor_shadows PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_indoor_shadows PRIVATE glm::glm)
endif()
add_test(NAME indoor_shadows COMMAND test_indoor_shadows)
register_test_target(test_indoor_shadows)

# ── test_transport_components ────────────────────────────────
add_executable(test_transport_components
    test_transport_components.cpp
    ${TEST_COMMON_SOURCES}
    ${CMAKE_SOURCE_DIR}/src/game/transport_clock_sync.cpp
    ${CMAKE_SOURCE_DIR}/src/game/transport_animator.cpp
    ${CMAKE_SOURCE_DIR}/src/math/spline.cpp
)
target_include_directories(test_transport_components PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_transport_components SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_transport_components PRIVATE catch2_main)
if(TARGET glm::glm)
    target_link_libraries(test_transport_components PRIVATE glm::glm)
endif()
add_test(NAME transport_components COMMAND test_transport_components)
register_test_target(test_transport_components)

# ── ASAN / UBSan for test targets ────────────────────────────
if(WOWEE_ENABLE_ASAN AND NOT MSVC)
    foreach(_t IN LISTS ALL_TEST_TARGETS)
        target_compile_options(${_t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
    endforeach()
    # catch2_main must also be compiled with the same flags
    target_compile_options(catch2_main PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(catch2_main PRIVATE -fsanitize=address,undefined)
    message(STATUS "Test targets: ASAN + UBSan ENABLED")
endif()
