# Tasks

## 1. Telemetry configuration

- [x] 1.1 Extend `AgentConfig` in `lib/common/config.{hpp,cpp}` with an optional `telemetry` section (`interval_s` default 1, `window_s` default 60); reject `interval_s < 1`, `window_s < interval_s`, wrong types, and unknown keys with `ConfigError` naming the field; add loader cases to `tests/config_test.cpp` (valid defaults, absent section, each invalid case)
- [x] 1.2 Add the section to `agent.example.json` and `docs/examples/agent.example.json`, and document the keys in `docs/configuration.md`; verify the examples load with `load_agent_config` in `tests/config_test.cpp`

## 2. Health sampler engine (pure, testable)

- [x] 2.1 Add `HealthState` (window storage, uptime/reconnect counters, staleness bookkeeping, aggregate math) in `lib/syscalls/health.hpp` with an injected clock; unit tests in `tests/health_test.cpp` pin: window fill/drain over fake ticks, first tick is baseline-only (no fabricated CPU), aggregates (cpu avg/max, mem max) over the configured span, sample age grows honestly when ticks are delayed
- [x] 2.2 Verify the snapshot JSON builder emits the design D5 field set in one line: parse every documented key in `tests/health_test.cpp` (latest, window, uptime_seconds, reconnects, sample_age_ms, interval_s)

## 3. OS probes

- [x] 3.1 Implement `IHealthHostProbe` for Windows (`GetSystemTimes` delta, `GlobalMemoryStatusEx`, Toolhelp process/thread snapshots) and add a smoke test to `tests/health_test.cpp` asserting plausible ranges (cpu in [0,100], total >= used, positives) on the Windows build
- [x] 3.2 Implement `IHealthHostProbe` for Linux (`/proc/stat` delta, `/proc/meminfo`, `/proc/loadavg` threads, `/proc` walk for processes) with the same plausible-range smoke test on the Linux build

## 4. The `health` command surface

- [x] 4.1 Add `Family::Telemetry` and `family_name`, and one `kCommandSet` row (`health`, non-destructive, both OSes); extend `tests/matrix_test.cpp`: `health` is in the closed set, the five-gate ordering still holds with the new row, and unknown verbs keep refusing with `unknown_command`
- [x] 4.2 Implement the telemetry family call: parameter-arity refusal (any args → structured refusal, no effect) and `sample_unavailable` mapping for a failing probe; pin both in `tests/matrix_test.cpp`
- [x] 4.3 Wire `install_commands` so the agent installs the family with `HealthState` + dial-loop counters injected; verify via a loopback command flow (`tests/command_session_test.cpp` pattern) that an issued `health` returns a parsable snapshot over an established session

## 5. Agent loop wiring

- [x] 5.1 Add `started_at`/`reconnects` counters in `agent/main.cpp`'s dial loop that outlive each link and increment on every successful re-establish; verify in a redial-flow test that reconnect count and uptime survive session churn
- [x] 5.2 Insert the interval/window-driven sampling tick into the established-session pump loop; verify with a short interval run over the loopback harness that the window fills while commands interleave and that later snapshots reflect delayed ticks (coverage/age fields) rather than frozen values

## 6. Console monitor

- [ ] 6.1 Add the `monitor` verb to `console/cli.cpp` (target agent; `--json` flag; unknown agent and unknown verb stay structured refusals); extend `tests/cli_verb_test.cpp` accordingly
- [ ] 6.2 Implement the observe loop in `console/main.cpp`: unbounded dial wait for this verb, fixed 1 s cadence, one human line per interval, unavailable rendering while the target is down, resume after re-registration, first-poll `unknown_command` refusal exits with the refusal outcome, cooperative SIGINT flag exits 0; pin the state transitions in `tests/cli_integration_test.cpp`
- [ ] 6.3 Pin the stream contract in `tests/cli_stream_test.cpp`: observations to stdout, status to stderr, and `--json` emits one parseable JSON object per line even when piped
- [ ] 6.4 Add console-side instrumentation: per-issued-seq latency sampling and counting `IByteSource`/`IByteSink` wrappers for throughput; verify counters and interval math in a small unit test feeding canned bytes/timestamps
- [ ] 6.5 Amend `console/audit.cpp` so a monitor invocation ends with exactly one audit record (command `monitor`, target parameter, duration, end state) and its internal polls emit nothing, while `exec <agent> health` still audits per issue; extend `tests/audit_test.cpp` and keep the existing job lifecycle scenarios green

## 7. End-to-end and docs

- [ ] 7.1 Extend the end-to-end harness (`tests/families_e2e_test.cpp` style or a focused `health_e2e_test.cpp`): real console+agent pair, dial-in, poll health, snapshot shape check, forced redial observed by the monitor's counters, one audit record
- [ ] 7.2 Update `docs/`: architecture.md (thread-model note: sampling ticks inline, no new thread), command-reference.md (`health` argument contract), operations.md (operator monitoring workflow with `monitor --json`), and the agent-vs-console example configs; verify each documented command/flag runs as written
- [ ] 7.3 Run the full test suite and a manual demo (console + agent loopback with a forced agent restart) to confirm monitor rendering, exit codes, and the audit trail end to end

## Workflow follow-up

- After review, run the apply workflow explicitly (`/opsx-apply`) to implement these tasks.
- Archive the change with `/opsx-archive` once the project's review requirements are satisfied.
