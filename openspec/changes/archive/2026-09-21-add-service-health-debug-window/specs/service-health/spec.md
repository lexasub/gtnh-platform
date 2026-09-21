## ADDED Requirements
### Requirement: Service health snapshot
The platform SHALL expose a gateway-mediated service health snapshot based on a Router probe of registered services. Each service row SHALL include a stable service name, transport state, probe state, and the elapsed time since its last successful probe response. A service with no completed probe SHALL be `UNKNOWN`; a service whose connection is closed or whose probe times out SHALL be `DOWN`; a successful probe SHALL be `UP`.

#### Scenario: Healthy registered services are shown
- **GIVEN** the Router has registered backend services and they answer the health probe
- **WHEN** the GameClient requests a service health snapshot through Gateway
- **THEN** Gateway returns one row per registered service with probe state `UP`
- **AND** each row includes the elapsed time since its last successful probe response

#### Scenario: Probe timeout is visible
- **GIVEN** a registered service does not answer a health probe before the timeout
- **WHEN** Gateway returns the snapshot
- **THEN** that service is shown as `DOWN`
- **AND** the snapshot does not report it as healthy merely because its TCP connection remains open

#### Scenario: Initial state is explicit
- **GIVEN** a service has registered but has not completed a health probe
- **WHEN** the snapshot is rendered
- **THEN** the service state is `UNKNOWN`
- **AND** the UI does not display a fabricated last-response time

### Requirement: Debug overlay service view
The GameClient SHALL extend its existing Debug ImGui overlay with an F3-toggleable service health section. The section SHALL provide a Refresh control and display service name, status, and last-response age. Health data SHALL be applied on the client/render thread only after a verified response.

#### Scenario: Refresh displays current service state
- **GIVEN** the Debug overlay is visible
- **WHEN** the user activates Refresh
- **THEN** the client sends a health request through Gateway
- **AND** the next verified response replaces the displayed snapshot

#### Scenario: F3 toggles the section
- **GIVEN** the GameClient is running
- **WHEN** the user presses F3
- **THEN** the service health section toggles without opening a modal game window
- **AND** existing world interaction remains available when the section is visible
