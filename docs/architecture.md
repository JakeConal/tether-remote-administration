# tether architecture

How the system is put together, end to end: the two runtimes, the layers
between them, the lifecycle of a single command, the thread model, and the
design decisions that shaped the code.

---

## 1. The shape of the system

tether is **two runtimes and one protocol**.

```
   operator seat                          target machines
┌────────────────────────┐              ┌──────────────────────────────┐
│  tether-console        │◄─── dials ───│  tether-agent                │
│                        │              │                              │
│  one invocation        │  authenticated│  one long-running process    │
│  = one verb            │   session    │  dials out forever           │
│                        │              │                              │
│  CLI11 parse           │              │  AgentSession                │
│  → registry lookup     │              │  → CommandDispatcher         │
│  → confirm gate        │              │  → 7 capability families     │
│  → TcpOperConsole      │              │                              │
│  → TcpConsoleTransport │              │  process · file · power      │
│  → ConsoleSession      │              │  capture · camera · input    │
│  → AgentRegistry       │              │  app                         │
│  → AuditStore          │              │                              │
└────────────────────────┘              └──────────────────────────────┘
```

**The agent dials out; nothing listens on the target.** The console binds one
port and accepts the agent's outbound connection. This is the single most
important structural decision in the project: it makes tether work through
NAT and firewalls without asking the operator to open an inbound port on a
machine they administer remotely.

The console is **not a daemon**. One `tether-console` invocation starts its
own listener, serves exactly one verb, and exits. The agent is the long-running
half. Because of that asymmetry, the console's session registry only ever
describes sessions established *during that invocation* — and the console
pumps a bounded **dial-in wait** (`agent_dial_wait_ms`, default 10 s) for the
agent's next dial to land.

---

## 2. Layers

Six static libraries and two executables. Each layer only knows about the
layer(s) beneath it.

```
  tether-console (exe)          tether-agent (exe)
        │                              │
        ▼                              ▼
  tether-cli                  tether-command-handler
  (console/cli, tcp_console,  (agent/command_handler)
   audit)                            │        │
        │                            ▼        ▼
        ▼                    tether-syscalls  tether-transport
  tether-transport          (matrix + 7      (transport, tcp, gmail,
  (transport, tcp, gmail,    families +       gmail_cpr)
   gmail_cpr)                jobs)                 │
        │                                           │
        ▼                                           │
  tether-protocol ◄─────────────────────────────────┘
  (envelope, payloads, handshake, session, bytes)
        │
        ▼
  tether-common
  (config, logging, base64)
```

`lib/syscalls` is deliberately self-contained: it includes no common, protocol,
or transport header, so the command surface cannot reach the wire or the
console by accident.

| Layer | Target | Responsibility | Must not know about |
| --- | --- | --- | --- |
| `tether-common` | `tether-common` | Config loading, structured logging, base64 | Anything tether-specific |
| `tether-protocol` | `tether-protocol` | Framing, payload codec, HMAC handshake, session state machines | Sockets, OS APIs, the command set |
| `tether-transport` | `tether-transport` | Byte streams, TCP, the Gmail slow path, the transport interface | The command set, OS capability APIs |
| `tether-syscalls` | `tether-syscalls` | The closed command set, the capability matrix, the OS-facing families | The wire, the console, CLI details |
| `tether-cli` | `tether-cli` | Verbs, exit codes, stdio discipline, the audit trail | OS capability APIs |
| `tether-command-handler` | `tether-command-handler` | Mapping a `Cmd` frame onto a family call and its reply pieces | stdio, exit codes |

The dependency edges are all `PUBLIC`/`PRIVATE`-declared in CMake; nothing uses
file globbing; there are no cycles. Note that `tether-cli` also links
`tether-protocol` directly (it speaks `protocol::Cmd` and
`protocol::CommandView`), and `tether-command-handler` links `tether-syscalls`
directly — the diagram above shows the main flows, not every edge.

---

## 3. The full command lifecycle

This is the path a single `exec` takes, from argv to exit code.

```
 1. ARGV          tether-console --config c.json exec lab-1 ls /tmp
                     │
 2. PARSE          CLI11 parses the verb and the positionals.
                   Leftover tokens are COMMAND ARGUMENTS (data), not flags.
                     │
 3. VERB GATE      Unknown verb → exit 2, before any config is read,
                   any console is built, anything is audited.
                     │
 4. AUDIT STORE    Built before the console exists, so a later failure
                   is still auditable.
                     │
 5. CONSOLE        TcpConsoleTransport::start() → bind → listen → accept.
                   TcpOperConsole::set_agent_dial_budget_ms(config).
                     │
 6. DIAL-IN WAIT   TcpOperConsole::wait_for_dial() pumps the transport
                   (io_context::poll + per-session pump) until the
                   targeted identity registers, or the budget expires.
                   Budget expiry collapses into the unknown-agent refusal.
                     │
 7. AGENT LOOKUP   has_agent(identity). Unknown → exit 3.
                   This precedes the confirmation gate.
                     │
 8. CONFIRM GATE   Destructive verb (delete / shutdown / restart) without
                   --yes and without a scripted "y" → exit 4.
                   NOTHING IS SENT.
                     │
 9. DISPATCH       ConsoleSession::start(cmd) allocates a seq (from 1),
                   records a tracker, sends the CMD frame.
                     │
10. WIRE           10-byte header + JSON payload. The agent's
                   AgentSession receives it, registers the seq as
                   in-flight, and invokes the command handler.
                     │
11. DISPATCHER     CommandDispatcher::dispatch(verb, args) runs the
                   five-gate chain (see §4) and calls the family.
                     │
12. REPLY          The Outcome becomes wire pieces:
                     - ok + small text  → RESULT_TEXT
                     - ok + file        → FILE_META, FILE_CHUNK*, FILE_END
                     - not ok            → ERR {code, message}
                     │
13. STREAM         stdout carries result bytes verbatim.
                   stderr carries status and errors.
                   File chunks are byte-exact; a short delivery is a
                   failure, not a truncated success.
                     │
14. AUDIT          One "command" record (executed / refused / failed)
                   plus any "job_event" records for background jobs.
                     │
15. EXIT           0 ok · 1 failure · 2 usage · 3 unknown agent
                   4 refused confirmation
```

Two details worth internalising:

- **The console never injects the `confirm` token.** The agent requires the
  literal string `confirm` as the first argument of a destructive command.
  The console's own gate is separate and comes first, and it covers a
  **narrower** set of verbs (`delete`, `shutdown`, `restart`) than the agent
  gate does (`delete`, `move`, `copy`, `shutdown`, `restart`). For `move` and
  `copy` only the agent gate applies — the console dispatches them and the
  agent refuses them with `missing_confirmation`.
- **Arguments are data, always.** They travel as an argv-style JSON array and
  are never re-joined into a shell line. `; | & > <` inside an argument are
  file names, not shell.

---

## 4. The dispatcher: five ordered gates

`CommandDispatcher::dispatch()` (`lib/syscalls/matrix.cpp`) refuses in this
order. Each gate runs *before* any capability is touched.

| # | Gate | Refusal code | Message shape |
| --- | --- | --- | --- |
| 1 | Verb is in the closed set? | `unknown_command` | `unknown command '<verb>'` |
| 2 | Is the verb supported on this platform? | `unsupported_platform` | `'<verb>' is not supported on <os> (family <family>): it is outside this platform's matrix` |
| 3 | Is the family wired in this build? | `unsupported_platform` | `... the family is not implemented in this build yet` |
| 4 | Destructive ⇒ `argv[0] == "confirm"`? | `missing_confirmation` | `'<verb>' is destructive and requires 'confirm' as its first argument; nothing was executed` |
| 5 | Execute the family | the family's own code | — |

The ordering is load-bearing: gate 3 precedes gate 4, so in a build without the
power family, `shutdown` refuses as `unsupported_platform` rather than
`missing_confirmation`. `tests/matrix_test.cpp` pins that ordering.

---

## 5. The capability matrix

`kCommandSet` in `lib/syscalls/matrix.cpp` is the closed set of 19 verbs across
7 families. Each row is pure data:

```cpp
struct CommandSpec {
    const char* verb;     // the wire command word
    Family family;        // Process | File | Power | Capture | Camera | App | Input
    bool destructive;     // the agent-layer confirmation gate applies
    bool on_windows;
    bool on_linux;
};
```

In the shipped table **every cell of every family × platform pair is
supported** — 19 verbs on both Windows and Linux. Platform gaps are not
expressed as matrix rows; they are raised at runtime from inside the family
as a structured `unsupported_platform` refusal (for example, the input family
on a Wayland-only host).

The five destructive rows are `delete`, `move`, `copy`, `shutdown`, `restart`.
`move` and `copy` are destructive because they replace their destination.

| Family | Verbs | Windows implementation | Linux implementation |
| --- | --- | --- | --- |
| Process | `list` `start` `stop` | Toolhelp32 snapshot, `CreateProcessW`, `TerminateProcess` | `/proc`, `fork`+`execvp`, `SIGTERM`→`SIGKILL` |
| File | `ls` `delete` `move` `copy` | `std::filesystem` (one implementation for both OSes) | `std::filesystem` |
| Power | `shutdown` `restart` | `ExitWindowsEx` + `SeShutdownPrivilege` | logind over `dlopen`'d sd-bus |
| Capture | `screenshot` | GDI `BitBlt` + `GetDIBits` | X11 `XCopyArea` + `XGetImage` |
| Camera | `start_webcam` `stop_webcam` | OpenCV `VideoCapture` (DirectShow/MSMF) | OpenCV `VideoCapture` (V4L2) |
| Input | `start_keylogger` `stop_keylogger` `lock_keyboard` `unlock_keyboard` | `WH_KEYBOARD_LL` hook, `BlockInput` | `XQueryKeymap`, `XGrabKeyboard` |
| App | `list_app` `start_app` `stop_app` | Start-menu `.lnk` via `IShellLinkW` | freedesktop `.desktop` entries |

See [command-reference.md](command-reference.md) for the argument contract of
each verb.

---

## 6. Background jobs

Two verbs — `start_webcam`/`stop_webcam` and `start_keylogger`/`stop_keylogger`
— are **managed background jobs**, not ordinary calls. They share one
`BackgroundJobRegistry` (`lib/syscalls/jobs.hpp`) with one named slot per
family (`"camera"`, `"input"`).

```
  start_webcam ──▶ registry.start("camera", worker)
                     ├─ slot occupied? → already_running, incumbent untouched
                     └─ slot free?     → worker on its OWN thread
                                         │
                                         ├─ publishes the latest frame
                                         └─ checks stop_requested() between steps
                                         │
  stop_webcam  ──▶ registry.stop("camera")
                     ├─ signal the worker
                     ├─ join it
                     └─ hand back the published deliverable
```

The worker owns its capture loop and never takes the session's locks;
deliverables cross threads through the handle's mutex. The session stays
responsive while a job runs — the job lives in the syscall layer, not in the
agent's event loop.

A duplicate start is refused with `already_running` and leaves the incumbent
untouched. A stop with no job yields `not_running`. A job that produced
nothing fails loudly (`execute_failed`) rather than reporting an apparently
successful empty deliverable.

---

## 7. The session state machines

Both peers implement the same four-state model (`lib/protocol/session.hpp`):

```
        Connecting ─────────────► Established ─────────────► Down (terminal)
             │                        │   ▲
             │ handshake failed       │   │ new session for the same identity
             ▼                        ▼   │
           Down                    Stale  (console side only)
```

| State | Meaning |
| --- | --- |
| `Connecting` | Handshake in progress. |
| `Established` | Authenticated. Commands are allowed. |
| `Stale` | Console-side only: a newer session for the same identity superseded this one. It is ineligible for commands and its timers are stopped. |
| `Down` | Terminal: refused, peer dead, stream error, or clean EOF. |

**Timers** (`SessionConfig`, all in milliseconds):

| Timer | Default | Effect |
| --- | ---: | --- |
| `command_timeout` | 30,000 | The command fails with error code `timeout`. **The session survives.** |
| `auth_timeout` | 10,000 | The handshake did not complete → the session goes down. |
| `keepalive_interval` | 10,000 | One `PING` per interval while established. |
| `keepalive_window` | 30,000 | No inbound frame for this long → the session goes down. |

Two invariants the tests pin:

- A failed, timed-out, or cancelled command **never** poisons the session.
- Every valid inbound frame is a life sign — not just `PONG`s.

---

## 8. Threading model

### The agent

The agent's own main thread does everything: connect, handshake, pump, dispatch.
There is **no thread pool and no work queue**. The pump cadence is 10 ms.

```
  agent main thread
    ├── establish_with_retries()   bounded backoff: 500 → 1000 → 2000 → 4000 → 8000 → 10000 ms (cap)
    ├── link->begin_handshake(now)
    ├── while (!down && !established) { pump(now); sleep 10 ms; }
    └── while (established)           { pump(now); sleep 10 ms; }
```

The backoff **resets on every successful connect**, so a flapping console
produces 500 ms retries rather than a 10 s cadence.

Background jobs add one thread each, but they live in the syscall layer
(`BackgroundJobRegistry::start` spawns a `std::thread` per slot).

### The console

The console has no thread of its own per invocation either. The transport is
**pump-driven**: `TcpConsoleTransport::pump(now)` runs
`io_context::poll()` (ready handlers only — never blocks), then pumps each
accepted session. `TcpOperConsole` drives it from a 2 ms sleep loop.

### Where the threads actually are

| Thread | Owner | Work |
| --- | --- | --- |
| Agent main | `agent/main.cpp` | Connect, handshake, pump, dispatch |
| Job thread | `BackgroundJobRegistry` | One per running background job |
| (Windows) hook thread | The job thread | The low-level keyboard hook is dispatched by its owning thread's message pump |

---

## 9. Transports

Both transports sit behind one interface (`lib/transport/transport.hpp`):

```cpp
class ITransport {
public:
    virtual std::uint32_t issue(const std::string& agent,
                                const protocol::Cmd& command,
                                const protocol::ResultHook& hook,
                                TransportMillis now) = 0;
    virtual void pump(TransportMillis now) = 0;
};
```

Two methods, an injected clock, and one rule: **no transport blocks waiting
for the network.** Blocking happens only inside an explicit `connect`/`accept`
call owned by the calling thread.

| | TCP | Gmail |
| --- | --- | --- |
| Role | Production | Optional slow path, off by default |
| `issue()` | Allocates a seq on a live session and sends `CMD` | **Always throws** — mail is the issuance mechanism, not the programmatic one |
| `pump()` | Accept completions + per-session pumps | One inbox poll per cadence interval |
| Failure model | `TransportError` | `TransportError` + exponential backoff on API errors |
| Results | On the session, correlated by seq | Reply mail to the requesting admin, correlated by `X-Tether-Seq` |

The Gmail adapter is a *consumer* of commands: it polls an inbox, applies an
acceptance rule (sender on the admin list **and** the adapter account in
`To`), parses the body with the protocol's own strict `CMD` decoder, routes it
through the same dispatcher, and mails the result back. A Gmail-enabled config
builds the adapter exactly once, and mail-issued commands ride the very same
TCP session stack as console-issued ones.

The shipped `tether-console` binary constructs the TCP transport directly;
the `gmail` section of the console config is parsed and validated but the
binary does not yet wire it in.

---

## 10. Authentication in one picture

```
  agent                                            console
    │                                                 │
    │  HELLO {agent_id, agent_name, protocol_version:2} │
    │────────────────────────────────────────────────►│
    │                                                 │  resolve the token
    │                                                 │  → unknown identity?
    │                                                 │    ERR auth_failed, go Down
    │  CHALLENGE {nonce}                              │
    │◄────────────────────────────────────────────────│
    │                                                 │
    │  decode nonce hex → 16 raw bytes                │
    │  HMAC-SHA256(token, 16 raw bytes) → 32 bytes    │
    │  CHALLENGE {nonce, hmac_response}               │
    │────────────────────────────────────────────────►│
    │                                                 │  recompute, compare
    │                                                 │  in constant time
    │  AUTH_OK {}                                     │
    │◄────────────────────────────────────────────────│
    │                                                 │
    │  Established — commands may flow                 │
```

The token **never crosses the wire** and is never logged. See
[protocol.md](protocol.md) for the byte-level walkthrough and
[security.md](security.md) for the threat model.

---

## 11. Design decisions and why

| Decision | Rationale | Where it lives |
| --- | --- | --- |
| Agent dials out | Works through NAT/firewalls without inbound ports on the target | `lib/transport/tcp.cpp` |
| One invocation = one verb | No console daemon to keep alive; the agent is the only long-running half | `console/main.cpp` |
| Strict payload shapes | Unknown fields are rejected so silent version drift is impossible | `lib/protocol/payloads.cpp` |
| Poisoning the stream on a framing violation | Alignment is unrecoverable; better a loud failure than a misread | `lib/protocol/envelope.cpp` |
| Arguments as argv arrays | Never re-join into a shell line; metacharacters are file names | `lib/protocol/payloads.cpp`, `lib/syscalls/process.cpp` |
| Two independent confirmation gates | The console refuses locally (nothing sent) for `delete`/`shutdown`/`restart`, *and* the agent refuses (nothing executed) for all five destructive verbs | `console/cli.cpp`, `lib/syscalls/matrix.cpp` |
| A closed 19-verb command set | The refusal of an unknown verb is structured and auditable | `lib/syscalls/matrix.cpp` |
| Capabilities behind interfaces + a shared job registry | Testable with fakes; one managed lifecycle for every background job | `lib/syscalls/*.hpp`, `lib/syscalls/jobs.hpp` |
| `std::filesystem` for both OSes in the file family | One implementation, one set of error codes, cross-platform parity | `lib/syscalls/file.cpp` |
| `dlopen` sd-bus instead of linking libsystemd | libsystemd becomes a *runtime* dependency; the build needs no systemd headers | `lib/syscalls/power.cpp` |
| Own-library X11 capture | Correct on composited roots (XWayland/WSLg), no external tool, ~2.3× faster than `scrot` | `lib/syscalls/capture.cpp`, [capture-notes.md](capture-notes.md) |
| `XQueryKeymap` instead of XRecord | XRecord needs libXtst and cannot honour the registry's bounded stop contract | `lib/syscalls/input.cpp` |
| `WH_KEYBOARD_LL` instead of `GetAsyncKeyState` | A real hook delivers events; polling cannot see keys pressed between polls | `lib/syscalls/input.cpp` |
| `XGrabKeyboard` holds the display open | The grab dies with the connection — the lock's scope is exactly the agent's session | `lib/syscalls/input.cpp` |
| doctest over Catch2 v3 | Single header, no compiled framework library on the critical path | [build.md](build.md) |

---

## 12. Where to look in the source

| Question | File |
| --- | --- |
| What can a command be? | `lib/syscalls/matrix.cpp` (`kCommandSet`) |
| How is a command dispatched? | `lib/syscalls/matrix.cpp` (`CommandDispatcher::dispatch`) |
| How is a refusal shaped? | `lib/syscalls/matrix.hpp` (`Outcome`), `lib/syscalls/errors.hpp` |
| How is a frame encoded? | `lib/protocol/envelope.cpp` |
| How is a payload validated? | `lib/protocol/payloads.cpp` |
| How is the handshake computed? | `lib/protocol/handshake.cpp` |
| How does a session transition? | `lib/protocol/session.cpp` |
| How does the console parse argv? | `console/cli.cpp` |
| How does the console write the audit trail? | `console/audit.cpp` |
| How does the agent loop? | `agent/main.cpp` |
| How does a Cmd frame become a family call? | `agent/command_handler.cpp` |
| How does the socket work? | `lib/transport/tcp.cpp` |
| How does the mail adapter work? | `lib/transport/gmail.cpp`, `lib/transport/gmail_cpr.cpp` |
| How is a config file parsed? | `lib/common/config.cpp` |
| What are the OS-specific bits? | `lib/syscalls/{process,file,power,capture,camera,input,app}.cpp` |

More detail in [developer-guide.md](developer-guide.md).
