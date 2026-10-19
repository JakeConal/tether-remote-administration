add_executable(tether-agent main.cpp)
target_compile_definitions(tether-agent PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
