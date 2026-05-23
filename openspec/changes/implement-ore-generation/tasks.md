## 1. Data Setup
- [ ] 1.1 Register ore block IDs in consumers.csv / producers.csv
- [ ] 1.2 Create ores.json config (vein height, threshold, frequency per ore type)

## 2. Generation
- [ ] 2.1 Implement 3D sinusoidal vein algorithm
- [ ] 2.2 Wire into WorldGenerator::generateChunk
- [ ] 2.3 Test: generated chunks contain ores at expected depths

## 3. Integration
- [ ] 3.1 Ensure chunk_store saves ore blocks correctly
- [ ] 3.2 Verify client renders ore blocks
