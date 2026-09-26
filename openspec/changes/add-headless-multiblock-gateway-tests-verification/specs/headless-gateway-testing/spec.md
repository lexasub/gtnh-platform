## ADDED Requirements
### Requirement: Headless EBF multiblock formation verification
The headless gateway test suite SHALL verify that an EBF multiblock forms correctly when all blocks are placed in the correct order.

#### Scenario: EBF forms with energy hatch present
- **WHEN** the controller block is placed after the energy hatch, cable and creative generator
- **THEN** the multiblock event is emitted with mb_type=1 and the energy hatch is marked present

#### Scenario: EBF recipe execution with iron dust
- **WHEN** iron dust is inserted into the input slot and energy is supplied
- **THEN** the BlockEntityUpdate reports progress >0 and output items contain iron_ingot count >=2

### Requirement: Headless LCR multiblock formation verification
The headless gateway test suite SHALL verify that an LCR multiblock forms with thermal power chain.

#### Scenario: LCR forms with energy hatch from thermal chain
- **WHEN** the LCR controller is placed after the energy hatch
- **THEN** the multiblock event is emitted and the energy hatch is reported in Hatches

#### Scenario: LCR recipe execution with iron ore
- **WHEN** iron ore is inserted and EU is supplied via thermal chain
- **THEN** the BlockEntityUpdate reports progress >0 and output items contain iron_ingot
