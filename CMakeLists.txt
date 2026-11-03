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

add_library(dirsize_core STATIC
    src/core/path_util.cpp
    src/core/dir_walker.cpp
)
target_include_directories(dirsize_core PUBLIC src)

add_executable(dirsize-cli src/cli/main.cpp)
target_link_libraries(dirsize-cli PRIVATE dirsize_core psapi)
