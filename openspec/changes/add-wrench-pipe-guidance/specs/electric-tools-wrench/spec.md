## ADDED Requirements

### Requirement: Wrench on Pipe Targets
When a `WRENCH_CYCLE` ToolAction targets a pipe block, SimulationCore SHALL route it through a dedicated pipe-wrench flow instead of the machine side-config flow, and SHALL return actionable connection guidance to the player.

#### Scenario: Wrenching an isolated pipe
- GIVEN a player wrenches a pipe block with no adjacent pipe or machine
- WHEN SimulationCore processes the `WRENCH_CYCLE` ToolAction for that position
- THEN SimulationCore SHALL classify the target block server-authoritatively (ECS machine entity first, then a ChunkStore block query)
- AND SHALL publish a `PipeWrenchAction { player_id, pos, face }` on topic `pipe.wrench.action`
- AND upon `PipeWrenchResp` guidance `CONNECT_PIPES` SHALL publish a `ToolActionResp` with a guidance message to place adjacent pipes
- AND SHALL NOT cycle any machine `side_config`

#### Scenario: Wrenching a pipe in a pipe-only network
- GIVEN a pipe block that is part of a pipe network with no machine connection
- WHEN the player wrenches it
- THEN upon guidance `CONNECT_TO_MACHINE` SimulationCore SHALL publish a `ToolActionResp` telling the player to connect the pipe to a machine

#### Scenario: Wrenching a connected pipe
- GIVEN a pipe block that is connected to a machine network
- WHEN the player wrenches it
- THEN upon guidance `CONNECTED` SimulationCore SHALL publish a `ToolActionResp` confirming the pipe is connected
- AND the guidance message SHALL include the connected network's node count (from `PipeWrenchResp.node_id` + component size) when available

#### Scenario: Server-authoritative target classification
- GIVEN a player wrenches any block position
- WHEN SimulationCore processes the action
- THEN an existing ECS machine entity at the position SHALL take precedence and use the machine side-config flow
- AND otherwise the target block SHALL be queried from ChunkStore, not taken from client-supplied data
- AND pipe blocks SHALL NOT be rejected with the generic `not_a_machine` error

#### Scenario: Non-machine, non-pipe targets still rejected
- GIVEN the player wrenches a block that is neither a machine nor a pipe
- WHEN SimulationCore classifies the target
- THEN it SHALL respond with `ToolActionResp(success=false)` as before

### Requirement: Client Wrench Guidance Toast
The client SHALL display the `message` field of a received `ToolActionResp` as a transient toast notification.

#### Scenario: Guidance message displayed
- GIVEN the server publishes a `ToolActionResp` with a non-empty `message`
- WHEN the client receives `kToolActionResp` (14)
- THEN the message SHALL be shown to the player as a toast overlay
- AND the toast SHALL auto-dismiss after a short lifetime

#### Scenario: Empty message shows nothing
- GIVEN a `ToolActionResp` with an empty `message`
- WHEN the client receives it
- THEN no toast SHALL be shown

### Requirement: GT-Style Wrench Overlay
When the player holds a wrench and targets a wrenchable block (machine or pipe), the client SHALL render a GregTech-style wrench overlay on the targeted block in addition to the highlight: an in-world markup (not a UI window) of corner crosses and direction bars on the block edges. The overlay SHALL act as a face selector — clicking a direction bar SHALL send the wrench action targeting that face, so the player can connect a pipe to a specific side by clicking that side's bar.

#### Scenario: Overlay rendered while holding wrench
- GIVEN the player holds a wrench and the raycast targets a machine or pipe block
- WHEN the frame renders
- THEN the client SHALL draw a screen-space silhouette of the block with four direction bars along its edges (top/bottom/left/right)
- AND SHALL draw crosses at the four corners where the bars intersect
- AND the overlay SHALL be rendered on the UI/HUD (not silently skipped)
- AND the bar matching the current raycast face SHALL be visually highlighted as preselected

#### Scenario: No overlay without wrench
- GIVEN the player does not hold a wrench
- WHEN the raycast targets any block
- THEN the client SHALL render only the plain highlight wireframe

#### Scenario: Clicking a direction bar selects that face
- GIVEN the player holds a wrench and a wrenchable block is targeted with the overlay visible
- WHEN the player clicks on the bar for the right side
- THEN the client SHALL send a wrench action (`WRENCH_CYCLE`) with the right-side face, regardless of where the raycast hit the block
- AND for a pipe target the connection evaluation SHALL be performed for that side

#### Scenario: Clicking a corner cross selects the front/back depth face
- GIVEN the overlay is visible on a wrenchable block with the four corner crosses drawn
- WHEN the player clicks a corner cross (where two edge bars intersect)
- THEN the client SHALL send the wrench action targeting the depth-axis face toward or away from the player (front/back), not a screen-edge face
- AND the cross SHALL be visually distinct from the edge bars to signal the perpendicular (connect-backward) direction
- AND for a pipe target the connection evaluation SHALL be performed on that depth face

#### Scenario: Raycast face remains the default
- GIVEN the overlay is visible and the player does not click a bar
- WHEN the player triggers the wrench action (G key)
- THEN the action SHALL target the current raycast face, as today

#### Scenario: Pipe connectable directions shown
- GIVEN the player holds a wrench and the raycast targets a pipe block
- WHEN an adjacent block in a face direction is itself a pipe or a machine
- THEN the direction bar for that face SHALL be rendered as connectable
- AND directions with no adjacent pipe or machine SHALL be dimmed or omitted
- AND the direction data SHALL be derived from client-local world state; the server response remains authoritative for actual connection guidance

#### Scenario: Overlay hidden when block not targeted
- GIVEN the player holds a wrench but no block is targeted
- WHEN the frame renders
- THEN no wrench overlay SHALL be drawn
