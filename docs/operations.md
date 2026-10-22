# Operations

Running tether in practice: the two runtimes, the observability they give
you, the failure modes you will actually hit, and what to do about each.

---

## 1. Running the two runtimes

### The agent (target machine)

The agent is long-running. It should be supervised — systemd, a Windows
service, a container restart policy.

```console
$ tether-agent --config /etc/tether/agent.json
```

The agent:

- loads its config (exits 1 on a config error),
- dials the console on a bounded backoff,
- authenticates,
- serves commands until the session drops,
- **redials** when the session drops, resetting the backoff to 500 ms,
- never exits because the console is absent.

A minimal systemd unit:

```ini
[Unit]
Description=tether agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/tether-agent --config /etc/tether/agent.json
Restart=always
RestartSec=2
User=tether
Group=tether

# The agent needs no special privileges for the file/process/app families.
# The power family needs logind to accept the request; see below.

[Install]
WantedBy=multi-user.target
```

### The console (operator machine)

The console is one invocation = one verb. Script it.

```console
$ tether-console --config /etc/tether/console.json agents
$ tether-console --config /etc/tether/console.json exec lab-1 ls /etc
```

Each invocation binds the listener, waits for the agent's dial-in, serves the
verb, and exits. Because the agent dials on a backoff, **the first invocation
after a console restart may need to wait** — the default `agent_dial_wait_ms`
of 10,000 ms covers the agent's re-dial cadence (it retries at most every
10 s).

For a scripted batch, invoke the console once per verb and accept the
per-invocation wait, or raise `agent_dial_wait_ms` to cover the worst-case
backoff (10,000 ms) plus a margin.

---

## 2. Observability

### Logs

Both binaries log to **stderr** in a structured key=value format:

```
2026-10-21T18:04:11Z [info] config loaded console_address=10.0.0.5 console_port=47474 agent_name=lab-1
2026-10-21T18:04:11Z [info] dial-out connected; starting session agent=lab-1 state=connecting
2026-10-21T18:04:12Z [warn] dial-out attempt failed agent=lab-1 attempt=1 error=...
2026-10-21T18:04:12Z [info] dial-out backed off agent=lab-1 backoff_ms=500
```

The **token is never logged**.

### The audit trail

Set `audit_path` in the console config. Each line is a JSON object with **twelve** keys — the writer always emits all
of them, and the loader requires all of them:

```json
{"agent":"lab-1","command":"delete","delivered":"","detail":"","event":"","job":"","outcome":"executed","parameters":["/tmp/scratch.txt"],"record_type":"command","seq":1,"time":"2026-10-21T18:05:02.114Z","time_epoch_ms":1791245102114}
{"agent":"lab-1","command":"start_keylogger","delivered":"","detail":"","event":"started","job":"input","outcome":"executed","parameters":["/tmp/keys.log"],"record_type":"job_event","seq":2,"time":"2026-10-21T18:05:09.882Z","time_epoch_ms":1791245109882}
```

(The key order above is alphabetical because the JSON object is key-sorted; the
field order in the record struct is the one described in the table below.)

Reading it:

```sh
# every command against one agent
jq 'select(.agent == "lab-1")' /var/lib/tether/audit.jsonl

# everything that was refused
jq 'select(.outcome == "refused")' /var/lib/tether/audit.jsonl

# background-job lifecycle
jq 'select(.record_type == "job_event")' /var/lib/tether/audit.jsonl

# the tail
tail -f /var/lib/tether/audit.jsonl | jq -c '{time, agent, command, outcome}'
```

**`seq: 0` means nothing was dispatched** — a local refusal, a lost session,
or a transport failure. A non-zero `seq` means the command reached the agent.

### The console's stdout/stderr split

| Stream | Use it for |
| --- | --- |
| stdout | Piping result content. `> shot.png`, `> process-list.txt`. |
| stderr | Status, progress, errors, and the confirmation prompt. |

A shell script should therefore treat stdout as data and stderr as the
progress channel.

---

## 3. The failure matrix

| Symptom | Where it shows up | Cause | What to do |
| --- | --- | --- | --- |
| `error: command outcome unavailable (agent session lost)`, exit 1 | console stderr | The session was lost before the command reached a terminal state | The agent will redial; retry |
| `error: command never reached a terminal outcome`, exit 1 | console stderr | The dispatch returned without a terminal status | The session is in an unexpected state; check the logs |
| `error: audit setup failed: …`, exit 1 | console stderr | The audit store could not be opened or re-opened | Fix the `audit_path` — the console refuses to run unaudited |
| `error: unknown verb '<x>'` | console stderr, exit 2 | The verb is not `agents`, `exec`, or `cancel` | Check the verb |
| `error: unknown agent '<x>'`, exit 3 | console stderr | The agent had not registered within `agent_dial_wait_ms` | Start the agent first; or raise the budget |
| `error: refusal: '<cmd>' … not confirmed`, exit 4 | console stderr | The console gate was not satisfied | Add `--yes` (and the literal `confirm` argument) |
| `error: command failed: <code>[: detail]`, exit 1 | console stderr | The agent refused or the command failed | The code is in the [refusal taxonomy](command-reference.md#5-refusal-codes) |
| `error: command cancelled (seq N)`, exit 1 | console stderr | The command was cancelled | — |
| `error: file delivery incomplete: expected N bytes, received M`, exit 1 | console stderr | The transfer was short | The session probably dropped mid-transfer; retry |
| `error: console setup failed: …`, exit 1 | console stderr | The config or the token store is bad | The message names the file and the field |
| `error: dispatch failed: …`, exit 1 | console stderr | The transport failed | Check that the console can bind the port |
| `refused: unknown agent identity '<x>'` | **console** log | The identity is not in the token store, or the token is empty | The names must match **exactly**. The agent's own log shows only the error *code*: `refused by console during handshake: auth_failed` |
| `refused during handshake by peer: auth_failed` | **console** log | The peer (the agent) sent an `ERR` while the console was still handshaking | A peer-side refusal during the handshake |
| `refused: hmac does not verify with the provisioned token` | **console** log | The agent's answer did not verify | The tokens differ, or the identity is unknown. This is the usual console-side symptom of a token mismatch. |
| The agent aborts with `refusing to dial out with an empty provisioned token` | agent stderr (uncaught exception) | The agent's `token` is empty | Provision the token. The `SessionError` is not caught by the agent's main loop, so the process terminates abnormally rather than exiting cleanly. |
| `dial-out attempt failed` (repeated) | agent log | The console is unreachable | Check the address, the port, and the firewall |
| `dial-out backed off` (repeated) | agent log | Normal — the console is not up yet | Expected while the console is down |
| `session down; redialing` | agent log | The session dropped | Expected when the console restarts |
| `refused: handshake did not complete within auth_timeout` | **console** log | The console did not answer the challenge in 10 s | Usually a clock or a very loaded machine. The agent's equivalent is `refused: console did not complete the handshake within auth_timeout`. |
| `keepalive window elapsed without any life sign from the peer` | **console** log | The peer stopped responding for 30 s | A network or a load problem. The agent's counterpart reads `keepalive window elapsed without any life sign from the console`. |
| `no X display is reachable (is DISPLAY set?)` | command result | The Linux agent has no X server | Run the agent in a graphical session, or under Xvfb |
| `'lock_keyboard' is unsupported in this environment` | command result | A Wayland-only host | The input family needs X11. The session stays usable. |
| `the camera device could not be opened` | command result | No camera, or it is in use | — |
| `a keylogger job is already running (slot 'input')` | command result | The job slot is occupied | `stop_keylogger` first |
| `no in-flight command with seq N on '<agent>'` | console stderr, exit 1 | The cancel targeted a finished command | — |
| `the family is not implemented in this build yet` | command result | The family slot is not wired | A build issue, not an operator one |

---

## 4. Platform notes for operators

### Linux

| Area | Requirement |
| --- | --- |
| Screen capture | An **X11** display. `$DISPLAY` must point at a reachable server. Under Wayland there is no capture parity, and the refusal is structured. |
| Input capture and the keyboard lock | **X11** as well. `XQueryKeymap` and `XGrabKeyboard` need it. |
| Power | `logind` must be reachable — `libsystemd.so.0` present, a system bus, and a login manager. The request is sent with `interactive=false`. |
| The agent's user | A normal user is enough for the file, process, and app families. The keyboard lock needs the agent to be able to grab the X server's keyboard — usually the same session as the desktop. |
| The keylogger's storage path | Must be writable by the agent's user. The file is opened **in append mode**; it *is* the accumulated capture. |

### Windows

| Area | Requirement |
| --- | --- |
| Screen capture | An interactive desktop session. A service in session 0 has no visible screen. |
| Keyboard lock | `BlockInput` requires **UI access on the desktop**. A service without it gets `access_denied`, and the keyboard is **not** locked. |
| Power | The agent's token needs `SeShutdownPrivilege`. The family enables it explicitly and refuses cleanly when it is absent. |
| The keylogger's storage path | Must be writable by the agent's account. |
| Applications | Start-menu `.lnk` files under `%APPDATA%` and `%ProgramData%`. |

---

## 5. Routine procedures

### Provisioning a new agent

1. Generate a high-entropy token (32 random bytes, hex-encoded):
   ```sh
   openssl rand -hex 32
   ```
   (The project's own crypto stack is mbedTLS; `openssl` is used here only as
   a convenient random-number source, not as a runtime dependency.)
2. Add it to the console's token store under the agent's `agent_name`.
3. Put the same token in the agent's config.
4. Start the agent.
5. Confirm with `tether-console --config console.json agents`.

### Rotating a token

1. Update the console's token store **and** the agent's config.
2. Restart the agent — it redials and re-authenticates.
3. The old token is invalid from that point.

The console refuses an unknown identity **before** issuing a challenge, so a
mismatched rotation shows up as `auth_failed`, not as a hang.

### Collecting a screenshot or a capture

```sh
tether-console --config console.json exec lab-1 screenshot > shot.png
file shot.png    # → PNG image data
```

The console writes the file bytes verbatim to stdout. A short delivery is a
failure (exit 1), not a truncated file.

### Collecting a keylog capture

```sh
tether-console --config console.json exec lab-1 start_keylogger /tmp/keys.log
# ... let it run ...
tether-console --config console.json exec lab-1 stop_keylogger > keys.txt
```

The storage file accumulates across start/stop cycles — it is opened in append
mode, so `stop_keylogger` delivers the whole history, not just the last
session's keys.

### Reviewing what happened

```sh
jq -r '[.time, .agent, .command, .outcome, (.detail // "")] | @tsv' \
   /var/lib/tether/audit.jsonl
```

---

## 6. Hardening notes

- Bind the console's listener to a **trusted interface**. `0.0.0.0` exposes the
  console to anyone who can reach it and knows a token.
- **Mode `0600`** on the token store, the agent config, and the Gmail
  credentials file.
- Put the audit file on **durable storage** that is not rotated away with the
  container.
- Remember that the traffic is **plaintext** — run tether inside a VPN or a
  tunnel if the network is not trusted.
- Keep the `gmail` section `null` unless the mail slow path is wanted.
- The audit trail is append-only **by convention**; enforce that with
  filesystem permissions, not with the application.
- The console binds its port for the duration of one invocation. If the port
  is taken, the invocation fails with `console setup failed` — check for
  another process on that port.

---

## 7. The narrated demo

[`scripts/demo.md`](../scripts/demo.md) is a scripted end-to-end session with
its real transcript — connection, listing, process control, a streamed
screenshot, a background job lifecycle, and a refused destructive command.

```sh
bash scripts/demo.sh ./build ./demo-run
```

It is the fastest way to see the whole system behave as documented.

---

## 8. Where to go next

| You want to… | Go to |
| --- | --- |
| Understand a message on the wire | [protocol.md](protocol.md) |
| Understand a refusal code | [command-reference.md](command-reference.md) |
| Change the code | [developer-guide.md](developer-guide.md) |
| Change a config key | [configuration.md](configuration.md) |
| Reason about the security posture | [security.md](security.md) |
| Debug a failing test | [testing.md](testing.md) |
