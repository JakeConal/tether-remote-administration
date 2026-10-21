add_library(tether-cli STATIC cli.cpp tcp_console.cpp audit.cpp)
target_include_directories(tether-cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether-cli PUBLIC tether-protocol tether-transport)
target_link_libraries(tether-cli PRIVATE CLI11::CLI11)
# Task 6.9: the audit store's JSONL codec (nlohmann; a private dependency -
# no audit header exposes JSON).
target_link_libraries(tether-cli PRIVATE nlohmann_json::nlohmann_json)

add_executable(tether-console main.cpp)
target_compile_definitions(tether-console PRIVATE TETHER_VERSION="${PROJECT_VERSION}")
target_link_libraries(tether-console PRIVATE tether-cli tether-common tether-protocol tether-transport)
