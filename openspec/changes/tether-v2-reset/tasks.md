# Tasks

Scope note (not tracked): the umbrella design's per-phase-change flow (D9) was collapsed into this single change by owner choice; groups below still follow the same dependency order as P1-P6. Design decisions D1-D8 are binding; the three design Open Questions are resolved inside the tasks marked "decide".

## 1. Repo and build rescue (P1: tooling-only foundation)

- [x] 1.1 Initialize git at the project root, add `.gitignore` for build directories, user property files, and any credential-bearing files, then create the founding commit preserving the current tree byte-for-byte and tag it `v1.0-final`; verify with `git log`, `git tag --list`, and confirm the v1 sources burn in via `git show v1.0-final --stat`
- [x] 1.2 Add a root `CMakeLists.txt` plus skeleton layout (`lib/{protocol,transport,syscalls}`, `console/`, `agent/`, `tests/`, `docs/`) with placeholder C++ targets that compile under MSVC; verify configure and `cmake --build` succeed locally on Windows with a clean build directory
- [x] 1.3 Add root vcpkg manifest wiring (minimal dependency set: no OpenCV/cpr yet); verify manifest-mode resolution completes and the placeholder targets still build
- [x] 1.4 Choose the test framework (Catch2 v3 vs doctest), record the rationale as a one-paragraph note in `docs/build.md`, and wire it into the manifest; verify the framework target resolves from vcpkg and a trivial test file compiles
- [x] 1.5 Add a smoke test exercising one trivial unit and register it with CTest; verify `ctest` reports it passing locally on Windows
- [x] 1.6 Add `.github/workflows/ci.yml` with a matrix (windows-latest, ubuntu-latest) running configure + build + test using the vcpkg manifest; verify the workflow passes on a pushed branch (commit `build platform` with history showing the v1 -> CI transition)

## 2. Core structure (P2: one codebase, real targets, config + logging)

- [x] 2.1 Promote skeleton placeholders into real CMake targets (`tether-console`, `tether-agent`, libs `tether-protocol`, `tether-transport`, `tether-syscalls`) with empty service mains; verify all five targets build clean under the CI matrix
- [x] 2.2 Implement config file loading for console (address, port, token store path) and agent (console address, agent name, token) with explicit failure on missing/invalid fields; verify unit tests cover valid load, missing field, and malformed JSON paths per design D8
- [x] 2.3 Implement structured logging (levels + timestamp + contextual fields including session and seq) replacing bare `cerr` usage in new code; verify a test renders entries with expected fields
- [x] 2.4 Confirm v1 sources remain untouched in the working tree at this point (`client/`, `server/` still present per migration plan step 3); verify by tree diff against `v1.0-final`

## 3. Protocol v2 (P3: correctness gets its tests)

- [x] 3.1 Implement the message envelope codec (`ver | type | seq | len`, little-endian) with strict length bounds and unknown-version rejection; verify unit tests cover round-trip, truncated frames, oversize `len`, and unknown version handling
- [x] 3.2 Implement JSON payload handling for the message types (`HELLO`, `CHALLENGE`, `AUTH_OK`, `CMD`, `RESULT_TEXT`, `FILE_META`, `FILE_CHUNK`, `FILE_END`, `CANCEL`, `PING`/`PONG`, `ERR`) with per-type field validation; verify unit tests reject malformed payload shapes per type
- [x] 3.3 Implement the HMAC challenge-response handshake over a provisioned token (nonce -> HMAC verify -> `AUTH_OK`) per design D4; verify tests that correct tokens pass, wrong answers fail, and replayed nonces are rejected
- [x] 3.4 Implement sequence correlation plus `CANCEL`, `PING`/`PONG`, and per-command timeouts as a session state machine per the `secure-session` delta; verify tests that concurrent commands keep results attributed to their own seq, timeout or cancel marks one command failed without poisoning the session, and keepalive marks dead sessions down beyond the configured window
- [x] 3.5 Wire the whole protocol suite into CTest as the boundary test suite; verify `ctest` green locally on Windows and commit so CI exercises the same suite on ubuntu-latest, and check the CI log for the Linux run

## 4. Transports (P4: TCP live end-to-end, Gmail adapter, v1 removal)

- [x] 4.1 Implement the TCP transport: console listener, agent dialer with bounded retry/backoff at startup per the dial-out requirement, and keepalive-based dead-session detection per the consolidation requirement; verify a loopback integration test where an agent dials, authenticates, executes a protocol command, and a killed agent process is detected down within the keepalive window
- [x] 4.2 Implement the Gmail adapter: OAuth refresh-token flow reading credentials from the configured path, polling at the configured interval, exponential backoff on API errors per the adapter deltas; verify unit tests (network mocked) for poll cadence bounds and backoff behavior
- [x] 4.3 Implement Gmail inbound acceptance (sender on configured admin list + destination adapter account) and MIME command parsing plus result delivery with attachments; verify unit tests (network mocked): non-admin mail ignored without executing or affecting cadence, admin mail becomes a well-formed protocol command, file results ship as MIME attachments, and text results reply to the requesting admin
- [x] 4.4 Add transport-equivalence tests issuing the same commands through both transports behind the transport interface (Gmail mocked) and a TCP-only configuration test asserting no Gmail polling occurs; verify `ctest` green including the TCP-only scenario
- [x] 4.5 Remove `client/` and `server/` from the working tree now that protocol v2 is proven end-to-end (migration plan step 3), keeping `report/` untouched; verify the tree diff shows only those directories removed, history still contains `v1.0-final`, and the build passes after removal

## 5. Operator console CLI (verbs, registry, streams, safety)

- [x] 5.1 Implement the agent registry backed by session state (live / stale / down) with state transition wiring from the session layer; verify tests per the registry requirement: mixed states listed correctly, re-registration flips down back to live, superseded session goes stale
- [x] 5.2 Implement verb parsing and dispatch (`agents`, `exec <agent> <command> [args]`, `cancel <agent> <seq>`) with structured stderr errors and distinguishable exit codes for unknown verb and unknown agent before any dispatch; verify tests per the verb-surface requirement including the scriptable exit-code scenarios
- [x] 5.3 Implement stdio streaming: text results to stdout, file results byte-exact to a redirected target file, progress/status to stderr; verify tests run both attached and redirected (piped stdout cleanliness, byte-exact file output per the streams requirement)
- [x] 5.4 Implement operator-side confirmation for shutdown, restart, and file deletion (`--yes` / interactive), refusing before dispatch otherwise; verify tests per the confirmation requirement (refused locally with nothing sent; confirmed commands dispatch)
- [x] 5.5 Verify an end-to-end CLI-through-protocol integration test: a fake agent over the real session stack executes a command issued via the CLI verbs, result streams to stdout, and stderr stays clean; verify the integration test passes in CTest

## 6. Commands and capability matrix (P5: the 19 commands, both OSes)

- [x] 6.1 Implement the capability matrix dispatch layer mapping each command to its family interface, with structured unsupported-platform results and closed-set rejection for unknown verbs; verify tests per the closed-set and matrix requirements (unknown verb produces no system effect; unsupported command returns a structured refusal and the session stays usable)
- [x] 6.2 Implement the process family (list/start/stop) with argv-based launches replacing shell splicing: Windows native/Win32 paths, Linux `/proc` inspection and signal-based stop; verify tests including the metacharacter-treated-as-data scenario and structured outcome reporting
- [x] 6.3 Implement the file family (list/delete/move/copy) over `std::filesystem` on both OSes with confirm handling; verify tests per the file scenarios including confirmation refusal at the agent layer
- [x] 6.4 Implement the power family (shutdown/restart) with explicit authorization handling: Windows desktop shutdown API, Linux logind/DBus interface; verify tests that unconfirmed commands are refused with structured results
- [x] 6.5 Implement the capture family: Windows GDI screenshot; Linux X11 capture after benchmarking own-lib implementation against shelling to existing utilities, recording the decision and benchmark note in `docs/capture-notes.md`; verify a produced screenshot is a decodable image file on each supported platform
- [x] 6.6 Implement the camera family over OpenCV with the shared background-job lifecycle (start / duplicate-start refusal / stop-and-deliver per the job requirement); verify job lifecycle tests including session-available-while-running behavior
- [x] 6.7 Implement the input family (keylogger start/stop with configurable capture storage, keyboard lock/unlock): Windows paths first, Linux X11 implementations, Wayland environments resolving to structured unsupported results; verify tests that unsupported resolution never wedges the session and job lifecycle tests apply to the keylogger
- [x] 6.8 Implement the app family (list/start/stop): Start-menu `.lnk` resolution via the shell API on Windows, `.desktop` entries on Linux; verify list/start/stop behavior tests on the platform in use
- [x] 6.9 Implement console-side audit records (time, agent identity, command, parameters, seq, outcome) with query/fixture access; verify tests per the audit requirement spanning executed, refused, and failed commands and background-job events
- [ ] 6.10 Extend the end-to-end suite with scenario checks for each command family on the platform in use (exec via CLI over TCP, background job start/stop with delivered capture, refusal paths); verify the whole CTest suite green and both-OS parity visible in CI logs

## 7. Docs, demo, and public-repo readiness (P6)

- [ ] 7.1 Write the protocol v2 spec document (`docs/protocol.md`) describing envelope, message types, handshake, and seq semantics; verify an engineer can implement a compatible peer from the document alone by checking it against the codec tests
- [ ] 7.2 Write the authorized-use statement (who may use it, target machines must be owned/authorized, no stealth features) and the README covering tether's purpose, commands, and build instructions; verify every build/run command in the README runs verbatim from a clean checkout on Windows and Linux
- [ ] 7.3 Decide and record the public-repo readiness items left open in the design: final license choice, README tone, and whether `report/` stays linked or private; record the decision in `docs/readiness-decisions.md` with the owner's sign-off
- [ ] 7.4 Record the allowed-use policy results: confirm no credentials or admin email lists appear anywhere in source, `admin.txt`-style files live only in operator config paths outside the repo; verify by searching the tree for the retired hardcoded addresses and addresses from v1
- [ ] 7.5 Add the demo script (`scripts/demo.md` or shell script): a narrated run showing agent connect, registry listing, process listing, screenshot streaming to a file, background camera job lifecycle, and a confirmation refusal; verify the demo script was executed end-to-end once and produced the expected outputs
- [ ] 7.6 Final integration check: full CI matrix green on the completed implementation (Windows + Ubuntu), protocol suite + transport suite + command scenario suite all passing as a single `ctest` run from a clean clone; verify by re-running CI from scratch on a fresh fork/branch after a code cleanup pass

## Workflow follow-up

- After the review requirements are satisfied, run `/opsx-verify` style review then archive the change with `openspec archive tether-v2-reset` so the four capability specs land in the main spec set.
- Verify the archived result: `openspec list --specs` shows `secure-session`, `command-transport`, `remote-commands`, `operator-console`.
