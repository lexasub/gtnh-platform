# Change: Implement Quest Book

## Why
Quest Book is the player's progression guide — equivalent of GTNH quest book. Shows what to craft, what to build, and unlocks next steps automatically. Currently no progression system exists.

## What Changes
- Quest data format (CSV reigistry + JSON graph)
- Quest storage in MetaDB SQLite
- Quest completion detection (craft, place block, charge tool, configure face)
- Quest unlock logic (DAG of quest nodes)
- Client UI: quest book window with era/section/quest browsing
- Auto-completion and manual completion
- Era progression (Vagrant → Apprentice → Expert → Administrator)

## Impact
- Affected specs: questbook (new)
- Affected code: meta_db (SQLite storage), game_client (UI window), message_router (quest topics)
