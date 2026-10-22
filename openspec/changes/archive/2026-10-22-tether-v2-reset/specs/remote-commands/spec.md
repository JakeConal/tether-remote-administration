# Spec Delta

## Purpose

Defines the agent's remote execution surface: the full command set carried over from v1, its per-OS capability behavior, safe parameter handling, streamed text and file results, lifecycle-managed background capture jobs, and the audit trail that makes the tool explainable as an authorized-use lab tool.

## ADDED Requirements

### Requirement: Closed command set with structured rejection
The agent SHALL accept exactly the v2 command set: process list/start/stop, file list/delete/move/copy, shutdown/restart, screenshot capture, camera start/stop, app list/start/stop, keylogger start/stop, and keyboard lock/unlock. A command outside this set SHALL be rejected with a structured error naming the offending verb, and SHALL produce no system effect.

#### Scenario: Unknown command is refused harmlessly
- **WHEN** a command whose verb is outside the defined set arrives at an agent
- **THEN** the initiator receives a structured error identifying the invalid verb, and the agent's system state is unchanged

#### Scenario: Every defined command resolves to a definitive outcome
- **WHEN** each command of the defined set is issued in turn to a healthy agent
- **THEN** each returns a success result or a structured error outcome, and never an unclassified failure

### Requirement: Capability matrix instead of silent gaps
For every command/operating-system pair, the agent SHALL resolve exactly one definitive behavior: normal execution or a structured "unsupported on this platform" error result. A command unavailable on a platform SHALL NOT crash the agent, wedge its session, or fall back to a silent no-op.

#### Scenario: Out-of-matrix capture command returns a structured refusal
- **WHEN** a screen capture command is issued on a platform where it does not exist in the capability matrix
- **THEN** the initiator receives a structured unsupported result, and the same session continues to serve later commands

#### Scenario: Windows retains its known-good command families
- **WHEN** the command families historically supported are issued on Windows
- **THEN** each behaves as its documented implementation intends, with no regression in result authority

### Requirement: Parameters are data, never shell syntax
For any command that accepts parameters (process names, file paths, app names), the agent SHALL pass parameters to operating-system interfaces as literal data and SHALL NOT re-splice them into shell command text; malformed parameters SHALL return a structured error without executing an unintended command.

#### Scenario: Parameter containing shell metacharacters is treated literally
- **WHEN** a file command receives a path argument containing shell-concatenation characters
- **THEN** the lookup is performed against that literal name and fails with a structured not-found-style error rather than executing anything unrelated

#### Scenario: Well-formed parameters use the structured path
- **WHEN** a command's parameters are valid
- **THEN** the agent performs the operation through direct interface calls, producing the expected result

### Requirement: Results stream completely or fail loudly
Text and file results SHALL be delivered in full for successful commands: a text result arrives complete, and a file result arrives byte-complete with its name; when delivery cannot be completed, the command SHALL report a delivery failure rather than an apparently successful truncated result.

#### Scenario: Large text result arrives complete
- **WHEN** a command produces a text result larger than one message's data portion
- **THEN** the initiator receives the full result content under the originating command's sequence number

#### Scenario: File result is complete and named
- **WHEN** a command produces a file result such as a captured image
- **THEN** the initiator receives the file's name and its complete byte content, usable as a file of that format

### Requirement: Background jobs have a managed lifecycle
Camera capture and keylogger operation SHALL run as managed background jobs: an explicit start initiates one, an explicit stop terminates it and makes its accumulated file result deliverable, and issuing a duplicate start or a stop on a non-running job SHALL resolve to a structured already-running / not-running error result.

#### Scenario: Background job runs without blocking the session
- **WHEN** a start command for a background capture job succeeds
- **THEN** further commands including a stop can be served normally while the capture continues in the background

#### Scenario: Duplicate start is refused with a structured result
- **WHEN** a start command arrives while its job is already running
- **THEN** the initiator receives an already-running error result and the existing job is unaffected

#### Scenario: Job stop releases and delivers the accumulated capture
- **WHEN** a stop command arrives for a running background job
- **THEN** the job terminates, and the job's accumulated file result is reported available and deliverable

### Requirement: Destructive commands carry explicit authorization
Shutdown, restart, and file deletion SHALL execute only when the command carries an explicit confirmation; otherwise the agent SHALL refuse it with a structured result explaining the missing confirmation. Confirmation gating exists at both ends: the operator surface (see `operator-console`) refuses to send it, and the agent refuses to run it.

#### Scenario: Missing confirmation blocks a destructive command
- **WHEN** a shutdown command arrives without its explicit confirmation
- **THEN** the agent returns a structured refusal and initiates no shutdown

#### Scenario: Confirmation-bearing destructive command proceeds
- **WHEN** a shutdown command arrives with explicit confirmation
- **THEN** the agent executes the system shutdown according to its implementation's outcome reporting

### Requirement: Executed commands are auditable
For every command the agent executes or refuses, the console SHALL retain an audit record containing time, agent identity, command, parameters, sequence number, and outcome; the record set SHALL be queryable during the console's operation and persists within the operator's configured storage.

#### Scenario: Command attempts are recorded in the audit trail
- **WHEN** a command is executed, refused, or fails over any transport
- **THEN** an audit record with those fields exists, retrievable from the console's history

#### Scenario: Audit trail stays accurate across background jobs
- **WHEN** a background job is started and later stopped
- **THEN** both events appear as their own audit records referencing the commands that initiated and finalized them
