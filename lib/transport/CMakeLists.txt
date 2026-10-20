add_library(tether-transport STATIC transport.cpp)
target_include_directories(tether-transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
