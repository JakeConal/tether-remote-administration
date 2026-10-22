# tether protocol v2 — wire specification

This document specifies tether's protocol v2 to the depth needed to implement a
compatible peer from this document alone. Every rule here is pinned by an
automated test; the final section maps each rule to the test suite that enforces
it (task 7.1's verification).

Roles follow the v2 architecture: the **agent** (target machine) dials out to the
**console** (operator machine) and keeps a persistent session open. The protocol
is transport-agnostic: it runs over any reliable bidirectional byte stream (TCP
today; the Gmail slow-path adapter feeds the same frames).

## 1. Envelope framing

Every frame begins with a fixed 10-byte header, all multi-byte fields
**little-endian**:

| Offset | Size | Field | Notes                                            |
|-------:|-----:|-------|--------------------------------------------------|
| 0      | 1    | `ver` | envelope version; **must be 2**                   |
| 1      | 1    | `type`| message discriminator; **must be 1–12** (§2)      |
| 2      | 4    | `seq` | sequence number, unsigned 32-bit LE               |
| 6      | 4    | `len` | payload byte count, unsigned 32-bit LE            |
| 10     | `len`| payload | JSON object (control frames) or raw bytes (`FILE_CHUNK`) |

Example header: `02 04 17 1C 00 00 1A 00 00 00` = version 2, type 4 (`CMD`),
seq 7191, 26-byte payload.

**Payload bounds are per-type and strict**: JSON control frames may carry at
most 1 MiB (1,048,576 bytes); `FILE_CHUNK` carries raw bytes of at most
256 KiB (262,144 bytes). A declared `len` past the type's bound is a protocol
violation.

**Framing violations poison the stream.** Three violations are detectable from
the header alone — unknown `ver`, unknown `type`, oversize `len` — and any of
them makes the stream's alignment unrecoverable: the decoder rejects the frame
without consuming payload bytes, and every later feed re-reports the same error.
A stream that ends while a frame is still incomplete reports `truncated`.
Violations are never "best-effort parsed".

Payload bytes consume exactly `len` bytes; no padding, no delimiter. The
remaining stream continues with the next header.

## 2. Message types

Values are fixed wire constants (protocol v2):

| Value | Name         | Payload                                   | Direction convention            |
|------:|--------------|-------------------------------------------|---------------------------------|
| 1     | `HELLO`      | JSON (§4.1)                               | agent → console                 |
| 2     | `CHALLENGE`  | JSON (§4.2)                               | console → agent *and* agent → console (overloaded, §4.3) |
| 3     | `AUTH_OK`    | JSON `{}`                                 | console → agent                 |
| 4     | `CMD`        | JSON (§5.1)                               | console → agent                 |
| 5     | `RESULT_TEXT`| JSON (§5.2)                               | agent → console                 |
| 6     | `FILE_META`  | JSON (§5.3)                               | agent → console                 |
| 7     | `FILE_CHUNK` | **raw bytes** (not JSON)                  | agent → console                 |
| 8     | `FILE_END`   | JSON `{}`                                 | agent → console                 |
| 9     | `CANCEL`     | JSON `{}`; target seq rides the envelope  | console → agent                 |
| 10    | `PING`       | JSON `{}`                                 | both directions                 |
| 11    | `PONG`       | JSON `{}`                                 | both directions                 |
| 12    | `ERR`        | JSON (§6)                                 | both directions                 |

The 12 types are a closed set; no new type numbers exist in protocol v2. A
receiver that sees any other value poisons the stream (§1).

## 3. Payload rules (JSON)

Control payloads are **JSON objects**, encoded as UTF-8 text. Receivers parse
the object and validate its **shape**; they do not care about key order or
whitespace, so any valid JSON encoding of the object is wire-acceptable.
(`FILE_CHUNK` is the exception: its payload is raw bytes, never parsed.)

Validation contract for every JSON payload:

- The payload must be valid JSON **and** a JSON object — arrays, strings, or
  scalars are protocol errors.
- **Missing required fields, wrong-typed fields, and unknown extra fields are
  all protocol errors** (strict shapes protect against silent version drift).
  There is no lenient mode and no optional-field fallback.
- Examples of the strictness: a wrong JSON type for a field (e.g. `size` as a
  string), an unrecognized field name, or a nonce of the wrong length each
  reject the frame.

## 4. Authentication handshake

Mutual authentication is mandatory before any command traffic. It is an
HMAC-SHA256 challenge-response over a **pre-shared token** provisioned per agent
at install time. The token never crosses the wire; only the agent's knowledge of
it does. Prerequisites: both peers reject an empty token at startup.

### 4.1 `HELLO` (agent → console) — step 1

```json
{"agent_id": "lab1-001", "agent_name": "lab-1", "protocol_version": 2}
```

| Field              | Type      | Rule                                          |
|--------------------|-----------|-----------------------------------------------|
| `protocol_version` | number    | must equal the envelope version (2)            |
| `agent_name`       | string    | non-empty; the unique registry identity        |
| `agent_id`         | string    | non-empty; the install-specific instance id    |

### 4.2 `CHALLENGE`, initiating form (console → agent) — step 2

```json
{"nonce": "0b1e2d3c4f5a69788796a5b4c3d2e1f0"}
```

The nonce is **16 fresh random bytes** per handshake (cryptographic entropy),
hex-encoded as exactly **32 lowercase hex characters**. Nonces are single-use:
the console remembers each issued nonce and rejects answers that reference a
nonce it did not issue, or one already consumed by a previous answer.

### 4.3 `CHALLENGE`, answer form (agent → console) — step 3

```json
{"agent_name-omitted": true, "nonce": "<same 32 hex as the challenge>", "hmac_response": "<64 hex>"}
```

(The answer re-uses the `CHALLENGE` type with an extra field; see §4.5.)

```json
{"hmac_response": "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
 "nonce": "0b1e2d3c4f5a69788796a5b4c3d2e1f0"}
```

- Decode the nonce hex to its 16 raw bytes.
- Compute `hmac_response = HMAC-SHA256(key = provisioned token (its ASCII
  bytes), message = the 16 raw nonce bytes)`, hex-encoded as exactly **64
  lowercase hex characters**.
- Echo the nonce unchanged.

### 4.4 `AUTH_OK` (console → agent) — step 4

```json
{}
```

Exactly the empty object. The console verifies with a **constant-time
comparison** of HMACs and sends `AUTH_OK` only on success. After `AUTH_OK` both
sides are `Established` and command traffic may begin.

Refusals: the console refuses `HELLO` from an **unknown identity before issuing
any challenge** (an attacker learns nothing verifiable), frames any failure as an
`ERR` with code `auth_failed`, and never sends `AUTH_OK` after a failure.

Failure taxonomy (all become `auth_failed` on the wire, with the specific reason
in the `message` field):

| Reason         | Meaning                                            |
|----------------|----------------------------------------------------|
| `not_begun`    | an answer arrived with no challenge outstanding     |
| `unknown_nonce`| the answer's nonce differs from the issued one      |
| `nonce_replay` | the issued nonce was already consumed               |
| `bad_hmac`     | `hmac_response` is not a 64-char hex string         |
| `wrong_token`  | the HMAC does not verify under the provisioned token|

### 4.5 The `CHALLENGE` overload (documented deviation)

Protocol v2 uses one type byte for both direction forms: console → agent
carries only `nonce`; the agent's answer echoes `nonce` **plus**
`hmac_response`. A receiver distinguishes the forms by the field set, and the
strict shape rules above still apply (the answer form must carry a valid
64-hex `hmac_response`).

**Worked primitive vector** (RFC 4231 test case 2, pinned by the test suite):
`HMAC-SHA256(key = "Jefe", message = "what do ya want for nothing?")` =
`5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843`.

## 5. Commands and results

### 5.1 `CMD` (console → agent)

```json
{"args": [], "command": "ps"}
```

| Field     | Type     | Rule                                             |
|-----------|----------|--------------------------------------------------|
| `command` | string   | non-empty; the command verb                       |
| `args`    | array    | possibly empty; **every element must be a string**|

Arguments are **data, always** — argv-style strings, never re-joined into a
shell line. Metacharacters (`; | & > <`) inside arguments have no special
meaning on the wire or at the agent.

### 5.2 Results

A command's result is a **stream of pieces** on the command's seq:

- **Text result**: one `RESULT_TEXT` frame closes the command.
  ```json
  {"text": "  1234 python3\n  456 tether-agent\n"}
  ```
  (`text` is a non-empty string.)
- **File result**: `FILE_META` (naming the file, `size` = total byte count,
  unsigned), followed by one or more `FILE_CHUNK` frames whose raw bytes sum to
  exactly `size`, followed by one `FILE_END` — or an `ERR` at any point.
  ```json
  {"name": "screenshot.png", "size": 18432}
  ```

### 5.3 Session seq semantics

- The `seq` field is the **command/result correlation key**. `CMD` and every
  piece of its result (`RESULT_TEXT`/`FILE_META`/`FILE_CHUNK`/`FILE_END`/`ERR`
  for that command) carry the command's seq; `CANCEL` carries the seq of the
  command being cancelled.
- Handshake and lifecycle frames — `HELLO`, `CHALLENGE`, `AUTH_OK`, `PING`,
  `PONG` — always use **seq 0**. Command seqs **start at 1**, so command traffic
  can never collide with control frames.
- Seq values range over the full unsigned 32-bit domain; the wire accepts any
  value. The console allocates increasing seqs per session.
- An agent must reject a `send_result` for an unknown, unauthenticated, or
  already-finished seq (protocol error, not a silent drop). Sending results
  after acknowledging a cancel is a handler bug.

## 6. Cancel, timeouts, keepalives

### 6.1 `CANCEL` (console → agent)

Payload is `{}`; the envelope's seq names the command being cancelled. On
receipt the agent marks that command's in-flight flag so the handler can unwind
early, and acknowledges by sending `ERR` with code `cancelled` **on the
cancelled seq**:

```json
{"code": "cancelled", "message": "cancelled by the console while in flight"}
```

That ack is what completes the command's `cancelled` state on the console.
Cancelling an unknown or non-pending seq is refused (structured `ERR`, see §6.5);
nothing on the session is poisoned by a cancel.

### 6.2 Timers (with v2 defaults)

| Timer                | Default    | Behavior                                                        |
|----------------------|-----------:|-----------------------------------------------------------------|
| auth timeout         | 10,000 ms  | handshake (HELLO→AUTH_OK) must complete within it, else the session goes down |
| command timeout      | 30,000 ms  | per command; a command with no terminal piece by the deadline is declared failed with error code `timeout` |
| keepalive interval   | 10,000 ms  | each side sends one `PING` per interval while established        |
| keepalive (dead-peer) window | 30,000 ms | no valid inbound frame for this long → the session goes down |

Rules: every duration must be positive, and the window must be at least the
interval (validated at startup). **Every valid inbound frame is a life sign** —
`PONG`s and traffic alike — not just `PONG`s. A `PING` is answered with `PONG`
(seq 0) immediately. Frames that fail validation count as protocol violations,
not life signs.

### 6.3 `PING` / `PONG`

Payload `{}` both ways, always seq 0. Each side pings on the interval; each side
answers a `PING` with a `PONG`.

### 6.4 Session states

| State         | Meaning                                                              |
|---------------|----------------------------------------------------------------------|
| `Connecting`  | handshake in progress                                                 |
| `Established` | authenticated; commands allowed                                       |
| `Stale`       | console-side only: a newer session superseded this identity (ineligible for commands until re-registration) |
| `Down`        | refused / peer dead / stream error / clean EOF — terminal             |

One live session per agent identity: when an agent re-dials, the console's
newest session wins and the superseded-but-connected one shows as `Stale` until
it goes down. The visible registry state is **derived at query time**:
`Established → live`, `Stale → stale`, otherwise `down`.

### 6.5 `ERR` and the error code taxonomy

```json
{"code": "no_handler", "message": "agent has no command handler for 'webcam'"}
```

| Field     | Type   | Rule                                        |
|-----------|--------|---------------------------------------------|
| `code`    | string | non-empty; stable short machine-readable code|
| `message` | string | non-empty; human-readable detail             |

Codes fixed by the protocol layer: `auth_failed` (handshake failures §4.4),
`cancelled` (cancel ack §6.1), `no_handler` (agent without a command handler).
Command-layer failures (agent capability/execution outcomes) also travel as
`ERR` frames whose `code` values come from the command families' structured
outcome taxonomy (e.g. `not_found`, `access_denied`, `invalid_args`); those
codes are data carried by `ERR`, not new frame types. A console-side command
deadline is not sent on the wire; it surfaces locally as the `timeout` error
code on that command's record.

## 7. Worked byte examples

Header is 10 bytes; `seq` and `len` are little-endian.

**PING** (seq 0, payload `{}` = `7B 7D`, 2 bytes):

```
02 0A 00 00 00 00 02 00 00 00 7B 7D
```

**HELLO** for `{"agent_id":"lab1-001","agent_name":"lab-1","protocol_version":2}`
(65 bytes — note this is how nlohmann's sorted-key dump serializes it; any valid
JSON encoding is equally wire-acceptable):

```
02 01 00 00 00 00 41 00 00 00 {"agent_id":"lab1-001","agent_name":"lab-1","protocol_version":2}
```

**CMD** `{"args":[],"command":"ps"}` (26 bytes) with seq 7191:

```
02 04 17 1C 00 00 1A 00 00 00 {"args":[],"command":"ps"}
```

## 8. Rule → test cross-checks (verification)

| Doc rule                                        | Pinned by                                     |
|-------------------------------------------------|-----------------------------------------------|
| Byte layout, LE order, 10-byte header            | envelope-test “wire byte layout: ver\|type\|seq\|len little-endian” |
| Round-trip preserves type/seq/payload             | envelope-test “round-trip preserves type, seq, and payload exactly” |
| Frame assembly across arbitrary feed slices       | envelope-test “decoder assembles frames across arbitrary feed slices” |
| Truncation at end-of-stream                       | envelope-test truncation cases (incl. hot-pipe consumption) |
| Per-type bounds (1 MiB control / 256 KiB chunk)   | envelope-test oversize/boundary cases          |
| Poison on unknown ver/type                        | envelope-test "unknown version is rejected and poisons the stream" + "unknown type is rejected both below and above the enum range" |
| Full uint32 seq domain                            | envelope-test “seq accepts the full uint32 range” |
| HELLO strict shape + version match                | payloads-test HELLO cases                      |
| CHALLENGE 32-hex nonce; 64-hex answer shape       | payloads-test CHALLENGE cases                  |
| AUTH_OK/FILE_END/CANCEL/PING/PONG = `{}` exactly  | payloads-test strictness cases                 |
| CMD strict argv strings; FILE_META 64-bit size    | payloads-test CMD / FILE_META cases            |
| FILE_CHUNK raw bytes, byte-exact                  | payloads-test “raw-byte branch”                |
| ERR code/message non-empty                        | payloads-test ERR cases                        |
| HMAC construction, single-use nonces, replay refusal | handshake-test (incl. RFC 4231 TC2 vector)   |
| Seq allocation (control 0, commands from 1), result correlation, cancel flow, timers, supersession | session-test |
| Unknown identity refused without a challenge       | session-test refusal cases                    |

A peer implementing §§1–7 that passes these test names as behavioral
assertions is wire-compatible with tether.
