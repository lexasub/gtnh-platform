# Spec Delta for verification change

This change references the existing thermal-power-chain specification defined in the core change.

## ADDED Requirements

### Requirement: Verification of existing thermal power chain spec
The platform SHALL retain and verify all requirements defined in `openspec/changes/add-headless-thermal-power-chain/specs/thermal-power-chain/spec.md`:
- Headless thermal power chain
- Separate turbine resource domains

#### Scenario: Spec reference preserved
- **WHEN** the verification change is applied
- **THEN** the existing thermal-power-chain requirements remain in effect and are subject to state-transition assertions and CI validation
