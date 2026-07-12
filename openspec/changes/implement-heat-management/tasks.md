## 1. Boiler→Steam Conversion
- [ ] 1.1 Implement BoilerSystem tick: consume fuel from firebox inventory
- [ ] 1.2 Consume HEAT from EnergyStorage, produce STEAM energy type
- [ ] 1.3 Water input hatch: consume water bucket → produce steam bucket
- [ ] 1.4 Wire BoilerSystem into SimulationEngine tick loop
- [ ] 1.5 Test: boiler converts water+heat→steam at expected rate

## 2. Coolant-Based Cooling
- [ ] 2.1 Add coolant item support (coolant bucket/item in inventory)
- [ ] 2.2 HeatTransferSystem: consume coolant to reduce HEAT
- [ ] 2.3 Coolant depletes over time (coolant → empty bucket)
- [ ] 2.4 Test: coolant reduces heat faster than environment cooling

## 3. Pipe Heat Transport
- [ ] 3.1 Extend PipeNetworkManager to transport heat through pipe graph
- [ ] 3.2 Heat flows from PRODUCER → pipe → remote CONSUMER
- [ ] 3.3 Heat dissipation per pipe block (loss over distance)
- [ ] 3.4 Test: heat reaches distant consumer through pipes

## 4. Client UI Warnings
- [ ] 4.1 MachineWindow: show yellow warning bar at 90% heat
- [ ] 4.2 MachineWindow: show red warning bar at 100% heat
- [ ] 4.3 Explosion visual effect on critical overheat
