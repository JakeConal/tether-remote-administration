# Spec Delta

## Purpose

Defines the pluggable delivery layer for commands and results: an interactive TCP transport as the primary path and a Gmail polling adapter as a deliberate slow path, both presenting equivalent command/result semantics so the system's behavior does not depend on which wire a command travels on.

## ADDED Requirements

### Requirement: Transport-equivalent command semantics
Issuing the same command to the same agent over any enabled transport SHALL produce equivalent observable semantics: the command is accepted or rejected the same way, result content is the same, and results remain correlated to their originating command sequence regardless of transport.

#### Scenario: Same command succeeds over either transport
- **WHEN** the same well-formed command is issued to the same agent first over TCP, then over the Gmail adapter
- **THEN** both invocations yield the same result content, each correlated to its own command sequence

#### Scenario: Sequencing is preserved across transport differences
- **WHEN** two commands are issued over the Gmail adapter before the first result is delivered
- **THEN** each result is attributed to its own command by sequence number, and never swaps with the other

### Requirement: TCP transport delivers results without polling delay
The TCP transport SHALL deliver a command's result over an established session when the result becomes available on the agent, without waiting for a periodic polling trigger.

#### Scenario: Fast command returns promptly over TCP
- **WHEN** a command completes near-instantly on the agent over an established TCP session
- **THEN** the result is delivered to the console as soon as the agent finishes, not at a future polling boundary

### Requirement: Gmail adapter polls on a bounded interval
The Gmail adapter SHALL process the command inbox at its configured poll interval, and this interval SHALL be honored even when the last check found no actionable mail, keeping polling load bounded regardless of inbox state.

#### Scenario: Steady-state polling cadence is bounded
- **WHEN** the inbox receives an email that matches no enabled acceptance rule
- **THEN** the adapter's next poll occurs no earlier than the configured interval, and polling does not fuse into a rapid loop

#### Scenario: API errors back off instead of spinning
- **WHEN** the Gmail API responds with an error or rate-limit signal during a poll cycle
- **THEN** the adapter backs off across subsequent cycles before resuming normal cadence

### Requirement: Gmail commands are accepted only from configured admins
The Gmail adapter SHALL act on an inbound email only when both the sender matches the configured admin list and the adapter service account owns the destination inbox; mail not meeting both conditions SHALL be ignored without executing or acknowledging any command.

#### Scenario: Non-admin sender is ignored harmlessly
- **WHEN** an email arrives at the adapter inbox from a sender not on the configured admin list
- **THEN** no command is executed or acknowledged, and the adapter's poll cadence is unchanged

#### Scenario: Admin-sender mail becomes a command
- **WHEN** an email arrives from a sender on the admin list with a well-formed command body
- **THEN** the adapter translates it into a protocol command and forwards it to the configured agent path

### Requirement: Results reply to the requesting admin
A result produced for a Gmail-transported command SHALL be delivered back to the requesting admin's address, and SHALL NOT be routed to any other recipient.

#### Scenario: Text results return to the requesting admin
- **WHEN** an admin-sourced command via Gmail produces a text result
- **THEN** the result email is delivered to the requesting admin's address

#### Scenario: File results are attached to the result email
- **WHEN** a Gmail-transported command produces a file result
- **THEN** the result email reaches the requesting admin with the file attached

### Requirement: Transports are independently enableable by configuration
The system SHALL operate with any subset of transports enabled through configuration, and the presence of an enabled or disabled transport SHALL NOT affect command exchange over the others.

#### Scenario: TCP-only configuration runs without Gmail involvement
- **WHEN** configuration enables only the TCP transport
- **THEN** command exchange works fully over TCP, and no Gmail polling occurs

#### Scenario: Adding the Gmail adapter leaves TCP behavior unchanged
- **WHEN** configuration later enables the Gmail adapter on a TCP-first deployment
- **THEN** existing TCP sessions and their command semantics are unchanged
