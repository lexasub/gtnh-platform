# Change: Add service health debug window

## Why
The game client already has a debug overlay, but it cannot show whether backend services are responding. A compact service-health snapshot makes local development failures visible without inspecting every service log.

## What Changes
- Add a Router health query that probes registered services and reports transport and application liveness with the age of the last successful probe response.
- Expose the snapshot through Gateway using FlatBuffers client messages.
- Add an F3-toggleable section to the existing GameClient Debug overlay with a Refresh button and one row per service.
- Keep the UI render-thread owned and display `unknown` until a probe has completed.

## Impact
- Affected specs: `service-health` (new), `protocol` (client wire message inventory)
- Affected code: MessageRouter, Gateway, shared FlatBuffers protocol, GameClient NetClient/RenderBridge, CMake generation, tests
- This adds new wire message IDs 50 and 51; existing message IDs are unchanged.
