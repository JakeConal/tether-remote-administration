# secure-session Specification

## Purpose

Governs how the tether agent and console establish and maintain a trustworthy connection: the agent dials out to the console, both sides authenticate before any command flows, and the session survives hung commands and dropped links. This is the trust and correctness backbone all command traffic rides on.

## Requirements

### Requirement: Agent-initiated outbound session
The agent SHALL connect to the console over an outbound TCP connection to the address configured on the agent, and the console SHALL accept inbound session establishment without requiring an inbound port to be reachable on the agent machine.

#### Scenario: Agent behind NAT connects without inbound ports
- **WHEN** the agent starts on a machine with no inbound ports open and the configured console address is reachable
- **THEN** the session is established and registered at the console, with no inbound connection to the agent machine having occurred

#### Scenario: Console unreachable at startup does not exit the agent
- **WHEN** the agent starts and the console address is unreachable
- **THEN** the agent retries connection attempts at a bounded interval instead of exiting, and connects once the console becomes reachable

### Requirement: Mandatory HMAC challenge-response authentication
The console SHALL require every connecting agent to answer an HMAC challenge computed with a pre-shared provisioned token before acknowledging the session, and SHALL NOT accept, register, or exchange commands with any agent that fails, omits, or delays the response.

#### Scenario: Agent with a valid provisioned token authenticates
- **WHEN** an agent connects and correctly answers the challenge over the session
- **THEN** the console acknowledges the session and command exchange becomes possible

#### Scenario: Agent with a wrong or missing token is refused
- **WHEN** an agent fails or omits the challenge response
- **THEN** the console refuses the session without processing any command, and the refusal is visible in the console's logs and agent listing

### Requirement: Session identity registration
The console SHALL register each authenticated session under the agent's configured unique identity and SHALL show only one live session per identity: when a newer session authenticates with an identity that already has a live session, the older one is marked stale and loses command eligibility until it reconnects.

#### Scenario: Two agents register distinct identities
- **WHEN** two agents authenticate with different configured identities
- **THEN** the console holds two independent sessions, each addressable by its own identity

#### Scenario: Duplicate identity makes the older session stale
- **WHEN** a newer session holds a live session for the same identity and a second connection authenticates again with that identity
- **THEN** the console marks the older session stale, and commands using that identity are served by the newer session until the stale one disconnects

### Requirement: Sequence-correlated results
Every message SHALL carry a sequence number, and the initiator of a command SHALL receive result messages that reference the command's sequence number; results of concurrently in-flight commands SHALL NOT be attributed to the wrong command.

#### Scenario: Concurrent commands keep their results separate
- **WHEN** two commands are issued consecutively over the same session before the first result is delivered
- **THEN** each result is delivered attributed to the sequence number of its own command, regardless of which command finishes first

#### Scenario: Result references its originating command across reconnecting workflows
- **WHEN** a command's result is unavailable immediately and arrives after later traffic
- **THEN** the result is still matched to its originating command by sequence number, not by arrival order

### Requirement: Session survives failed and cancelled commands
A timed-out, errored, or cancelled command SHALL NOT poison the session: the session SHALL report the command's failure and then remain usable for subsequent commands. The agent-side handler SHALL be cancellable, and the console SHALL deliver CANCEL to the agent when the operator cancels an in-flight command.

#### Scenario: Hung command times out without ending the session
- **WHEN** a command exceeds its configured timeout while the session is healthy
- **THEN** the command is reported as failed and the next issued command executes normally on the same session

#### Scenario: Cancellation stops reporting work early
- **WHEN** an in-flight command is cancelled by the operator while the agent is still working
- **THEN** the attempt is reported as cancelled and the session remains usable for subsequent commands

### Requirement: Dead session detection and consolidation
Robustness lives at the session layer: each side SHALL exchange keepalive messages at a bounded configured interval and SHALL mark a peer down when keepalives fail beyond that window, so an agent that vanishes without disconnecting, or a console that dies, no longer appears as an eligible session.

#### Scenario: Agent process disappears without a disconnect frame
- **WHEN** the agent machine loses the process (crash or cable pull) while a session is open
- **THEN** the console detects the dead peer within its keepalive window, marks the session down, and attempts from that agent start a new registration

#### Scenario: Console restart re-registers its agents
- **WHEN** the console process restarts while agents hold live sessions
- **THEN** the agents re-establish and re-authenticate their sessions to the restarted console within their retry cadence
