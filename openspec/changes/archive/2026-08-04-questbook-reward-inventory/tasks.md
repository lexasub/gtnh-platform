## 1. Redemption

- [x] 1.1 Wire `RedeemPlayerQuestReward()` to inventory API — add reward items to player inventory, not just mark redeemed
- [x] 1.2 Verify idempotency — redeemed=1 rows must not double-grant on re-redemption

## 2. Tests

- [x] 2.1 Test: quest completed → reward item appears in player inventory
- [x] 2.2 Test: re-redeem attempt does not duplicate items
