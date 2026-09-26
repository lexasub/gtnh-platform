# Tasks: Headless Gateway multiblock testing verification

## 4. Persistence
- [x] 4.1 Start EntityStateStore with isolated storage and explicit readiness in integration TestMain.
- [~] 4.2 Inspect type-4 MultiblockState through EntityStateStore RPC and cover restore after the production save boundary.
      - EntityStateStore starts under the harness (main_test.go:106-110, with an
        isolated `MkdirTemp` db dir) and `TestGateway_EBFMultiblockLifecycle` covers
        formation/destruction, but there is NO RPC-level inspection of a type-4
        MultiblockState and no restore-after-restart coverage. Remaining.

## 5. Verification
- [x] 5.1 Run `openspec validate add-headless-multiblock-gateway-tests --strict`.
- [x] 5.2 Run Go tests and the focused integration scenario against the required services.
- [x] 5.3 Run full C++ ctest suite: 18/18 pass at the time this change was written (`toctou_test` disabled, pre-existing). The suite has since grown to 57 tests, all passing in both Debug and Release.
- [x] 5.4 Run full Go integration suite: 5 new tests pass (Base, ChemicalReactor, FullBase, PipeWrench, SingleBlockSideConfig). The pre-existing reds (block/chunk/crafting/gateway/inventory/machine) were since fixed in 1e1ee1cb — all were broken tests, not server bugs. Two stress tests still fail; tracked as gp-o11l.

## 6. Demo verification (manual stack, `chunkdb/`)
- [x] 6.1 gateway_cli `place` y-shift fix: sends (x, y+1, z) + face=0 so the block lands exactly at (x,y,z) — matches `testutil.PlaceBlockAndWait`.
- [x] 6.2 EBF multiblock formed on a clean zone: `Matched multiblock 'ebf' #8 at (46501,200,46501)`.
- [x] 6.3 Single-block machines verified via `open`: chem reactor LV 0xE403, steam boiler 0xE600 (type=2 fluid), heat furnace 0xE000, creative gen 0xE800 (energy rising).
- [x] 6.4 Thermal chain verified: heat gen 0xE002 → heat boiler 0xE601 → turbine 0xE42C → battery 0xEA00 charging (2.7k → 36k/40k).
- [x] 6.5 Item pipes verified via `pipe`: 0xF800 registered as nodes 4/5 in the pipe network.
- [x] 6.6 EntityStateStore leak resolved (MDB_FIXEDMAP removed): DB 12K, tmpfs 505M/24G stable.
- [x] 6.7 Double SetBlockAction dispatch (ACCEPTED + phantom REJECTED/CONFLICT per place) confirmed pre-existing and tolerated — tests pass with it.
