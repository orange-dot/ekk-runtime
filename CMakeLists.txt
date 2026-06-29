cmake_minimum_required(VERSION 3.16)

# SPDX-License-Identifier: MIT
# Copyright (c) 2026 mamut-studio.com

project(
    ekk_runtime
    VERSION 0.1.0
    DESCRIPTION "ekk coordination runtime"
    LANGUAGES C
)

option(EKX_BUILD_EXAMPLES "Build standalone examples" ON)
option(EKX_BUILD_TESTS "Build test executable" ON)

find_package(Threads REQUIRED)

set(EKX_CORE_SOURCES
    src/ekk_types.c
    src/ekk_field.c
    src/ekk_topology.c
    src/ekk_heartbeat.c
    src/ekk_consensus.c
    src/ekk_module.c
    src/ekk_init.c
    src/hal/ekk_hal_posix.c
)

function(ekx_add_core target_name)
    add_library(${target_name} STATIC ${EKX_CORE_SOURCES})

    target_include_directories(${target_name}
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_features(${target_name} PUBLIC c_std_99)

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(${target_name} PUBLIC Threads::Threads)
endfunction()

ekx_add_core(ekx_core)

ekx_add_core(ekx_core_ek3)
target_compile_definitions(ekx_core_ek3 PUBLIC EKK_MAX_MODULES=96)

if(EKX_BUILD_EXAMPLES)
    add_executable(ekx_basic examples/basic.c)
    target_link_libraries(ekx_basic PRIVATE ekx_core)

    add_executable(ekx_multi_module examples/multi_module.c)
    target_link_libraries(ekx_multi_module PRIVATE ekx_core)

    add_executable(ekx_ek3_rack examples/ek3_rack.c)
    target_link_libraries(ekx_ek3_rack PRIVATE ekx_core_ek3)
endif()

if(EKX_BUILD_TESTS)
    enable_testing()

    add_executable(ekx_tests tests/test_main.c)
    target_link_libraries(ekx_tests PRIVATE ekx_core)

    add_test(NAME ekx_tests COMMAND ekx_tests)

    if(EKX_BUILD_EXAMPLES)
        add_test(NAME ekx_ek3_rack_7 COMMAND ekx_ek3_rack --modules 7)
        add_test(NAME ekx_ek3_rack_84 COMMAND ekx_ek3_rack --modules 84)
    endif()
endif()
