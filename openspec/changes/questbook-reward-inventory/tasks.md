## 1. Redemption

- [ ] 1.1 Wire `RedeemPlayerQuestReward()` to inventory API — add reward items to player inventory, not just mark redeemed
- [ ] 1.2 Verify idempotency — redeemed=1 rows must not double-grant on re-redemption

## 2. Tests

- [ ] 2.1 Test: quest completed → reward item appears in player inventory
- [ ] 2.2 Test: re-redeem attempt does not duplicate items
