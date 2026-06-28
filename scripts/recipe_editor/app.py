"""Main application window."""

from __future__ import annotations

import os
import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional

from .models import Recipe, MachineVariant
from .utils import item_name, DATA_RECIPES
from .data import load_machines, load_recipes, save_recipes
from .dialogs.recipe_edit import RecipeEditDialog


class RecipeEditor(tk.Tk):
    """Main editor window."""

    def __init__(self):
        super().__init__()
        self.title("GTNH Recipe Editor")
        self.geometry("1100x650")

        # ── Data ────────────────────────────────────────────────────────
        self._machines = load_machines()
        self._recipes: dict[str, list[Recipe]] = load_recipes()
        self._dirty = False

        # ── Build UI ────────────────────────────────────────────────────
        self._build_menu()
        self._build_layout()
        self._populate_class_list()
        self._update_status()

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── Menu ──────────────────────────────────────────────────────────

    def _build_menu(self) -> None:
        menubar = tk.Menu(self)
        file_menu = tk.Menu(menubar, tearoff=False)
        file_menu.add_command(label="Save (Ctrl+S)", command=self._save, accelerator="Ctrl+S")
        file_menu.add_command(label="Reload", command=self._reload)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self._on_close)
        menubar.add_cascade(label="File", menu=file_menu)
        self.config(menu=menubar)
        self.bind("<Control-s>", lambda _: self._save())

    # ── Layout ────────────────────────────────────────────────────────

    def _build_layout(self) -> None:
        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True)

        # ── Left: class list ────────────────────────────────────────────
        left = ttk.Frame(paned, padding=4)
        ttk.Label(left, text="Machine Classes", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W)
        self._class_listbox = tk.Listbox(left, font=("Segoe UI", 10), exportselection=False)
        self._class_listbox.pack(fill=tk.BOTH, expand=True, pady=(4, 0))
        self._class_listbox.bind("<<ListboxSelect>>", self._on_class_select)
        paned.add(left, weight=1)

        # ── Right: recipe list ──────────────────────────────────────────
        right = ttk.Frame(paned, padding=4)
        ttk.Label(right, text="Recipes", font=("Segoe UI", 10, "bold")).pack(anchor=tk.W)

        toolbar = ttk.Frame(right)
        toolbar.pack(fill=tk.X, pady=(2, 4))
        ttk.Button(toolbar, text="+ Add", command=self._add_recipe).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(toolbar, text="Edit", command=self._edit_recipe).pack(side=tk.LEFT, padx=(0, 4))
        ttk.Button(toolbar, text="Delete", command=self._delete_recipe).pack(side=tk.LEFT)

        # ── Recipe list (Treeview) ──────────────────────────────────────
        columns = ("name", "summary")
        self._recipe_tree = ttk.Treeview(right, columns=columns, show="headings",
                                         selectmode="browse")
        self._recipe_tree.heading("name", text="Name")
        self._recipe_tree.heading("summary", text="Summary")
        self._recipe_tree.column("name", width=200)
        self._recipe_tree.column("summary", width=500)
        self._recipe_tree.pack(fill=tk.BOTH, expand=True)
        self._recipe_tree.bind("<Double-1>", lambda _: self._edit_recipe())
        self._recipe_tree.bind("<Return>", lambda _: self._edit_recipe())

        paned.add(right, weight=3)

        # ── Status bar ──────────────────────────────────────────────────
        self._status_var = tk.StringVar()
        status = ttk.Label(self, textvariable=self._status_var, relief=tk.SUNKEN, anchor=tk.W)
        status.pack(fill=tk.X, side=tk.BOTTOM)

    # ── Class list ────────────────────────────────────────────────────

    def _populate_class_list(self) -> None:
        self._class_listbox.delete(0, tk.END)
        for name in sorted(set(list(self._machines.keys()) + list(self._recipes.keys()))):
            self._class_listbox.insert(tk.END, name)

    def _on_class_select(self, _=None) -> None:
        sel = self._class_listbox.curselection()
        if not sel:
            return
        class_name = self._class_listbox.get(sel[0])
        self._show_recipes(class_name)

    # ── Recipe list ───────────────────────────────────────────────────

    def _show_recipes(self, class_name: str) -> None:
        for row in self._recipe_tree.get_children():
            self._recipe_tree.delete(row)

        recipes = self._recipes.get(class_name, [])
        for r in recipes:
            self._recipe_tree.insert("", tk.END, values=(
                r.name,
                r.summary(item_name),
            ))

    def _selected_class(self) -> Optional[str]:
        sel = self._class_listbox.curselection()
        if not sel:
            return None
        return self._class_listbox.get(sel[0])

    def _selected_recipe(self) -> Optional[Recipe]:
        sel = self._recipe_tree.selection()
        if not sel:
            return None
        values = self._recipe_tree.item(sel[0], "values")
        if not values:
            return None
        name = values[0]
        cls = self._selected_class()
        if not cls:
            return None
        for r in self._recipes.get(cls, []):
            if r.name == name:
                return r
        return None

    # ── Actions ───────────────────────────────────────────────────────

    def _add_recipe(self) -> None:
        cls = self._selected_class()
        if not cls:
            messagebox.showinfo("Info", "Select a machine class first.")
            return
        dlg = RecipeEditDialog(self, class_name=cls, title="New Recipe")
        if dlg.result:
            self._recipes.setdefault(cls, []).append(dlg.result)
            self._dirty = True
            self._show_recipes(cls)
            self._update_status()

    def _edit_recipe(self) -> None:
        recipe = self._selected_recipe()
        if not recipe:
            return
        cls = self._selected_class()
        dlg = RecipeEditDialog(self, recipe, title=f"Edit {recipe.name}")
        if dlg.result:
            recipes = self._recipes.get(cls or "", [])
            for i, r in enumerate(recipes):
                if r.name == recipe.name:
                    recipes[i] = dlg.result
                    break
            self._dirty = True
            self._show_recipes(cls or "")
            self._update_status()

    def _delete_recipe(self) -> None:
        recipe = self._selected_recipe()
        if not recipe:
            return
        ok = messagebox.askyesno("Delete", f"Delete recipe '{recipe.name}'?")
        if not ok:
            return
        cls = self._selected_class() or ""
        recipes = self._recipes.get(cls, [])
        self._recipes[cls] = [r for r in recipes if r.name != recipe.name]
        if not self._recipes[cls]:
            del self._recipes[cls]
        self._dirty = True
        self._show_recipes(cls)
        self._update_status()

    # ── Save / Reload ─────────────────────────────────────────────────

    def _save(self) -> None:
        try:
            paths = save_recipes(self._recipes)
            self._dirty = False
            self._update_status()
            messagebox.showinfo("Saved", f"Written {len(paths)} file(s).")
        except Exception as e:
            messagebox.showerror("Save failed", str(e))

    def _reload(self) -> None:
        if self._dirty:
            ok = messagebox.askyesno("Unsaved changes", "Discard changes and reload?")
            if not ok:
                return
        self._machines = load_machines()
        self._recipes = load_recipes()
        self._dirty = False
        self._populate_class_list()
        self._update_status()

    # ── Status ────────────────────────────────────────────────────────

    def _update_status(self) -> None:
        total = sum(len(v) for v in self._recipes.values())
        classes = len(self._recipes)
        dirty = " ● unsaved" if self._dirty else ""
        self._status_var.set(f"{classes} classes, {total} recipes{dirty}  |  {DATA_RECIPES}")

    # ── Close ─────────────────────────────────────────────────────────

    def _on_close(self) -> None:
        if self._dirty:
            ok = messagebox.askyesno("Unsaved changes", "Save before closing?")
            if ok:
                self._save()
        self.destroy()


def main() -> None:
    app = RecipeEditor()
    app.mainloop()


if __name__ == "__main__":
    main()
