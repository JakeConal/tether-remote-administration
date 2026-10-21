# tether — operator console and dial-out agent for authorized machine administration

[![CI](https://github.com/kyries-uni-projects/Networking-Project-Semester-3/actions/workflows/ci.yml/badge.svg)](https://github.com/kyries-uni-projects/Networking-Project-Semester-3/actions/workflows/ci.yml)
![license](https://img.shields.io/badge/license-MIT-blue.svg)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue.svg)

tether is a two-runtime tool for administering machines you own from one
operator seat: a CLI-side **agent** daemon on each target machine dials out and
holds an authenticated session; an operator **console** lists its dialed-in
agents and drives them with a small, testable command set. A real screenshot,
a process listing, or a webcam frame streams back over one protocol with
per-command sequence correlation — no scraping, no locale-dependent parsing.

**Agents dial to the console — never the reverse.** The target machine runs one
outbound TCP connection from its own config; nothing on the target listens, so
tether works through NAT and firewalls without opening inbound ports.

## Authorized use

tether is an authorized-use tool. Its keylogger, keyboard lock, process control,
and file operations exist for administering machines **you own or are explicitly
authorized to administer** — lab machines, your own PCs, fleet hosts you
operate. Every action is an explicit, audit-visible console command (the
console keeps a JSONL audit trail of executed, refused, and failed commands and
background-job events). There are no stealth features: nothing hides, nothing
persists behind the operator's back, and destructive commands are refused by
default at both the console and the agent until explicitly confirmed. For the
policy statement and the no-secrets-in-tree verification, see
[docs/readiness-decisions.md](docs/readiness-decisions.md).

## How it fits together

```
   operator seat                         target machines
┌────────────────────┐             ┌──────────────────────┐
│ tether console     │◄── dials ───│ tether agent (lab-1) │
│ CLI: agents/exec/  │             │ capability families: │
│ cancel + audit     │   persistent┌─│ process · file ·     │
│                    │   HMAC-     │ │ power · capture ·    │
│ listens :47474     │   session   └─│ camera · input · app │
└────────────────────┘             └──────────────────────┘
```

- **Session security.** Every session is authenticated before any command
  traffic: an HMAC-SHA256 challenge-response over a pre-shared token provisioned
  per agent at install time. The token never crosses the wire. Spec:
  [docs/protocol.md](docs/protocol.md).
- **One closed command set.** Nineteen verbs, dispatched through a capability
  matrix into per-OS families: Windows-native paths on the Windows agent;
  `/proc`, X11, logind, and freedesktop paths on the Linux agent. A verb a
  platform cannot support returns a structured refusal that names the platform
  and family — and the session stays usable.
- **Parameters are data.** Command arguments travel as argv-style arrays and are
  never re-joined into a shell line. `; | & > <` inside an argument are file
  names, not shell.
- **Everything streams.** Text results stream to stdout; file results (screens,
  frames, logs) stream as chunked file transfers you redirect to a file.
- **Managed background jobs.** Webcam capture and the keylogger are named jobs
  in one shared registry: duplicate starts are refused with the incumbent
  untouched, stop-and-deliver hands back the accumulated capture, and the
  session stays responsive while a job runs.
- **Auditability.** The console's optional audit trail (`audit_path` in the
  config) records every command attempt with time, agent identity, command,
  arguments, seq, and outcome.

## Commands

All nineteen commands ride one console verb: `tether exec <agent> <command>
[args...]`. Console surface: `tether agents` (registry listing — live/stale/
down), `tether cancel <agent> <seq>` (stop an in-flight command), `--config
<path>` (console config file; the agent uses `tether-agent --config <path>`).

| Command                                   | Family | Confirm-gated? | Notes                                                          |
|-------------------------------------------|--------|:---:|----------------------------------------------------------------|
| `list` · `start` · `stop`                 | process|      | process snapshot (`<pid> <name>` rows); argv-based launch; stop by pid or exact image name |
| `ls` · `delete` · `move` · `copy`         | file   | 🔒`delete`·`move`·`copy` | `std::filesystem` on both OSes; same verbs everywhere |
| `shutdown` · `restart`                    | power  | 🔒     | Windows shutdown API / Linux logind; refusals unless fully confirmed |
| `screenshot`                              | capture|      | full-screen grab, delivered as a PNG file result                |
| `start_webcam` · `stop_webcam`            | camera |      | background job: **stop_webcam delivers the accumulated frame** as a PNG |
| `start_keylogger <storage>` · `stop_keylogger` | input |  | background job with configurable capture storage; stop delivers the capture file |
| `lock_keyboard` · `unlock_keyboard`       | input  |      | X11 on Linux; structured refusal on Wayland, session stays usable |
| `list_app` · `start_app` · `stop_app`     | app    |      | Start-menu `.lnk` (Windows) / `.desktop` entries (Linux), launched as argv — no shell |

🔒 **The confirm gate.** For the five destructive verbs the operator confirms
**twice, deliberately**: the console refuses the dispatch outright without
`--yes` (or an interactive `y/N`) — *nothing reaches the agent* — and even then
the agent's own gate refuses unless the command's first argument is the literal
`confirm` token. A confirmed example:

```console
$ tether exec lab-1 delete confirm /tmp/scratch.txt --yes
deleted /tmp/scratch.txt
```

Exit codes: `0` success · `1` command failure · `2` usage/unknown verb ·
`3` unknown agent · `4` refused confirmation.

## Build

CMake + vcpkg (manifest mode). A machine needs a vcpkg checkout — clone and
bootstrap [microsoft/vcpkg](https://github.com/microsoft/vcpkg), or on CI
images use `VCPKG_INSTALLATION_ROOT`. Then, from a clean checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure -C Release
```

The same three commands are what CI runs from a clean clone on both legs
(windows-latest · ubuntu-latest). The Linux leg additionally installs
`libx11-dev` + `xvfb` and runs ctest under `xvfb-run -a` for the X11 capture
scenarios. Dependencies (nlohmann-json, mbedtls, asio, cpr, opencv4, cli11,
doctest) come from [vcpkg.json](vcpkg.json) alone. Build-system detail lives in
[docs/build.md](docs/build.md).

Binaries land in `build/console/` and `build/agent/` (Visual Studio multi-config
generators nest under `build/<target>/Release/`).

## Configure and run

1. **Provision a token** for each agent and give both sides the same value:
   the token store the console reads is a JSON object at `token_store_path`
   (`{"lab-1": "<token>"}`); the agent's own token sits in its config file.
2. Give the console a config file ([docs/examples/console.example.json](docs/examples/console.example.json)):
   `listen_address`, `port`, `token_store_path`, and optionally `audit_path`
   (turns the JSONL audit trail on) and a `gmail` section (the optional
   slow-path transport; its credential file path points **outside the repo**).
3. Give each agent its config ([docs/examples/agent.example.json](docs/examples/agent.example.json)):
   `console_address` and `console_port` of the console it dials, `agent_name`
   (its registry identity), and the pre-shared `token`.
4. Start both ends — agent first is unnecessary; it dials and retries:

```console
# operator seat
$ tether-console --config console.json

# target machine
$ tether-agent --config agent.json
```

Once connected, the console shows it in the registry and every command works
from any directory:

```console
$ tether --config console.json agents
lab-1 live

$ tether --config console.json exec lab-1 list
  4521 tether-agent
  4588 bash
  5102 python3

$ tether --config console.json exec lab-1 screenshot > shot.png
$ tether --config console.json exec lab-1 start_webcam
$ tether --config console.json exec lab-1 stop_webcam > frame.png
```

For a narrated walkthrough of a whole session — connection, listing, process
control, streamed screenshot, background camera job, and a refused destructive
command — see [scripts/demo.md](scripts/demo.md).

## Docs

- [docs/protocol.md](docs/protocol.md) — protocol v2 wire spec (envelope,
  message types, HMAC handshake, seq semantics), written to be implementable
  against alone and cross-checked against the codec tests
- [docs/capture-notes.md](docs/capture-notes.md) — why the Linux capture family
  is own-lib X11 (with the benchmark that decided it)
- [docs/readiness-decisions.md](docs/readiness-decisions.md) — owner decisions
  (license, README tone, report publication) and the allowed-use policy record
- The OpenSpec records under `openspec/` hold the change history: proposal,
  specs (`secure-session`, `command-transport`, `remote-commands`,
  `operator-console`), design, and the phase-by-phase tasks it was built from.

## Quality gates

- The test suite runs as a single `ctest` invocation on both platforms and
  covers the protocol boundary (envelope codec, JSON payload validation,
  HMAC handshake against RFC 4231 vectors, session state machine), transports
  (TCP loopback end-to-end, Gmail adapter behind the transport interface), and
  the full command surface — including per-family suites and end-to-end
  scenarios over the real loopback stack for every family on every platform.
- CI (GitHub Actions, windows-latest + ubuntu-latest) builds from a clean clone
  and runs the suite on every push to `main`; per-test and per-job wall-clock
  guards fail a wedged run loudly instead of parking the runner.

## License

[MIT](LICENSE) — carried since the project's founding commit, unchanged.
