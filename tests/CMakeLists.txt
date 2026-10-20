add_executable(tether-smoke-tests smoke.cpp)
target_link_libraries(tether-smoke-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME smoke COMMAND tether-smoke-tests)

add_executable(tether-config-tests config_test.cpp)
target_link_libraries(tether-config-tests PRIVATE doctest::doctest tether-common)
add_test(NAME config-test COMMAND tether-config-tests)

add_executable(tether-logging-tests logging_test.cpp)
target_link_libraries(tether-logging-tests PRIVATE doctest::doctest tether-common)
add_test(NAME logging-test COMMAND tether-logging-tests)

# Protocol v2 boundary suite (task 3.5): one doctest TU per protocol layer so
# a failing assert pins exactly one suite; all tests run over in-memory pipes.
# Suites are appended per task (3.1..3.4) in dependency order.
add_executable(tether-envelope-tests envelope_test.cpp)
target_link_libraries(tether-envelope-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME envelope-test COMMAND tether-envelope-tests)

add_executable(tether-payloads-tests payloads_test.cpp)
target_link_libraries(tether-payloads-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME payloads-test COMMAND tether-payloads-tests)

add_executable(tether-handshake-tests handshake_test.cpp)
target_link_libraries(tether-handshake-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME handshake-test COMMAND tether-handshake-tests)

add_executable(tether-session-tests session_test.cpp)
target_link_libraries(tether-session-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME session-test COMMAND tether-session-tests)
