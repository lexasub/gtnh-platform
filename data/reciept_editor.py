import json
import csv
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

# ------------------------------
# Load item data from CSV
# ------------------------------
def load_items(csv_path):
    items = {}
    try:
        with open(csv_path, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            for row in reader:
                idx = int(row['id'])
                items[idx] = {
                    'name': row['name'],
                    'stack_size': int(row['stack_size']),
                    'meta': int(row['meta'])
                }
    except FileNotFoundError:
        messagebox.showerror("Error", f"CSV file not found: {csv_path}")
        return {}
    return items

# ------------------------------
# Normalize a slot (input/output entry) to a standard dict
# ------------------------------
def normalize_slot(slot):
    """Convert a raw JSON slot (list) to a dict with keys: id, count, options"""
    if not isinstance(slot, list):
        slot = [slot]  # fallback
    if len(slot) == 1:
        # Legacy format: single-item [id]
        return {'id': slot[0], 'count': 1, 'options': None}
    elif len(slot) == 2:
        # Could be [id, count] or [id, dict]? We assume count is int.
        if isinstance(slot[1], dict):
            return {'id': slot[0], 'count': 1, 'options': slot[1]}
        # Preserve count even when it's 1 for new format
        return {'id': slot[0], 'count': slot[1], 'options': None}
    else:
        # [id, count, options]
        return {'id': slot[0], 'count': slot[1], 'options': slot[2]}

def denormalize_slot(slot):
    """Convert normalized slot back to JSON-compatible list"""
    # Accept both dict and list input
    if isinstance(slot, dict):
        # Dict format: {'id': x, 'count': y, 'options': z}
        id_ = slot['id']
        count = slot['count']
        opts = slot.get('options')
        if opts is None:
            # Always include count to match raw format
            return [id_, count]
        else:
            return [id_, count, opts]
    else:
        # List format (legacy)
        if len(slot) == 1:
            return slot
        id_ = slot[0]
        count = slot[1]
        opts = slot[2] if len(slot) > 2 else None
        if opts is None:
            # Always include count to match raw format
            return [id_, count]
        else:
            return [id_, count, opts]

# ------------------------------
# Item Picker popup
# ------------------------------
class ItemPicker:
    def __init__(self, parent, items_dict, callback):
        self.items_dict = items_dict
        self.callback = callback
        self.dialog = tk.Toplevel(parent)
        self.dialog.title("Pick Item")
        self.dialog.geometry("420x400")
        self.dialog.transient(parent)
        self.dialog.grab_set()

        ttk.Label(self.dialog, text="Search:").pack(fill=tk.X, padx=5, pady=(5,0))
        self.search_var = tk.StringVar()
        search_entry = ttk.Entry(self.dialog, textvariable=self.search_var)
        search_entry.pack(fill=tk.X, padx=5, pady=5)
        self.search_var.trace('w', lambda *a: self._refresh_list())

        self.listbox = tk.Listbox(self.dialog)
        self.listbox.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)
        self.listbox.bind('<Double-Button-1>', lambda e: self._pick())
        list_scroll = ttk.Scrollbar(self.listbox, orient=tk.VERTICAL, command=self.listbox.yview)
        list_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.listbox.configure(yscrollcommand=list_scroll.set)

        btn_frame = ttk.Frame(self.dialog)
        btn_frame.pack(pady=5)
        ttk.Button(btn_frame, text="OK", command=self._pick).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", command=self.dialog.destroy).pack(side=tk.LEFT, padx=5)

        self._refresh_list()
        search_entry.focus_set()

    def _refresh_list(self):
        self.listbox.delete(0, tk.END)
        query = self.search_var.get().lower()
        self._item_ids = []
        for iid, info in sorted(self.items_dict.items()):
            name = info.get('name', '')
            if query in name.lower() or query in str(iid):
                display = f"{name} (id: {iid})"
                self.listbox.insert(tk.END, display)
                self._item_ids.append(iid)

    def _pick(self):
        sel = self.listbox.curselection()
        if sel:
            iid = self._item_ids[sel[0]]
            self.callback(iid)
        self.dialog.destroy()

# ------------------------------
# Recipe editor for non-crafting (variable slots)
# ------------------------------
class SlotListEditor:
    def __init__(self, parent, label, slot_list, items_dict, on_change):
        self.parent = parent
        self.items_dict = items_dict
        self.on_change = on_change
        self.slot_list = slot_list  # list of normalized slot dicts

        self.frame = ttk.LabelFrame(parent, text=label)
        self.frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Treeview to show slots
        cols = ('ID', 'Name', 'Count', 'Options')
        self.tree = ttk.Treeview(self.frame, columns=cols, show='headings', height=6)
        for col in cols:
            self.tree.heading(col, text=col)
            self.tree.column(col, width=100)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        scrollbar = ttk.Scrollbar(self.frame, orient=tk.VERTICAL, command=self.tree.yview)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.configure(yscrollcommand=scrollbar.set)

        # Buttons
        btn_frame = ttk.Frame(self.frame)
        btn_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=5)

        ttk.Button(btn_frame, text="Add Slot", command=self.add_slot).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="Edit Slot", command=self.edit_slot).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="Delete Slot", command=self.delete_slot).pack(side=tk.LEFT, padx=2)

        self.refresh_tree()

    def refresh_tree(self):
        for row in self.tree.get_children():
            self.tree.delete(row)
        for idx, slot in enumerate(self.slot_list):
            name = self.items_dict.get(slot['id'], {}).get('name', f"Unknown({slot['id']})")
            opts = json.dumps(slot['options']) if slot['options'] else ''
            self.tree.insert('', tk.END, values=(slot['id'], name, slot['count'], opts))
        self.on_change()

    def add_slot(self):
        self._edit_slot(None)

    def edit_slot(self):
        sel = self.tree.selection()
        if not sel:
            messagebox.showwarning("Warning", "Select a slot to edit")
            return
        idx = self.tree.index(sel[0])
        self._edit_slot(idx)

    def _edit_slot(self, idx):
        dialog = tk.Toplevel(self.parent)
        dialog.title("Edit Slot")
        dialog.geometry("400x250")
        dialog.transient(self.parent)
        dialog.grab_set()

        ttk.Label(dialog, text="Item:").grid(row=0, column=0, sticky='e', padx=5, pady=5)
        id_var = tk.IntVar(value=self.slot_list[idx]['id'] if idx is not None else 0)
        name_label = ttk.Label(dialog, text="", width=20)
        name_label.grid(row=0, column=2, padx=5, pady=5)

        def pick_item():
            def on_pick(item_id):
                id_var.set(item_id)
            ItemPicker(dialog, self.items_dict, on_pick)
        ttk.Button(dialog, text="Pick Item", command=pick_item).grid(row=0, column=1, padx=5, pady=5)

        def update_name(*args):
            try:
                iid = id_var.get()
                name = self.items_dict.get(iid, {}).get('name', 'Unknown')
                name_label.config(text=f"({name})")
            except:
                name_label.config(text="(invalid)")
        id_var.trace('w', update_name)
        update_name()

        ttk.Label(dialog, text="Count:").grid(row=1, column=0, sticky='e', padx=5, pady=5)
        count_var = tk.IntVar(value=1 if idx is None else self.slot_list[idx]['count'])
        count_spin = ttk.Spinbox(dialog, from_=1, to=64, textvariable=count_var, width=18)
        count_spin.grid(row=1, column=1, padx=5, pady=5)

        ttk.Label(dialog, text="Options (JSON):").grid(row=2, column=0, sticky='ne', padx=5, pady=5)
        opts_text = tk.Text(dialog, height=5, width=40)
        opts_text.grid(row=2, column=1, columnspan=2, padx=5, pady=5)
        if idx is not None and self.slot_list[idx]['options']:
            opts_text.insert('1.0', json.dumps(self.slot_list[idx]['options'], indent=2))

        def save():
            iid = id_var.get()
            count = count_var.get()
            opts_str = opts_text.get('1.0', tk.END).strip()
            opts = None
            if opts_str:
                try:
                    opts = json.loads(opts_str)
                except:
                    messagebox.showerror("Error", "Invalid JSON for options")
                    return
            new_slot = {'id': iid, 'count': count, 'options': opts}
            if idx is None:
                self.slot_list.append(new_slot)
            else:
                self.slot_list[idx] = new_slot
            self.refresh_tree()
            dialog.destroy()

        ttk.Button(dialog, text="Save", command=save).grid(row=3, column=1, pady=10)

    def delete_slot(self):
        sel = self.tree.selection()
        if not sel:
            return
        idx = self.tree.index(sel[0])
        del self.slot_list[idx]
        self.refresh_tree()

    def get_value(self):
        return [denormalize_slot(s) for s in self.slot_list]

# ------------------------------
# Special editor for crafting table (3x3 grid)
# ------------------------------
class CraftingGridEditor:
    def __init__(self, parent, label, slot_list_9, items_dict, on_change):
        self.parent = parent
        self.items_dict = items_dict
        self.on_change = on_change
        # slot_list_9 should be a list of 9 normalized slots (or None)
        if len(slot_list_9) != 9:
            # initialize empty grid
            self.slot_list = [{'id': 0, 'count': 0, 'options': None} for _ in range(9)]
        else:
            self.slot_list = slot_list_9

        self.frame = ttk.LabelFrame(parent, text=label)
        self.frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        self.entries = []  # list of (id_var, count_var)
        # 3x3 grid
        for i in range(3):
            for j in range(3):
                idx = i*3 + j
                subframe = ttk.Frame(self.frame, relief=tk.RIDGE, borderwidth=1)
                subframe.grid(row=i, column=j, padx=2, pady=2, sticky='nsew')

                id_var = tk.IntVar(value=self.slot_list[idx]['id'])
                count_var = tk.IntVar(value=self.slot_list[idx]['count'])

                name_label = ttk.Label(subframe, text="Empty", width=14, anchor=tk.CENTER)
                name_label.pack(side=tk.TOP, fill=tk.X, padx=2, pady=2)

                def update_name(*args, i=idx):
                    val = self.entries[i][0].get()
                    name = self.items_dict.get(val, {}).get('name', '')
                    name_label.config(text=name if name else "Empty")
                id_var.trace('w', update_name)
                update_name()

                pick_btn = ttk.Button(subframe, text="Pick", command=lambda i=idx: self._open_picker(i))
                pick_btn.pack(side=tk.TOP, padx=2, pady=2)

                count_spin = ttk.Spinbox(subframe, from_=0, to=64, textvariable=count_var, width=5)
                count_spin.pack(side=tk.TOP, padx=2, pady=2)

                self.entries.append((id_var, count_var))

                id_var.trace('w', lambda *args, i=idx: self.update_slot(i))
                count_var.trace('w', lambda *args, i=idx: self.update_slot(i))

        for i in range(3):
            self.frame.columnconfigure(i, weight=1)
            self.frame.rowconfigure(i, weight=1)

    def update_slot(self, idx):
        try:
            iid = int(self.entries[idx][0].get())
        except:
            iid = 0
        try:
            cnt = int(self.entries[idx][1].get())
        except:
            cnt = 0
        self.slot_list[idx] = {'id': iid, 'count': cnt, 'options': None}
        self.on_change()

    def _open_picker(self, idx):
        def on_pick(item_id):
            self.entries[idx][0].set(item_id)
        ItemPicker(self.frame, self.items_dict, on_pick)

    def get_value(self):
        # convert to raw JSON format: list of [id, count] (0,0 for empty)
        raw = []
        for slot in self.slot_list:
            if slot['id'] == 0 and slot['count'] == 0:
                raw.append([0, 0])
            else:
                raw.append([slot['id'], slot['count']])
        return raw

# ------------------------------
# Main Application
# ------------------------------
class RecipeEditorApp:
    def __init__(self, root, items):
        self.root = root
        self.items = items
        self.current_recipes = None
        self.current_filepath = None

        self.root.title("Recipe JSON Editor")
        self.root.geometry("1200x700")

        # Menu
        menubar = tk.Menu(root)
        file_menu = tk.Menu(menubar, tearoff=0)
        file_menu.add_command(label="Open JSON", command=self.open_file)
        file_menu.add_command(label="Save As", command=self.save_as)
        file_menu.add_command(label="Save", command=self.save_file)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=root.quit)
        menubar.add_cascade(label="File", menu=file_menu)
        root.config(menu=menubar)

        # Left frame: recipe list
        left_frame = ttk.Frame(root, width=250)
        left_frame.pack(side=tk.LEFT, fill=tk.Y, padx=5, pady=5)

        ttk.Label(left_frame, text="Recipes").pack(anchor=tk.W)
        self.recipe_listbox = tk.Listbox(left_frame)
        self.recipe_listbox.pack(fill=tk.BOTH, expand=True)
        self.recipe_listbox.bind('<<ListboxSelect>>', self.on_recipe_select)

        btn_frame = ttk.Frame(left_frame)
        btn_frame.pack(fill=tk.X, pady=5)
        ttk.Button(btn_frame, text="Add Recipe", command=self.add_recipe).pack(side=tk.LEFT, padx=2)
        ttk.Button(btn_frame, text="Delete Recipe", command=self.delete_recipe).pack(side=tk.LEFT, padx=2)

        # Right frame: recipe editor
        self.right_frame = ttk.Frame(root)
        self.right_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=5, pady=5)

        self.current_recipe_key = None
        self.current_data = None
        self.editor_widgets = {}  # store references to editors

    def open_file(self):
        filepath = filedialog.askopenfilename(
            title="Select JSON recipe file",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        if not filepath:
            return
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                self.current_recipes = json.load(f)
            self.current_filepath = filepath
            self.refresh_recipe_list()
            self.clear_editor()
            self.root.title(f"Recipe Editor - {filepath}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load JSON: {e}")

    def refresh_recipe_list(self):
        self.recipe_listbox.delete(0, tk.END)
        if self.current_recipes:
            for key in sorted(self.current_recipes.keys()):
                self.recipe_listbox.insert(tk.END, key)



    def save_as(self):
        """Save recipes to a new JSON file"""
        if not self.current_recipes:
            messagebox.showwarning("Warning", "No recipes to save")
            return

        filename = asksaveasfilename(
            title="Save Recipes As",
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        if not filename:
            return

        try:
            with open(filename, 'w') as f:
                json.dump(self.current_recipes, f, indent=2)
            messagebox.showinfo("Success", f"Recipes saved to {filename}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save: {e}")
    def clear_editor(self):
        for widget in self.right_frame.winfo_children():
            widget.destroy()
        self.current_recipe_key = None
        self.current_data = None
        self.editor_widgets = {}



    def save_file(self):
        """Save current recipe to file"""
        if not self.current_recipe_key:
            messagebox.showwarning("Warning", "No recipe selected to save")
            return

        filename = asksaveasfilename(
            title="Save Recipe",
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        if not filename:
            return

        try:
            with open(filename, 'w') as f:
                json.dump(self.current_data, f, indent=2)
            messagebox.showinfo("Success", f"Recipe saved to {filename}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save: {e}")
    def on_recipe_select(self, event):
        selection = self.recipe_listbox.curselection()
        if not selection:
            return
        key = self.recipe_listbox.get(selection[0])
        self.load_recipe(key)

    def load_recipe(self, key):
        self.clear_editor()
        self.current_recipe_key = key
        self.current_data = self.current_recipes[key].copy()

        # Top fields: m, dur, eu (optional)
        top_frame = ttk.Frame(self.right_frame)
        top_frame.pack(fill=tk.X, padx=5, pady=5)

        ttk.Label(top_frame, text="Machine type (m):").grid(row=0, column=0, sticky='e')
        m_labels = ["0 - crafting_table", "1 - furnace", "2 - assembler", "3 - crystallizer", "4 - electrolyser", "5 - chemical_reactor"]
        m_var = tk.StringVar(value=m_labels[self.current_data.get('m', 0)])
        m_combo = ttk.Combobox(top_frame, textvariable=m_var, values=m_labels, width=25)
        m_combo.grid(row=0, column=1, sticky='w')
        ttk.Label(top_frame, text="Duration (dur):").grid(row=0, column=2, sticky='e', padx=(10,0))
        dur_var = tk.IntVar(value=self.current_data.get('dur', 0))
        dur_spin = ttk.Spinbox(top_frame, from_=0, to=9999, textvariable=dur_var, width=8)
        dur_spin.grid(row=0, column=3, sticky='w')
        ttk.Label(top_frame, text="EU (optional):").grid(row=0, column=4, sticky='e', padx=(10,0))
        eu_var = tk.StringVar(value=str(self.current_data.get('eu', '')))
        eu_entry = ttk.Entry(top_frame, textvariable=eu_var, width=8)
        eu_entry.grid(row=0, column=5, sticky='w')

        # Store variables for later saving
        self.editor_widgets['m'] = m_var
        self.editor_widgets['dur'] = dur_var
        self.editor_widgets['eu'] = eu_var

        # Input and output editors
        # Normalize input slots
        raw_in = self.current_data.get('in', [])
        # For m=0, enforce 9 slots
        if int(m_var.get().split(" - ")[0]) == 0:
            # Convert raw_in to list of 9 normalized slots
            norm_in = []
            for i in range(9):
                if i < len(raw_in):
                    slot = normalize_slot(raw_in[i])
                else:
                    slot = {'id': 0, 'count': 0, 'options': None}
                norm_in.append(slot)
            self.in_editor = CraftingGridEditor(self.right_frame, "Input (3x3)", norm_in, self.items, self.on_data_changed)
        else:
            norm_in = [normalize_slot(s) for s in raw_in]
            self.in_editor = SlotListEditor(self.right_frame, "Input Slots", norm_in, self.items, self.on_data_changed)

        # Output slots (always variable list, but for crafting table output is single slot usually)
        raw_out = self.current_data.get('out', [])
        norm_out = [normalize_slot(s) for s in raw_out]
        self.out_editor = SlotListEditor(self.right_frame, "Output Slots", norm_out, self.items, self.on_data_changed)

        self.editor_widgets['in'] = self.in_editor
        self.editor_widgets['out'] = self.out_editor

    def on_data_changed(self):
        # This is called whenever any slot editor changes; we could auto-update current_data, but we'll rebuild on save
        pass

    def gather_current_data(self):
        if not self.current_data:
            return None
        data = {}
        m_str = self.editor_widgets['m'].get()
        data['m'] = int(m_str.split(" - ")[0])
        data['dur'] = self.editor_widgets['dur'].get()
        eu_val = self.editor_widgets['eu'].get().strip()
        if eu_val:
            try:
                data['eu'] = float(eu_val)
            except:
                pass
        # Get in/out
        data['in'] = self.editor_widgets['in'].get_value()
        data['out'] = self.editor_widgets['out'].get_value()
        return data

    def _validate(self):
        errors = []
        data = self.gather_current_data()
        if not data:
            return False, ["No data to validate"]
        if data.get('dur', 0) <= 0:
            errors.append("Duration (dur) must be greater than 0")
        for slot_type in ['in', 'out']:
            slots = data.get(slot_type, [])
            for slot in slots:
                if isinstance(slot, list) and len(slot) >= 1:
                    item_id = slot[0]
                    if item_id != 0 and item_id not in self.items:
                        errors.append(f"Item ID {item_id} not found in items.csv ({slot_type})")
        has_input = any(s[0] != 0 for s in data.get('in', []) if isinstance(s, list))
        has_output = any(s[0] != 0 for s in data.get('out', []) if isinstance(s, list))
        if not has_input and not has_output:
            errors.append("At least one input or output slot must be non-empty")
        return len(errors) == 0, errors

    def add_recipe(self):
        if self.current_recipes is None:
            messagebox.showwarning("Warning", "Open a JSON file first")
            return
        new_key = simpledialog.askstring("New Recipe", "Enter recipe key (e.g. minecraft:something):")
        if not new_key:
            return
        if new_key in self.current_recipes:
            messagebox.showerror("Error", "Recipe key already exists")
            return
        # Create default recipe
        self.current_recipes[new_key] = {
            "m": 0,
            "in": [[0,0]]*9 if 0 else [],
            "out": [[0,0]],
            "dur": 100
        }
        self.refresh_recipe_list()
        # Select and load new recipe
        self.recipe_listbox.selection_clear(0, tk.END)
        idx = self.recipe_listbox.get(0, tk.END).index(new_key)
        self.recipe_listbox.selection_set(idx)
        self.load_recipe(new_key)

    def delete_recipe(self):
        if self.current_recipe_key is None:
            return
        if messagebox.askyesno("Confirm", f"Delete recipe '{self.current_recipe_key}'?"):
            del self.current_recipes[self.current_recipe_key]
            self.refresh_recipe_list()
            self.clear_editor()

    def save_as(self):
        if self.current_recipes is None:
            messagebox.showwarning("Warning", "No recipe data to save")
            return
        filepath = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        if not filepath:
            return
        self.current_filepath = filepath
        self.save_file()

    def save_file(self):
        if self.current_recipes is None:
            return
        if self.current_recipe_key is not None:
            valid, errors = self._validate()
            if not valid:
                messagebox.showerror("Validation Error", "\n".join(errors))
                return
            updated_data = self.gather_current_data()
            if updated_data:
                self.current_recipes[self.current_recipe_key] = updated_data
        try:
            with open(self.current_filepath, 'w', encoding='utf-8') as f:
                json.dump(self.current_recipes, f, indent=2, ensure_ascii=False)
            self.root.title(f"Recipe Editor - {self.current_filepath}")
            messagebox.showinfo("Saved", f"Saved to {self.current_filepath}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save: {e}")

# ------------------------------
# Main
# ------------------------------
if __name__ == "__main__":
    # Load items.csv (assume in same directory as script)
    import os
    items_dict = load_items("registry/items.csv")
    if not items_dict:
        # try to get from script location
        script_dir = os.path.dirname(os.path.abspath(__file__))
        csv_path = os.path.join(script_dir, "registry/items.csv")
        items_dict = load_items(csv_path)

    root = tk.Tk()
    app = RecipeEditorApp(root, items_dict)
    root.mainloop()