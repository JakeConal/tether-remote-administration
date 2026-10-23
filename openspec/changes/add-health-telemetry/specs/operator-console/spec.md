# Spec Delta

## ADDED Requirements

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
