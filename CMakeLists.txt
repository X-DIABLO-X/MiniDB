# =============================================================================
# MiniDB - Top-level CMake Build
# -----------------------------------------------------------------------------
# This CMakeLists.txt is the single entry point for building MiniDB.
# It produces a static library `minidb_core` that every other target links
# against, plus an `minidb` CLI executable and one binary per benchmark/test.
#
# Build (from project root):
#   cmake -S . -B build
#   cmake --build build --config Release -j
#
# Run the CLI:
#   ./build/bin/minidb                       (Linux/macOS)
#   ./build/bin/Release/minidb.exe           (MSVC)
#   ./build/bin/minidb.exe                   (MinGW, single config)
#
# Run tests:
#   ctest --test-dir build --output-on-failure
#
# Why one library + many binaries?
# - Each team member works on a different module. By exposing all modules as
#   a single static library, anyone can `target_link_libraries(my_target
#   minidb_core)` and pull in every other module's stubs without waiting for
#   them to be implemented. Modules are still logically separated by folder
#   and header namespace; the library just removes the build-order coupling.
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(minidb
    VERSION     0.1.0
    DESCRIPTION "MiniDB - a lightweight relational database (Advanced DBMS capstone)"
    LANGUAGES   CXX
)

# -----------------------------------------------------------------------------
# Global compile settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Warnings: treat as warnings, not errors, so partial WIP code still builds.
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
# Collect sources by glob so the tree stays "add a file, get it built".
# We exclude benchmarks/tests from the main library glob on purpose.
# -----------------------------------------------------------------------------
file(GLOB_RECURSE MINIDB_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.hpp"
)

# Headers are not compiled but listing them in the target makes IDEs happy.
add_library(minidb_core STATIC ${MINIDB_CORE_SOURCES})
target_include_directories(minidb_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(minidb_core PUBLIC
    MINIDB_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MINIDB_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MINIDB_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# -----------------------------------------------------------------------------
# CLI executable - a thin REPL/shell that wires every module together.
# -----------------------------------------------------------------------------
add_executable(minidb src/cli/main.cpp)
target_link_libraries(minidb PRIVATE minidb_core)

# -----------------------------------------------------------------------------
# Tests - one binary per test folder so failures are localised.
# -----------------------------------------------------------------------------
enable_testing()

set(MINIDB_TEST_DIRS
    storage
    index
    parser
    executor
    transaction
    recovery
)

foreach(test_dir IN LISTS MINIDB_TEST_DIRS)
    file(GLOB test_sources "${CMAKE_SOURCE_DIR}/tests/${test_dir}/*.cpp")
    if(test_sources)
        add_executable(test_${test_dir} ${test_sources})
        target_link_libraries(test_${test_dir} PRIVATE minidb_core)
        add_test(NAME ${test_dir} COMMAND test_${test_dir})
    endif()
endforeach()

# -----------------------------------------------------------------------------
# Benchmarks - intentionally NOT in ctest (they are slow, not pass/fail).
# -----------------------------------------------------------------------------
file(GLOB benchmark_sources "${CMAKE_SOURCE_DIR}/benchmark/*.cpp")
foreach(bench_src IN LISTS benchmark_sources)
    get_filename_component(bench_name ${bench_src} NAME_WE)
    add_executable(bench_${bench_name} ${bench_src})
    target_link_libraries(bench_${bench_name} PRIVATE minidb_core)
    set_target_properties(bench_${bench_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/benchmarks"
    )
endforeach()

# -----------------------------------------------------------------------------
# Install layout (optional, but useful for distribution).
# -----------------------------------------------------------------------------
install(TARGETS minidb RUNTIME DESTINATION bin)
install(DIRECTORY data/ DESTINATION data)
