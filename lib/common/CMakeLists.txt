add_library(tether-common STATIC base64.cpp config.cpp logging.cpp)
target_include_directories(tether-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether-common PUBLIC nlohmann_json::nlohmann_json)
