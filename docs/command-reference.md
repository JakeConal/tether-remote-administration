# Command reference

Every verb the console accepts, every exit code, and the full contract of the
19 commands the agent can execute.

---

## 1. Console verbs

```
tether-console [--config PATH] <verb> [args...]
```

`--config PATH` is accepted **anywhere** in argv and defaults to `console.json`.
A known consequence: a command argument literally named `--config` is consumed
by the global parser and can never reach the agent. The same is true of
`--yes` on the `exec` verb.

The verb must be one of exactly three. Anything else fails with
`error: unknown verb '<verb>'` and **exit code 2**, before any config file is
read, any console is built, and anything is audited.

| Verb | Form | Purpose |
| --- | --- | --- |
| `agents` | `tether-console agents` | List the registry: one `<identity> <state>` row per agent. |
| `exec` | `tether-console exec <agent> <command> [args...] [--yes]` | Run one command on one agent. |
| `cancel` | `tether-console cancel <agent> <seq>` | Cancel an in-flight command by its seq. |

### `agents`

```console
$ tether-console --config console.json agents
lab-1 live
lab-2 down
lab-3 stale
```

- Output goes to **stdout only**, one row per entry, `<identity> <state>`.
- `<state>` is `live`, `stale`, or `down` — derived at query time from the
  session state (`Established → live`, `Stale → stale`, otherwise `down`).
- Rows are sorted by identity, and a superseded-but-still-connected session of
  the same identity appears as an additional `stale` row.
- Because the registry is per-invocation, this lists what registered while
  *this* invocation was waiting.

### `exec`

```console
$ tether-console --config console.json exec lab-1 list
$ tether-console --config console.json exec lab-1 ls /tmp
$ tether-console --config console.json exec lab-1 delete confirm /tmp/x --yes
```

- `<agent>` is the registry identity, `<command>` is the verb from the closed
  set, and everything after it is **command data**.
- Leftover tokens are command arguments, never CLI flags — except `--yes`,
  which the verb's own flag parser claims.
- `--yes` satisfies the console-side confirmation gate (see §4).

**Execution order inside `exec`** — each of these precedes the next:

1. Unknown agent → `error: unknown agent '<agent>'`, **exit 3**, an audit
   record with outcome `refused` and `seq = 0`.
2. Confirmation gate → `error: refusal: '<cmd>' on '<agent>' is destructive and
   was not confirmed (nothing was sent)`, **exit 4**, audit outcome `refused`.
3. Dispatch.
4. Terminal status handling → exit 0 or 1.

### `cancel`

```console
$ tether-console --config console.json cancel lab-1 7
status: cancel sent for seq 7
```

- `<seq>` must be a non-negative integer ≤ `0xFFFFFFFF`; anything else is a
  usage failure (**exit 2**).
- Outcomes: `Sent` → exit 0; `NotPending` → `error: no in-flight command with
  seq N on '<agent>'`, exit 1; `NotEligible` → `error: agent '<agent>' cannot be
  cancelled right now (session not eligible)`, exit 1.
- Cancelling an unknown seq is refused **locally**: nothing is put on the wire.

### Exit codes

| Code | Constant | Meaning |
| ---: | --- | --- |
| `0` | `kExitOk` | Success. |
| `1` | `kExitFailure` | Command failure, transport failure, or console setup failure. |
| `2` | `kExitUsage` | Unknown verb or malformed arguments. Nothing was dispatched. |
| `3` | `kExitUnknownAgent` | Unknown agent, detected before any dispatch. |
| `4` | `kExitRefused` | Unconfirmed destructive command. Nothing was sent. |

### Stream discipline

| Stream | Carries |
| --- | --- |
| stdout | Result content only — `RESULT_TEXT` verbatim, `FILE_CHUNK` bytes verbatim. |
| stderr | Everything else — `status: …`, `error: …`, the confirmation prompt, file-transfer progress. |

A file result is byte-exact. If the delivered byte count does not match the
`FILE_META` size, the console reports
`error: file delivery incomplete: expected N bytes, received M` and exits 1 —
a truncated delivery is a **failure**, not a partial success.

---

## 2. The closed command set

Nineteen verbs across seven families. Matching is **exact and
case-sensitive** — `List` is outside the set and is refused with
`unknown_command`.

| Command | Family | Destructive | Args | Success output |
| --- | --- | :---: | --- | --- |
| `list` | process | — | 0 | One `<pid> <name>` line per process |
| `start` | process | — | ≥ 1 | `started pid <pid>` |
| `stop` | process | — | 1 | `stopped pid <pid>` (plus `('<name>')` when matched by name) |
| `ls` | file | — | 0–1 | `<name>/` for a directory, `<name> <size>` for a file |
| `delete` | file | ✔ | 1 | `deleted <path>` |
| `move` | file | ✔ | 2 | `moved <src> -> <dst>` |
| `copy` | file | ✔ | 2 | `copied <src> -> <dst>` |
| `shutdown` | power | ✔ | 0 | `shutdown initiated` |
| `restart` | power | ✔ | 0 | `restart initiated` |
| `screenshot` | capture | — | 0 | `captured the screen as screenshot.png` **+ a PNG file result** |
| `start_webcam` | camera | — | 0 | `camera job started` |
| `stop_webcam` | camera | — | 0 | `camera job stopped; delivered webcam.png` **+ a PNG file result** |
| `start_keylogger` | input | — | 0–1 | `keylogger job started` |
| `stop_keylogger` | input | — | 0 | `keylogger job stopped; delivered <name>` **+ a capture file result** |
| `lock_keyboard` | input | — | 0 | `keyboard locked` |
| `unlock_keyboard` | input | — | 0 | `keyboard unlocked` |
| `list_app` | app | — | 0 | One `<name> <path>` line per entry |
| `start_app` | app | — | 1 | `started pid <pid>` |
| `stop_app` | app | — | 1 | Same as `stop` |

Every command is supported on **both** Windows and Linux. Platform gaps (for
example the input family on a Wayland-only host) surface at runtime as a
structured `unsupported_platform` refusal, not as a missing matrix row.

---

## 3. Per-command contracts

### Process family

**`list`** — no arguments. Snapshots the process table.
- Windows: `CreateToolhelp32Snapshot` + `Process32FirstW`/`Process32NextW`.
  PID 0 is skipped.
- Linux: `/proc/<pid>/cmdline` (basename), falling back to
  `/proc/<pid>/comm`, then `"unknown"`.
- Refusal: `invalid_args` (`list takes no arguments, got N`).

**`start <program> [args...]`** — at least one argument. The argument vector
is passed to the OS **as argv**; it is never handed to a shell.

```console
$ tether-console exec lab-1 start /bin/sh -c 'echo hi'
```

- Windows: `CreateProcessW` with a hand-rolled MS-CRT quoting routine, so
  `a;b`, `c|d`, `e&f`, `g>h`, `has space`, `ap"ple`, `tr\ail\` all arrive
  one-to-one. `lpApplicationName` is `nullptr`, so the CRT resolves the first
  token.
- Linux: `fork()` + `execvp()` (PATH-searched, never `/bin/sh`), with a
  `FD_CLOEXEC` report pipe so an exec failure is reported as `not_found`
  (`ENOENT`) or `access_denied` (`EACCES`) instead of a silent child death.
- Refusal: `invalid_args` (`start needs at least the program to run as its
  first argument`).

**`stop <pid|name>`** — exactly one argument.

- A **fully numeric** argument is always read as a PID (never as a name).
  PID 0 is refused with `invalid_args`.
- A non-numeric argument is matched as an **exact image name**, and **every**
  matching PID is stopped.
- Windows: `OpenProcess` + `TerminateProcess` — immediate, no grace period.
- Linux: `SIGTERM`, then a bounded grace loop (16 × 3 ms), then `SIGKILL`,
  then a second bounded loop (64 × 3 ms); still alive ⇒ `execute_failed`.
- Refusals: `invalid_args` (arity), `not_found`, `access_denied`.

### File family

One `std::filesystem` implementation serves both operating systems. Paths are
decoded as **UTF-8** on Windows too (`fs::u8path`, not the ANSI-code-page
`path(string)` constructor).

**`ls [dir]`** — zero or one argument; the default directory is `.`.

```console
$ tether-console exec lab-1 ls /etc
alpha.txt 5
beta/
```

- Directories are listed with a trailing `/` and no size; files with their
  size. Sorted by name.
- Refusals: `not_found` (missing path, or not a directory), `access_denied`.

**`delete <path>`** — exactly one argument. Removes a file or a **single empty
directory**. Recursive tree deletion is deliberately not offered.
- Refusals: `not_found`, `directory_not_empty`, `access_denied`.

**`move <src> <dst>`** — exactly two arguments. Replaces an existing
destination (POSIX `rename` semantics).
- Refusals: `not_found`, `cross_device`.

**`copy <src> <dst>`** — exactly two arguments. Overwrites the destination and
recurses into directories.
- Refusals: `not_found`.

### Power family

**`shutdown`** and **`restart`** take no arguments beyond the confirmation
token.

- Windows: `OpenProcessToken` + `LookupPrivilegeValueW("SeShutdownPrivilege")`
  + `AdjustTokenPrivileges`, then `ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG)`
  or `ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG)` with the reason
  `SHTDN_REASON_MAJOR_SYSTEM | SHTDN_REASON_MINOR_MAINTENANCE`.
- Linux: `sd_bus_call_method` to `org.freedesktop.login1.Manager` with
  `PowerOff` / `Reboot`, and `interactive=false` — an explicitly authorized
  request, never a prompt. The availability check is
  `CanPowerOff` on the same interface.
- If logind is unavailable (no `libsystemd`, no system bus, no login manager),
  the call is refused with `execute_failed` and **nothing is executed**.
- Refusals: `access_denied` (privilege not held), `execute_failed`.

> **No test in this repository ever triggers a real power transition.** The
> dispatcher's platform is injectable, so both matrix rows are exercised on
> both CI legs against an injected backend.

### Capture family

**`screenshot`** — no arguments. Delivers a **PNG file result** named
`screenshot.png` (plus the text line `captured the screen as screenshot.png`).

- Windows: `GetDC(nullptr)` → `CreateCompatibleDC` → `CreateCompatibleBitmap`
  → `BitBlt(..., SRCCOPY | CAPTUREBLT)` → `GetDIBits` into a top-down
  32-bit `BITMAPINFOHEADER`, then de-interleave BGRX → BGR.
- Linux: `XOpenDisplay(nullptr)` → `XCreatePixmap` → `XCopyArea` → `XSync` →
  `XGetImage` → channel masks decomposed into shifts → BGR. The **temporary
  pixmap** is essential: a direct root `XGetImage` fails with `BadMatch` on
  composited roots (XWayland, WSLg).
- An X error handler that swallows errors is installed first — without it,
  Xlib's default handler *prints and terminates the process*.
- Refusals: `execute_failed` (no display reachable, grab failed),
  `unsupported_platform` (Wayland, no capture parity).

See [capture-notes.md](capture-notes.md) for the benchmark behind the
own-library X11 decision.

### Camera family

**`start_webcam`** — no arguments. Starts a background job in the shared
registry's `"camera"` slot.

- The source is `cv::VideoCapture` opened with `CAP_ANY` (DirectShow/MSMF on
  Windows, V4L2 on Linux).
- The worker publishes one PNG every 250 ms and checks the stop flag between
  steps.
- A duplicate start is refused with `already_running`; the incumbent job is
  untouched.

**`stop_webcam`** — no arguments. Signals the worker, joins it, and delivers
the accumulated frame as a **PNG file result** named `webcam.png`.

- No job running ⇒ `not_running`.
- A device that could not be opened, or that stopped delivering frames ⇒
  `execute_failed` at stop time.
- A job that produced no frame ⇒ `execute_failed`
  (`the camera job produced no frames before stopping; failed loudly instead of
  apparently succeeding`).

### Input family

**`start_keylogger [storage]`** — zero or one argument. The storage path
defaults to `keylog.txt` (`kDefaultCaptureStorage`).

- The capture file is opened in **append** mode: the file *is* the accumulated
  capture across start/stop cycles.
- The storage path is opened **before** the job is seated, so an unopenable
  path fails with `access_denied` and **no job is started**.
- The result text includes a machine-readable job event:
  `{"tether_job_event":{"job":"input","event":"started"}}`.
- Windows: a `WH_KEYBOARD_LL` low-level hook, dispatched by the job thread's
  message pump. Key-down events only, with readable names (`<Return>`,
  `<Backspace>`, …) and `<vk=N>` for unnamed codes.
- Linux: `XQueryKeymap` polled every 15 ms; a down transition is one event.
  Key names come from `XkbKeycodeToKeysym` → `XKeysymToString`.
- Refusals: `already_running`, `access_denied`, `execute_failed`.

**`stop_keylogger`** — no arguments. Signals the worker, joins it, and
delivers the accumulated capture as a file result. An empty capture is
delivered as an honest **zero-byte** file, not as an error.

**`lock_keyboard`** / **`unlock_keyboard`** — no arguments.

- Windows: `BlockInput(TRUE)` / `BlockInput(FALSE)`. A refusal
  (`ERROR_ACCESS_DENIED`, typically no UI access on the desktop) fails with
  `access_denied` and the keyboard is **not** locked.
- Linux: `XGrabKeyboard` / `XUngrabKeyboard`. The display connection is held
  open while locked, so the grab dies with the connection — the lock's scope
  is exactly the agent's session.
- Locking twice ⇒ `already_running`; unlocking when not engaged ⇒
  `not_running`.
- On a Wayland-only host the whole input family refuses with
  `unsupported_platform`, and **the session stays usable**.

### App family

**`list_app`** — no arguments. Lists launchable applications, sorted by
(name, path).

- Windows: `*.lnk` files under `%APPDATA%\Microsoft\Windows\Start Menu\Programs`
  and `%ProgramData%\Microsoft\Windows\Start Menu\Programs`. The entry name is
  the filename without its extension.
- Linux: `Type=Application` `.desktop` entries in the injected roots. The entry
  name is `Name=`.

**`start_app <name>`** — exactly one argument, matching a name from
`list_app` (the first exact match in sorted order).

- Windows: resolved through COM (`IShellLinkW` + `IPersistFile`), then the
  target path **and** its arguments are launched as argv. `SLR_NO_UI`
  guarantees no modal prompt.
- Linux: the `Exec=` line is parsed **as argv** per the desktop-entry spec,
  honouring its quoting rules.
- Launching never goes through a shell.
- Refusals: `not_found` (unknown name, unresolvable shortcut), `access_denied`,
  `execute_failed`.

**`stop_app <pid|name>`** — exactly one argument, identical semantics to
`stop`. It reuses the same `IProcessControl` instance as the process family.

---

## 4. The two confirmation gates

Destructive verbs are `delete`, `move`, `copy`, `shutdown`, `restart`.

```
  operator          console                         agent
     │                │                               │
     │ exec lab-1     │                               │
     │ delete /tmp/x  │                               │
     │────────────────▶│                               │
     │                │ GATE A: --yes or an           │
     │                │ interactive y?                 │
     │                │   no  → exit 4, nothing sent  │
     │                │   yes → strip nothing, forward │
     │                │──────────── CMD ──────────────▶│
     │                │                               │ GATE B: args[0]
     │                │                               │   == "confirm"?
     │                │                               │   no  → missing_
     │                │                               │         confirmation
     │                │◄──────────── ERR ──────────────│   yes → strip the
     │                │                               │         token, run
```

**Gate A (console side)** — `console/cli.cpp`:

- Without `--yes`: if stdin is not a TTY the console refuses immediately. If it
  is a TTY, the console prompts on stderr
  (`status: '<cmd>' targets '<agent>'; confirm? [y/N] `) and accepts only `y`
  or `yes` (case-insensitive, surrounding whitespace ignored). EOF or anything
  else refuses.
- `--yes` skips the prompt.
- A refusal is `error: refusal: '<cmd>' on '<agent>' is destructive and was not
  confirmed (nothing was sent)` with **exit code 4**. **Nothing is sent to the
  agent.**

**Gate B (agent side)** — `lib/syscalls/matrix.cpp`:

- The command's first argument must be the literal string `confirm`
  (`kConfirmMarker`). The marker is then **erased** before the family sees the
  argv.
- A missing marker is refused with `missing_confirmation`:
  `'<verb>' is destructive and requires 'confirm' as its first argument;
  nothing was executed`.

The console does **not** inject `confirm`. Both gates must be satisfied:

```console
$ tether-console exec lab-1 delete confirm /tmp/scratch.txt --yes
```

---

## 5. Refusal codes

Every refusal is a structured `{code, message}` pair. The code is stable and
machine-readable; the message is human detail.

| Code | Raised by | Meaning |
| --- | --- | --- |
| `unknown_command` | The dispatcher | The verb is outside the closed set. |
| `unsupported_platform` | The dispatcher / a family | The capability is not available on this platform or in this environment. |
| `missing_confirmation` | The dispatcher | A destructive command without the `confirm` token. |
| `invalid_args` | A family | The argument shape is wrong (arity, type). |
| `not_found` | A family | The target does not exist. |
| `access_denied` | A family | The OS refused the operation (permissions, privileges). |
| `already_running` | A family | A background job or a lock is already engaged. |
| `not_running` | A family | Nothing to stop or release. |
| `directory_not_empty` | The file family | Recursive deletion is not offered. |
| `cross_device` | The file family | A cross-device move was refused. |
| `execute_failed` | A family | The operation failed for a reason with no better classification. |
| `internal_error` | The dispatcher | An unexpected exception escaped a family. |

At the protocol layer there are three more codes that the protocol itself
defines: `auth_failed` (handshake failure), `cancelled` (the cancel ack), and
`no_handler` (the agent has no handler for that command).

### Which failures are "refusals" in the audit trail

`classify_command_outcome()` in `console/audit.hpp` maps an agent error code
to an audit outcome:

- **Refused** (no system effect): `unknown_command`, `unsupported_platform`,
  `missing_confirmation`, `invalid_args`, `already_running`, `not_running`.
- **Failed**: every other code.

The outcome strings are `executed`, `refused`, `failed`.

---

## 6. Result delivery

A command's result is a **stream of pieces** on the command's seq.

| Piece | Terminal? | Meaning |
| --- | :---: | --- |
| `RESULT_TEXT` | yes | The whole text result. `text` must be non-empty. |
| `FILE_META` | no | `{"name": "...", "size": N}` — the file name and its total byte count. |
| `FILE_CHUNK` | no | Raw bytes (never JSON), ≤ 256 KiB each. |
| `FILE_END` | yes | The last chunk has arrived. |
| `ERR` | yes | The command failed. |

The agent's shaping rules (`agent/command_handler.cpp`):

- A failed outcome becomes exactly one `ERR` frame.
- A successful outcome with text ≤ 128 KiB becomes one `RESULT_TEXT`.
- A successful outcome with a file deliverable becomes `FILE_META` +
  `FILE_CHUNK`(128 KiB each) + `FILE_END`.
- An **empty** listing rides the file channel with a synthesized `result.txt`
  deliverable — an empty `RESULT_TEXT` is rejected at decode time and would
  poison the session.

---

## 7. Cancellation

```console
$ tether-console --config console.json cancel lab-1 7
```

1. The console sends a `CANCEL` frame carrying the target seq.
2. The agent sets the command's cancel flag, erases the in-flight entry, and
   acknowledges with `ERR {code: "cancelled"}` **on the cancelled seq**.
3. The console marks the command `Cancelled` and fires the result hook with
   `terminal = true`.
4. The session stays `Established` and is immediately usable again.

Cancelling an unknown or non-pending seq is refused **locally** — the console
logs a warning and sends nothing. Nothing on the session is poisoned by a
cancel.
