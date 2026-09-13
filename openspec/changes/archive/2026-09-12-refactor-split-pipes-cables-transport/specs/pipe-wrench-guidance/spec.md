## ADDED Requirements

### Requirement: Pipe Wrench Awareness
PipeNetwork SHALL learn about wrench events on pipe blocks, evaluate the wrenched pipe's connection state, and report guidance to SimulationCore without mutating the pipe graph.

#### Scenario: PipeNetwork receives a wrench event on a registered pipe
- GIVEN a pipe block is registered as a node in PipeNetwork
- WHEN a `PipeWrenchAction { player_id, pos, face }` is published on topic `pipe.wrench.action`
- THEN PipeNetwork SHALL locate the node at the position
- AND SHALL evaluate its connection state (pipe-node neighbors and adjacent machine blocks)
- AND SHALL publish a `PipeWrenchResp` on topic `pipe.wrench.response` carrying the guidance enum and the node id

#### Scenario: Isolated pipe reports CONNECT_PIPES
- GIVEN a pipe node with no adjacent pipe node and no adjacent machine block
- WHEN PipeNetwork evaluates it after a wrench event
- THEN the response SHALL carry guidance `CONNECT_PIPES`

#### Scenario: Pipe-only network reports CONNECT_TO_MACHINE
- GIVEN a pipe node with at least one adjacent pipe node but no adjacent machine block
- WHEN PipeNetwork evaluates it after a wrench event
- THEN the response SHALL carry guidance `CONNECT_TO_MACHINE`

#### Scenario: Machine-connected pipe reports CONNECTED
- GIVEN a pipe node with an adjacent machine block (registered machine node or side-config cache entry)
- WHEN PipeNetwork evaluates it after a wrench event
- THEN the response SHALL carry guidance `CONNECTED`

#### Scenario: Steam pipe adjacent to a boiler reports CONNECTED
- GIVEN a pipe node adjacent to a steam-producing boiler machine that publishes a STEAM source node update
- WHEN PipeNetwork evaluates it after a wrench event
- THEN the response SHALL carry guidance `CONNECTED`

#### Scenario: Wrench event on a non-pipe position
- GIVEN a `PipeWrenchAction` targets a position with no registered pipe node
- WHEN PipeNetwork processes it
- THEN it SHALL publish guidance `NOT_A_PIPE`
- AND SHALL NOT mutate the pipe graph

#### Scenario: Wrench evaluation does not change the graph
- GIVEN any `PipeWrenchAction`
- WHEN PipeNetwork processes it
- THEN no node, edge, or network structure SHALL be added, removed, or rebuilt
