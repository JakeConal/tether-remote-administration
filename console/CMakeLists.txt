add_executable(tether-console main.cpp)
target_compile_definitions(tether-console PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
