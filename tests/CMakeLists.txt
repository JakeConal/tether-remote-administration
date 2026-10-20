add_executable(tether-smoke-tests smoke.cpp)
target_link_libraries(tether-smoke-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME smoke COMMAND tether-smoke-tests)

add_executable(tether-config-tests config_test.cpp)
target_link_libraries(tether-config-tests PRIVATE doctest::doctest tether-common)
add_test(NAME config-test COMMAND tether-config-tests)

add_executable(tether-logging-tests logging_test.cpp)
target_link_libraries(tether-logging-tests PRIVATE doctest::doctest tether-common)
add_test(NAME logging-test COMMAND tether-logging-tests)
