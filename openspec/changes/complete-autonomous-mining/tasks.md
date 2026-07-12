## 1. Persistence
- [ ] 1.1 DrillComponent state save/load via EntityStateStore RPC
- [ ] 1.2 Restore drill state on SimulationCore restart

## 2. Item Pipe Integration
- [ ] 2.1 Drill output buffer push to connected item pipe
- [ ] 2.2 Auto-eject when output is full

## 3. Client UI
- [ ] 3.1 Drill machine window (progress bar, tier, energy level)
- [ ] 3.2 Output buffer slots display
- [ ] 3.3 Power indicator (connected/disconnected)

## 4. Multi-dimension
- [ ] 4.1 Drill dimension-aware (dimension field in DrillComponent)
- [ ] 4.2 Support drilling in any dimension

## Note: DrillSystem core (spiral BFS, mining progress, energy, output buffer) is implemented in DrillSystem.cpp (241 lines). This change covers remaining gaps: persistence, pipe integration, UI, multi-dim.
