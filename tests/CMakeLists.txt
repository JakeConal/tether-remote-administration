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

# Phase 4: transports (appended per task 4.1-4.4; same one-TU-per-suite rule).
add_executable(tether-base64-tests base64_test.cpp)
target_link_libraries(tether-base64-tests PRIVATE doctest::doctest tether-common)
add_test(NAME base64-test COMMAND tether-base64-tests)

add_executable(tether-tcp-tests tcp_loopback_test.cpp)
target_link_libraries(tether-tcp-tests PRIVATE doctest::doctest tether-transport tether-protocol)
add_test(NAME tcp-loopback-test COMMAND tether-tcp-tests)

add_executable(tether-gmail-tests gmail_test.cpp)
target_link_libraries(tether-gmail-tests PRIVATE doctest::doctest tether-transport tether-protocol tether-common)
add_test(NAME gmail-test COMMAND tether-gmail-tests)

add_executable(tether-transport-equiv-tests transport_equiv_test.cpp)
target_link_libraries(tether-transport-equiv-tests PRIVATE doctest::doctest tether-transport tether-protocol tether-common)
add_test(NAME transport-equiv-test COMMAND tether-transport-equiv-tests)
