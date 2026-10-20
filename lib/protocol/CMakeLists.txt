add_library(tether-protocol STATIC protocol.cpp)
target_include_directories(tether-protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
