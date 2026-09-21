# Design: Separate HBF and EBF blast furnaces

## Decisions

- Keep pattern IDs 1=EBF, 2=large boiler, 3=LCR and add pattern 4=HBF. Initially reuse the current 3×4×3 shell and hatch layout so geometry is not mixed with the domain migration.
- Assign canonical controller IDs from free registry slots only after promoting them into `items.csv`; retain 1001–1003 as a compatibility mapping for existing fixtures until migration is complete.
- EBF is an EU consumer. It requires a physical LV ENERGY hatch, publishes its sink at that hatch coordinate, and uses the controller entity as owner.
- HBF is a HEAT/HU consumer. It has no EU requirement and uses the existing HeatIntake/typed HU reservation path.
- Share hatch slot resolution, recipe input/output, progress, and persistence behavior through a parameterized blast-furnace execution helper; domain policy remains separate.
- Keep canonical recipes dust-based: iron dust produces two iron ingots and gold dust produces two gold ingots. Steel remains a separate recipe only where explicitly defined.

## Risks

- The current EBF implementation uses HEAT/HU and conflicts with historical L2 wording. Migrate tests and registration atomically so an EBF cannot silently run on heat.
- Deferred controller and coil IDs are not yet authoritative. A registry promotion step is required before live Gateway tests can use them.
- Existing coil constants are legacy IDs and heat thresholds exceed the default heat buffer. Make coil data and threshold capacity coherent before relying on HBF tier tests.
