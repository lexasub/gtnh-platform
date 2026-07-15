#!/usr/bin/env python3
UINT16_MAX = 65535

NODE_MIN_W = 160
NODE_H = 45
ITEM_H = 15
H_GAP = 25
V_GAP = 60
CANVAS_PAD = 50
BAR_W = 100
BAR_H = 6

COLORS = {
    "bg": "#0d1117",
    "node_fill": "#1a1a2e",
    "node_root": "#16213e",
    "node_outline": "#0f3460",
    "node_selected": "#e94560",
    "node_drop": "#FFC107",
    "bar_bg": "#333",
    "bar_green": "#4CAF50",
    "bar_yellow": "#FFC107",
    "bar_red": "#f44336",
    "text_white": "white",
    "text_dim": "#aaa",
    "text_item": "#888",
    "text_more": "#555",
    "conn": "#0f3460",
}

FONTS = {
    "node_title": ("Arial", 9, "bold"),
    "node_count": ("Consolas", 7),
    "node_item": ("Consolas", 7),
    "node_more": ("Consolas", 7, "italic"),
    "drag_ghost": ("Consolas", 9, "bold"),
}
