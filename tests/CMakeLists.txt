add_executable(tether-smoke-tests smoke.cpp)
target_link_libraries(tether-smoke-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME smoke COMMAND tether-smoke-tests)
