# Tasks: Headless Gateway multiblock testing verification

## 4. Persistence
- [ ] 4.1 Start EntityStateStore with isolated storage and explicit readiness in integration TestMain.
- [ ] 4.2 Inspect type-4 MultiblockState through EntityStateStore RPC and cover restore after the production save boundary.

## 5. Verification
- [ ] 5.1 Run `openspec validate add-headless-multiblock-gateway-tests --strict`.
- [ ] 5.2 Run Go tests and the focused integration scenario against the required services.
