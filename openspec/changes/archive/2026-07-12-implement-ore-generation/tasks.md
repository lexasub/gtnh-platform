## 1. Data Setup
- [x] 1.1 Register ore block IDs in consumers.csv / producers.csv
- [x] 1.2 Create ores.json config (vein height, threshold, frequency per ore type)

## 2. Generation
- [x] 2.1 Implement 3D sinusoidal vein algorithm
- [x] 2.2 Wire into WorldGenerator::generateChunk
- [ ] 2.3 Test: generated chunks contain ores at expected depths

## 3. Integration
- [x] 3.1 Ensure chunk_store saves ore blocks correctly
- [ ] 3.2 Verify client renders ore blocks
