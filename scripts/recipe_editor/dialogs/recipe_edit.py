"""Recipe edit/create dialog."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional

from ..models import Recipe, Ingredient, OutputItem
from ..utils import item_name, item_id as resolve_item_id
from .item_picker import ItemPicker


class RecipeEditDialog(tk.Toplevel):
    """Modal dialog to view/edit/create a single recipe."""

    def __init__(
        self,
        parent: tk.Widget,
        recipe: Optional[Recipe] = None,
        *,
        class_name: str = "",
        title: str = "Recipe",
    ):
        super().__init__(parent)
        self.transient(parent)
        self.title(title)

        self._original = recipe
        self.result: Optional[Recipe] = None

        if recipe:
            cls = recipe.machine_class or class_name
        else:
            cls = class_name

        # ── Fields (top section) ────────────────────────────────────────
        main = ttk.Frame(self, padding=8)
        main.pack(fill=tk.BOTH, expand=True)

        row = 0
        ttk.Label(main, text="Name:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        self._name_var = tk.StringVar(value=recipe.name if recipe else "")
        ttk.Entry(main, textvariable=self._name_var, width=50).grid(row=row, column=1, sticky=tk.EW)
        main.columnconfigure(1, weight=1)

        row += 1
        ttk.Label(main, text="Class:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        self._class_var = tk.StringVar(value=cls)
        ttk.Entry(main, textvariable=self._class_var, width=50).grid(row=row, column=1, sticky=tk.EW)

        row += 1
        self._energy_in_var = tk.StringVar(value=recipe.energy_type or "")
        ttk.Label(main, text="Energy in:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        energy_box = ttk.Combobox(main, textvariable=self._energy_in_var,
                                  values=["", "HEAT", "STEAM", "ELECTRICITY"],
                                  state="readonly", width=16)
        energy_box.grid(row=row, column=1, sticky=tk.W)

        row += 1
        ttk.Label(main, text="Duration:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        self._dur_var = tk.StringVar(value=str(recipe.duration) if recipe else "200")
        ttk.Entry(main, textvariable=self._dur_var, width=10).grid(row=row, column=1, sticky=tk.W)

        row += 1
        ttk.Label(main, text="EU/t:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        self._eu_var = tk.StringVar(value=str(recipe.energy_cost) if recipe and recipe.energy_cost else "0")
        ttk.Entry(main, textvariable=self._eu_var, width=10).grid(row=row, column=1, sticky=tk.W)

        row += 1
        ttk.Label(main, text="Energy output:").grid(row=row, column=0, sticky=tk.W, padx=(0, 6))
        self._eo_var = tk.StringVar(value=str(recipe.energy_output) if recipe and recipe.energy_output else "0")
        ttk.Entry(main, textvariable=self._eo_var, width=10).grid(row=row, column=1, sticky=tk.W)

        row += 1
        f_tier = ttk.Frame(main)
        f_tier.grid(row=row, column=0, columnspan=2, sticky=tk.W, pady=(4, 0))
        ttk.Label(f_tier, text="Tier:").pack(side=tk.LEFT, padx=(0, 6))
        self._min_tier_var = tk.StringVar(value=str(recipe.min_tier) if recipe else "0")
        ttk.Entry(f_tier, textvariable=self._min_tier_var, width=5).pack(side=tk.LEFT)
        ttk.Label(f_tier, text="–").pack(side=tk.LEFT, padx=2)
        self._max_tier_var = tk.StringVar(value=str(recipe.max_tier) if recipe and recipe.max_tier != 32767 else "∞")
        ttk.Entry(f_tier, textvariable=self._max_tier_var, width=5).pack(side=tk.LEFT)

        # ── Inputs / Outputs tabs ───────────────────────────────────────
        nb = ttk.Notebook(main)
        nb.grid(row=row + 1, column=0, columnspan=2, sticky=tk.NSEW, pady=(8, 0))
        main.rowconfigure(row + 1, weight=1)

        self._inputs_frame = _SlotList(nb, "Inputs", recipe.inputs if recipe else [])
        self._outputs_frame = _SlotList(nb, "Outputs", recipe.outputs if recipe else [])
        nb.add(self._inputs_frame, text="Inputs")
        nb.add(self._outputs_frame, text="Outputs")

        # ── Buttons ─────────────────────────────────────────────────────
        btn_frame = ttk.Frame(self, padding=8)
        btn_frame.pack(fill=tk.X)
        ttk.Button(btn_frame, text="Save", command=self._save).pack(side=tk.RIGHT, padx=(4, 0))
        ttk.Button(btn_frame, text="Cancel", command=self.destroy).pack(side=tk.RIGHT)

        self._setup_bindings()
        self.wait_window()

    # ── Internals ───────────────────────────────────────────────────────

    def _setup_bindings(self) -> None:
        self._listbox = self._inputs_frame
        self.bind("<Escape>", lambda _: self.destroy())

    def _save(self) -> None:
        name = self._name_var.get().strip()
        if not name:
            messagebox.showwarning("Validation", "Recipe name is required.", parent=self)
            return
        cls = self._class_var.get().strip()
        if not cls:
            messagebox.showwarning("Validation", "Machine class is required.", parent=self)
            return

        try:
            dur = int(self._dur_var.get())
        except ValueError:
            messagebox.showwarning("Validation", "Duration must be an integer.", parent=self)
            return

        eu = _parse_float(self._eu_var.get())
        eo = _parse_float(self._eo_var.get())

        min_tier = int(self._min_tier_var.get()) if self._min_tier_var.get() not in ("", "∞") else 0
        max_raw = self._max_tier_var.get().strip()
        max_tier = 32767 if max_raw in ("", "∞") else int(max_raw)

        et = self._energy_in_var.get().strip() or None

        inputs = self._inputs_frame.values()
        outputs = self._outputs_frame.values()

        if not outputs:
            messagebox.showwarning("Validation", "At least one output is required.", parent=self)
            return

        self.result = Recipe(
            name=name, machine_class=cls,
            inputs=inputs, outputs=outputs,
            duration=dur, energy_cost=eu, energy_output=eo,
            min_tier=min_tier, max_tier=max_tier,
            energy_type=et,
        )
        self.destroy()


def _parse_float(s: str) -> float:
    s = s.strip()
    if not s or s == "0":
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


class _SlotList(ttk.Frame):
    """Editable list of ingredient/output slots with item picker."""

    def __init__(
        self,
        parent: tk.Widget,
        label: str,
        items: list,
    ):
        super().__init__(parent)
        self._items: list = list(items)
        self._is_output = label == "Outputs"
        self._slots: list[dict] = []  # each: {"id": IntVar, "count": IntVar, ...}

        top = ttk.Frame(self)
        top.pack(fill=tk.X, pady=(0, 4))
        ttk.Button(top, text="Add slot", command=self._add_slot).pack(side=tk.LEFT)

        self._canvas_frame = ttk.Frame(self)
        self._canvas_frame.pack(fill=tk.BOTH, expand=True)

        self._rebuild()

    def _rebuild(self) -> None:
        for w in self._canvas_frame.winfo_children():
            w.destroy()
        self._slots.clear()

        for idx, item in enumerate(self._items):
            self._add_slot_row(idx, item)

    def _add_slot(self) -> None:
        self._items.append(Ingredient() if not self._is_output else OutputItem())
        self._add_slot_row(len(self._items) - 1, self._items[-1])

    def _add_slot_row(self, idx: int, item) -> None:
        row = ttk.Frame(self._canvas_frame)
        row.pack(fill=tk.X, pady=1)

        # item ID label (clickable)
        id_str = str(item.item_id) if item.item_id else "0"
        name = item_name(item.item_id) if item.item_id else "(none)"
        lbl = ttk.Label(row, text=f"[{id_str}] {name}", width=40, anchor=tk.W,
                        foreground="#0055cc", cursor="hand2")
        lbl.pack(side=tk.LEFT, padx=(0, 4))
        lbl.bind("<Button-1>", lambda _, i=idx: self._pick_item(i))

        # count
        ttk.Label(row, text="×").pack(side=tk.LEFT)
        count_var = tk.StringVar(value=str(item.count))
        entry = ttk.Entry(row, textvariable=count_var, width=4)
        entry.pack(side=tk.LEFT, padx=(2, 6))

        if not self._is_output:
            # consume checkbox
            consume_var = tk.BooleanVar(value=getattr(item, "consume", True))
            ttk.Checkbutton(row, text="consume", variable=consume_var).pack(side=tk.LEFT, padx=(0, 4))
        else:
            consume_var = None

        ttk.Button(row, text="×", width=2,
                   command=lambda i=idx: self._remove_slot(i)).pack(side=tk.RIGHT)

        slot = {
            "idx": idx,
            "count_var": count_var,
            "consume_var": consume_var,
        }
        self._slots.append(slot)

    def _remove_slot(self, idx: int) -> None:
        if 0 <= idx < len(self._items):
            del self._items[idx]
            self._rebuild()

    def _pick_item(self, idx: int) -> None:
        picker = ItemPicker(self, title="Pick item for slot")
        if picker.result is not None and 0 <= idx < len(self._items):
            self._items[idx].item_id = picker.result
            self._rebuild()

    def values(self):
        """Return current list of Ingredient/OutputItem from UI state."""
        result = []
        for slot in self._slots:
            idx = slot["idx"]
            if idx >= len(self._items):
                continue
            item = self._items[idx]
            try:
                count = int(slot["count_var"].get())
            except ValueError:
                count = 1

            if self._is_output:
                result.append(OutputItem(
                    item_id=item.item_id,
                    count=count,
                    metadata=getattr(item, "metadata", 0),
                    display_name=getattr(item, "display_name", None),
                    color=getattr(item, "color", None),
                ))
            else:
                result.append(Ingredient(
                    item_id=item.item_id,
                    count=count,
                    consume=slot["consume_var"].get() if slot["consume_var"] else True,
                ))
        return result
