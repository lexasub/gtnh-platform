#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from OpenGL.GL import *
from OpenGL.GLU import *
from OpenGL.GLUT import *

TILE_SIZE = 16
ATLAS_TILES_X = 16
ATLAS_TILES_Y = 16
ATLAS_W = TILE_SIZE * ATLAS_TILES_X
ATLAS_H = TILE_SIZE * ATLAS_TILES_Y

FACE_NAMES = ["+X", "-X", "+Y", "-Y", "+Z", "-Z"]

CUBE_FACES = {
    "+X": [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)],
    "-X": [(0, 1, 0), (0, 0, 0), (0, 0, 1), (0, 1, 1)],
    "+Y": [(0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)],
    "-Y": [(0, 0, 1), (1, 0, 1), (1, 0, 0), (0, 0, 0)],
    "+Z": [(1, 0, 1), (1, 1, 1), (0, 1, 1), (0, 0, 1)],
    "-Z": [(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)],
}

FACE_NORMALS = {
    "+X": (1, 0, 0),
    "-X": (-1, 0, 0),
    "+Y": (0, 1, 0),
    "-Y": (0, -1, 0),
    "+Z": (0, 0, 1),
    "-Z": (0, 0, -1),
}


class TileInfo:
    __slots__ = ("tile_id", "filename", "tile_x", "tile_y", "rotate")

    def __init__(self, tile_id: int, filename: str, tile_x: int, tile_y: int, rotate: int = 0):
        self.tile_id = tile_id
        self.filename = filename
        self.tile_x = tile_x
        self.tile_y = tile_y
        self.rotate = rotate


class BlockFaceInfo:
    __slots__ = ("block_id", "faces", "transparent")

    def __init__(self, block_id: str, faces: list[int], transparent: bool = False):
        self.block_id = block_id
        self.faces = faces
        self.transparent = transparent


def load_tiles(csv_path: Path) -> dict[int, TileInfo]:
    tiles: dict[int, TileInfo] = {}
    with open(csv_path, newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if len(row) < 4:
                continue
            tile_id = int(row[0])
            filename = row[1].strip()
            tile_x = int(row[2])
            tile_y = int(row[3])
            rotate = int(row[4]) if len(row) > 4 and row[4].strip() else 0
            tiles[tile_id] = TileInfo(tile_id, filename, tile_x, tile_y, rotate)
    return tiles


def load_block_faces(csv_path: Path) -> dict[int, BlockFaceInfo]:
    blocks: dict[int, BlockFaceInfo] = {}
    with open(csv_path, newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for row in reader:
            if len(row) < 7:
                continue
            block_id = row[0]
            faces = [int(row[i]) for i in range(1, 7)]
            transparent = len(row) > 7 and row[7].strip() == "1"
            blocks[block_id] = BlockFaceInfo(block_id, faces, transparent)
    return blocks


def extract_tile_pixels(
    img: Image.Image, tile_x: int, tile_y: int, rotate: int
) -> Image.Image:
    px = tile_x * TILE_SIZE
    py = tile_y * TILE_SIZE
    tile = img.crop((px, py, px + TILE_SIZE, py + TILE_SIZE))
    if rotate == 1:
        tile = tile.transpose(Image.Transpose.ROTATE_270)
    elif rotate == 2:
        tile = tile.transpose(Image.Transpose.ROTATE_180)
    elif rotate == 3:
        tile = tile.transpose(Image.Transpose.ROTATE_90)
    return tile


def build_atlas(
    data_dir: Path, tiles: dict[int, TileInfo]
) -> tuple[Image.Image, dict[int, tuple[int, int]]]:
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    slot_map: dict[int, tuple[int, int]] = {}
    png_cache: dict[str, Image.Image] = {}
    sorted_tiles = sorted(tiles.values(), key=lambda t: t.tile_id)

    for idx, tile in enumerate(sorted_tiles):
        col = idx % ATLAS_TILES_X
        row = idx // ATLAS_TILES_X
        if row >= ATLAS_TILES_Y:
            print(f"Warning: atlas full, tile {tile.tile_id} dropped")
            continue
        slot_map[tile.tile_id] = (col, row)

        png_key = tile.filename
        if png_key not in png_cache:
            png_path = data_dir / tile.filename
            if not png_path.exists():
                print(f"Warning: {png_path} not found, using checkerboard")
                checker = Image.new("RGBA", (TILE_SIZE, TILE_SIZE))
                for y in range(TILE_SIZE):
                    for x in range(TILE_SIZE):
                        c = 255 if (x // 8 + y // 8) % 2 == 0 else 0
                        checker.putpixel((x, y), (255, 0, 255, 255) if c else (0, 0, 0, 255))
                png_cache[png_key] = checker
            else:
                png_cache[png_key] = Image.open(png_path).convert("RGBA")

        tile_img = extract_tile_pixels(
            png_cache[png_key], tile.tile_x, tile.tile_y, tile.rotate
        )
        atlas.paste(tile_img, (col * TILE_SIZE, row * TILE_SIZE))

    return atlas, slot_map, png_cache


def add_rotated_tile(
    atlas: Image.Image,
    slot_map: dict[int | tuple[int, int], tuple[int, int]],
    png_cache: dict[str, Image.Image],
    data_dir: Path,
    tile_id: int,
    target_rotation: int,
) -> tuple[int, int]:
    next_idx = max(v[0] + v[1] * ATLAS_TILES_X for v in slot_map.values()) + 1
    col = next_idx % ATLAS_TILES_X
    row = next_idx // ATLAS_TILES_X
    if row >= ATLAS_TILES_Y:
        raise RuntimeError("atlas full, cannot add rotated tile")

    tile_info = None
    with open(data_dir / "textures.csv", newline="") as f:
        reader = csv.reader(f)
        next(reader)
        for csv_row in reader:
            if len(csv_row) >= 4 and int(csv_row[0]) == tile_id:
                tile_info = TileInfo(
                    tile_id, csv_row[1].strip(),
                    int(csv_row[2]), int(csv_row[3]),
                    int(csv_row[4]) if len(csv_row) > 4 and csv_row[4].strip() else 0,
                )
                break

    if tile_info is None:
        raise RuntimeError(f"tile_id {tile_id} not found in textures.csv")

    png_key = tile_info.filename
    if png_key not in png_cache:
        png_path = data_dir / tile_info.filename
        if not png_path.exists():
            raise RuntimeError(f"{png_path} not found")
        png_cache[png_key] = Image.open(png_path).convert("RGBA")

    tile_img = extract_tile_pixels(
        png_cache[png_key], tile_info.tile_x, tile_info.tile_y, target_rotation
    )
    atlas.paste(tile_img, (col * TILE_SIZE, row * TILE_SIZE))
    slot_key = (tile_id, target_rotation)
    slot_map[slot_key] = (col, row)
    return slot_key


def upload_texture(img: Image.Image) -> int:
    tex = glGenTextures(1)
    glBindTexture(GL_TEXTURE_2D, tex)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE)
    data = img.tobytes()
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, data,
    )
    return tex


def get_tile_uv(
    tile_id: int | tuple[int, int],
    slot_map: dict[int | tuple[int, int], tuple[int, int]],
) -> tuple[float, float, float, float]:
    if tile_id not in slot_map:
        return (0.9375, 0.9375, 1.0, 1.0)
    col, row = slot_map[tile_id]
    inv_w = 1.0 / ATLAS_W
    inv_h = 1.0 / ATLAS_H
    u0 = col * TILE_SIZE * inv_w + inv_w * 0.5
    v0 = row * TILE_SIZE * inv_h + inv_h * 0.5
    u1 = (col + 1) * TILE_SIZE * inv_w - inv_w * 0.5
    v1 = (row + 1) * TILE_SIZE * inv_h - inv_h * 0.5
    return (u0, v0, u1, v1)


def draw_textured_cube(
    ox: float, oy: float, oz: float,
    block_faces: list[int | tuple[int, int]],
    slot_map: dict[int | tuple[int, int], tuple[int, int]],
):
    glEnable(GL_TEXTURE_2D)
    glBegin(GL_QUADS)
    for i, face_name in enumerate(FACE_NAMES):
        tile_id = block_faces[i]
        u0, v0, u1, v1 = get_tile_uv(tile_id, slot_map)
        nx, ny, nz = FACE_NORMALS[face_name]
        glNormal3f(nx, ny, nz)
        verts = CUBE_FACES[face_name]
        uvs = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
        for v, uv in zip(verts, uvs):
            glTexCoord2f(*uv)
            glVertex3f(ox + v[0], oy + v[1], oz + v[2])
    glEnd()


def draw_wire_cube(ox: float, oy: float, oz: float, r: float = 1.0, g: float = 1.0, b: float = 1.0):
    glDisable(GL_TEXTURE_2D)
    glColor3f(r, g, b)
    glBegin(GL_LINES)
    edges = [
        (0, 0, 0, 1, 0, 0), (1, 0, 0, 1, 1, 0), (1, 1, 0, 0, 1, 0), (0, 1, 0, 0, 0, 0),
        (0, 0, 1, 1, 0, 1), (1, 0, 1, 1, 1, 1), (1, 1, 1, 0, 1, 1), (0, 1, 1, 0, 0, 1),
        (0, 0, 0, 0, 0, 1), (1, 0, 0, 1, 0, 1), (1, 1, 0, 1, 1, 1), (0, 1, 0, 0, 1, 1),
    ]
    for x1, y1, z1, x2, y2, z2 in edges:
        glVertex3f(ox + x1, oy + y1, oz + z1)
        glVertex3f(ox + x2, oy + y2, oz + z2)
    glEnd()
    glColor3f(1, 1, 1)


def draw_grid_lines(count: int):
    glDisable(GL_TEXTURE_2D)
    glColor4f(0.4, 0.4, 0.4, 0.5)
    glBegin(GL_LINES)
    for i in range(count + 1):
        glVertex3f(i, -0.001, 0)
        glVertex3f(i, -0.001, count)
        glVertex3f(0, -0.001, i)
        glVertex3f(count, -0.001, i)
    glEnd()
    glColor3f(1, 1, 1)


def draw_hud_text(text: str, x: int, y: int):
    glWindowPos2f(x, y)
    for ch in text:
        glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, ord(ch))


class AppState:
    def __init__(self):
        self.cam_yaw = 30.0
        self.cam_pitch = 25.0
        self.cam_dist = 5.0
        self.cam_target = np.array([0.5, 0.5, 0.5])
        self.mouse_x = 0
        self.mouse_y = 0
        self.mouse_down = False
        self.mouse_button = 0
        self.block_ids: list[str] = []
        self.block_idx = 0
        self.tiling_mode = False
        self.grid_size = 3
        self.block_faces: dict[int, BlockFaceInfo] = {}
        self.slot_map: dict[int, tuple[int, int]] = {}
        self.atlas_tex: int = 0
        self.win_w = 900
        self.win_h = 700
        self._cli_block_faces: list[int | tuple[int, int]] | None = None
        self._neighbor_faces: list[int | tuple[int, int]] | None = None


STATE = AppState()


def reshape(w: int, h: int):
    STATE.win_w = max(w, 1)
    STATE.win_h = max(h, 1)
    glViewport(0, 0, w, h)
    glMatrixMode(GL_PROJECTION)
    glLoadIdentity()
    gluPerspective(45.0, w / max(h, 1), 0.1, 100.0)
    glMatrixMode(GL_MODELVIEW)


def _update_camera():
    glLoadIdentity()
    yaw = math.radians(STATE.cam_yaw)
    pitch = math.radians(STATE.cam_pitch)
    d = STATE.cam_dist
    cx, cy, cz = STATE.cam_target
    ex = cx + d * math.cos(pitch) * math.sin(yaw)
    ey = cy + d * math.sin(pitch)
    ez = cz + d * math.cos(pitch) * math.cos(yaw)
    gluLookAt(ex, ey, ez, cx, cy, cz, 0, 1, 0)


def display():
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
    _update_camera()

    glEnable(GL_DEPTH_TEST)
    glEnable(GL_LIGHTING)
    glEnable(GL_LIGHT0)
    glEnable(GL_COLOR_MATERIAL)
    glEnable(GL_NORMALIZE)
    glShadeModel(GL_SMOOTH)

    glLightfv(GL_LIGHT0, GL_POSITION, [3.0, 5.0, 4.0, 0.0])
    glLightfv(GL_LIGHT0, GL_AMBIENT, [0.4, 0.4, 0.4, 1.0])
    glLightfv(GL_LIGHT0, GL_DIFFUSE, [0.8, 0.8, 0.8, 1.0])

    if not STATE.block_ids:
        glutSwapBuffers()
        return

    block_id = STATE.block_ids[STATE.block_idx]
    if STATE._cli_block_faces is not None:
        faces = STATE._cli_block_faces
    else:
        bf = STATE.block_faces.get(block_id)
        faces = bf.faces if bf else [block_id] * 6

    if STATE.tiling_mode:
        n = STATE.grid_size
        half = n / 2.0
        draw_grid_lines(n)
        for bx in range(n):
            for by in range(n):
                for bz in range(n):
                    ox = bx - half + 0.5
                    oy = by - half + 0.5
                    oz = bz - half + 0.5
                    draw_textured_cube(ox, oy, oz, faces, STATE.slot_map)
                    draw_wire_cube(ox, oy, oz, 0.5, 0.5, 0.5)
        if STATE._neighbor_faces is not None:
            neighbor_off = half + 1.0
            for bx in range(n):
                for by in range(n):
                    for bz in range(n):
                        ox = bx + neighbor_off + 0.5
                        oy = by - half + 0.5
                        oz = bz - half + 0.5
                        draw_textured_cube(ox, oy, oz, STATE._neighbor_faces, STATE.slot_map)
                        draw_wire_cube(ox, oy, oz, 0.4, 0.6, 0.8)
    else:
        draw_textured_cube(0, 0, 0, faces, STATE.slot_map)
        draw_wire_cube(0, 0, 0, 0.6, 0.6, 0.6)

    glDisable(GL_LIGHTING)
    glDisable(GL_DEPTH_TEST)
    glDisable(GL_TEXTURE_2D)

    mode_str = "TILING" if STATE.tiling_mode else "SINGLE"
    grid_str = f"  grid={STATE.grid_size}x{STATE.grid_size}x{STATE.grid_size}" if STATE.tiling_mode else ""
    neighbor_str = " + NEIGHBOR" if STATE._neighbor_faces is not None else ""
    info_lines = [
        f"Block {block_id}  |  Mode: {mode_str}{grid_str}{neighbor_str}",
        "[SPACE] toggle mode  [<->] switch block  [v/^] grid size  [r] reset  [q] quit",
    ]

    y = STATE.win_h - 20
    for line in info_lines:
        draw_hud_text(line, 10, y)
        y -= 16

    glutSwapBuffers()


def mouse(button, state, x, y):
    if button == GLUT_LEFT_BUTTON:
        STATE.mouse_down = (state == GLUT_DOWN)
        STATE.mouse_x = x
        STATE.mouse_y = y
    elif button == 3:
        STATE.cam_dist = max(1.0, STATE.cam_dist - 0.5)
        glutPostRedisplay()
    elif button == 4:
        STATE.cam_dist = min(30.0, STATE.cam_dist + 0.5)
        glutPostRedisplay()


def motion(x, y):
    if STATE.mouse_down:
        dx = x - STATE.mouse_x
        dy = y - STATE.mouse_y
        STATE.cam_yaw += dx * 0.5
        STATE.cam_pitch += dy * 0.5
        STATE.cam_pitch = max(-89.0, min(89.0, STATE.cam_pitch))
        STATE.mouse_x = x
        STATE.mouse_y = y
        glutPostRedisplay()


def keyboard(key, x, y):
    k = key.decode("utf-8", errors="replace") if isinstance(key, bytes) else key
    if k in ("q", "Q", "\x1b"):
        sys.exit(0)
    elif k == " ":
        STATE.tiling_mode = not STATE.tiling_mode
    elif k in ("r", "R"):
        STATE.cam_yaw = 30.0
        STATE.cam_pitch = 25.0
        STATE.cam_dist = 5.0
    glutPostRedisplay()


def special(key, x, y):
    if key == GLUT_KEY_RIGHT:
        STATE.block_idx = (STATE.block_idx + 1) % len(STATE.block_ids)
    elif key == GLUT_KEY_LEFT:
        STATE.block_idx = (STATE.block_idx - 1) % len(STATE.block_ids)
    elif key == GLUT_KEY_UP:
        STATE.grid_size = min(5, STATE.grid_size + 1)
    elif key == GLUT_KEY_DOWN:
        STATE.grid_size = max(1, STATE.grid_size - 1)
    glutPostRedisplay()


def main():
    parser = argparse.ArgumentParser(description="GTNH 3D Block Viewer")
    parser.add_argument(
        "--data-dir",
        default=str(Path(__file__).resolve().parent.parent / "data" / "textures"),
        help="Path to data/textures/ directory",
    )
    parser.add_argument("--block-id", type=int, help="Start with this block ID")
    parser.add_argument("--grid-size", type=int, default=3, help="Tiling grid size (1-5, default 3)")
    parser.add_argument("--tiling", action="store_true", help="Start in tiling mode")
    parser.add_argument("--zoom", type=float, default=5.0, help="Initial camera distance (default 5.0)")
    parser.add_argument(
        "--faces",
        help="Per-face tile IDs: PX,NX,PY,NY,PZ,NZ (e.g. --faces 0,0,2,1,3,3)",
    )
    parser.add_argument(
        "--face-rot",
        help="Per-face rotation 0-3: PX,NX,PY,NY,PZ,NZ (e.g. --face-rot 0,0,1,0,0,0)",
    )
    parser.add_argument("--transparent", action="store_true", help="Mark block as transparent")
    parser.add_argument(
        "--neighbor",
        help="Neighbor block face tiles for stitching check: PX,NX,PY,NY,PZ,NZ",
    )
    args = parser.parse_args()
    data_dir = Path(args.data_dir)

    if not data_dir.exists():
        print(f"Error: data directory not found: {data_dir}")
        sys.exit(1)

    cli_face_ids: list[int] | None = None
    cli_face_rots: list[int] | None = None
    if args.faces:
        parts = [p.strip() for p in args.faces.split(",")]
        if len(parts) != 6:
            print("Error: --faces needs exactly 6 comma-separated tile IDs")
            sys.exit(1)
        cli_face_ids = [int(p) for p in parts]
    if args.face_rot:
        parts = [p.strip() for p in args.face_rot.split(",")]
        if len(parts) != 6:
            print("Error: --face-rot needs exactly 6 comma-separated rotations (0-3)")
            sys.exit(1)
        cli_face_rots = [int(p) for p in parts]
        if any(r < 0 or r > 3 for r in cli_face_rots):
            print("Error: rotations must be 0-3")
            sys.exit(1)

    print("Loading textures.csv...")
    tiles = load_tiles(data_dir / "textures.csv")
    print(f"  {len(tiles)} tiles registered")

    print("Loading block_faces.csv...")
    block_faces = load_block_faces(data_dir / "block_faces.csv")
    print(f"  {len(block_faces)} block definitions")

    print("Building atlas...")
    atlas_img, slot_map, png_cache = build_atlas(data_dir, tiles)
    print(f"  Atlas: {atlas_img.size}, {len(slot_map)} tiles placed")

    cli_faces_key = "__cli_override__"
    if cli_face_ids is not None:
        face_list: list[int | tuple[int, int]] = []
        for i in range(6):
            tile_id = cli_face_ids[i]
            rot = cli_face_rots[i] if cli_face_rots else 0
            csv_tile = tiles.get(tile_id)
            csv_rot = csv_tile.rotate if csv_tile else 0
            if rot != csv_rot:
                key = add_rotated_tile(atlas_img, slot_map, png_cache, data_dir, tile_id, rot)
                face_list.append(key)
            else:
                face_list.append(tile_id)
        block_faces[9999] = BlockFaceInfo(9999, [0] * 6, args.transparent)
        STATE._cli_block_faces = face_list
        print(f"  CLI override: faces={[str(x) for x in face_list]}")

    if args.neighbor:
        parts = [p.strip() for p in args.neighbor.split(",")]
        if len(parts) != 6:
            print("Error: --neighbor needs exactly 6 comma-separated tile IDs")
            sys.exit(1)
        neighbor_ids = [int(p) for p in parts]
        neighbor_rots = cli_face_rots if cli_face_rots else [0] * 6
        neighbor_list: list[int | tuple[int, int]] = []
        for i in range(6):
            tile_id = neighbor_ids[i]
            rot = neighbor_rots[i]
            csv_tile = tiles.get(tile_id)
            csv_rot = csv_tile.rotate if csv_tile else 0
            if rot != csv_rot:
                key = add_rotated_tile(atlas_img, slot_map, png_cache, data_dir, tile_id, rot)
                neighbor_list.append(key)
            else:
                neighbor_list.append(tile_id)
        STATE._neighbor_faces = neighbor_list
        print(f"  Neighbor: faces={[str(x) for x in neighbor_list]}")

    atlas_debug_path = data_dir / "_atlas_preview.png"
    atlas_img.save(atlas_debug_path)
    print(f"  Saved preview: {atlas_debug_path}")

    glutInit(sys.argv)
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGBA | GLUT_DEPTH)
    glutInitWindowSize(STATE.win_w, STATE.win_h)
    glutInitWindowPosition(100, 100)
    glutCreateWindow(b"GTNH Block Viewer")

    STATE.atlas_tex = upload_texture(atlas_img)
    STATE.block_faces = block_faces
    STATE.slot_map = slot_map
    STATE.block_ids = sorted(block_faces.keys())
    if not STATE.block_ids:
        STATE.block_ids = sorted(tiles.keys())

    if cli_face_ids is not None:
        STATE.block_ids = [9999]
        STATE.block_idx = 0
    elif args.block_id is not None:
        if args.block_id in STATE.block_ids:
            STATE.block_idx = STATE.block_ids.index(args.block_id)
        else:
            print(f"Warning: block_id {args.block_id} not in block_faces, using first available")

    STATE.grid_size = max(1, min(5, args.grid_size))
    STATE.tiling_mode = args.tiling or STATE._neighbor_faces is not None
    STATE.cam_dist = max(1.0, args.zoom)

    glClearColor(0.18, 0.20, 0.24, 1.0)
    glEnable(GL_BLEND)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)

    glutDisplayFunc(display)
    glutReshapeFunc(reshape)
    glutMouseFunc(mouse)
    glutMotionFunc(motion)
    glutKeyboardFunc(keyboard)
    glutSpecialFunc(special)

    print()
    print("Controls:")
    print("  Mouse drag       Orbit camera")
    print("  Scroll           Zoom")
    print("  SPACE            Toggle single / tiling mode")
    print("  LEFT / RIGHT     Switch block ID")
    print("  UP / DOWN        Adjust grid size (1-5)")
    print("  r                Reset camera")
    print("  ESC / q          Quit")
    print()

    glutMainLoop()


if __name__ == "__main__":
    main()
