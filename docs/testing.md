# Testing

How the suite is organised, how to run it, what each suite proves, and how the
CI uses it.

---

## 1. The shape of the suite

| | |
| --- | --- |
| Framework | [doctest](https://github.com/doctest/doctest), single-header mode |
| Driver | CTest — `add_test()` per suite |
| Suites | 27 doctest translation units (26 standalone + the partitioned families-e2e TU) |
| Test cases | **240** `TEST_CASE` registrations at source level |
| CTest entries | **37** (26 suites + 11 per-case e2e partitions) |
| Helper executables | 2 (`tether-argv-probe`, `tether-capture-bench`) |
| Sleeps | **none in the protocol layer**, and only one bounded sleep site in the transport layer (the dial-out backoff, exercised by the loopback suite with a 1–2 ms policy). Everything else is injected time. The test binaries themselves sleep only inside bounded poll loops (see §4). |

The rule the suite follows, stated in `tests/CMakeLists.txt`:

> one doctest TU per protocol layer so a failing assert pins exactly one suite;
> all tests run over in-memory pipes.

Every test TU starts with:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

There is no shared test-support library. Each suite declares its own helpers
in an anonymous namespace.

---

## 2. Running the tests

### Everything

```sh
ctest --test-dir build --output-on-failure -C Release
```

On Linux the capture suites need an X server:

```sh
xvfb-run -a ctest --test-dir build --output-on-failure -C Release
```

### One suite

```sh
ctest --test-dir build -R envelope-test --output-on-failure -C Release
```

### One test case inside a suite

The test binaries are ordinary executables, so doctest's own filters work:

```sh
./build/tests/tether-envelope-tests                 # everything
./build/tests/tether-envelope-tests -tc="*poison*"   # a substring match
./build/tests/tether-envelope-tests -ltc="*round-trip*"
./build/tests/tether-envelope-tests --list-test-cases
```

### One family

```sh
ctest --test-dir build -R "file-family-test" --output-on-failure -C Release
```

### The per-case e2e partition

The end-to-end suite is registered as **eleven** ctest entries, each running
the same binary with a different doctest case filter:

```sh
ctest --test-dir build -R "families-e2e-" --output-on-failure -C Release
ctest --test-dir build -R "families-e2e-process" --output-on-failure -C Release
```

The partitioning exists because the e2e composition had wedged on
`windows-latest` with zero captured output. With one ctest entry per case, a
wedge **names its exact case** in the ctest log instead of hiding behind the
other ten passing cases.

### Verbose output

```sh
ctest --test-dir build -R envelope-test -V --output-on-failure -C Release
```

---

## 3. Suite inventory

| CTest name | File | Cases | Links | What it proves |
| --- | --- | ---: | --- | --- |
| `smoke` | `smoke.cpp` | 1 | protocol | The framework and the CTest integration work end to end. |
| `config-test` | `config_test.cpp` | 21 | common | Every config field loads; every rejection names the file and the field. |
| `logging-test` | `logging_test.cpp` | 3 | common | The record renders level, timestamp, message, and contextual fields; level filtering works. |
| `envelope-test` | `envelope_test.cpp` | 14 | protocol | The byte layout, round-trips, arbitrary slice boundaries, truncation, oversize lengths, and the poison semantics. |
| `payloads-test` | `payloads_test.cpp` | 16 | protocol | Strict per-type payload shapes for all 12 types; every malformed shape names the offending field. |
| `handshake-test` | `handshake_test.cpp` | 10 | protocol | The HMAC against RFC 4231, nonce entropy, and every refusal path including replay. |
| `session-test` | `session_test.cpp` | 20 | protocol | The handshake end to end, seq correlation, timeouts, cancellation, keepalives, supersession. |
| `base64-test` | `base64_test.cpp` | 6 | common | The RFC 4648 vectors, both alphabets, whitespace tolerance, and the rejection cases. |
| `tcp-loopback-test` | `tcp_loopback_test.cpp` | 8 | transport, protocol | The TCP transport on **real loopback sockets**, including a spawned production `tether-agent` child that is hard-killed. |
| `gmail-test` | `gmail_test.cpp` | 15 | transport, protocol, common | The Gmail adapter with the network **fully mocked** — no real API call is ever made. |
| `transport-equiv-test` | `transport_equiv_test.cpp` | 5 | transport, protocol, common | Both transports behind one interface; the same command yields equal semantics over TCP and over mail. |
| `registry-test` | `registry_test.cpp` | 3 | protocol | The live / stale / down registry states over real handshakes on in-memory pipes. |
| `cli-verb-test` | `cli_verb_test.cpp` | 9 | cli, protocol | Verb parsing, exit codes, and the dispatch contract against a fake console. |
| `cli-stream-test` | `cli_stream_test.cpp` | 5 | cli, protocol | stdout carries results byte-exactly; stderr stays clean; an incomplete delivery is a failure. |
| `cli-confirm-test` | `cli_confirm_test.cpp` | 6 | cli, protocol | The confirmation gate refuses with **zero dispatch calls** — the "nothing was sent" witness. |
| `cli-integration-test` | `cli_integration_test.cpp` | 3 | cli, transport, protocol, common | The CLI driving the **real session stack** over live loopback sockets, including the dial-in budget. |
| `matrix-test` | `matrix_test.cpp` | 8 | syscalls | The closed set, the refusals, and the "no system effect at all" instrument. |
| `process-family-test` | `process_family_test.cpp` | 6 | syscalls | The **real** per-OS process family: argv arrives verbatim, stop works, failures are structured. |
| `file-family-test` | `file_family_test.cpp` | 11 | syscalls | The **real** file family on temporary disk trees, with the agent-layer confirmation gate. |
| `power-family-test` | `power_family_test.cpp` | 5 | syscalls | The refusal path against the real per-OS family; the confirmed path against an injected backend only. |
| `capture-family-test` | `capture_family_test.cpp` | 6 | syscalls | A produced screenshot is a **decodable image file** (`cv::imdecode` back into real pixels). |
| `camera-family-test` | `camera_family_test.cpp` | 6 | command-handler, syscalls | The camera family over OpenCV with the shared job registry, using an injected fake source. |
| `command-session-test` | `command_session_test.cpp` | 5 | command-handler, syscalls, common | The dispatcher behind a real session pair over in-memory pipes. |
| `input-family-test` | `input_family_test.cpp` | 13 | syscalls | The keylogger job lifecycle, configurable storage, the keyboard-lock state, and the Wayland refusal. |
| `app-family-test` | `app_family_test.cpp` | 3 | syscalls | `.lnk` / `.desktop` listing, argv-resolved launches, and stop reuse. |
| `audit-test` | `audit_test.cpp` | 21 | cli, common | The JSONL store, the query API, the CLI record hooks, persistence, and strict re-open. |
| `families-e2e-*` (11 entries) | `families_e2e_test.cpp` | 11 | cli, command-handler, syscalls, protocol, transport, common | Every command family over the **real loopback stack**, per case. |

### Helper executables (not ctest entries)

| Target | Source | What it is |
| --- | --- | --- |
| `tether-argv-probe` | `probe_child.cpp` | A tiny child that prints its argv verbatim. The process and app suites launch it to observe argv one-to-one. |
| `tether-capture-bench` | `capture_bench.cpp` | The capture benchmark. **Not** a ctest — it is run manually on a desktop session; the recorded decision lives in [capture-notes.md](capture-notes.md). It is only built on UNIX. |

---

## 4. How the suites avoid flakiness

### Injected clocks

The protocol and transport layers take time as a parameter. The session suite
uses a `SessionConfig{100, 200, 100, 300}` (command / auth / ping / dead-peer
window in milliseconds) and pumps the sessions by hand. Nothing sleeps.

### Bounded waits, not blind sleeps

Every real-time test uses a **budget** and a polling loop:

| Helper | Budget | Where |
| --- | ---: | --- |
| `handshake_until_established` | 1,500 ms | `tcp_loopback_test.cpp`, `transport_equiv_test.cpp`, `cli_integration_test.cpp` |
| `pump_until_terminal` | 400 ms | the same |
| `console_goes_down_within` | window + tolerance | `tcp_loopback_test.cpp` |
| `await_probe` | ~400 s (2,000,000 × 200 µs) | `camera_family_test.cpp` |

Most of these helpers return `bool` and are wrapped in `REQUIRE`, so a timeout
**fails loudly at the call site**. One exception: the transport-equivalence
suite's `pump_until_terminal` returns `void` and simply gives up on a
timeout — there, a timeout shows up as a downstream assertion failure rather
than a named one.

### The e2e watchdog

`tests/families_e2e_test.cpp` carries its own watchdog: a budget of **90
seconds**, after which it calls `_Exit(34)` with the full captured stderr. The
ctest `TIMEOUT` of 240 s is the outer backstop, so a wedge resolves to a
**normal failed test, named per case**, long before ctest would kill it.

### Per-test ctest timeouts

Every suite has `TIMEOUT 600`; the eleven e2e partitions have `TIMEOUT 240`.
Nothing in this repository legitimately runs near those numbers — a healthy
whole-suite run finishes inside two minutes.

### Injected fakes for anything device-shaped

| Reality | Replaced by | In |
| --- | --- | --- |
| Webcam | `FakeFrameSource` | `camera_family_test.cpp` |
| Screen | `FakeScreenCapture` (a real 4×4 PNG) | `capture_family_test.cpp` |
| Keyboard events | `FakeSource` | `input_family_test.cpp` |
| Power backend | `FakePowerControl`, `FakeLogindBinding` | `power_family_test.cpp` |
| Process control (in the matrix suite) | `FakeProcessControl` | `matrix_test.cpp` |
| Gmail API | `FakeGmailClient`, `RecordingDispatcher` | `gmail_test.cpp` |
| Console | `FakeConsole`, `ConfirmSpy`, `StreamingFake` | the `cli_*` suites |

This is why the suite is green on a CI runner with no webcam, no display, and
no desktop.

### What is deliberately **not** faked

The process, file, and app suites use the **real** per-OS implementations on
real temporary disk trees and real child processes. The capture suite proves
the produced PNG decodes back into the expected pixels. The loopback suite
spawns the actual `tether-agent` binary and hard-kills it.

---

## 5. The CI

`.github/workflows/ci.yml`. Runs on every push to `main` and every pull
request **targeting `main`**.

```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      - os: ubuntu-latest
        ctest_jobs: 4
        compiler_cache_launcher: "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
      - os: windows-latest
        ctest_jobs: 2
        compiler_cache_launcher: ""
```

| Aspect | Detail |
| --- | --- |
| Legs | `ubuntu-latest` and `windows-latest`, in parallel, `fail-fast: false` |
| Job timeout | **60 minutes** — the outer backstop over the per-test timeouts |
| Concurrency | A new push to the same branch cancels the queued run |
| Permissions | `contents: read` — least privilege; the CI publishes nothing |
| Linux extras | `libx11-dev`, `xvfb`, `ccache` installed explicitly |
| Cache layer 1 | `ccache` on the repo's own translation units (Linux only) |
| Cache layer 2 | the vcpkg **binary cache**, keyed on `hashFiles('vcpkg.json')` |
| Test run | `xvfb-run -a ctest ... --parallel N --output-junit test-results.xml` (Linux); the same `ctest` invocation without the `xvfb-run` wrapper (Windows) |
| Artifacts | `test-results.xml` uploaded as a JUnit artifact, always |
| Cache hygiene | `ccache --cleanup` and `--show-stats` before the cache is archived |

**Why `ctest_jobs` differs.** The Linux leg runs a 4-vCPU runner with one
shared `xvfb` display; every suite is X-safe by design, so full parallelism is
safe. The Windows leg is deliberately conservative at 2-way, because the e2e
composition has wedged on `windows-latest` before. `ccache` is Linux-only
because `CMAKE_CXX_COMPILER_LAUNCHER` is ignored by the Visual Studio
generator.

**Why the vcpkg binary cache matters.** vcpkg in manifest mode compiles the
whole dependency graph from source on each run — opencv4 alone is ~15–25
minutes on these runners. The binary cache stores content-addressed prebuilt
packages and restores them in seconds. The archive filenames embed each
package's ABI hash, so a stale snapshot can be restored and vcpkg still only
reuses exactly the builds that match the current manifest.

---

## 6. Adding a test

### To an existing suite

Add a `TEST_CASE` to the suite's `.cpp` file. Nothing else changes — the
binary is already registered with ctest.

```cpp
TEST_CASE("the outcome of a new behaviour is explicit") {
    // arrange
    // act
    // assert
}
```

### A new suite

1. Create `tests/<area>_test.cpp`.
2. Add the target to `tests/CMakeLists.txt`:

   ```cmake
   add_executable(tether-<area>-tests <area>_test.cpp)
   target_link_libraries(tether-<area>-tests PRIVATE doctest::doctest tether-<dep>)
   add_test(NAME <area>-test COMMAND tether-<area>-tests)
   ```

   The existing binaries do not follow a strict file-name-to-target mapping —
   `tcp_loopback_test.cpp` builds `tether-tcp-tests`, `file_family_test.cpp`
   builds `tether-file-tests`, `capture_family_test.cpp` builds
   `tether-capture-tests`. Pick a name and keep it consistent within your
   change.

3. Add the ctest name to the `set_tests_properties(... PROPERTIES TIMEOUT 600)`
   list.
4. If the suite needs a helper child, add the dependency:
   `add_dependencies(tether-<area>-tests tether-argv-probe)`.

### Conventions

- **No sleeps.** Inject the clock. If a test needs real time, use a bounded
  budget and a poll loop.
- **No network.** The Gmail suite mocks the client; the TCP suite uses
  loopback only.
- **No real device.** Inject a fake for anything camera-, screen-, keyboard-,
  or power-shaped.
- **No real power transition.** Ever.
- **Assert the absence of effect, not just the presence of a result.** The
  matrix suite's `total_family_calls()` is the instrument for "nothing
  happened"; the confirm suite's dispatch counters are the "nothing was sent"
  witness.
- **Assert the exact string** where a message is part of the contract — the
  refusal codes and the exit codes are contractual.

---

## 7. Coverage map

| Requirement area | Suites that pin it |
| --- | --- |
| Envelope framing, bounds, poisoning | `envelope-test` |
| Payload strictness | `payloads-test` |
| HMAC handshake, replay, refusals | `handshake-test` |
| Session states, timers, seq correlation, cancellation, supersession | `session-test`, `registry-test` |
| Transport interface, TCP, Gmail, equivalence | `tcp-loopback-test`, `gmail-test`, `transport-equiv-test` |
| Verbs, exit codes, streams, confirmation | `cli-verb-test`, `cli-stream-test`, `cli-confirm-test`, `cli-integration-test` |
| The closed set and the capability matrix | `matrix-test` |
| Each command family | `process-family-test`, `file-family-test`, `power-family-test`, `capture-family-test`, `camera-family-test`, `input-family-test`, `app-family-test` |
| The dispatcher behind a real session | `command-session-test` |
| The audit trail | `audit-test` |
| Every family over the real stack | `families-e2e-*` (11 partitions) |
| Configuration, logging, base64 | `config-test`, `logging-test`, `base64-test` |

The wire specification in [protocol.md](protocol.md) ends with a **rule → test
cross-check table** mapping each documented rule to the suite that enforces it.
When you change the protocol, update both.

---

## 8. Known test gaps

Stated plainly, so nobody is surprised:

| Gap | Why it exists |
| --- | --- |
| No test triggers a real power transition | A deliberate binding — a CI runner would shut itself down. |
| The Gmail adapter never touches the network | The client is mocked; a live-API test would need real credentials. |
| No test installs a real keyboard hook or locks a real desktop | The input family is exercised through injected fakes. |
| The Linux capture family is only executed where an X server exists | CI wraps the Linux leg in `xvfb-run`; a WSL host runs under WSLg. The other platform's branch is compile- and link-checked only. |
| The `gmail` section of the console config is parsed but not wired into the shipped binary | The adapter itself is fully implemented and tested; the factory (`build_console_transports`) is what the binary does not yet call. |
| `ConsoleSession::opened_at_` is never assigned | A latent bug: the console's auth timeout is measured from time 0, not from the session's open. The shipped TCP transport masks it by rebasing the session clock per connection (accept = 0). An absolute-clock caller would see the console's auth timeout fire on the first pump. |
