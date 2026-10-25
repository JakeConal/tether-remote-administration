# Spec Delta

## MODIFIED Requirements

### Requirement: Closed command set with structured rejection
The agent SHALL accept exactly the v2 command set: process list/start/stop, file list/delete/move/copy, shutdown/restart, screenshot capture, camera start/stop, app list/start/stop, keylogger start/stop, and keyboard lock/unlock, plus the `health` telemetry snapshot verb of the `telemetry` family. A command outside this set SHALL be rejected with a structured error naming the offending verb, and SHALL produce no system effect.

#### Scenario: Unknown command is refused harmlessly
- **WHEN** a command whose verb is outside the defined set arrives at an agent
- **THEN** the initiator receives a structured error identifying the invalid verb, and the agent's system state is unchanged

#### Scenario: Every defined command resolves to a definitive outcome
- **WHEN** each command of the defined set is issued in turn to a healthy agent
- **THEN** each returns a success result or a structured error outcome, and never an unclassified failure

### Requirement: Executed commands are auditable
For every operator-initiated command the agent executes or refuses, the console SHALL retain an audit record containing time, agent identity, command, parameters, sequence number, and outcome; a monitor invocation is covered by its own single record, and the `health` polls it issues internally are outside the audited set (the monitoring contract lives in `operator-console`); an operator-issued `health` outside monitor mode remains audited normally. The record set SHALL be queryable during the console's operation and persists within the operator's configured storage.

#### Scenario: Command attempts are recorded in the audit trail
- **WHEN** a command is executed, refused, or fails over any transport
- **THEN** an audit record with those fields exists, retrievable from the console's history

#### Scenario: Audit trail stays accurate across background jobs
- **WHEN** a background job is started and later stopped
- **THEN** both events appear as their own audit records referencing the commands that initiated and finalized them
