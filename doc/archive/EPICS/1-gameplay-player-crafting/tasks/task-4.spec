{
  "title": "Базовые правила",
  "description": "System rules: no items drop on block break, no tools, no durability (except block creation), full client-server trust.",
  "ecs_components": [],
  "flatbuffers_schemas": [],
  "service_architecture": "Enforces gameplay rules through server-side validation and client-side trust. No automatic item destruction or durability.",
  "inputs": {},
  "constraints": [
    "No falling items",
    "No tools (breaking same as any item)",
    "No durability",
    "No item consumption on block break",
    "Full client trust: client reports actions, server verifies only rules, not state integrity"
  ],
  "test_requirements": "Test that items are not dropped when breaking blocks, verify no items are consumed, and ensure rules are enforced server-side."
}