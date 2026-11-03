cmake_minimum_required(VERSION 3.20)
project(DirSize VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: users do not need the VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
add_compile_options(/utf-8 /W4 /permissive-)

# AddressSanitizer build for tests: cmake -DDIRSIZE_ASAN=ON (use the Release
# config; Debug adds /RTC1, which ASan does not support). With /MT the exe
# needs clang_rt.asan_dynamic-x86_64.dll from the MSVC tools bin folder.
option(DIRSIZE_ASAN "Build with /fsanitize=address" OFF)
if(DIRSIZE_ASAN)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
endif()

add_library(dirsize_core STATIC
    src/core/path_util.cpp
    src/core/dir_walker.cpp
    src/core/dir_listing.cpp
    src/core/index.cpp
    src/core/layout_parser.cpp
    src/core/layout_index_builder.cpp
    src/core/walk_index.cpp
    src/core/ntfs_index.cpp
)
target_include_directories(dirsize_core PUBLIC src)

add_executable(dirsize-cli src/cli/main.cpp)
target_link_libraries(dirsize-cli PRIVATE dirsize_core psapi)

# Phase 2 feasibility probe (measurement only, not shipped).
add_executable(layout-probe tools/layout_probe.cpp)
target_link_libraries(layout-probe PRIVATE dirsize_core psapi)

# Unit tests (no elevation needed): ctest -C Release
enable_testing()
add_executable(core-tests tests/core_tests.cpp)
target_link_libraries(core-tests PRIVATE dirsize_core)
add_test(NAME core-tests COMMAND core-tests)
