"""Dataclasses for the recipe editor domain."""

from __future__ import annotations

import dataclasses
from typing import Optional


@dataclasses.dataclass
class Ingredient:
    item_id: int = 0
    count: int = 1
    consume: bool = True
    replace_item: int = 0
    replace_meta: int = 0


@dataclasses.dataclass
class OutputItem:
    item_id: int = 0
    count: int = 1
    metadata: int = 0
    display_name: Optional[str] = None
    color: Optional[str] = None
    unlocalized_name: Optional[str] = None
    lore: Optional[list[str]] = None


@dataclasses.dataclass
class Recipe:
    name: str = ""
    machine_class: str = ""
    inputs: list[Ingredient] = dataclasses.field(default_factory=list)
    outputs: list[OutputItem] = dataclasses.field(default_factory=list)
    duration: int = 200
    energy_cost: float = 0.0
    energy_output: float = 0.0
    min_tier: int = 0
    max_tier: int = 32767
    energy_type: Optional[str] = None  # "HEAT" | "STEAM" | "ELECTRICITY" | None=any

    def energy_type_yaml(self) -> Optional[str]:
        """Return energy_type for YAML output (None → omit)."""
        return self.energy_type

    def summary(self, name_of) -> str:
        """Short one-liner: name_of is a callable (int → str)."""
        def fmt(items, show_count=True):
            parts = []
            for it in items:
                name = name_of(it.item_id)
                if show_count and it.count > 1:
                    parts.append(f"{it.count}×{name}")
                else:
                    parts.append(name)
            return " + ".join(parts) if parts else "(none)"

        out = ""
        if self.energy_cost > 0:
            out += f" [{self.energy_cost}EU]"
        if self.energy_output > 0:
            out += f" →{self.energy_output}"
        et = f" ({self.energy_type})" if self.energy_type else ""
        return f"{fmt(self.inputs)} → {fmt(self.outputs)}  {self.duration}t{et}{out}"


@dataclasses.dataclass
class MachineVariant:
    block_id: int = 0
    name: str = ""
    energy_in: Optional[str] = None
    energy_out: Optional[str] = None
    tier: int = 0
    role: str = "consumer"
    slots: Optional[dict[str, int]] = None
    energy: Optional[dict] = None
    multiplier: Optional[dict[str, float]] = None


@dataclasses.dataclass
class MachineClass:
    name: str = ""
    variants: list[MachineVariant] = dataclasses.field(default_factory=list)
