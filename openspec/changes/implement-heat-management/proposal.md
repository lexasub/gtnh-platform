# Change: Implement Heat Management

## Why
HeatTransferSystem exists (6-neighbor propagation, overheat detection, explosion) but boiler→steam conversion, coolant-based cooling, and pipe heat transport are missing. Userflow 03 specifies these mechanics.

## What Changes
- Implement BoilerSystem for water→steam conversion (firebox fuel, water input hatch, steam output)
- Add coolant-based cooling (coolant items absorb heat, deplete over time)
- Implement pipe heat transport (heat flows through pipe network to distant consumers)
- Wire overheat UI warnings to client (yellow/red indicators in MachineWindow)
- Add environment cooling variants (radiator blocks, water adjacency bonuses)

## Impact
- Affected specs: heat-management (new)
- Affected code: simulation_core (BoilerSystem, HeatTransferSystem), pipe_network (heat transport), game_client (UI warnings)
