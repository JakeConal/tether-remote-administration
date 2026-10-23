# Design

## Context

tether is two runtimes and one strict protocol: the agent is the long-lived
half (single main thread, dial-out loop, 10 ms pump); the console is
one-invocation-one-verb with a bounded dial-in wait; the wire carries exactly
12 `MsgType` values and both session state machines treat unexpected frame
types as `Down`. Results already travel as text and are parsed by scripts.
See `proposal.md` for why this change adds telemetry at all.

The design below rests on the exploration conclusions: the hybrid fork
(pull-shaped wire over a windowed sampler), a `monitor` invocation as "one
verb served until interrupted", and no queue machinery.

## Goals / Non-Goals

**Goals:**

- A standing, on-demand view of host health and agent link history that is
  honest about staleness (`sample_age_ms`, window coverage).
- Zero wire-protocol surface change: no new `MsgType`, no envelope/session
  edits, no handshake negotiation.
- Same observable behavior on both OSes and over both transports.
- Sampling cost so small it disappears against the existing 10 ms pump.

**Non-Goals:**

- No push channel, no bounded queue, no sampler thread, no drop accounting —
  the "queue depth / dropped samples" vocabulary from the original request is
  deliberately reinterpreted into sample age and window coverage.
- No multi-agent monitor layout (follow-up once the single-target verb is
  proven).
- No telemetry persistence on the agent (the window is memory, cleared on
  process restart), no history export file.
- No new console configuration section (the monitor cadence is fixed).

## Decisions

### D1: Snapshot skill, not streaming — `health` rides `Cmd`/`RESULT_TEXT`

Health travels as an ordinary command on the existing correlation path. The
result text is a single-line JSON document (hundreds of bytes, far under the
1 MiB control bound).

*Why this over a `TELEMETRY(13)` push channel:* a 13th `MsgType` must be
embraced by `ConsoleSession::handle_frame` and `AgentSession::handle_frame`
or any mixed-version pairing goes `Down` ("unexpected frame while
established"); it also needs a strict payload codec branch, ring/drop
accounting, and would be absurd over the Gmail cadence (one mail per sample).
The push feature is justified only if the streaming pipeline itself is the
product — the recorded decision from exploration is that the end metrics are
the product.

*Hybrid part:* sampling still happens at a fixed interval independent of
polling, and the window aggregates (cpu avg/max, mem max over the window) make
spikes that fall between two polls visible in any snapshot. **Alternative:** a
pure pull (sample only when `health` arrives) — rejected: it loses spike
visibility and degrades freshness under command load.

### D2: Health capability lives in `tether-syscalls` as `Family::Telemetry`

- `enum class Family { ..., Telemetry }` and `family_name` returns
  `"telemetry"` (refusal text).
- One `kCommandSet` row: `health`, non-destructive, Windows + Linux, so all
  five dispatch gates apply unchanged and `matrix_test` ordering holds.
- New interfaces behind a factory pair, matching the other capability
  surfaces:
  - `IHealthHostProbe` (OS-backed, injected): one call returns the instant
    host sample (cpu percent delta window, memory used/total, process count,
    thread count). Windows: `GetSystemTimes` diff against the previous tick +
    `GlobalMemoryStatusEx` + Toolhelp snapshots
    (`TH32CS_SNAPPROCESS`/`SNAPTHREAD`); Linux: `/proc/stat` diff,
    `/proc/meminfo`, and the `/proc/loadavg` 4th field (total threads) with a
    `/proc` directory walk for the process count.
  - `HealthState` (pure, testable): owns the rolling window, uptime/reconnect
    counters, staleness bookkeeping (config injected), and is the only place
    window math lives. `IHealthHostProbe` is faked in tests; OS
    implementations stay thin.

*Why syscalls and not the agent layer:* the command set is closed at the
dispatcher; health becomes a citizen of that set with no special-casing, and
the "gate 3 precedes gate 4" test discipline extends to one more row.

### D3: Sampling ticks inline in the agent's existing main thread

The agent's established-session loop tracks the next due tick (`interval_s`
from config). On each 10 ms pump cycle, when a tick is due it samples and
pushes into `HealthState`. There is **no thread and no queue**.

A command that occupies the main thread delays ticks; `HealthState` records
the real wall time, so `sample_age_ms` grows truthfully and the window simply
receives fewer ticks for that span. That honesty is a spec requirement
("Snapshot freshness is honest about staleness"), not an accident.

*Alternative rejected:* a `BackgroundJobRegistry`-style sampler thread with a
bounded ring. It introduces exactly the machinery this system doesn't want
(threads crossing into session state, drop counters that only mean something
when drops happen, and memory/complexity), and nothing observable improves:
the console can't poll faster than the link anyway.

### D4: Link counters live in the dial loop, not the session

`agent/main.cpp`'s `for(;;)` loop keeps `started_at` (steady clock) and
`reconnects` count, incrementing on every successful `establish_with_retries`.
Sessions and links are rebuilt per dial; counters must outlive them. The
`telemetry` family handler is constructed with references to these (plus
`HealthState`) during `install_commands` wiring, refreshed per session
establishment.

### D5: Snapshot JSON contract

Single-line snapshot shape (field names are design-fixed; strict JSON):

```json
{"uptime_seconds":3601,"reconnects":2,
 "latest":{"cpu_percent":12.5,"memory_used":6291456,"memory_total":16777216,
           "processes":214,"threads":3406,"sampled_at_ms":1020304050},
 "window":{"span_seconds":60,"ticks":59,
           "cpu_percent_avg":9.8,"cpu_percent_max":43.0,
           "memory_used_max":8388608},
 "sample_age_ms":412,"interval_s":1}
```

`sampling failure` maps to a structured error code (`sample_unavailable`),
keeping the fail-loudly rule from the background jobs precedent. Parameter
arity is checked by the family: any args → structured refusal.

*Alternative:* an opaque text rendering — rejected: the console and scripts
both parse the same document; the protocol's strict-shape ethos applies to
result payloads just as it does to control frames.

### D6: Monitor is a console invocation shape, not a protocol feature

`cli.cpp` gains a `monitor` verb (target agent; `--json` flag). After the
ordinary dial wait (unbounded for this verb by loop shape), `console/main.cpp`
runs the observe loop at a fixed 1 s cadence:

1. issue `health` through the existing command path (so confirmation gates,
   seq correlation, and registry behavior are untouched),
2. render one line (human) or one JSON object (JSONL) per cadence,
3. while no live session: render the unavailable line, keep looping.

`SIGINT`/`SIGBREAK` is handled cooperatively: the handler only sets a flag;
the loop exits at the next iteration with exit code 0 (the "interrupted"
code). A first poll refused as `unknown_command` (old agent) terminates the
monitor immediately with the structured refusal — a fast, clear upgrade
signal rather than an hour of identical errors.

### D7: Console-side stats are measured locally, no agent involvement

- **Latency:** per issued `health` seq the console records issue time;
  the terminal result hook stamps completion. Monotonic buffer + p50/max.
- **Throughput:** counting wrappers around the accepted session's
  `IByteSource`/`IByteSink` (console layer only); throughput = counter delta
  over the recent interval. No protocol or agent change.

*Alternative:* agent-reported values in the snapshot — rejected: doubles the
payload and duplicates what the consumer already sees; link truth belongs to
the link's local end.

### D8: Audit = one record at monitor completion

The existing audit trail is append-only JSONL written on command issue. The
monitor writes ONE record when the invocation ends: command `monitor`,
parameters (target agent), observed duration, and the end state (interrupted
/ target refused / error). Internal polls are routed below the CLI's audit
emit point. `exec <agent> health` stays a normal audited command.

*Alternative:* a start+stop record pair — rejected: the delta spec pins "one
audit record covering the invocation"; two records would rewrite that shape.
A console crash mid-monitor leaves no record: accepted lab-tool limitation
(see risks).

### D9: Config: one optional section, strict validation

`agent.json` gains:

```json
"telemetry": { "interval_s": 1, "window_s": 60 }
```

- Section absent → defaults apply (backward compatible).
- `interval_s < 1`, `window_s < interval_s`, wrong types, or extra keys →
  `ConfigError` naming the field (`telemetry.interval_s` etc.), matching the
  loader's strictness (unknown fields rejected elsewhere in this file).
- Example `agent.example.json` gains the section.

## Risks / Trade-offs

- [Sampling tick cost (Toolhelp snapshots on Windows, /proc walk on
  Linux)] → one tick per second on the main thread; both probes measure in
  single-digit milliseconds at lab scale; the tick runs only between pump
  cycles. Probe benchmarks are part of tasks and any regression parks the
  sampler behind the existing gate-3 wiring (family isolated).
- [First tick per session has no CPU delta baseline] → `HealthState` marks
  the first tick as baseline-only; the snapshot reports it via
  coverage/sample_age fields instead of a fake 0%.
- [Monitor record lost on a console crash mid-monitor] → accepted; per-invocation
  record-at-completion keeps the trail truthful without mutation. The audit
  requirement's existing scenarios remain intact.
- [`SIGINT` plumbing differs by platform] → cooperative flag set in a
  handler, exit at next loop iteration; handler does no I/O. Behavior pinned
  by CLI tests on both OSes.
- [Monitor against an old agent] → structured `unknown_command` on first poll
  terminates cleanly with a distinguishable exit; no stream poisoning (the
  refusal is an in-band `Err`).
- [Window drift over long runs] → window is tick-indexed, not
  wall-clock-indexed; after a long command the window holds fewer ticks for
  that span — coverage field exposes it. Aggregates remain sound.

## Migration Plan

Ships as a co-built pair (single repo, v2 protocol unchanged): no wire
migration, no capability removal, config section is optional with defaults.
Rollback = stop using `health`/`monitor`; the old command surface is
byte-identical. Docs updated in the same pass (architecture thread table,
agent-thread-model narrative, command-reference entry, operations section).

## Open Questions

None. Deferred (non-blocking, out of scope here): multi-agent monitor
columns; per-core CPU breakdown; monitor cadence as a config knob.
