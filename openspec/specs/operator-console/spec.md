# operator-console Specification

## Purpose

Defines the operator-facing console: the dial-in listener agents register against, the session registry, and the verb-style command-line surface with streaming outputs to standard streams, so an operator (or any script) can drive agents composably from a shell.

## Requirements

### Requirement: Agent registry reflects live session state
The console SHALL list every known agent identity with its current state when registry is queried: live for authenticated eligible sessions, stale for superseded-but-connected ones, and down for agents that disconnected or were detected dead; list state SHALL change as sessions change.

#### Scenario: Multiple agents are listed with states
- **WHEN** two agents were connected and a third is down after losing its session
- **THEN** the registry shows the three identities as live, live, and down respectively

#### Scenario: Down agent returns to live on re-registration
- **WHEN** a previously down agent reconnects and re-authenticates
- **THEN** the registry marks that identity live without operator intervention

### Requirement: Verb-style command surface
The console SHALL expose a verb-style command line targeting a named agent: listing agents, executing commands with parameters, and canceling in-flight commands. An unknown verb or unknown agent SHALL fail with a structured error on the error stream and a distinguishable exit code, without dispatching to any agent.

#### Scenario: Executing a command targets the named agent
- **WHEN** the operator issues a command verb addressing a specific live agent with valid parameters
- **THEN** the command reaches only that agent, and its result is the console's response for the invocation

#### Scenario: Unknown agent or verb is rejected before dispatch
- **WHEN** a command verb is issued with a nonexistent agent name, or an unknown verb is used
- **THEN** the console reports a structured error on stderr with a non-success exit code and no message is sent

### Requirement: Standard streams are the UI for results
Text results SHALL be written to stdout, and file results SHALL be written to a file chosen by the operator with byte-exact content; progress and status reporting SHALL go to stderr so redirected stdout remains clean. Faceless usage: reading a result through a shell redirect or pipe with no terminal attached yields identical output content.

#### Scenario: Redirected file output is byte-exact
- **WHEN** a file-producing command is executed with its output directed to a path via a shell redirect
- **THEN** the file at that path is byte-identical to the file the agent produced

#### Scenario: Text result pipes cleanly without status noise
- **WHEN** a text-result command executes while stdout is piped to another program
- **THEN** the piped stream contains the result content only, with all progress/status messages on stderr

### Requirement: Destructive verbs require operator confirmation
Shutdown, restart, and file-deletion commands SHALL be refused at the console before dispatch unless explicitly confirmed via the CLI's confirmation form (flag or interactive response); the post-confirmation dispatch remains subject to the agent's own authorization requirement.

#### Scenario: Unconfirmed destructive command fails locally
- **WHEN** a destructive command is issued without confirmation
- **THEN** the console refuses it with a structured refusal on stderr, a non-success exit code, and sends no protocol message

#### Scenario: Confirmed destructive command dispatches
- **WHEN** the same command is issued with explicit confirmation
- **THEN** the console dispatches the command, and the agent's own confirmation requirement governs execution

### Requirement: One-shot console invocations await the agent's bounded dial
The runtime shape is one invocation = one verb with its own transient listener, while agents dial out forever on a bounded backoff; a console invocation SHALL therefore wait up to a configured budget (`agent_dial_wait_ms`, defaulting to cover one full agent backoff cycle) for the targeted agent — or any agent, for the listing verb — to dial in and register, driving its own accept/handshake pump during the wait. Wait expiry SHALL produce the same unknown-agent refusal as an unknown name: structured error on stderr, distinguishable exit code, and no message dispatched.

#### Scenario: A verb serves an agent that dials during the wait
- **WHEN** the operator issues a verb while the registry is empty but the agent's dial-out loop is running
- **THEN** the invocation waits for the agent's next dial, and that agent is registered and served within the same invocation

#### Scenario: The budget expires without a dial
- **WHEN** no agent dials within the configured budget
- **THEN** the invocation reports the structured unknown-agent refusal with no message sent

### Requirement: Scriptable exit codes and output form
Console invocations SHALL exit with success code on success and a distinguishable non-success code on failure, refusal, unsupported outcome, or transport error, so shell scripts can react to outcomes. Result text SHALL remain stable enough that automation parses it without interactive formatting.

#### Scenario: Script reacts to outcome by exit code
- **WHEN** console invocations are run from a scripted loop, once succeeding and once failing
- **THEN** the differing outcomes are distinguishable purely by their exit codes

#### Scenario: Failure details never flood the result channel
- **WHEN** a command fails at any stage
- **THEN** the error description targets the error stream while the stdout output remains either empty or result-only, never mixed with diagnostics

### Requirement: Monitor invocations serve one verb until interrupted
The console SHALL provide a monitoring verb that targets a named agent and
stays alive from its own listener: it waits for the agent's dial-in without a
deadline, re-issues `health` at a fixed cadence while a live session exists,
and renders one observation per cadence until the operator interrupts it.

#### Scenario: Interrupted monitor exits distinguishably
- **WHEN** the operator interrupts a running monitor invocation
- **THEN** the console exits with a code scripts can distinguish from
  failure outcomes

### Requirement: Monitor observes through an agent restart
While the target has no live session, the monitor SHALL present the target as
unavailable and SHALL resume observations alongside re-registration instead of
exiting.

#### Scenario: Monitor bridges an agent restart without operator action
- **WHEN** the monitored agent's session drops and the agent later re-dials
  and re-registers during the same monitor invocation
- **THEN** the monitor reports the target unavailable during the gap and
  resumes observations after re-registration without exiting

### Requirement: Monitor output streams and parses
Monitor observations SHALL follow the standard-streams contract: the
observation stream goes to stdout and status to stderr, with identical output
content for faceless (piped) usage. A JSON output shape SHALL emit one
self-contained JSON object per observation line, remaining parseable by
automation without interactive formatting; the human shape SHALL stay
readable at a glance.

#### Scenario: JSON monitor stream parses line by line
- **WHEN** monitor runs in its JSON output shape while stdout is piped into
  another program
- **THEN** each line is a complete JSON object and the target of the pipe can
  consume observations one line at a time, with no status noise mixed in

### Requirement: Monitor observations include console-measured link health
Alongside the agent's snapshot, the monitor SHALL report link health measured
from the console side: end-to-end latency of commands it has issued (derived
from the cadence polls it issues itself), transport throughput over the recent
interval, the observed connection uptime, and the number of reconnects it has
observed during this invocation. These values derive from console activity
and require no additional agent data.

#### Scenario: Latency and throughput appear without extra commands
- **WHEN** monitor has been running with its own cadence polls in flight
- **THEN** each observation line carries the sampled end-to-end latency of
  those polls and the bytes-per-second measured over the recent interval
  without any operator-issued measurement command

### Requirement: Monitor audits itself once, not per poll
A monitor invocation SHALL record one audit record covering the invocation
(including its command parameters: target agent and monitored verb). The
health polls the monitor issues internally within that invocation SHALL NOT
produce individual audit records; an operator-issued `health` outside monitor
mode is unaffected and remains audited.

#### Scenario: A long-running monitor leaves a minimal audit trail
- **WHEN** a monitor invocation runs for one hour at one poll per second
- **THEN** the audit trail records the monitor invocation itself and not
  3600 health records
