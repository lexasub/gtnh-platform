"""Searchable item picker dialog."""

from __future__ import annotations

import tkinter as tk
from tkinter import ttk

from ..utils import all_items

ITEMS_PER_PAGE = 50


class ItemPicker(tk.Toplevel):
    """Modal dialog: search & pick an item by name or ID."""

    def __init__(self, parent: tk.Widget, title: str = "Pick an item"):
        super().__init__(parent)
        self.transient(parent)
        self.title(title)

        self._result: int | None = None
        self._all_items = all_items()  # [(id, name), ...]
        self._filtered: list[tuple[int, str]] = list(self._all_items)

        # ── Search bar ──────────────────────────────────────────────────
        frame = ttk.Frame(self, padding=6)
        frame.pack(fill=tk.X)

        ttk.Label(frame, text="Search:").pack(side=tk.LEFT)
        self._search_var = tk.StringVar()
        self._search_var.trace_add("write", lambda *_: self._apply_filter())
        search_entry = ttk.Entry(frame, textvariable=self._search_var, width=30)
        search_entry.pack(side=tk.LEFT, padx=(4, 0), fill=tk.X, expand=True)
        search_entry.focus_set()

        # ── Listbox + scroll ────────────────────────────────────────────
        list_frame = ttk.Frame(self, padding=(6, 0, 6, 6))
        list_frame.pack(fill=tk.BOTH, expand=True)

        scrollbar = ttk.Scrollbar(list_frame, orient=tk.VERTICAL)
        self._listbox = tk.Listbox(
            list_frame,
            yscrollcommand=scrollbar.set,
            font=("Segoe UI", 10),
            activestyle="none",
            exportselection=False,
        )
        scrollbar.config(command=self._listbox.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self._listbox.pack(fill=tk.BOTH, expand=True)

        # ── Buttons ─────────────────────────────────────────────────────
        btn_frame = ttk.Frame(self, padding=6)
        btn_frame.pack(fill=tk.X)

        ttk.Button(btn_frame, text="OK", command=self._confirm).pack(side=tk.RIGHT, padx=(4, 0))
        ttk.Button(btn_frame, text="Cancel", command=self.destroy).pack(side=tk.RIGHT)

        self._listbox.bind("<Double-1>", lambda _: self._confirm())
        self._listbox.bind("<Return>", lambda _: self._confirm())
        self._listbox.bind("<Escape>", lambda _: self.destroy())

        self._populate()
        self.wait_window()

    # ── API ─────────────────────────────────────────────────────────────

    @property
    def result(self) -> int | None:
        return self._result

    # ── Internals ───────────────────────────────────────────────────────

    def _populate(self) -> None:
        self._listbox.delete(0, tk.END)
        for item_id, name in self._filtered[:ITEMS_PER_PAGE]:
            self._listbox.insert(tk.END, f"[{item_id:4d}] {name}")
        if len(self._filtered) > ITEMS_PER_PAGE:
            self._listbox.insert(tk.END, f"… and {len(self._filtered) - ITEMS_PER_PAGE} more")

    def _apply_filter(self) -> None:
        q = self._search_var.get().strip().lower()
        if not q:
            self._filtered = list(self._all_items)
        elif q.isdigit():
            self._filtered = [(i, n) for i, n in self._all_items if str(i).startswith(q)]
        else:
            self._filtered = [(i, n) for i, n in self._all_items if q in n.lower()]
        self._populate()

    def _confirm(self) -> None:
        sel = self._listbox.curselection()
        if not sel:
            return
        idx = sel[0]
        if idx >= len(self._filtered):
            return
        item_id, _ = self._filtered[idx]
        self._result = item_id
        self.destroy()
