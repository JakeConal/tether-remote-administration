add_library(tether-syscalls STATIC syscalls.cpp)
target_include_directories(tether-syscalls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
