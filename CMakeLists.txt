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
    # The code deliberately uses the portable standard C++ `getenv` (the
    # values are non-secret paths read once at startup, not credentials).
    # MSVC deprecates it in favour of the non-portable `_dupenv_s`; silence
    # that deprecation instead of forking the API per platform.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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
