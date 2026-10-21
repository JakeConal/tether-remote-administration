# Getting started

From a clean clone to a live, authenticated session — on Windows or Linux.

---

## 1. Prerequisites

| Tool | Version | Notes |
| --- | --- | --- |
| [CMake](https://cmake.org/download/) | ≥ 3.24 | Required by the root `CMakeLists.txt`. |
| A C++17 toolchain | MSVC ≥ 2022, GCC ≥ 11, or Clang ≥ 13 | The build sets `CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`. |
| [vcpkg](https://github.com/microsoft/vcpkg) | latest | Manifest mode. Clone and bootstrap once, or point `VCPKG_INSTALLATION_ROOT` at a CI image's copy. |
| (Linux) `libx11-dev` | distro package | The Linux capture family links X11. |
| (Linux) `xvfb` | distro package | Needed **at test time** for the screenshot suites. |

All C++ dependencies come from `vcpkg.json` alone — `mbedtls`, `doctest`,
`nlohmann-json`, `asio`, `cpr`, `cli11`, `opencv4`. Nothing else needs to be
installed by hand.

> **Not a build dependency:** `libsystemd`. The Linux power family `dlopen`s
> `libsystemd.so.0` at runtime and resolves the six sd-bus symbols it needs.
> If the library or a symbol is missing, the family returns a structured
> `execute_failed` refusal; the build needs no systemd headers.

---

## 2. Build and test

```sh
# one-time, on a fresh machine
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh        # bootstrap-vcpkg.bat on Windows

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure -C Release
```

Those three commands are exactly what CI runs from a clean clone on both legs
(`windows-latest`, `ubuntu-latest`).

**Where the binaries land**

| Generator | Console | Agent |
| --- | --- | --- |
| Single-config (Ninja, Makefiles) | `build/console/tether-console` | `build/agent/tether-agent` |
| Visual Studio (multi-config) | `build/console/Release/tether-console.exe` | `build/agent/Release/tether-agent.exe` |

**Linux test runs need an X server** for the capture suites:

```sh
xvfb-run -a ctest --test-dir build --output-on-failure -C Release
```

Build-system detail and the framework decision live in [build.md](build.md).
Test detail lives in [testing.md](testing.md).

---

## 3. Configure

tether has three configuration artefacts. None of them belong in version
control.

### 3.1 The token store (operator side)

A JSON object mapping an agent identity to its pre-shared token:

```json
{
    "lab-1": "3f9c1d7e5b2a8046c1f9e0d3b7a4c5e2d8b6f1a0c3e7d9b4f2a6c8e0d1b3f5a7"
}
```

Start from [`docs/examples/tokens.example.json`](examples/tokens.example.json).
The **token must be the same string** in the token store and in the agent's
config file. It never crosses the wire — see [security.md](security.md) for how
to choose one.

### 3.2 The console config

```json
{
    "listen_address": "0.0.0.0",
    "port": 47474,
    "token_store_path": "/etc/tether/tokens.json",
    "audit_path": "/var/lib/tether/audit.jsonl",
    "agent_dial_wait_ms": 10000,
    "gmail": null
}
```

Start from [`docs/examples/console.example.json`](examples/console.example.json).
Every key, its default, and its validation rule are documented in
[configuration.md](configuration.md).

### 3.3 The agent config

```json
{
    "console_address": "10.0.0.5",
    "console_port": 47474,
    "agent_name": "lab-1",
    "instance_id": "lab-1",
    "token": "3f9c1d7e5b2a8046c1f9e0d3b7a4c5e2d8b6f1a0c3e7d9b4f2a6c8e0d1b3f5a7"
}
```

Start from [`docs/examples/agent.example.json`](examples/agent.example.json).
`agent_name` is the **registry identity** — the console addresses agents by
exactly this string.

> The repository root also ships `console.example.json` and `agent.example.json`
> as quick-start templates. The `docs/examples/` copies are the canonical ones
> referenced by the documentation.

---

## 4. Run it

### 4.1 Start the agent (on the target machine)

The agent is the long-running half. Start it first — it dials out on a bounded
backoff and keeps dialing until a console invocation meets it.

```console
$ tether-agent --config agent.json
{"timestamp":"2026-10-21T18:04:11Z","level":"info","message":"config loaded",...}
{"timestamp":"2026-10-21T18:04:11Z","level":"info","message":"dial-out connected; starting session",...}
```

The agent never exits because the console is absent — it retries forever
(500 ms, doubling to a 10 s cap). The token is never logged.

### 4.2 Run a console verb (on the operator machine)

Each invocation is one verb. The console starts its own listener, waits up to
`agent_dial_wait_ms` for the agent's dial-in, serves the verb, and exits.

```console
$ tether-console --config console.json agents
lab-1 live

$ tether-console --config console.json exec lab-1 list
1 init
4521 tether-agent
4588 bash

$ tether-console --config console.json exec lab-1 ls /tmp
config.txt 128
logs/

$ tether-console --config console.json exec lab-1 screenshot > shot.png

$ tether-console --config console.json exec lab-1 start_webcam
$ tether-console --config console.json exec lab-1 stop_webcam > frame.png
```

> **`agents` shows one row per identity.** Because the console's registry is
> per-invocation, `agents` reports what registered during *that* invocation —
> typically the agent that dialed in while the verb was waiting.

### 4.3 Exit codes

| Code | Meaning |
| ---: | --- |
| `0` | Success. |
| `1` | Command failure, transport failure, or a console setup failure. |
| `2` | Usage failure: unknown verb or malformed arguments. Nothing was dispatched. |
| `3` | Unknown agent — detected against the registry before any dispatch. |
| `4` | A destructive command was not confirmed. Nothing was sent. |

---

## 5. Destructive commands need two confirmations

Destructive verbs (`delete`, `move`, `copy`, `shutdown`, `restart`) must pass
**two independent gates**:

1. **Console gate** — `--yes`, or an interactive `y` on a TTY. Without it the
   console refuses locally: nothing is sent, exit code `4`.
2. **Agent gate** — the command's first argument must be the literal token
   `confirm`. Without it the agent refuses: nothing is executed.

```console
$ tether-console --config console.json exec lab-1 delete confirm /tmp/scratch.txt --yes
deleted /tmp/scratch.txt
```

The console does **not** inject `confirm` for you. Both are required.

---

## 6. A narrated walkthrough

[`scripts/demo.md`](../scripts/demo.md) is a fully scripted end-to-end session
with its real transcript: connection, listing, process control, a streamed
screenshot, a background job lifecycle, and a refused destructive command.

```sh
bash scripts/demo.sh ./build ./demo-run
```

---

## 7. Common setup mistakes

| Symptom | Cause | Fix |
| --- | --- | --- |
| `error: unknown agent 'lab-1'` (exit 3) | The agent had not dialed in within `agent_dial_wait_ms` | Start the agent first; or raise `agent_dial_wait_ms` (the default 10 s covers one full agent backoff cycle) |
| The console exits with `error: console setup failed: …` | The config file is missing, malformed, or names a bad field | The message names the file and the field; see [configuration.md](configuration.md) |
| `error: unknown agent identity 'lab-1'` in the agent log | The identity is not in the console's token store, or the spelling differs | The token store keys must match `agent_name` **exactly** |
| The console refuses every agent | The token store is empty or the tokens differ from the agents' | A token mismatch fails the HMAC handshake → `auth_failed` |
| `error: refusal: 'delete' on 'lab-1' is destructive and was not confirmed` | The console gate was not satisfied | Add `--yes` (and the literal `confirm` argument) |
| `error: command failed: missing_confirmation` | The agent gate was not satisfied | Put `confirm` first: `exec lab-1 delete confirm <path> --yes` |
| The screenshot fails with a `DISPLAY` message | The Linux agent has no reachable X server | Run the agent under a graphical session, or use Xvfb — the capture family is X11-only on Linux |
| `unsupported_platform` on `lock_keyboard` | The host is Wayland-only | The input family needs X11; the refusal is structured and the session stays usable |
| The agent logs `refusing to dial out with an empty provisioned token` | The agent's `token` field is empty | Put the same token in the agent config and the console's token store |
