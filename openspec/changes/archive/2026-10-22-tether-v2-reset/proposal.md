# Proposal

## Why

The current codebase is a semester-3 coursework build of a Gmail-commanded remote administration tool (admin emails commands to a system inbox, a relay polls the inbox and forwards them over raw TCP to the machine being driven). It works, but it is held together by assumptions that cap it: no version control, build tooling, CI, or tests; the control TCP port accepts any connected caller unauthenticated; the Gmail polling loop spins hot on non-admin mail; command/result correlation across email is racy; output parsing depends on English locale; and the roles are named backwards from convention ("client" = relay, "server" = target daemon). To become a professional, portfolio-grade project (and to support its feature ambitions), the architecture needs a deliberate reset, not incremental patches. This change is that reset: it captures the v2 architecture decisions the implementation phases will build against.

## What Changes

- **BREAKING** - Both v1 programs are replaced by a new project named **tether** consisting of two runtimes: an operator `console` (CLI) and a dialing `agent` daemon. v1 sources are preserved as an archived snapshot and the LaTeX course report remains as historical documentation.
- **BREAKING** - Connection direction inverts: agents dial out to the console and keep a persistent authenticated session (NAT/firewall-friendly); the v1 requirement that the target machine accept inbound connections disappears. Role naming aligns with convention (console = operator side, agent = target side).
- New **protocol v2**: versioned message envelope `[ver | type | seq | len]` with JSON control payloads and raw binary file chunks; sequence-based command/result correlation (solves the v1 email race); `CANCEL`/`PING` message types; per-command timeouts.
- **BREAKING** - Session authentication becomes mandatory: HMAC challenge-response over a pre-shared token established at agent install. v1's open TCP channel is retired.
- New operator console: composable streaming CLI verbs (`tether agents`, `tether ps`, `tether shot <agent> > shot.png`), replacing the ANSI-art menu UI on the relay.
- The full 19-command set is kept as first-class, mapped to a per-OS capability matrix: Windows capabilities on the Windows agent, X11-complete capabilities on the Linux agent, Wayland explicitly documented as a limitation (input capture and input locking are not attempted on Wayland). Authorized-use framing governs all documentation.
- Gmail is repositioned as an optional slow-path transport behind the same command interface (a polling adapter), not the primary channel.
- Engineering platform modernized: git repository initialized (v1 snapshot as the founding commit), CMake + vcpkg manifests replacing MSBuild-only solutions, GitHub Actions CI matrix (Windows + Ubuntu), one shared codebase (`console/`, `agent/`, `lib/{protocol,transport,syscalls}`) ending the duplicated `socket.cpp`, config files replacing hardcoded ports/addresses/admin lists, structured logging, and a test suite anchored at the protocol and transport boundaries.
- Sequencing constraint: implementation proceeds through phase-sized follow-up changes (repo/build rescue, structure, protocol, transports, commands, docs), each planned against this umbrella design.

## Capabilities

### New Capabilities

- `secure-session`: durable session behavior - agent identity at connect time, dial-out connection direction, HMAC challenge-response handshake, envelope framing with sequence correlation, and session robustness (timeouts, cancel, ping/pong).
- `command-transport`: pluggable transport semantics - the TCP transport and the Gmail polling adapter must present equivalent command/result semantics behind one transport interface.
- `remote-commands`: the remote execution surface - the 19-command set with per-OS capability behavior, streamed text and file results, and lifecycle-managed background jobs (keylogger, webcam).
- `operator-console`: the operator surface - session registry of dialed-in agents, CLI command verbs, streaming results to stdio, and confirmation gates for destructive operations.

### Modified Capabilities

(none - the project has no existing specs)

## Impact

- **Code**: near-total rewrite of both v1 programs into four CMake targets: `tether-console`, `tether-agent`, and shared libraries `tether-protocol`, `tether-transport`, `tether-syscalls`. The v1 tree (`client/`, `server/`) survives only as the archived snapshot commit; the duplicated `socket.{h,cpp}` pair is eliminated in favor of the shared protocol stack.
- **Dependencies**: `nlohmann-json` and `cpr` retained (Gmail adapter), `opencv2` retained (webcam). New: a C++ test framework (Catch2 or doctest) and portable socket I/O (Asio or similar) for the cross-platform transport layer. No GUI/TUI framework (CLI was chosen).
- **Build/CI**: MSBuild `.sln` files and per-side vcpkg usage are replaced by a root CMake build with vcpkg manifests per target, plus a GitHub Actions matrix (windows-latest, ubuntu-latest) that builds and tests both platforms.
- **Behavioral risk areas**: Linux input capture and keyboard locking are only achievable via X11 (impossible under Wayland's security model) - the capability matrix caps Linux capture features at X11-complete and documents the limitation rather than attempting parity.
- **Docs**: the README, architecture notes, and authorized-use statement are produced in a later docs phase once the implementation is real enough to describe honestly.
