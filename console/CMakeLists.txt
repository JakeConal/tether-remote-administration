add_executable(tether-console main.cpp)
target_compile_definitions(tether-console PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
target_link_libraries(tether-console PRIVATE tether-common tether-protocol tether-transport)
