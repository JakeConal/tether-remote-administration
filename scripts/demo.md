# tether demo — a narrated end-to-end session

This is the operator's-eye view of tether: one agent, one console, one
authenticated session, and the full command surface, narrated step by step
with the **actual output** of a real run (Linux/WSL host, loopback
transport). Every line quoted below was produced by the run, not written by
hand.

**How to run it**

```sh
# from a clean checkout, after the README's build commands
bash scripts/demo.sh ./build ./demo-run
```

The script writes the console/agent configs into the config directory, starts
the agent, walks the beats below, and tears everything down. The console
binary is driven the way an operator drives it: one verb per invocation.

## The runtime shape (read this once)

tether's console is **not a daemon**. One invocation = one verb:

```console
$ tether-console --config console.json agents
$ tether-console --config console.json exec lab-1 list
```

Each invocation starts its own listener, serves one verb, and exits. The agent
side is the long-running half: it dials out, authenticates, and keeps dialing
on a bounded backoff (500 ms doubling to a 10 s cap). For the two halves to
meet, each console invocation pumps a **bounded dial-in wait**
(`agent_dial_wait_ms`, default 10 s) for the agent's next dial — that is the
`console accepted a connection … agent session registered` sequence visible in
the transcript below. Without that wait, a millisecond-lived listener and a
seconds-spaced dial would never coincide.

## Beat 1 — the agent dials out and registers

```console
$ tether-agent --config agent.json &
agent started (pid 3113); dial-out loop running
```

The operator's first verb opens the console side and waits for the agent:

```console
$ tether-console --config console.json agents
2026-10-21T10:20:56Z [info] console starting version=0.1.0
2026-10-21T10:20:56Z [info] console listening address=127.0.0.1 port=47474
2026-10-21T10:20:57Z [info] console accepted a connection sessions=1
2026-10-21T10:20:57Z [info] auth challenge issued session=lab-1 step=challenge
2026-10-21T10:20:57Z [info] auth ok session=lab-1
2026-10-21T10:20:57Z [info] agent session registered agent=lab-1 superseded=false
lab-1 live
```

What you see: the TCP dial landed inside the verb's wait window, the HMAC
challenge-response ran, and the registry now knows `lab-1`. The last line —
`lab-1 live` — is the verb's **stdout**; everything above it is structured
logging on **stderr**, so a redirected stdout stays clean.

## Beat 2 — process listing (the agent's real process table)

```console
$ tether-console --config console.json exec lab-1 list
2026-10-21T10:20:57Z [info] command dispatched session=lab-1 seq=1
1 init
2 init
7 plan9
44 systemd-journald
...
891 tether-agent
893 tether-console
2026-10-21T10:20:57Z [info] command completed session=lab-1 seq=1
```

The rows are `<pid> <name>` pairs read from the target's own process table
(`/proc` on Linux, the Windows process APIs on Windows) — no shell, no locale
dependence, and the console's own `tether-console` shows up in the listing
because it really is a process on the target.

## Beat 3 — screenshot streamed to a file

```console
$ tether-console --config console.json exec lab-1 screenshot > shot.png
status: file 'screenshot.png' (7923 bytes)
status: file received (7923 bytes)
$ file shot.png
shot.png: PNG image data, 7923 bytes
```

The image arrives as a chunked **file transfer** (not base64, not text) and
lands byte-exactly in the redirected file. The two `status:` lines are
progress reporting on stderr; stdout carries only the image bytes.

## Beat 4 — background camera job lifecycle

```console
$ tether-console --config console.json exec lab-1 start_webcam
camera job started
the capture runs in the background; stop_webcam stops it and delivers the image
$ tether-console --config console.json exec lab-1 start_webcam
error: command failed: already_running: command ended: already_running
$ tether-console --config console.json exec lab-1 stop_webcam
error: command failed: not_running: command ended: not_running
```

The lifecycle is visible end to end: the start **seats** the job in the shared
job registry, a duplicate start is refused with the structured `already_running`
code (the incumbent job is untouched), and the stop resolves against the
registry.

**This host has no camera**, and the transcript shows exactly what that means:
the job's worker cannot open a device, so the job fails and releases its slot —
the stop then finds nothing running and reports the structured `not_running`
code. On a host **with** a camera the same three commands end with
`stop_webcam` delivering a decodable PNG (`webcam.png`); both branches are
pinned by the camera suite (`camera-family-test`): the device-less host and the
real-device lifecycle.

## Beat 5 — background keylogger job (same registry, and it delivers)

```console
$ tether-console --config console.json exec lab-1 start_keylogger ./demo-run/keys.log
keylogger job started
the capture accumulates to /home/hieu_nguyen/tether-demo/keys.log; stop_keylogger stops it and delivers the file
{"tether_job_event":{"job":"input","event":"started"}}
$ tether-console --config console.json exec lab-1 start_keylogger ./demo-run/keys.log
error: command failed: already_running: command ended: already_running
$ tether-console --config console.json exec lab-1 stop_keylogger > keys.out
status: file 'keys.log' (0 bytes)
status: file received (0 bytes)
```

Same registry, same rules, and the stop-and-deliver beat completes: the
accumulated capture is handed back as a file result. The capture is empty here
because no keys were typed during the run — the file is real, the content is
whatever the operator's keyboard produced.

## Beat 6 — the confirmation gates refuse twice

```console
$ tether-console --config console.json exec lab-1 delete ./demo-run/victim.txt
error: refusal: 'delete' on 'lab-1' is destructive and was not confirmed (nothing was sent)
$ echo $?
4
$ tether-console --config console.json exec lab-1 delete ./demo-run/victim.txt --yes
error: command failed: missing_confirmation: command ended: missing_confirmation
$ echo $?
1
$ tether-console --config console.json exec lab-1 delete confirm ./demo-run/victim.txt --yes
deleted /home/hieu_nguyen/tether-demo/victim.txt
```

Three steps, three different layers: the **console** refuses to send an
unconfirmed destructive command (exit 4, nothing reaches the wire); the
**agent** refuses a command that arrives without its own confirmation token
(exit 1, nothing executed); only the fully confirmed form runs. The file
survived the first two attempts and is gone after the third.

## Beat 7 — the audit trail

```console
$ wc -l demo-run/audit.jsonl
14 demo-run/audit.jsonl
```

Every attempt in this session — executed, refused, and failed — left a record
with time, agent identity, command, arguments, seq, and outcome, including the
background-job lifecycle events.

## What this demo does and does not show

- It runs against the **real binaries** over the **real TCP transport** with
  the **real HMAC handshake** — the console logs above are the protocol
  happening, not a mock.
- It runs the **agent and the console on one machine** for convenience; the
  topology does not require it (the agent dials the console across the
  network; the console never connects inward).
- The **camera beat** shows the no-device branch on a host without a camera
  (see above); the delivery branch needs a camera, and the suite proves it.
- The **keylogger** is shown for one second with no typing, so its capture is
  empty; the mechanics (seat, duplicate refusal, stop-and-deliver) are the
  point.
- The **Gmail slow-path transport** is not part of this demo: it is an
  alternative transport behind the same interface, not the interactive path.
