cmake_minimum_required(VERSION 3.16)

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Elektrokombinacija

project(
    ekk_runtime
    VERSION 0.1.0
    DESCRIPTION "ekk coordination runtime"
    LANGUAGES C
)

option(EKX_BUILD_EXAMPLES "Build standalone examples" ON)
option(EKX_BUILD_TESTS "Build test executable" ON)

add_library(ekx_core STATIC
    src/ekk_types.c
    src/ekk_field.c
    src/ekk_topology.c
    src/ekk_heartbeat.c
    src/ekk_consensus.c
    src/ekk_module.c
    src/ekk_init.c
    src/hal/ekk_hal_posix.c
)

target_include_directories(ekx_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ekx_core PUBLIC c_std_99)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ekx_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ekx_core PUBLIC Threads::Threads)

if(EKX_BUILD_EXAMPLES)
    add_executable(ekx_basic examples/basic.c)
    target_link_libraries(ekx_basic PRIVATE ekx_core)

    add_executable(ekx_multi_module examples/multi_module.c)
    target_link_libraries(ekx_multi_module PRIVATE ekx_core)
endif()

if(EKX_BUILD_TESTS)
    enable_testing()

    add_executable(ekx_tests tests/test_main.c)
    target_link_libraries(ekx_tests PRIVATE ekx_core)

    add_test(NAME ekx_tests COMMAND ekx_tests)
endif()
