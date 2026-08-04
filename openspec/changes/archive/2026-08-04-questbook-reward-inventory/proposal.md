# Change: Questbook Reward Inventory Integration

## Why
`RedeemPlayerQuestReward()` in MetaDB marks rewards as redeemed but never inserts the items into the player inventory. Rewards are persisted in `player_quest_rewards` but not granted.

## What Changes
- Wire `RedeemPlayerQuestReward()` to the MetaDB inventory API to add reward items to the player inventory.
- Keep `player_quest_rewards` as the redemption ledger (idempotency: redeemed=1 prevents double-grant).

## Impact
- Affected specs: questbook-reward-inventory (new)
- Affected code:
  - `src/services/meta_db/reward_handlers.go` — `RedeemPlayerQuestReward()`, `BatchRedeemPlayerQuestRewards()`
  - `src/services/meta_db/` — inventory API (player saves, inventories)
