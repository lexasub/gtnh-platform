# Change: Fix Ore Processing Chain Bugs

## Why
Userflow 07 describes ore→macerator→furnace→compressor chain. Code has recipes but compressor has a bug (iron output=item_id 4, same as input). Also missing: crusher recipes for all ore types, complete voltage tier integration.

## What Changes
- Fix compressor recipe bug (iron_ingot→iron_plate wrong output item_id)
- Add missing macerator recipes (copper, tin, lead, silver, zinc)
- Add missing furnace recipes (tin, copper, lead, silver ingots)
- Add compressor recipes for all ore plates
- Verify end-to-end chain works with item pipe transport

## Impact
- Affected specs: ore-processing (new)
- Affected code: data/recipes/ (macerator.yaml, furnace.yaml, compressor.yaml)
