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
    src/core/live_index.cpp
    src/core/usn_parser.cpp
    src/core/ntfs_live.cpp
    src/core/crc32.cpp
    src/core/index_cache.cpp
    src/core/cache_store.cpp
)
target_include_directories(dirsize_core PUBLIC src)
target_link_libraries(dirsize_core PUBLIC shell32 ole32 advapi32)

# Phase 4 service components.
add_library(dirsize_service STATIC
    src/service/protocol.cpp
    src/service/access.cpp
    src/service/volume_worker.cpp
    src/service/pipe_server.cpp
    src/service/service_core.cpp
    src/client/pipe_client.cpp
)
target_include_directories(dirsize_service PUBLIC src)
target_link_libraries(dirsize_service PUBLIC dirsize_core)

# Phase 5 front end: pure logic (unit-tested by ui-tests).
add_library(dirsize_ui_core STATIC
    src/ui/format.cpp
    src/ui/list_model.cpp
    src/ui/palette.cpp
    src/ui/placement.cpp
    src/ui/panel_rules.cpp
)
target_include_directories(dirsize_ui_core PUBLIC src)

# Phase 5 front end: window pieces (theme needs C++/WinRT: WindowsApp.lib).
add_library(dirsize_ui STATIC
    src/ui/strings.cpp
    src/ui/theme.cpp
    src/ui/size_list.cpp
    src/ui/folder_source.cpp
    src/ui/explorer_tracker.cpp
)
target_link_libraries(dirsize_ui PUBLIC dirsize_ui_core dirsize_service WindowsApp dwmapi ole32 oleaut32 uuid shell32)

# The front end executable (Windows subsystem).
add_executable(dirsize WIN32 src/app/main.cpp src/app/panel.cpp src/app/dirsize.rc src/app/dirsize.manifest)
target_link_libraries(dirsize PRIVATE dirsize_ui)

add_executable(dirsize-cli src/cli/main.cpp)
target_link_libraries(dirsize-cli PRIVATE dirsize_service psapi)

# The Windows service (docs/DESIGN-phase4.md).
add_executable(dirsize-service src/service/service_main.cpp)
target_link_libraries(dirsize-service PRIVATE dirsize_service)

# Phase 2 feasibility probe (measurement only, not shipped).
add_executable(layout-probe tools/layout_probe.cpp)
target_link_libraries(layout-probe PRIVATE dirsize_core psapi)

# Phase 3 feasibility probe (measurement only, not shipped).
add_executable(usn-probe tools/usn_probe.cpp)
target_link_libraries(usn-probe PRIVATE dirsize_core)

# Phase 4 feasibility probe (measurement only, not shipped).
add_executable(service-probe tools/service_probe.cpp)
target_link_libraries(service-probe PRIVATE dirsize_core)

# Phase 5 feasibility probe (measurement only, not shipped).
add_executable(explorer-probe tools/explorer_probe.cpp)
target_link_libraries(explorer-probe PRIVATE ole32 oleaut32 uuid shell32 dwmapi)

# Unit tests (no elevation needed): ctest -C Release
enable_testing()
add_executable(core-tests tests/core_tests.cpp)
target_link_libraries(core-tests PRIVATE dirsize_core)
add_test(NAME core-tests COMMAND core-tests)
add_executable(live-tests tests/live_index_tests.cpp)
target_link_libraries(live-tests PRIVATE dirsize_core)
add_test(NAME live-tests COMMAND live-tests)
add_executable(service-tests tests/service_tests.cpp)
target_link_libraries(service-tests PRIVATE dirsize_service)
add_test(NAME service-tests COMMAND service-tests)
add_executable(ui-tests tests/ui_tests.cpp)
target_link_libraries(ui-tests PRIVATE dirsize_ui_core)
add_test(NAME ui-tests COMMAND ui-tests)
