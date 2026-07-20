#!/usr/bin/env python3
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from pathlib import Path
from typing import Dict, List, Tuple, Optional

from tools.editor_config import (
    UINT16_MAX, NODE_MIN_W, NODE_H, ITEM_H, H_GAP, V_GAP, CANVAS_PAD,
    BAR_W, BAR_H, COLORS, FONTS,
)
from tools.editor_model import (
    Item, parse_csv, build_tree, subtree_count, validate_ids, find_next_id,
)


class TreeCanvas:
    def __init__(self, canvas: tk.Canvas, editor: "ItemRegistryEditor"):
        self.canvas = canvas
        self.editor = editor
        self.nodes: Dict[str, dict] = {}
        self.selected: Optional[str] = None
        self.drag_data: Optional[dict] = None
        self.drag_ghost: Optional[int] = None
        self.drag_highlight: Optional[int] = None
        self._bind_events()

    def _bind_events(self):
        self.canvas.bind("<B1-Motion>", self._on_drag_motion)
        self.canvas.bind("<ButtonRelease-1>", self._on_drag_stop)
        self.canvas.bind("<Double-Button-1>", self._on_double_click)

    def layout(self):
        self.canvas.delete("all")
        self.nodes.clear()
        tree = self.editor.tree
        labels = self.editor.labels
        self._measure_node("", tree, labels)
        self._place_node("", tree, CANVAS_PAD, CANVAS_PAD + self.nodes[""]["h"] / 2)

        max_x = max((n["x"] + n["w"] / 2 for n in self.nodes.values()), default=400) + CANVAS_PAD
        max_y = max((n["y"] + n["h"] / 2 for n in self.nodes.values()), default=300) + CANVAS_PAD
        view_w = max(self.canvas.winfo_width(), 800)
        if max_x < view_w:
            offset = (view_w - max_x) / 2
            for n in self.nodes.values():
                n["x"] += offset
            max_x += offset

        self.canvas.configure(scrollregion=(0, 0, max_x, max_y))
        self._draw_connections("", tree)
        for prefix in sorted(self.nodes.keys(), key=lambda p: len(p.split(":"))):
            self._draw_node(prefix, self.nodes[prefix])

    def _measure_node(self, prefix: str, node: dict, labels: dict):
        items = node.get("__items__", [])
        label = labels.get(prefix, prefix or "root")
        max_text = len(label)
        for _, name, _, _ in items:
            max_text = max(max_text, len(name) + 2)
        w = max(NODE_MIN_W, max_text * 7 + 30)
        subtree = subtree_count(node)
        h = NODE_H + len(items) * ITEM_H + 10
        self.nodes[prefix] = {"w": w, "h": h, "label": label, "items": items, "subtree": subtree, "x": 0, "y": 0}

        children = [k for k in node if k != "__items__"]
        if not children:
            self.nodes[prefix]["subtree_w"] = w
            self.nodes[prefix]["subtree_cx"] = w / 2
            return

        child_data = []
        total_child_w = 0
        for ck in children:
            cp = f"{prefix}:{ck}" if prefix else ck
            self._measure_node(cp, node[ck], labels)
            cw = self.nodes[cp]["subtree_w"]
            ccx = self.nodes[cp]["subtree_cx"]
            child_data.append((cp, cw, ccx))
            total_child_w += cw

        gaps = H_GAP * (len(children) - 1)
        children_total = total_child_w + gaps
        total_w = max(w, children_total)
        cx = total_w / 2
        offset = (total_w - children_total) / 2
        current_x = offset
        child_offsets = {}
        for cp, cw, ccx in child_data:
            child_offsets[cp] = current_x + ccx
            current_x += cw + H_GAP

        self.nodes[prefix]["subtree_w"] = total_w
        self.nodes[prefix]["subtree_cx"] = cx
        self.nodes[prefix]["child_offsets"] = child_offsets

    def _place_node(self, prefix: str, node: dict, left_x: float, center_y: float):
        self.nodes[prefix]["x"] = left_x + self.nodes[prefix]["subtree_cx"]
        self.nodes[prefix]["y"] = center_y

        children = [k for k in node if k != "__items__"]
        if not children:
            return

        parent_h = self.nodes[prefix]["h"]
        for ck in children:
            cp = f"{prefix}:{ck}" if prefix else ck
            child_offset = self.nodes[prefix]["child_offsets"][cp]
            child_cx = self.nodes[cp]["subtree_cx"]
            child_left = left_x + child_offset - child_cx
            child_h = self.nodes[cp]["h"]
            child_y = center_y + parent_h / 2 + V_GAP + child_h / 2
            self._place_node(cp, node[ck], child_left, child_y)

    def _draw_connections(self, prefix: str, node: dict):
        children = [k for k in node if k != "__items__"]
        if not children:
            return
        parent = self.nodes.get(prefix)
        if not parent:
            return
        for ck in children:
            cp = f"{prefix}:{ck}" if prefix else ck
            child = self.nodes.get(cp)
            if child:
                self.canvas.create_line(
                    parent["x"], parent["y"] + parent["h"] / 2,
                    child["x"], child["y"] - child["h"] / 2,
                    fill=COLORS["conn"], width=1.5, tags=("conn",)
                )
                self._draw_connections(cp, node[ck])

    def _draw_node(self, prefix: str, node: dict):
        x, y = node["x"], node["y"]
        w, h = node["w"], node["h"]
        label = node["label"]
        items = node["items"]
        subtree = node["subtree"]
        is_root = prefix == ""
        is_selected = prefix == self.selected
        is_drop_target = self.drag_data and self.drag_data.get("target") == prefix

        fill = COLORS["node_root"] if is_root else COLORS["node_fill"]
        outline = COLORS["node_selected"] if is_selected else COLORS["node_drop"] if is_drop_target else COLORS["node_outline"]

        self.canvas.create_rectangle(
            x - w / 2, y - h / 2, x + w / 2, y + h / 2,
            fill=fill, outline=outline, width=2,
            tags=(f"node:{prefix}",)
        )

        title_y = y - h / 2 + 14
        self.canvas.create_text(
            x, title_y, text=label,
            fill=COLORS["text_white"], font=FONTS["node_title"],
            tags=(f"node:{prefix}",)
        )
        if prefix:
            self.canvas.create_text(
                x, title_y + 14, text=prefix,
                fill=COLORS["text_dim"], font=FONTS["node_count"],
                tags=(f"node:{prefix}",)
            )

        if not is_root:
            parent_count = self._parent_count(prefix)
            count = subtree
            pct = count / parent_count if parent_count > 0 else 0
            bar_x = x - BAR_W / 2
            bar_y = title_y + 28
            self.canvas.create_rectangle(bar_x, bar_y, bar_x + BAR_W, bar_y + BAR_H, fill=COLORS["bar_bg"], outline="", tags=(f"node:{prefix}",))
            color = COLORS["bar_green"] if pct < 0.5 else COLORS["bar_yellow"] if pct < 0.8 else COLORS["bar_red"]
            self.canvas.create_rectangle(bar_x, bar_y, bar_x + BAR_W * pct, bar_y + BAR_H, fill=color, outline="", tags=(f"node:{prefix}",))
            self.canvas.create_text(
                x, bar_y + BAR_H + 10, text=f"{count}/{parent_count} ({pct*100:.0f}%)",
                fill=COLORS["text_dim"], font=FONTS["node_count"],
                tags=(f"node:{prefix}",)
            )
            item_y = bar_y + BAR_H + 20
        else:
            item_y = title_y + 18

        for item_id, name, stack, meta in items:
            self.canvas.create_text(
                x, item_y, text=f"  {name}",
                fill=COLORS["text_item"], font=FONTS["node_item"],
                anchor=tk.CENTER, tags=(f"item:{item_id}",)
            )
            item_y += ITEM_H

        self.canvas.tag_bind(f"node:{prefix}", "<ButtonPress-1>", lambda e, p=prefix: self._on_node_press(p, e))
        for item_id, _, _, _ in items:
            self.canvas.tag_bind(f"item:{item_id}", "<ButtonPress-1>", lambda e, iid=item_id: self._on_item_press(iid, e))
            self.canvas.tag_bind(f"item:{item_id}", "<Double-1>", lambda e, iid=item_id: self._on_item_double_click(iid))

    def _parent_count(self, prefix: str) -> int:
        if not prefix:
            return 1
        segs = prefix.split(":")
        node = self.editor.tree
        for seg in segs[:-1]:
            node = node.get(seg, {})
        return subtree_count(node)

    def _on_node_press(self, prefix: str, event):
        self._on_node_click(prefix)
        self.start_drag_node(prefix, event)

    def _on_node_click(self, prefix: str):
        self.selected = prefix
        self.layout()

    def _on_item_press(self, item_id: str, event):
        self.start_drag_item(item_id, event)

    def _on_item_double_click(self, item_id: str):
        for iid, name, stack, meta in self.editor.items:
            if iid == item_id:
                self.editor._edit_item_dialog(item_id, name, stack, meta)
                break

    def _on_double_click(self, event):
        items = self.canvas.find_withtag("current")
        if not items:
            return
        tags = self.canvas.gettags(items[0])
        for tag in tags:
            if tag.startswith("node:"):
                prefix = tag[5:]
                if prefix in self.editor.grouped:
                    self.editor._add_item_to_prefix(prefix)
                break

    def start_drag_item(self, item_id: str, event):
        self.drag_data = {"type": "item", "id": item_id, "x": event.x, "y": event.y}

    def start_drag_node(self, prefix: str, event):
        self.drag_data = {"type": "node", "prefix": prefix, "x": event.x, "y": event.y}

    def _on_drag_motion(self, event):
        if not self.drag_data:
            return
        if self.drag_ghost:
            self.canvas.delete(self.drag_ghost)
        cx = self.canvas.canvasx(event.x)
        cy = self.canvas.canvasy(event.y)
        drag_id = self.drag_data.get("id") or self.drag_data.get("prefix")
        self.drag_ghost = self.canvas.create_text(
            cx, cy, text=f"↔ {drag_id}",
            fill=COLORS["node_selected"], font=FONTS["drag_ghost"], anchor=tk.CENTER
        )
        target = self._node_at(cx, cy)
        if target and target != self.drag_data.get("target"):
            self._clear_highlight()
            self._highlight_node(target)
            self.drag_data["target"] = target
        elif not target and self.drag_data.get("target"):
            self._clear_highlight()
            self.drag_data["target"] = None

    def _on_drag_stop(self, event):
        if not self.drag_data:
            return
        self._clear_highlight()
        if self.drag_ghost:
            self.canvas.delete(self.drag_ghost)
            self.drag_ghost = None
        target = self.drag_data.get("target")
        drag_type = self.drag_data["type"]

        if drag_type == "item":
            item_id = self.drag_data["id"]
            if target and target in self.editor.grouped:
                old_prefix = ":".join(item_id.split(":")[:-1])
                if old_prefix == target:
                    self.drag_data = None
                    return
                item_data = None
                for it in self.editor.items:
                    if it[0] == item_id:
                        item_data = it
                        break
                if item_data:
                    if old_prefix in self.editor.grouped:
                        self.editor.grouped[old_prefix] = [it for it in self.editor.grouped[old_prefix] if it[0] != item_id]
                        if not self.editor.grouped[old_prefix]:
                            del self.editor.grouped[old_prefix]
                    new_id = find_next_id(self.editor.grouped, target)
                    new_item = (new_id, item_data[1], item_data[2], item_data[3])
                    for i, it in enumerate(self.editor.items):
                        if it[0] == item_id:
                            self.editor.items[i] = new_item
                            break
                    if target not in self.editor.grouped:
                        self.editor.grouped[target] = []
                    self.editor.grouped[target].append(new_item)
                    self.editor.modified = True
                    self.editor._refresh()

        elif drag_type == "node":
            src_prefix = self.drag_data["prefix"]
            if target and target != src_prefix and not target.startswith(src_prefix + ":") and not src_prefix.startswith(target + ":"):
                self.editor._regroup(src_prefix, target)

        self.drag_data = None

    def _node_at(self, x: float, y: float) -> Optional[str]:
        for prefix, node in self.nodes.items():
            nx, ny, nw, nh = node["x"], node["y"], node["w"], node["h"]
            if nx - nw / 2 <= x <= nx + nw / 2 and ny - nh / 2 <= y <= ny + nh / 2:
                return prefix
        return None

    def _highlight_node(self, prefix: str):
        node = self.nodes.get(prefix)
        if not node:
            return
        self.drag_highlight = self.canvas.create_rectangle(
            node["x"] - node["w"] / 2 - 3, node["y"] - node["h"] / 2 - 3,
            node["x"] + node["w"] / 2 + 3, node["y"] + node["h"] / 2 + 3,
            outline=COLORS["node_drop"], width=3, dash=(5, 3)
        )

    def _clear_highlight(self):
        if self.drag_highlight:
            self.canvas.delete(self.drag_highlight)
            self.drag_highlight = None


class ItemRegistryEditor:
    def __init__(self, root: tk.Tk, csv_path: str):
        self.root = root
        self.root.title("Item Registry Editor")
        self.root.geometry("1200x800")
        self.csv_path = csv_path
        self.items: List[Item] = []
        self.grouped: Dict[str, List[Item]] = {}
        self.labels: Dict[str, str] = {}
        self.tree: dict = {}
        self.modified = False
        self._setup_styles()
        self._create_menu()
        self._create_ui()
        self._load_data()

    def _setup_styles(self):
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Treeview", rowheight=24, font=("Consolas", 10))

    def _create_menu(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)
        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="Open...", command=self._open_file, accelerator="Ctrl+O")
        file_menu.add_command(label="Save", command=self._save_file, accelerator="Ctrl+S")
        file_menu.add_command(label="Save As...", command=self._save_as)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self._on_exit)
        edit_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Edit", menu=edit_menu)
        edit_menu.add_command(label="Add Item", command=self._add_item, accelerator="Ctrl+N")
        edit_menu.add_command(label="Delete", command=self._delete_item, accelerator="Del")
        self.root.bind("<Control-o>", lambda e: self._open_file())
        self.root.bind("<Control-s>", lambda e: self._save_file())
        self.root.bind("<Control-n>", lambda e: self._add_item())
        self.root.bind("<Delete>", lambda e: self._delete_item())

    def _create_ui(self):
        toolbar = ttk.Frame(self.root)
        toolbar.pack(fill=tk.X, padx=5, pady=5)
        ttk.Label(toolbar, text="Search:").pack(side=tk.LEFT, padx=(0, 5))
        self.search_var = tk.StringVar()
        ttk.Entry(toolbar, textvariable=self.search_var, width=25).pack(side=tk.LEFT, padx=(0, 15))
        ttk.Button(toolbar, text="+ Item", command=self._add_item).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="+ Group", command=self._add_group).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="Delete", command=self._delete_item).pack(side=tk.LEFT, padx=2)
        ttk.Separator(toolbar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=5)
        ttk.Button(toolbar, text="Validate", command=self._validate).pack(side=tk.LEFT, padx=2)
        self.counter_var = tk.StringVar()
        self.counter_label = ttk.Label(toolbar, textvariable=self.counter_var, font=("Consolas", 9))
        self.counter_label.pack(side=tk.LEFT, padx=(10, 5))
        self.status_var = tk.StringVar(value="Ready")
        ttk.Label(toolbar, textvariable=self.status_var).pack(side=tk.RIGHT, padx=5)

        canvas_frame = ttk.Frame(self.root)
        canvas_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=(0, 5))
        v_scroll = ttk.Scrollbar(canvas_frame, orient=tk.VERTICAL)
        v_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        h_scroll = ttk.Scrollbar(canvas_frame, orient=tk.HORIZONTAL)
        h_scroll.pack(side=tk.BOTTOM, fill=tk.X)
        self.canvas = tk.Canvas(canvas_frame, bg=COLORS["bg"], highlightthickness=0,
                                xscrollcommand=h_scroll.set, yscrollcommand=v_scroll.set)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        v_scroll.config(command=self.canvas.yview)
        h_scroll.config(command=self.canvas.xview)
        self.canvas.bind("<Configure>", lambda e: self._redraw())

        bottom = ttk.Frame(self.root)
        bottom.pack(fill=tk.X, padx=5, pady=(0, 5))
        self.validation_text = tk.Text(bottom, height=3, wrap=tk.WORD, state=tk.DISABLED, font=FONTS["node_count"])
        self.validation_text.pack(fill=tk.X)

    def _load_data(self):
        self.items, self.grouped, self.labels = parse_csv(self.csv_path)
        self.tree = build_tree(self.grouped)
        self._redraw()
        self._validate()

    def _redraw(self):
        self.canvas.update_idletasks()
        self.tree_canvas = TreeCanvas(self.canvas, self)
        self.tree_canvas.layout()
        self._update_counter()

    def _update_counter(self):
        total = len(self.items)
        pct = (total / UINT16_MAX) * 100
        self.counter_var.set(f"{total}/{UINT16_MAX} ({pct:.1f}%)")
        if pct < 50:
            self.counter_label.configure(foreground="green")
        elif pct < 80:
            self.counter_label.configure(foreground="#b8860b")
        else:
            self.counter_label.configure(foreground="red")

    def _regroup(self, src_prefix: str, new_parent: str):
        src_seg = src_prefix.split(":")[-1]
        new_prefix = f"{new_parent}:{src_seg}" if new_parent else src_seg

        # Same parent = no-op
        old_parent = ":".join(src_prefix.split(":")[:-1])
        if old_parent == new_parent:
            return

        to_move = [p for p in self.grouped if p == src_prefix or p.startswith(src_prefix + ":")]
        if not to_move:
            return

        for old_p in to_move:
            suffix = old_p[len(src_prefix):]
            new_p = new_prefix + suffix
            if new_p in self.grouped:
                messagebox.showerror("Error", f"Target group {new_p} already exists")
                return

        new_grouped: Dict[str, List[Item]] = {}
        new_labels_moved: Dict[str, str] = {}

        for old_p, old_items in self.grouped.items():
            if old_p in to_move:
                suffix = old_p[len(src_prefix):]
                new_p = new_prefix + suffix
                new_items = []
                for item_id, name, stack, meta in old_items:
                    new_id = new_p + ":" + item_id.split(":")[-1]
                    new_items.append((new_id, name, stack, meta))
                    for i, (iid, n, s, m) in enumerate(self.items):
                        if iid == item_id:
                            self.items[i] = (new_id, n, s, m)
                            break
                new_grouped[new_p] = new_items
                if old_p in self.labels:
                    new_labels_moved[new_p] = self.labels.pop(old_p)
            else:
                new_grouped[old_p] = old_items

        self.grouped = new_grouped
        self.labels.update(new_labels_moved)
        self._refresh()

    def _add_item(self):
        if len(self.items) >= UINT16_MAX:
            messagebox.showerror("Error", f"uint16_t limit: {UINT16_MAX}")
            return
        prefix = self.tree_canvas.selected if hasattr(self, "tree_canvas") else None
        if not prefix or prefix not in self.grouped:
            messagebox.showwarning("Warning", "Select a group")
            return
        self._add_item_to_prefix(prefix)

    def _add_item_to_prefix(self, prefix: str):
        new_id = find_next_id(self.grouped, prefix)
        data = self._edit_item_dialog(new_id, "new_item", "", "0")
        if data:
            self.items.append(data)
            if prefix not in self.grouped:
                self.grouped[prefix] = []
            self.grouped[prefix].append(data)
            self.modified = True
            self._refresh()

    def _add_group(self):
        parent = self.tree_canvas.selected if hasattr(self, "tree_canvas") else ""
        children = [k for k in self.tree if k != "__items__"]
        if parent:
            node = self.tree
            for seg in parent.split(":"):
                node = node.get(seg, {})
            children = [k for k in node if k != "__items__"]

        dialog = tk.Toplevel(self.root)
        dialog.title(f"Add Group → {parent or 'root'}")
        dialog.geometry("400x200")
        dialog.transient(self.root)
        dialog.grab_set()

        ttk.Label(dialog, text=f"Parent: {parent or 'root'}").grid(row=0, column=0, columnspan=2, padx=5, pady=5, sticky=tk.W)

        if children:
            ttk.Label(dialog, text="Existing children:").grid(row=1, column=0, columnspan=2, padx=5, pady=(5, 0), sticky=tk.W)
            child_list = tk.Listbox(dialog, height=min(len(children), 6), font=("Consolas", 10))
            child_list.grid(row=2, column=0, columnspan=2, padx=5, pady=2, sticky=tk.EW)
            for c in sorted(children):
                cp = f"{parent}:{c}" if parent else c
                cnt = subtree_count(self.tree.get(c, {}))
                child_list.insert(tk.END, f"  {c}  ({cnt} items)")
        else:
            ttk.Label(dialog, text="(no children yet)").grid(row=1, column=0, columnspan=2, padx=5, pady=(5, 0), sticky=tk.W)

        ttk.Label(dialog, text="New segment:").grid(row=3, column=0, padx=5, pady=5, sticky=tk.W)
        seg_var = tk.StringVar()
        seg_entry = ttk.Entry(dialog, textvariable=seg_var, width=15)
        seg_entry.grid(row=3, column=1, padx=5, pady=5, sticky=tk.W)
        seg_entry.focus_set()

        def create(_event=None):
            seg = seg_var.get().strip()
            if not seg:
                messagebox.showerror("Error", "Segment required")
                return
            new_prefix = f"{parent}:{seg}" if parent else seg
            if new_prefix in self.grouped:
                messagebox.showerror("Error", f"Group exists: {new_prefix}")
                return
            self.grouped[new_prefix] = []
            self.tree = build_tree(self.grouped)
            self._refresh()
            dialog.destroy()

        ttk.Button(dialog, text="Create", command=create).grid(row=4, column=1, padx=5, pady=10, sticky=tk.E)
        seg_entry.bind("<Return>", create)

    def _delete_item(self):
        prefix = self.tree_canvas.selected if hasattr(self, "tree_canvas") else None
        if not prefix:
            return
        if prefix in self.grouped:
            if not messagebox.askyesno("Delete", f"Delete group {prefix} and all items?"):
                return
            for item_id, _, _, _ in self.grouped[prefix]:
                self.items = [i for i in self.items if i[0] != item_id]
            del self.grouped[prefix]
        else:
            if not messagebox.askyesno("Delete", f"Delete {prefix}?"):
                return
            self.items = [i for i in self.items if i[0] != prefix]
            for p in list(self.grouped.keys()):
                self.grouped[p] = [i for i in self.grouped[p] if i[0] != prefix]
                if not self.grouped[p]:
                    del self.grouped[p]
        self._refresh()

    def _edit_item_dialog(self, item_id: str, name: str, stack: str, meta: str) -> Optional[Item]:
        dialog = tk.Toplevel(self.root)
        dialog.title(f"Edit: {name}")
        dialog.geometry("350x180")
        dialog.transient(self.root)
        dialog.grab_set()

        result: Optional[Item] = None

        ttk.Label(dialog, text="ID:").grid(row=0, column=0, padx=5, pady=5, sticky=tk.W)
        ttk.Label(dialog, text=item_id).grid(row=0, column=1, padx=5, pady=5, sticky=tk.W)
        ttk.Label(dialog, text="Name:").grid(row=1, column=0, padx=5, pady=5, sticky=tk.W)
        name_var = tk.StringVar(value=name)
        ttk.Entry(dialog, textvariable=name_var, width=25).grid(row=1, column=1, padx=5, pady=5)
        ttk.Label(dialog, text="Stack:").grid(row=2, column=0, padx=5, pady=5, sticky=tk.W)
        stack_var = tk.StringVar(value=stack)
        ttk.Entry(dialog, textvariable=stack_var, width=10).grid(row=2, column=1, padx=5, pady=5, sticky=tk.W)
        ttk.Label(dialog, text="Meta:").grid(row=3, column=0, padx=5, pady=5, sticky=tk.W)
        meta_var = tk.StringVar(value=meta)
        ttk.Entry(dialog, textvariable=meta_var, width=10).grid(row=3, column=1, padx=5, pady=5, sticky=tk.W)

        def save():
            nonlocal result
            new_name = name_var.get().strip()
            if not new_name:
                messagebox.showerror("Error", "Name required")
                return
            result = (item_id, new_name, stack_var.get().strip(), meta_var.get().strip())
            dialog.destroy()

        def cancel():
            dialog.destroy()

        ttk.Button(dialog, text="Save", command=save).grid(row=4, column=1, padx=5, pady=10, sticky=tk.E)
        ttk.Button(dialog, text="Cancel", command=cancel).grid(row=4, column=0, padx=5, pady=10, sticky=tk.W)

        self.root.wait_window(dialog)
        return result

    def _update_item(self, item_id: str, name: str, stack: str, meta: str):
        for i, (iid, _, _, _) in enumerate(self.items):
            if iid == item_id:
                self.items[i] = (iid, name, stack, meta)
                break
        segs = item_id.split(":")
        prefix = ":".join(segs[:-1])
        if prefix in self.grouped:
            for i, (iid, _, _, _) in enumerate(self.grouped[prefix]):
                if iid == item_id:
                    self.grouped[prefix][i] = (item_id, name, stack, meta)
                    break
        self._refresh()

    def _validate(self):
        errors = validate_ids(self.items)
        self.validation_text.config(state=tk.NORMAL)
        self.validation_text.delete(1.0, tk.END)
        if errors:
            for err in errors:
                self.validation_text.insert(tk.END, f"✗ {err}\n")
            self.validation_text.config(fg="red")
            self.status_var.set(f"{len(errors)} error(s)")
        else:
            self.validation_text.insert(tk.END, f"✓ {len(self.items)} items valid")
            self.validation_text.config(fg="green")
            self.status_var.set("valid")
        self.validation_text.config(state=tk.DISABLED)
        self._update_counter()

    def _refresh(self):
        self.tree = build_tree(self.grouped)
        self._redraw()
        self._validate()

    def _open_file(self):
        path = filedialog.askopenfilename(title="Open", filetypes=[("CSV", "*.csv"), ("All", "*.*")])
        if path:
            self.csv_path = path
            self._load_data()
            self.modified = False

    def _save_file(self):
        if not self.csv_path:
            self._save_as()
            return
        self._write_csv(self.csv_path)

    def _save_as(self):
        path = filedialog.asksaveasfilename(title="Save", defaultextension=".csv", filetypes=[("CSV", "*.csv"), ("All", "*.*")])
        if path:
            self.csv_path = path
            self._write_csv(path)

    def _write_csv(self, path: str):
        try:
            with open(path, "w", newline="") as f:
                f.write("# int,name,stack,meta\n")
                for prefix in sorted(self.grouped.keys()):
                    label = self.labels.get(prefix, "")
                    if label:
                        f.write(f"\n# {label} (prefix {prefix})\n")
                    elif prefix:
                        f.write(f"\n# {prefix}\n")
                    else:
                        f.write("\n")
                    for item_id, name, stack, meta in self.grouped[prefix]:
                        f.write(f"{item_id},{name},{stack},{meta}\n")
            self.modified = False
            self.status_var.set(f"Saved: {path}")
        except Exception as e:
            messagebox.showerror("Error", str(e))

    def _on_exit(self):
        if self.modified and messagebox.askyesno("Unsaved", "Save?"):
            self._save_file()
        self.root.destroy()


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent.parent / "data" / "registry" / "items.csv")
    if not Path(csv_path).exists():
        print(f"Not found: {csv_path}")
        sys.exit(1)
    root = tk.Tk()
    ItemRegistryEditor(root, csv_path)
    root.mainloop()


if __name__ == "__main__":
    main()