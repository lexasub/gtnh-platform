## 1. Data
- [ ] 1.1 quests.csv — quest registry (id, name, description, era, section, requirements)
- [ ] 1.2 quest_graph.json — DAG edges (quest dependencies)
- [x] 1.3 MetaDB SQLite schema for quest progress per player

## 2. Detection
- [ ] 2.1 Craft completion: automatically detect crafted items
- [ ] 2.2 Block placed completion: detect specific block placement
- [ ] 2.3 Tool charge completion: detect tool fully charged
- [ ] 2.4 Side config completion: detect machine side configured

## 3. Client UI
- [x] 3.1 Quest book window (Q key)
- [x] 3.2 Era tabs: Vagrant, Apprentice, Expert, Administrator
- [x] 3.3 Section tree within era
- [x] 3.4 Quest detail view (description, requirements, rewards)
- [ ] 3.5 Completion indicator + unlock animation

## 4. Progression
- [ ] 4.1 DAG unlock logic (complete parent → unlock children)
- [ ] 4.2 Era transition (all quests in era complete → next era)
- [x] 4.3 Quest progress sync between client and MetaDB

## Note: MetaDB quest storage (quest_handlers.go, quest_progress.go, reward_handlers.go) and QuestBookWindow (234 lines) are implemented. Gaps: quest data files, completion detection logic, DAG unlock, era transitions.
