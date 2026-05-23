# Tasks: World Generation — Ore Veins

- [ ] Ore item_id в items.csv/items.db (iron, copper, tin, coal, gold, redstone, lapis, diamond)
- [ ] Ore vein config (ores.json или inline)
- [ ] 3D синусоидальная генерация (FastNoiseLite)
- [ ] Разные высоты для разных руд:
  - Coal: y < 80
  - Iron: y < 40
  - Copper: y < 60
  - Tin: y < 50
  - Gold: y < 30
  - Diamond: y < 15 (редкая)
- [ ] WorldGenerator публикует SetBlock для руд при генерации чанка
- [ ] Руды добываются рукой (без инструментов MVP)
- [ ] ChunkStore сохраняет руду в LMDB
- [ ] Vein continuity: проверка что жилы выглядят естественно
