cmake_minimum_required(VERSION 3.24)

project(tether
    VERSION 0.1.0
    DESCRIPTION "tether - authorized-use remote administration lab tool (Phase 2 core structure)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

add_subdirectory(lib/common)
add_subdirectory(lib/protocol)
add_subdirectory(lib/transport)
add_subdirectory(lib/syscalls)
add_subdirectory(console)
add_subdirectory(agent)
add_subdirectory(tests)
