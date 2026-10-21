# tether documentation

Official documentation for **tether** — an authorized-use remote administration
lab tool made of two runtimes: an operator **console** and a target-side
**agent** that dials out and holds an authenticated session.

This set is written for two audiences:

- **Developers working on tether** — how the code is laid out, how to build it,
  how to extend it, and which invariants must not be broken.
- **Readers learning the project** — how the pieces fit together, what the
  protocol looks like on the wire, and how the command surface behaves.

Every factual claim here is pinned to the source tree. Where a document
describes behaviour, the behaviour is enforced by a test (see
[testing.md](testing.md)) or by a code path that is named inline.

## Reading paths

| If you want to… | Read |
| --- | --- |
| Build and run it for the first time | [getting-started.md](getting-started.md) |
| Understand the system end to end | [architecture.md](architecture.md) |
| Change the code | [developer-guide.md](developer-guide.md) |
| Look up a command or an exit code | [command-reference.md](command-reference.md) |
| Configure a console or an agent | [configuration.md](configuration.md) |
| Call the libraries from your own code | [api-reference.md](api-reference.md) |
| Run, extend, or debug the tests | [testing.md](testing.md) |
| Reason about the security model | [security.md](security.md) |
| Deploy it and troubleshoot it | [operations.md](operations.md) |
| Implement another peer | [protocol.md](protocol.md) |

## Document map

| Document | What it covers |
| --- | --- |
| [architecture.md](architecture.md) | Layering, the two runtimes, the full command lifecycle, module responsibilities, thread model, design decisions |
| [getting-started.md](getting-started.md) | Prerequisites, build, configure, first session, common setup mistakes |
| [developer-guide.md](developer-guide.md) | Source-tree map, per-module walkthrough, coding conventions, how to add a command / message type / family, review checklist |
| [command-reference.md](command-reference.md) | Console verbs, exit codes, all 19 commands with their argument contracts, refusal codes, the two confirmation gates |
| [configuration.md](configuration.md) | Every config key of the console, the agent, the token store, and the Gmail credentials file, with defaults and error text |
| [api-reference.md](api-reference.md) | Public API of `tether-common`, `tether-protocol`, `tether-transport`, `tether-syscalls`, `tether-cli`, `tether-command-handler` |
| [testing.md](testing.md) | Complete suite inventory, how to run one test / one family / everything, CI topology, how to add a test |
| [security.md](security.md) | Threat model, the handshake, key handling, the confirmation and audit controls, known limitations |
| [operations.md](operations.md) | Running the two runtimes, troubleshooting matrix, observability, the audit trail, failure modes |
| [protocol.md](protocol.md) | Protocol v2 wire specification — envelope, message types, HMAC handshake, seq semantics (existing) |
| [build.md](build.md) | Build-system notes and the framework decision (existing) |
| [capture-notes.md](capture-notes.md) | Why the Linux capture family is own-library X11, with the benchmark that decided it (existing) |
| [readiness-decisions.md](readiness-decisions.md) | Owner decisions and the allowed-use policy record (existing) |

## Source of truth

| Question | Authoritative source |
| --- | --- |
| What does a byte on the wire look like? | `lib/protocol/envelope.cpp` + [protocol.md](protocol.md) |
| What may a command be, and where? | `lib/syscalls/matrix.cpp` (`kCommandSet`) |
| What does the console accept? | `console/cli.cpp` |
| What does the agent do with a command? | `agent/command_handler.cpp` → `lib/syscalls/matrix.cpp` |
| What may a config file contain? | `lib/common/config.cpp` |
| What is a refusal code? | `lib/syscalls/errors.hpp` + the `Outcome` struct in `lib/syscalls/matrix.hpp` |

When a document and the code disagree, the code wins — and the document is a bug.

## Version

This documentation set describes **tether 0.1.0** (protocol v2), at commit
`0d2354c`. The build stamps `TETHER_VERSION="0.1.0"` into both binaries.

## Authorized use

tether administers machines **you own or are explicitly authorized to
administer**. The keylogger, keyboard-lock, process-control, and file
surfaces exist as explicit, audit-visible console commands. There are no
stealth features: nothing hides, nothing persists behind the operator's back,
and destructive commands are refused by default at both ends until explicitly
confirmed. See [security.md](security.md) and
[readiness-decisions.md](readiness-decisions.md) for the full policy record.
