add_library(tether-syscalls STATIC matrix.cpp process.cpp file.cpp)
target_include_directories(tether-syscalls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
