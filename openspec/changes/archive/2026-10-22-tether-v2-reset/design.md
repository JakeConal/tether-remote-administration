# Design

## Context

The v1 tree contains two independent Visual Studio solutions (`client/`, `server/`) sharing a drifted copy of `socket.{h,cpp}`, with behavior encoded as hardcoded constants: port `12345`, a system Gmail address, an admin email list in source, and an OAuth refresh token in a plaintext file. The relay polls the Gmail inbox (tight inner loop, no backoff), forwards one command per request over unauthenticated TCP, streams text/file responses over an ad-hoc `int`-length framing, and emails results back. There is no version control, no CMake, no CI, no tests, no logging. See `proposal.md` for the full motivation; this document fixes the v2 architecture those facts rule out.

Constraints carried forward:
- C++17 (matches existing usage; no reason to raise it mid-reset).
- Windows behavior must not regress: the Windows agent keeps full capability.
- The Gmail path remains explainable as the original coursework idea, demoted to an adapter.
- `report/` stays untouched as historical course documentation.

## Goals / Non-Goals

**Goals:**
- A single portable codebase producing `tether-console` and `tether-agent`, buildable and testable on Windows and Linux by CI.
- A versioned, authenticated, transport-agnostic command protocol with sequence-correlated results and timeouts.
- The full 19-command set preserved as first-class behavior, with a per-OS capability matrix instead of silent feature gaps.
- An engineering platform that can be shown as a portfolio artifact: git history that displays the transformation, CMake, CI matrix, tests at the protocol boundary.

**Non-Goals:**
- No GUI or dashboard; the operator surface is the CLI (deliberately chosen over TUI/GUI).
- No Wayland capture/input parity on Linux; X11-only, documented as a limitation.
- No stealth, persistence, or anti-analysis features of any kind; this is a documented, authorized-use lab tool.
- No multi-console clustering or central server deployment model; one console per operator.
- v1 protocol compatibility: broken by design; the Gmail adapter speaks protocol v2 only.

## Decisions

### D1 - Topology: agents dial out to the console

```
+------------------+   persistent session (agent dials out)   +---------------+
| tether-console   |<========================================>| tether-agent  |
| (operator side)  |     HMAC handshake, framed messages      | (target side) |
+------------------+                                          +---------------+
```

Rationale: outbound connections from the agent survive NAT/firewalls that kill inbound connects (v1 required reaching port 12345 on the target). It also aligns naming with convention and makes multi-agent a registry concern rather than a topology one. Alternatives considered: console-dials-agent inbound (v1 direction; requires port-forwarding and keeps the inverted naming), and Gmail-only (see D2).

### D2 - Transports: TCP primary, Gmail adapter behind one interface

A `transport` interface with two implementations: direct TCP sessions (primary, interactive latency) and a Gmail polling adapter (slow path, minutes-scale latency, files as MIME attachments). Both present equivalent command/result semantics; the protocol never embeds transport assumptions. Rationale: keeps the coursework identity usable for demonstrations while removing Gmail-as-C2 fragility (OAuth dance, poll races, 25 MB attachment cap) from the core path. Alternatives considered: Gmail-primary (caps everything: multi-agent, streaming, UI responsiveness), Gmail-retired (loses the original story the owner wants to keep).

### D3 - Protocol v2: versioned envelope with sequence correlation

```
+---------+---------+---------+----------+--------------------------+
| 1B ver  | 1B type | 4B seq  | 4B len   | payload (JSON | bytes)  |
+---------+---------+---------+----------+--------------------------+
```

- `type` enum: `HELLO`, `CHALLENGE`, `AUTH_OK`, `CMD`, `RESULT_TEXT`, `FILE_META`, `FILE_CHUNK`, `FILE_END`, `CANCEL`, `PING`/`PONG`, `ERR`.
- `seq` correlates every result frame to the command frame that caused it - this single field removes the v1 email/command race and makes multi-session correctness free.
- Payloads: JSON for control and command frames (human-debuggable, `nlohmann-json` already in the tree); raw bytes for file chunks.
- Per-command timeouts plus `CANCEL` (a hung webcam capture can never wedge a session) and `PING`/`PONG` keepalives.

Rationale for JSON over alternatives: line-based text limits structured payloads (paths, options) and drifts into ad-hoc parsing (v1's `pop_back()` CRLF hacks); protobuf/byte-struct is fast but kills debuggability with a netcat and adds toolchain weight for no scale need. Envelope endianness: little-endian, fixed per protocol version (both v1 sides already ran little-endian; removal of native-`int` assumptions is what matters, not an exotic endianness story).

### D4 - Authentication: HMAC challenge-response over a pre-shared token

At agent install, a token is provisioned into agent config. Handshake: console sends `CHALLENGE` (nonce); agent answers with HMAC of the nonce; console verifies and issues `AUTH_OK`. No shared secret ever crosses the wire, and the console side never trusts "first connector wins" (the v1 hole). Alternatives considered: plaintext token at connect (trivially replayable), mutual TLS with certificates (heavier operational story than a personal lab tool needs; revisit if v2 ever leaves LAN/VPN scale). `chacha20poly1305`-style full session encryption is a possible later add - deliberately deferred, an explicit design boundary, not an oversight.

### D5 - Operator surface: composable CLI

Verb-style CLI (`tether agents`, `tether exec <agent> <command>`, `tether shot <agent> > shot.png`) with streaming output to stdio. The shell is the UI; redirection, piping, and scripting come from Unix semantics instead of bespoke TUI machinery. Alternatives considered: rich TUI (nice demo, high machinery cost), GUI/dashboard (largest cost, deferred).

**Amendment (2026-10-21, discovered during the task 7.5 demo verification).** The built runtime shape is one invocation = one verb with its own transient listener, while the agent side (per D1) is an always-running dial-out loop on a bounded backoff (500 ms doubling to a 10 s cap). Left unbudgeted, the two never meet in real binaries: a verb's listener exists only for milliseconds, and the registry check inside `exec`/`cancel`/listing is instant — every real invocation refused `unknown agent` (proven live: 12 consecutive refusals over a running agent). The session/verb behavior itself was already fully validated through the in-process integration shape (Phase 5/6 harnesses pre-dial the agent). Resolution: each console invocation now pumps a bounded dial-in wait (`agent_dial_wait_ms`, default 10 s = one full agent backoff cycle; 0 disables), letting the agent's next dial register and be served inside the invocation, and expires into the unchanged unknown-agent refusal otherwise. Recorded as the "One-shot console invocations await the agent's bounded dial" requirement in the operator-console delta; no D1-D9 decision is revoked — this closes the gap between D1's dial-out loop and D5's one-verb-per-invocation shape.

### D6 - Cross-platform command layer via a capability matrix

`syscalls` becomes an interface layer (`IProcessControl`, `IFileSystem`, `IPowerControl`, `IScreenCapture`, `ICameraCapture`, `IInputControl`, `IAppLauncher`) with per-OS implementations and a declared capability matrix:

| Command family | Windows | Linux |
|---|---|---|
| process list/start/stop | `tasklist`/`taskkill`/Win32 | `/proc` inspection, `pgrep`/`kill` |
| files (ls/delete/move/copy) | Win32 | `std::filesystem` |
| shutdown/restart | Win32 | logind/DBus |
| screenshot | GDI | X11 grab (Wayland: documented gap) |
| webcam | OpenCV | OpenCV |
| keylogger / keyboard lock | `GetAsyncKeyState` / `BlockInput` | X11 grabs (Wayland: documented gap) |
| app list/start/stop | Start-menu `.lnk` + ShellAPI | `.desktop` entries |

Rationale: the matrix replaces silent feature holes with declared behavior - commands outside an OS's capability return a structured "unsupported on this platform" result instead of failing mysteriously. Alternatives considered: Windows-only hardwired (cheapest, forecloses the portfolio story), full Wayland parity (not achievable for global input capture; X11 is the deliverable). Avoid shell-splicing where a structured API exists (v1's `system("start " + input)` injection dies here; process launches take argv arrays).

### D7 - Repository and build: one repo, four targets, history-first

Layout: `console/`, `agent/`, `lib/{protocol,transport,syscalls}`, `docs/`, root `CMakeLists.txt` with vcpkg manifests per target (`opencv2` only where used). GitHub Actions matrix (windows-latest, ubuntu-latest): configure, build, test. The founding commit preserves the v1 tree verbatim; modernization proceeds as visible commits. Status: the directory is not yet a git repo, so this is a plain `git init` + snapshot commit + `v1.0-final` tag before any restructuring.

### D8 - Configuration and observability replace hardcoded values

Config files (console: `config.yaml`/JSON with listen address, port, token store location; agent: `agent.toml`/JSON with console address, agent name, token) replace baked-in constants; secrets stay out of the repo. Structured logging with levels on both sides replaces bare `cerr`; every command, result, auth attempt, and transport event logs with session and seq context.

### D9 - Phased implementation (each phase is a follow-up change)

```
P1 repo/build rescue        git init, snapshot, CMake skeleton, CI matrix (spec-less, tooling)
P2 structure                shared libs, console/agent targets, config+logging in place
P3 protocol v2              envelope, handshake, transports seam; test suite lands here
P4 transports               TCP transport live; Gmail adapter follows
P5 commands + matrix        capability layer, per-OS syscalls, background job lifecycle
P6 docs/demo                README, protocol spec, authorized-use note, demo script
```

Rationale: P1 makes everything reviewable before engineering; P3 is where correctness gets its tests; P5 is the largest surface and therefore last into a stable skeleton; P6 last so documentation describes reality. Each phase change defines its own spec deltas for the capabilities named in the proposal (`secure-session`, `command-transport`, `remote-commands`, `operator-console`).

## Risks / Trade-offs

- [Wayland regresses Linux capture commands] -> Capability matrix gates them; structured "unsupported" results; X11 documented as the scope boundary.
- [Gmail adapter inherits OAuth fragility + 25 MB limits] -> Accepted by design: slow path only, never the demo-critical route; poll interval with exponential backoff on API errors, credential file read from config path outside the repo.
- [Full cross-platform capture code is the largest single cost] -> D6's interface layer lets Windows implementations land first (parity with v1) while Linux implementations phase in per command family; CI matrix makes each step visible.
- [Deferring session encryption] -> HMAC tokens resist impersonation but frames remain readable in transit; acceptable for LAN/VPN lab scale, documented boundary, revisit before any non-personal use.
- [Porting MSBuild muscle-memory habits into CMake] -> CI matrix from P1 treats Linux as a first-class breakage signal instead of a surprise at the end.
- [Umbrella scope drift] -> Each design decision here is binding; a phase that wants to change D1-D8 must update this design first, not silently fork it.

## Migration Plan

1. `git init`; founding commit preserves current v1 tree byte-for-byte; tag `v1.0-final` (rollback anchor: any v1 state remains retrievable).
2. Phases P1-P6 land as sequential changes against this design; each phase's change is validated and built by CI before the next starts.
3. v1 directories (`client/`, `server/`) are removed from the working tree only after P3 proves protocol v2 end-to-end; remnants stay reachable through history and `v1.0-final`.
4. Gmail credentials and `admin.txt` never enter any technology from v2 config; expected state after P6: no hardcoded addresses anywhere in source.

## Open Questions

- Linux screenshot/input-capture implementation form: own X11 code (libX11/XGetImage) vs shelling to existing utilities (`import`, `xwd`) - settle in P5's change with a benchmark note.
- Public-repo readiness details: final license choice, README tone, and whether the course report is linked or kept private - owner decision, deferred to P6 where it has no retroactive architecture impact.
- Test framework pick (Catch2 vs doctest) is confined to P1/P2 tooling choice; both satisfy the protocol-boundary testing requirement.
