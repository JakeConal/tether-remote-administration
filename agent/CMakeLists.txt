# The agent-side command surface (tasks 6.1-6.3): a library layer so the
# executor is reachable from the service main AND the test suites, with the
# thin main below it.
add_library(tether-command-handler STATIC command_handler.cpp)
target_include_directories(tether-command-handler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether-command-handler
    PUBLIC tether-common tether-protocol tether-syscalls tether-transport)

add_executable(tether-agent main.cpp)
target_compile_definitions(tether-agent PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
target_link_libraries(tether-agent PRIVATE tether-common tether-command-handler)
