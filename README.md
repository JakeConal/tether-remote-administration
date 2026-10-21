<div align="center">

# tether

Operator console and dial-out agent for administering machines you own — from one seat.

[![CI](https://github.com/kyries-uni-projects/Networking-Project-Semester-3/actions/workflows/ci.yml/badge.svg)](https://github.com/kyries-uni-projects/Networking-Project-Semester-3/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue.svg?style=flat-square)]()
[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?style=flat-square&logo=cplusplus&logoColor=white)]()

[Overview](#overview) · [Getting started](#getting-started) · [Usage](#usage) · [Commands](#commands) · [Documentation](#documentation)

</div>

## Overview

tether is a two-runtime tool for administering machines you own from one operator seat. Each target machine runs an **agent** that dials out and holds an authenticated session; the operator **console** lists its dialed-in agents and drives them with a small, testable command set. A screenshot, a process listing, or a webcam frame streams back over one protocol with per-command sequence correlation — no scraping, no locale-dependent parsing.

```
   operator seat                         target machines
┌────────────────────┐             ┌──────────────────────┐
│ tether console     │◄── dials ───│ tether agent (lab-1) │
│ CLI: agents/exec/  │             │ capability families: │
│ cancel + audit     │ authenticated┌─│ process · file ·     │
│                    │   HMAC-     │ │ power · capture ·    │
│ one verb per call  │   session   └─│ camera · input · app │
└────────────────────┘             └──────────────────────┘
```

- **Dial-out, never listen.** The agent makes one outbound TCP connection from its own config; nothing on the target listens, so tether works through NAT and firewalls without opening inbound ports.
- **Session security.** Every session authenticates before any command traffic: an HMAC-SHA256 challenge-response over a pre-shared token provisioned at install time. The token never crosses the wire.
- **One closed command set.** Nineteen verbs dispatch through a per-OS capability matrix into native families — Windows paths on the Windows agent, `/proc`, X11, logind and freedesktop paths on the Linux agent. An unsupported combination returns a structured refusal that names the platform and family, and the session stays usable.
- **Parameters are data.** Command arguments travel as argv-style arrays and are never re-joined into a shell line — `; | & > <` inside an argument are file names, not shell.
- **Everything streams.** Text results stream to stdout; file results stream as chunked transfers you redirect to a file.
- **Managed background jobs.** Camera capture and the keylogger are named jobs in one shared registry: duplicate starts are refused, stop-and-deliver hands back the accumulated capture, and the session stays responsive while a job runs.
- **Auditability.** The console's optional JSONL audit trail records every command attempt with time, agent identity, command, arguments, seq, and outcome.

> [!IMPORTANT]
> **Authorized use only.** The keylogger, keyboard lock, process control, and file operations exist exclusively for administering machines **you own or are explicitly authorized to administer**. Every action is an explicit, audit-visible console command; there are no stealth features — nothing hides, nothing persists behind the operator's back — and destructive commands are refused by default at both ends until explicitly confirmed. See [docs/readiness-decisions.md](docs/readiness-decisions.md) for the policy record.

## Getting started

### Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| [CMake](https://cmake.org/download/) | ≥ 3.24 | — |
| C++17 toolchain | MSVC ≥ 2022 / GCC ≥ 11 / Clang ≥ 13 | — |
| [vcpkg](https://github.com/microsoft/vcpkg) | latest | clone & bootstrap once, or use `VCPKG_INSTALLATION_ROOT` on CI images |
| (Linux) `libx11-dev`, `xvfb` | distro packages | X11 capture at build time; X server under `xvfb-run` at test time |

### Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure -C Release
```

The same three commands are what CI runs from a clean clone on both legs (windows-latest · ubuntu-latest). Dependencies (`mbedtls`, `nlohmann-json`, `asio`, `cpr`, `cli11`, `opencv4`, `doctest`) come from [vcpkg.json](vcpkg.json) alone. Binaries land in `build/console/` and `build/agent/` (Visual Studio multi-config generators nest under `build/<target>/Release/`); build-system detail lives in [docs/build.md](docs/build.md).

### Configure and run

1. Provision a per-agent token in a JSON token store the console reads (`token_store_path`): `{"lab-1": "<token>"}` — the agent holds the same token in its own config file.
2. Give the console a config file using [docs/examples/console.example.json](docs/examples/console.example.json): `listen_address`, `port`, `token_store_path`, and optionally `audit_path` (enables the audit trail), `agent_dial_wait_ms` (the dial-in wait described below) and a `gmail` section (the optional slow-path transport, disabled by default).
3. Give each agent a config using [docs/examples/agent.example.json](docs/examples/agent.example.json): `console_address`, `console_port` of the console it dials, `agent_name`, and the pre-shared `token`.
4. Start the **agent** on the target — it is the long-running half: it dials the console, authenticates, and keeps dialing on a bounded backoff (500 ms doubling to a 10 s cap) until a console invocation meets it.

```console
# target machine (the long-running half)
$ tether-agent --config agent.json
```

## Usage

The console is **one invocation = one verb**: each `tether-console` call starts
its own listener, serves one verb, and exits — there is no console daemon to
keep alive. Because the agent dials on a bounded backoff, every invocation
pumps a **dial-in wait** for the agent's next dial (`agent_dial_wait_ms` in
the console config, default 10 s — one full agent backoff cycle; `0` disables
it). When the wait expires, the verb reports the usual `unknown agent`
refusal.

```console
$ tether-console --config console.json agents
lab-1 live

$ tether-console --config console.json exec lab-1 list
1 init
4521 tether-agent
4588 bash

$ tether-console --config console.json exec lab-1 screenshot > shot.png
$ tether-console --config console.json exec lab-1 start_webcam
$ tether-console --config console.json exec lab-1 stop_webcam > frame.png
```

Console surface: `agents` (registry listing — live/stale/down), `exec <agent> <command> [args...]`, `cancel <agent> <seq>`, and `--config <path>`. Exit codes: `0` success · `1` command failure · `2` usage/unknown verb · `3` unknown agent · `4` refused confirmation.

For a narrated, fully scripted end-to-end walkthrough — connection, listing, process control, a streamed screenshot, a background job lifecycle, and a refused destructive command — see [scripts/demo.md](scripts/demo.md) (and run it with `bash scripts/demo.sh ./build ./demo-run`).

## Commands

All nineteen commands ride one console verb: `tether exec <agent> <command> [args...]`.

| Command | Family | Confirm | Notes |
| --- | --- | :---: | --- |
| `list` · `start` · `stop` | process | — | process snapshot; argv-based launch; stop by pid or exact image name |
| `ls` · `delete` · `move` · `copy` | file | `delete` · `move` · `copy` | `std::filesystem` on both OSes; same verbs everywhere |
| `shutdown` · `restart` | power | ✔ | Windows shutdown API / Linux logind |
| `screenshot` | capture | — | full-screen grab, delivered as a PNG file result |
| `start_webcam` · `stop_webcam` | camera | — | background job; **stop delivers the accumulated frame** |
| `start_keylogger <storage>` · `stop_keylogger` | input | — | background job; stop delivers the capture file |
| `lock_keyboard` · `unlock_keyboard` | input | — | X11 on Linux; structured refusal on Wayland, session stays usable |
| `list_app` · `start_app` · `stop_app` | app | — | Start-menu `.lnk` (Windows) / `.desktop` entries (Linux), launched as argv — no shell |

### The confirm gate

Destructive verbs require deliberate confirmation **twice**: the console refuses outright without `--yes` or an interactive `y/N` — nothing reaches the agent — and even then the agent's own gate refuses unless the command's first argument is the literal `confirm` token.

```console
$ tether exec lab-1 delete confirm /tmp/scratch.txt --yes
deleted /tmp/scratch.txt
```

## Documentation

| Document | Contents |
| --- | --- |
| [docs/protocol.md](docs/protocol.md) | protocol v2 wire spec (envelope, message types, HMAC handshake, seq semantics), written to be implementable against alone and cross-checked against the codec tests |
| [docs/build.md](docs/build.md) | build-system notes and decisions (toolchain, [doctest](https://github.com/doctest/doctest) pick) |
| [docs/capture-notes.md](docs/capture-notes.md) | why the Linux capture family is own-lib X11, with the benchmark that decided it |
| [docs/readiness-decisions.md](docs/readiness-decisions.md) | owner decisions and the allowed-use policy record |
| [scripts/demo.md](scripts/demo.md) | the narrated end-to-end session (real binaries, real protocol) and its transcript |
| `openspec/` | the change history: proposal, capability specs (`secure-session`, `command-transport`, `remote-commands`, `operator-console`), design, and phase-by-phase tasks |

## Quality

- The test suite runs as a single `ctest` invocation on both platforms and covers the protocol boundary (envelope codec, JSON payload validation, HMAC handshake against [RFC 4231](https://datatracker.ietf.org/doc/html/rfc4231) vectors, session state machine), transports (TCP loopback end-to-end, Gmail adapter behind the transport interface), and the full command surface — per-family suites plus end-to-end scenarios over the real loopback stack for every family on both OSes.
- CI runs on every push to `main` and every PR (windows-latest + ubuntu-latest, run in parallel): per-test and per-job wall-clock guards fail a wedged run loudly instead of parking the runner, and warm dependency/compile caches keep typical runs in the minutes range.
