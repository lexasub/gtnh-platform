## 1. Parsing Refactor

- [ ] 1.1 Refactor `updateQuestStatus()` (`QuestBookWindow.cpp:200-215`) to use FlatBuffers deserialization instead of raw binary parsing
- [ ] 1.2 Refactor `OnNetworkUpdate()` (`QuestBookWindow.cpp:217-224`) to use GatewayMsg enum constants instead of hardcoded `19`

## 2. Features

- [ ] 2.1 Manual completion button — render in detail view when quest status is AVAILABLE; send `QuestProgressUpdate` on click
- [ ] 2.2 Unlock animation — when new quests become available, show brief visual effect (color pulse, icon change)
- [ ] 2.3 Completion indicator — badges on era tabs showing completion ratio (e.g., "3/12")
- [ ] 2.4 Era lock/unlock visual state — locked era tabs dimmed/gray until unlocked

## 3. Client→Server Routing

- [ ] 3.1 Add quest message routing from client to MetaDB — forward `quest.get` and `quest.set` from client to router topics (currently handled by generic message forwarding)

## 4. Notifications

- [ ] 4.1 Handle msgType 20 (`QuestUnlockNotification`) in `OnNetworkUpdate()` — visual notification + highlight newly unlocked quests
- [ ] 4.2 Handle msgType 21 (`QuestCompletedNotification`) — visual notification + reward info
