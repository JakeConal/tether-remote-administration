# Proposal

## Why

The console can only see an agent at the instant it issues a command. There is
no standing view of target health: when a session behaves badly (slow results,
a pegged CPU, climbing memory) the operator has no data to distinguish
link trouble from host stress. Health telemetry gives the operator a
continuous in-session view of the target without adding any wire machinery,
new message types, or a console daemon.

## What Changes

- The agent gains an always-on sampled rolling window of host health: CPU
  usage, memory usage, process count, and thread count, plus agent-self
  counters (connection uptime, reconnect count since start). Sampling runs
  inline in the agent's existing pump loop at a configurable interval
  (default 1 s) - **no new thread, no queue, no wire change**.
- A new `health` verb (new `Telemetry` matrix family) returns a structured
  JSON snapshot on demand: latest sample plus window aggregates
  (cpu avg/max, cpu peak, etc.) and `sample_age_ms` / sampling-window
  coverage so the operator can judge freshness. Spikes between two polls
  remain visible through the window aggregates.
- The console gains a `monitor` verb: a long-lived invocation that keeps the
  session, re-issues `health` at a cadence, and renders a live line per
  interval. The console additionally computes stats locally from its own
  activity: end-to-end command latency, transport throughput, connection
  uptime, and reconnects observed during the invocation. `monitor` survives
  agent restarts (down → re-registration) and offers a `--json` JSONL output
  shape for scripting.
- Audit posture: the `monitor` invocation writes ONE audit record for itself
  (started/ended); its internal 1 s health polls are samples of that one
  command, not repeated operator actions, and are not individually audited.
  An operator-issued `exec <agent> health` is audited normally.
- Agent config gains a `telemetry` section (`interval_s` default 1,
  `window_s` default 60), validated by the strict config loader and covered
  by the example `agent.example.json`.

Deliberate reinterpretation of the original request: "queue depth" and
"dropped samples" do not exist in tether and are not built here. A pull
snapshot design has no queue; their operational purpose (can I trust this
number?) is served instead by reported sample age and window coverage.

## Capabilities

### New Capabilities

- `health-telemetry`: the agent's health sampling and its on-demand snapshot
  exposure - what is sampled on each OS, how the rolling window is
  maintained, the `health` command contract (snapshot shape, structured
  refusals, freshness fields), and the invariant that sampling must not
  disrupt the session or command path.

### Modified Capabilities

- `operator-console`: adds the `monitor` invocation form - a verb the console
  serves until interrupted rather than until one result lands, with unbounded
  dial-in wait specific to that verb, standard-stream discipline, and its
  single audit record.
- `remote-commands`: the closed command set grows by one non-destructive verb
  (`health`, `Telemetry` family, supported on both OSes), and the audit
  requirement wording is amended so `monitor`'s internal polls are explicitly
  outside the audited set while operator-issued `health` remains audited.

## Impact

- `lib/protocol`: **no changes** - no new `MsgType`; health rides the existing
  `Cmd` / `RESULT_TEXT` correlation path. Old agents paired with new consoles
  degrade gracefully: the dispatcher refuses `health` as covered by gate 1
  (`unknown_command`), a structured refusal the monitor can render.
- `lib/syscalls`: new `HealthSampler` capability with Windows and Linux OS
  implementations (`GetSystemTimes`/`GlobalMemoryStatusEx`/Toolhelp32 vs
  `/proc/stat`/`/proc/meminfo`/`/proc` walk or `/proc/loadavg`); one matrix
  row behind the five existing gates.
- `agent/main.cpp`: sampling tick wired into the established-session pump
  loop; dial-loop counters (uptime, reconnects) that outlive each link.
- `lib/common/config.{hpp,cpp}`: `AgentConfig` telemetry section + strict
  validation; `docs/examples` updated.
- `console/`: `monitor` verb in the CLI surface; the console render loop;
  local-instrumentation of the TCP console transport (latency history
  per issued seq, byte counters for throughput); audit writer stops
  short-audit-records for monitor-initiated polls.
- Tests: sampler (both OSes), config validation, matrix gate ordering with
  the new row, CLI monitor behavior, and an end-to-end health/monitor flow;
  no changes to envelope/session/framing tests beyond their invariants
  remaining intact.
