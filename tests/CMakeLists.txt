# Phase 0: Unit test infrastructure using Catch2 v3 (amalgamated)

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
)
target_include_directories(test_entity PRIVATE ${TEST_INCLUDE_DIRS})
target_include_directories(test_entity SYSTEM PRIVATE ${TEST_SYSTEM_INCLUDE_DIRS})
target_link_libraries(test_entity PRIVATE catch2_main)
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
