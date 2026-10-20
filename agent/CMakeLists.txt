add_executable(tether-agent main.cpp)
target_compile_definitions(tether-agent PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
target_link_libraries(tether-agent PRIVATE tether-common tether-protocol tether-transport tether-syscalls)
