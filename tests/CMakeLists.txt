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

# Phase 5: operator console CLI (appended per task 5.1-5.5; one TU per suite).
add_executable(tether-registry-tests registry_test.cpp)
target_link_libraries(tether-registry-tests PRIVATE doctest::doctest tether-protocol)
add_test(NAME registry-test COMMAND tether-registry-tests)

# Task 5.2: verb surface, exit codes, dispatch contract (fake console).
add_executable(tether-cli-verb-tests cli_verb_test.cpp)
target_link_libraries(tether-cli-verb-tests PRIVATE doctest::doctest tether-cli tether-protocol)
add_test(NAME cli-verb-test COMMAND tether-cli-verb-tests)

# Task 5.3: stdout/stderr streaming and byte-exact files (fake console).
add_executable(tether-cli-stream-tests cli_stream_test.cpp)
target_link_libraries(tether-cli-stream-tests PRIVATE doctest::doctest tether-cli tether-protocol)
add_test(NAME cli-stream-test COMMAND tether-cli-stream-tests)

# Task 5.4: destructive-command confirmation gate (fake console).
add_executable(tether-cli-confirm-tests cli_confirm_test.cpp)
target_link_libraries(tether-cli-confirm-tests PRIVATE doctest::doctest tether-cli tether-protocol)
add_test(NAME cli-confirm-test COMMAND tether-cli-confirm-tests)

# Task 5.5: end-to-end CLI-through-protocol over the real session stack.
add_executable(tether-cli-integration-tests cli_integration_test.cpp)
target_link_libraries(tether-cli-integration-tests PRIVATE doctest::doctest tether-cli tether-protocol tether-transport tether-common)
add_test(NAME cli-integration-test COMMAND tether-cli-integration-tests)

# Phase 6: commands and capability matrix (appended per task 6.1-6.4; one TU
# per suite). tether-argv-probe is not a test: it is the tiny child the
# process-family suite launches to observe argv verbatim.
add_executable(tether-argv-probe probe_child.cpp)

# Task 6.1: closed-set rejection, matrix refusals, definitive outcomes
# (recording fakes only - no sockets, no children, no disk).
add_executable(tether-matrix-tests matrix_test.cpp)
target_link_libraries(tether-matrix-tests PRIVATE doctest::doctest tether-syscalls)
add_test(NAME matrix-test COMMAND tether-matrix-tests)

# Task 6.2: the process family over its real per-OS implementation. The
# suite locates the probe next to its own binary (repo probing pattern).
add_executable(tether-process-tests process_family_test.cpp)
target_link_libraries(tether-process-tests PRIVATE doctest::doctest tether-syscalls)
add_dependencies(tether-process-tests tether-argv-probe)
add_test(NAME process-family-test COMMAND tether-process-tests)

# Task 6.3: the file family on real temporary disk with the agent-layer
# confirmation gate (dispatcher-level, identical gates to the wired agent).
add_executable(tether-file-tests file_family_test.cpp)
target_link_libraries(tether-file-tests PRIVATE doctest::doctest tether-syscalls)
add_test(NAME file-family-test COMMAND tether-file-tests)

# Task 6.4: the power family. Refusal path tested against the REAL per-OS
# family (nothing executes behind the gate); the confirmed path rides
# fake/injected backends only - no test ever triggers a real power
# transition on any platform (task 6.4 binding).
add_executable(tether-power-tests power_family_test.cpp)
target_link_libraries(tether-power-tests PRIVATE doctest::doctest tether-syscalls)
add_test(NAME power-family-test COMMAND tether-power-tests)

# Task 6.5: the capture family. The injected-source cases run on both legs;
# the REAL per-OS grab (GDI desktop / X11) runs where its server exists -
# the Linux leg is wrapped in xvfb-run in CI, WSL runs under WSLg.
add_executable(tether-capture-tests capture_family_test.cpp)
target_link_libraries(tether-capture-tests PRIVATE doctest::doctest tether-syscalls)
add_test(NAME capture-family-test COMMAND tether-capture-tests)

# Task 6.5: the capture benchmark (NOT a ctest). Executed manually on a
# desktop session; the recorded decision lives in docs/capture-notes.md.
if(UNIX)
    add_executable(tether-capture-bench capture_bench.cpp)
    target_link_libraries(tether-capture-bench PRIVATE tether-syscalls)
endif()

# Task 6.6: the camera family over OpenCV with the shared background-job
# registry lifecycle; all device interaction rides injected fake sources
# (CI runners have no webcams). The session-level lifecycle case runs the
# real session stack (the same harness command-session-test wires).
add_executable(tether-camera-tests camera_family_test.cpp)
target_link_libraries(tether-camera-tests
    PRIVATE doctest::doctest tether-command-handler tether-syscalls)
add_test(NAME camera-family-test COMMAND tether-camera-tests)

# Tasks 6.1-6.3 wiring: the dispatcher behind a REAL session pair over
# in-memory pipes (fakes for process, real temporary disk for file).
add_executable(tether-command-session-tests command_session_test.cpp)
target_link_libraries(tether-command-session-tests
    PRIVATE doctest::doctest tether-command-handler tether-syscalls tether-common)
add_test(NAME command-session-test COMMAND tether-command-session-tests)

# Task 6.7: the input family - keylogger job lifecycle on the shared registry,
# configurable capture storage, keyboard-lock state, and the structured
# Wayland/unsupported resolution that never wedges the session. All platform
# reality rides injected fakes: no test installs a real hook or locks a real
# desktop.
add_executable(tether-input-tests input_family_test.cpp)
target_link_libraries(tether-input-tests PRIVATE doctest::doctest tether-syscalls)
add_test(NAME input-family-test COMMAND tether-input-tests)

# Task 6.8: the app family - .lnk/.desktop listing, argv-resolved launches
# over the injected process control, and stop reuse. The launch assertions
# ride the argv probe child; listing roots are injected fixture directories.
add_executable(tether-app-tests app_family_test.cpp)
target_link_libraries(tether-app-tests PRIVATE doctest::doctest tether-syscalls)
add_dependencies(tether-app-tests tether-argv-probe)
add_test(NAME app-family-test COMMAND tether-app-tests)

# Task 6.9: console-side audit records - the JSONL store, query API, the
# CLI hooks' executed/refused/failed command records and job_event records
# (fake console, no sockets), persistence + strict re-open.
add_executable(tether-audit-tests audit_test.cpp)
target_link_libraries(tether-audit-tests PRIVATE doctest::doctest tether-cli tether-common)
add_test(NAME audit-test COMMAND tether-audit-tests)

# Task 6.10: per-family E2E scenario checks over REAL loopback TCP sessions.
# Every verb rides the full operator stack (run_cli -> TcpOperConsole ->
# TcpConsoleTransport -> live sockets -> AgentSession -> install_commands ->
# the REAL capability dispatcher), with per-family injections only where a
# host device would be required (power tripwire, fake screen/camera/input
# sources); process/file/app reality stays per-OS real. Launch assertions
# ride the argv probe child (same dependency pattern as the 6.2/6.8 suites).
add_executable(tether-families-e2e-tests families_e2e_test.cpp)
target_link_libraries(tether-families-e2e-tests PRIVATE
    doctest::doctest tether-cli tether-command-handler tether-syscalls
    tether-protocol tether-transport tether-common)
add_dependencies(tether-families-e2e-tests tether-argv-probe)
add_test(NAME families-e2e-test COMMAND tether-families-e2e-tests)

# Wall-clock guard: a wedged suite (e.g. on a CI runner whose environment
# silently broke) must fail loudly at the timeout with its ctest start
# marker, naming where the leg stopped, instead of parking the runner for
# hours (which hung one leg for ~1 h while every death was unobservable).
# Nothing in this repo runs anywhere near minutes: the budgets inside the
# suites stay in the 1-15 s band and whole ctest runs finish inside 2 min.
set_tests_properties(
    smoke config-test logging-test envelope-test payloads-test
    handshake-test session-test base64-test tcp-loopback-test gmail-test
    transport-equiv-test registry-test cli-verb-test cli-stream-test
    cli-confirm-test cli-integration-test matrix-test process-family-test
    file-family-test power-family-test capture-family-test camera-family-test
    command-session-test input-family-test app-family-test audit-test
    families-e2e-test
    PROPERTIES TIMEOUT 600)
# The e2e suite runs in ~2 s when healthy; a tighter wall clock keeps diagnosis
# cycles fast (the per-case stderr markers name any wedging case on timeout).
set_tests_properties(families-e2e-test PROPERTIES TIMEOUT 150)
