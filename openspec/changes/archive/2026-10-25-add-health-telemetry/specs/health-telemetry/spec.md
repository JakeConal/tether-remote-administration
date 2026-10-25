# Spec Delta

## Purpose

Gives the operator a continuously maintained view of target health: the agent
samples host CPU, memory, process, and thread state plus its own connection
counters into a bounded rolling window, and exposes the window plus the latest
sample on demand through the `health` command, so monitoring reflects what
happened between commands, not just what happened at one instant.

## ADDED Requirements

### Requirement: Agent samples host health at a steady interval
While the agent process runs, it SHALL sample host CPU usage, memory usage
with its total, host process count, and host thread count at the configured
sampling interval, and SHALL maintain a bounded rolling window of samples
spanning the configured window, independently of whether an agent session is
established.

#### Scenario: Sampling continues without any console session
- **WHEN** the agent has been running with no console session established
- **THEN** a first snapshot after re-registration reflects the recent host
  state maintained during the disconnected period rather than empty data

#### Scenario: Same snapshot fields on both supported operating systems
- **WHEN** the agent runs on Windows or on Linux
- **THEN** each snapshot resolves the same host fields (CPU, memory, process
  count, thread count) through that platform's own interfaces

### Requirement: Agent reports its connection-self counters
Each snapshot SHALL include the agent's uptime since process start and the
number of successful reconnects it has performed since start, so link history
is attributable from the target side.

#### Scenario: Reconnect count reflects real redials
- **WHEN** the agent's session has dropped and re-established twice since the
  process started
- **THEN** the snapshot reports reconnect count 2 with uptime still counting
  from the original process start

### Requirement: The health command returns a complete snapshot
The `health` verb SHALL return a text result whose content is a single JSON
document covering the latest sampled host values, window aggregates for the
configured sampling window, the agent's uptime and reconnect count, and
freshness reporting (sample age and the window's effective span and tick
coverage).

#### Scenario: Snapshot over an established session carries the full shape
- **WHEN** `health` is issued on an established session
- **THEN** the reply's text result parses as JSON containing the latest host
  values (CPU, memory, process count, thread count), the window aggregates
  (CPU average and peak, memory peak), the agent counters, and the freshness
  fields

### Requirement: The health command takes no parameters
Issuing `health` with any parameter SHALL produce a structured refusal with
no side effect.

#### Scenario: Health refuses parameters harmlessly
- **WHEN** `health` is issued with any argument
- **THEN** the agent returns a structured error naming the reason and nothing
  on the host or session state changes

### Requirement: Snapshot freshness is honest about staleness
The snapshot SHALL report how old its latest sample is and how completely the
window is populated. When a long-running command delays sampling, the reported
staleness SHALL reflect the real age rather than presenting lagged values as
current.

#### Scenario: A long command shows up as sample age
- **WHEN** a command that occupies the agent for longer than the sampling
  interval completes and a snapshot is taken
- **THEN** the sampled values are reported with a sample age consistent with
  the delay rather than being labeled as current

### Requirement: Sampling failure fails loudly, never with zeros
When host sampling fails on a platform where it is supported, the `health`
command SHALL fail with a structured error outcome; the system SHALL NOT
present fabricated or zero-filled sample values as a successful snapshot.

#### Scenario: A sampler that cannot read the host yields a structured error
- **WHEN** the host sampling interfaces fail and `health` is issued
- **THEN** the initiator receives a structured error result and no snapshot
  with fabricated values is delivered

### Requirement: Telemetry configuration is strictly validated
The agent configuration SHALL accept a telemetry section defining the sampling
interval and window span. Both values SHALL have documented defaults (interval
1 second, window 60 seconds). An interval below one second, a window shorter
than the interval, or a malformed telemetry section SHALL be rejected at
config load time with an error naming the offending field.

#### Scenario: Invalid interval is refused before the agent starts
- **WHEN** the agent config sets a sampling interval below one second
- **THEN** config loading fails with a structured error naming
  `telemetry.interval_s`, and the agent does not start

#### Scenario: Defaults apply when the section is absent
- **WHEN** the agent config omits the telemetry section entirely
- **THEN** the agent samples at the default interval with the default window

### Requirement: Health rides every enabled transport
The `health` command SHALL observe the same transport-equivalent result
semantics as the existing command set: over the Gmail slow path the snapshot
returns as the text content of the reply mail correlated to the command
sequence, rather than being restricted to the TCP transport.

#### Scenario: Health issued over the mail path returns a snapshot
- **WHEN** `health` reaches the agent through the Gmail adapter from a
  permitted admin
- **THEN** the reply mail to that admin carries the same JSON snapshot shape
  a TCP invocation would, correlated by sequence
