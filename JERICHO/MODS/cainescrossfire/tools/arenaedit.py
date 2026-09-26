#!/usr/bin/env python3
"""arenaedit.py - the Caine's Crossfire ARENA editor, top-down.

Edits the `.cca` arena files (see ../ARENAS.md) - the region (the barrier) and
the spawn points (position + heading) - in a top-down view. It reads and writes
exactly the format the game reads, so it and the in-game editor (arenas/arena.c)
are two views of one file: what one saves, the other loads.

    python arenaedit.py arenas/*.cca                 # open the editor
    python arenaedit.py chicago.cca --map city.png \
        --map-world -450000 -580000 450000 580000    # with a background image
    python arenaedit.py chicago.cca --obj CITY.obj --cells 448 576  # obj background
    python arenaedit.py chicago.cca --render out.png # headless snapshot
    python arenaedit.py chicago.cca --check          # validate, print, exit

The view is in WORLD units (the units the .cca stores and the game uses), so no
calibration is needed to place things. A background is optional and purely
decorative. Give it as an IMAGE stretched over a world rectangle (--map
--map-world), or as a level .obj from DriverLevelTool (--obj), whose own
bounding box is stretched onto --obj-world (or the centered cell grid from
--cells). The .obj is in the tool's model units, not world units, so its
mapping is approximate unless you give the exact rectangle; the game's own
overmap (DriverLevelTool -overmap) is the alternative when its map tiles are
present. Without a background you get a coordinate grid.

Mouse (editor):
  left-click         select a spawn (or the nearest to the click)
  left-drag          move the selected spawn
  right-click        add a spawn at the click (or delete, over one)
  drag a region knob set a region corner
  wheel              zoom about the cursor
  middle-drag        pan
Keys:
  n                  new arena (from the current one)
  d                  delete the selected spawn
  r                  cycle region: none -> rect -> none
  s / Ctrl-S         save the current file
  Tab / Shift-Tab    next / previous file
  q / Esc            quit

Spawn 0 is the PLAYER; the rest are opponents in order. A heading is PSX angle
units (0..4095); the arrow shows which way the car points.
"""

import argparse
import glob
import os
import sys

SPAWN_MAX = 16
HEADING_MAX = 4096
CITIES = {0: "CHICAGO", 1: "HAVANA", 2: "VEGAS", 3: "RIO"}
CITY_INDEX = {v: k for k, v in CITIES.items()}
CITY_NAMES = {v: k for k, v in CITY_INDEX.items()}	# index -> NAME


def _default_arena_dir():
    """The mod's own arena folder (this script lives in <mod>/tools/)."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "arenas"))


# ---------------------------------------------------------------------------
# the data model + the file format (mirrors arenas/arenafile.c)
# ---------------------------------------------------------------------------
class Arena:
    def __init__(self, internal="", display="", city=0, mp_level=1, mp_arena=0,
                 region=None, spawns=None, pickups=None):
        self.internal = internal
        self.display = display
        self.city = city
        self.mp_level = mp_level
        self.mp_arena = mp_arena
        self.region = region            # None, or (x0, z0, x1, z1)
        self.spawns = list(spawns or [])  # list of (x, z, heading)
        # pickups: list of {"type": "weapon"|"health", "weapon": name|None,
        #                   "amount": int, "x": int, "z": int}
        self.pickups = list(pickups or [])
        self.path = None
        self.mtime = None       # last seen on-disk mtime (for the live reload)
        self.dirty = False      # has unsaved top-down edits

    def clone(self, internal):
        return Arena(internal, internal, self.city, self.mp_level, self.mp_arena,
                     self.region, self.spawns, self.pickups)


def _city_from_token(tok):
    tok = tok.strip()
    if tok.upper() in CITY_INDEX:
        return CITY_INDEX[tok.upper()]
    try:
        return int(tok)
    except ValueError:
        return None


def load_arena(path):
    a = Arena()
    a.path = path
    saw_name = False
    with open(path, "r", errors="ignore") as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line or ":" not in line:
                continue
            key, val = line.split(":", 1)
            key = key.strip().lower()
            val = val.strip()
            if key == "arena":
                a.internal = val
                saw_name = True
            elif key == "name":
                a.display = val
            elif key == "city":
                c = _city_from_token(val)
                if c is not None:
                    a.city = c
            elif key == "mp":
                parts = val.split()
                if parts:
                    a.mp_level = 1 if int(parts[0]) else 0
                if len(parts) > 1:
                    a.mp_arena = 1 if int(parts[1]) else 0
                elif val.lower() in ("full", "city"):
                    a.mp_level = 0
            elif key == "region":
                if val.lower() in ("none", "whole", ""):
                    a.region = None
                else:
                    nums = [int(x) for x in val.split()]
                    if len(nums) == 4:
                        x0, z0, x1, z1 = nums
                        a.region = (min(x0, x1), min(z0, z1), max(x0, x1), max(z0, z1))
            elif key == "spawn":
                nums = [int(x) for x in val.split()]
                if len(nums) >= 2 and len(a.spawns) < SPAWN_MAX:
                    x, z = nums[0], nums[1]
                    h = nums[2] if len(nums) > 2 else 0
                    y = nums[3] if len(nums) > 3 else None   # height, optional
                    a.spawns.append((x, z, h & (HEADING_MAX - 1), y))
            elif key == "pickup":
                parts = val.split()
                if parts and parts[0].lower() == "weapon" and len(parts) >= 4:
                    a.pickups.append({"type": "weapon", "weapon": parts[1],
                                      "amount": int(parts[4]) if len(parts) > 4 else 0,
                                      "x": int(parts[2]), "z": int(parts[3])})
                elif parts and parts[0].lower() == "health" and len(parts) >= 3:
                    a.pickups.append({"type": "health", "weapon": None,
                                      "amount": int(parts[3]) if len(parts) > 3 else 0,
                                      "x": int(parts[1]), "z": int(parts[2])})
    if not a.display:
        a.display = a.internal
    try:
        a.mtime = os.path.getmtime(path)
    except OSError:
        a.mtime = None
    return a, saw_name


def save_arena(a):
    lines = [
        "# cainescrossfire arena -- see ARENAS.md",
        "arena: %s" % a.internal,
        "name: %s" % a.display,
        "city: %d" % a.city,
        "mp: %d %d" % (1 if a.mp_level else 0, 1 if a.mp_arena else 0),
    ]
    if a.region:
        lines.append("region: %d %d %d %d" % a.region)
    else:
        lines.append("region: none")
    lines.append("# spawn: x z heading [y]  (first = player, rest = opponents; y = height)")
    for (x, z, h, y) in a.spawns:
        if y is None:
            lines.append("spawn: %d %d %d" % (x, z, h))
        else:
            lines.append("spawn: %d %d %d %d" % (x, z, h, y))
    lines.append("# pickup: weapon <name> x z [ammo]   |   pickup: health x z [amount]")
    for p in a.pickups:
        if p["type"] == "weapon":
            lines.append("pickup: weapon %s %d %d %d" % (p["weapon"], p["x"], p["z"], p["amount"]))
        else:
            lines.append("pickup: health %d %d %d" % (p["x"], p["z"], p["amount"]))
    with open(a.path, "w") as f:
        f.write("\n".join(lines) + "\n")
    try:
        a.mtime = os.path.getmtime(a.path)
    except OSError:
        a.mtime = None
    a.dirty = False


def check_arena(a):
    """Return a list of warning strings (empty = clean)."""
    w = []
    if not a.internal:
        w.append("no arena: name")
    if a.city < 0 or a.city > 3:
        w.append("city %d out of range 0..3" % a.city)
    if a.region:
        x0, z0, x1, z1 = a.region
        if x0 >= x1 or z0 >= z1:
            w.append("region is empty/backwards: %r" % (a.region,))
    for i, (x, z, h, y) in enumerate(a.spawns):
        if a.region:
            x0, z0, x1, z1 = a.region
            if not (x0 <= x <= x1 and z0 <= z <= z1):
                w.append("spawn %d (%d,%d) is OUTSIDE the region" % (i, x, z))
        if not (0 <= h < HEADING_MAX):
            w.append("spawn %d heading %d out of range" % (i, h))
    if len(a.spawns) == 0:
        w.append("no spawns (the game falls back to its own placement)")
    for i, p in enumerate(a.pickups):
        if a.region:
            x0, z0, x1, z1 = a.region
            if not (x0 <= p["x"] <= x1 and z0 <= p["z"] <= z1):
                w.append("pickup %d (%d,%d) is OUTSIDE the region" % (i, p["x"], p["z"]))
    return w


def describe(a):
    who = "player" if a.spawns else "-"
    opp = max(0, len(a.spawns) - 1)
    reg = "none" if not a.region else "%d,%d..%d,%d" % a.region
    return ("%s (%s) city=%s mp=%d/%d region=%s spawns=%d (%s + %d opp) pickups=%d"
            % (a.internal, a.display, CITIES.get(a.city, a.city), a.mp_level,
               a.mp_arena, reg, len(a.spawns), who, opp, len(a.pickups)))


# ---------------------------------------------------------------------------
# view transform: world (x right, z down) <-> screen pixels
# ---------------------------------------------------------------------------
class View:
    def __init__(self, world_rect):
        self.wx0, self.wz0, self.wx1, self.wz1 = world_rect
        self.cx = (self.wx0 + self.wx1) / 2.0
        self.cz = (self.wz0 + self.wz1) / 2.0
        self.scale = 1.0
        self.ox = 0.0
        self.oy = 0.0

    def world_to_screen(self, x, z):
        return (self.ox + (x - self.cx) * self.scale,
                self.oy + (z - self.cz) * self.scale)

    def screen_to_world(self, sx, sy):
        return (self.cx + (sx - self.ox) / self.scale,
                self.cz + (sy - self.oy) / self.scale)


def _bounds(arenas, pad=0.15):
    xs0 = zs0 = 1e18
    xs1 = zs1 = -1e18
    for a in arenas:
        if a.region:
            xs0 = min(xs0, a.region[0]); zs0 = min(zs0, a.region[1])
            xs1 = max(xs1, a.region[2]); zs1 = max(zs1, a.region[3])
        for (x, z, _h, _y) in a.spawns:
            xs0 = min(xs0, x); zs0 = min(zs0, z)
            xs1 = max(xs1, x); zs1 = max(zs1, z)
    if xs0 > xs1 or zs0 > zs1:
        return (-5000, -5000, 5000, 5000)
    w = max(1.0, xs1 - xs0)
    h = max(1.0, zs1 - zs0)
    return (xs0 - w * pad, zs0 - h * pad, xs1 + w * pad, zs1 + h * pad)


def _heading_vec(h):
    """Screen direction for a PSX heading (0..4095). Best-effort orientation."""
    import math
    ang = h / float(HEADING_MAX) * 2.0 * math.pi
    return (math.sin(ang), -math.cos(ang))


# ---------------------------------------------------------------------------
# background sources: a prepared image, or a level .obj point cloud
# ---------------------------------------------------------------------------
def load_obj_points(path, world_rect, size=(1400, 1000), sample=2):
    """Stream a level .obj (DriverLevelTool -world/-models) and rasterise its XZ
    vertex cloud into a background image, mapped onto `world_rect`.

    The .obj is in the level tool's model units, not the game's world units, so
    the correspondence is given by `world_rect` (the world rectangle the obj's
    own XZ bounding box is stretched onto) - see --obj-world / --cells. Without a
    correct rectangle the picture is a shape reference, not an aligned map.
    """
    from PIL import Image

    W, H = size
    img = Image.new("L", size, 0)
    px = img.load()
    x0, z0, x1, z1 = world_rect
    sx = W / float(x1 - x0)
    sz = H / float(z1 - z0)
    n = 0
    kept = 0
    with open(path, "r", errors="ignore") as f:
        for line in f:
            if line[:2] != "v ":
                continue
            n += 1
            if sample > 1 and (n % sample):
                continue
            parts = line.split()
            try:
                x = float(parts[1]); z = float(parts[3])
            except (IndexError, ValueError):
                continue
            ix = int((x - x0) * sx)
            iz = int((z - z0) * sz)
            if 0 <= ix < W and 0 <= iz < H:
                v = px[ix, iz]
                if v < 255:
                    px[ix, iz] = min(255, v + 60)
                kept += 1
    img = img.convert("RGB")
    print("obj %s: %d verts, %d plotted" % (path, n, kept))
    return img


# ---------------------------------------------------------------------------
# level rips: a city's DriverLevelTool model, drawn top-down and CACHED
# ---------------------------------------------------------------------------
# DriverLevelTool writes the level model at 1/4096 (the engine's ONE) with X
# mirrored, so: world_x = -4096 * obj_x and world_z = +4096 * obj_z. Using that
# instead of stretching the obj's bounding box is what makes the picture land on
# the arena's own world coordinates.
LEVEL_SCALE = 4096


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(here))))


def level_rip_paths(city):
    """(obj, png, json) for a city's rip in DriverLevelTool/ - any may be missing."""
    base = os.path.join(repo_root(), "DriverLevelTool", "%s_LEVELMODEL" % city.upper())
    return base + ".obj", base + ".topdown.png", base + ".topdown.json"


def build_level_map(city, size=(2000, 2000), sample=4, rebuild=False,
                    allow_build=True, verbose=True):
    """The top-down background for a city, from its DriverLevelTool rip.

    Returns (PIL image, world_rect), or (None, None) when there is no rip (or no
    cache and allow_build is off). The picture is CACHED as
    `<CITY>_LEVELMODEL.topdown.png` plus a `.json` sidecar with the world rect, so
    the slow pass over a ~185 MB .obj happens once and later opens are instant -
    and the PNG alone is shareable, since the sidecar carries the alignment.

    `sample` keeps every Nth vertex (the cloud is ~10M points; every 4th is
    plenty at this resolution).
    """
    import json
    from array import array
    from PIL import Image

    obj, png, side = level_rip_paths(city)

    def _cached():
        if not (os.path.exists(png) and os.path.exists(side)):
            return None

        try:
            meta = json.load(open(side))
            rect = tuple(meta["rect"])
        except Exception:
            return None

        # invalidate only when the .obj is present AND has changed; a PNG on its
        # own (the obj deleted, or a shared cache) is still good
        if os.path.exists(obj) and not rebuild:
            st = os.stat(obj)

            if meta.get("obj_size") != st.st_size or meta.get("obj_mtime") != int(st.st_mtime):
                if verbose:
                    print("level map cache for %s is stale - rebuilding" % city.upper())
                return None

        return Image.open(png).convert("RGB"), rect

    if not rebuild:
        got = _cached()

        if got is not None:
            return got

    if not os.path.exists(obj):
        if verbose:
            print("no level rip for %s (looked for %s)" % (city.upper(), obj))
        return None, None

    if not allow_build:
        if verbose:
            print("no cached level map for %s yet - run with --level %s once to build it"
                  % (city.upper(), city.upper()))
        return None, None

    if verbose:
        print("building the %s level map (one pass over %s, then cached)"
              % (city.upper(), os.path.basename(obj)))

    xs = array("f")
    zs = array("f")
    n = 0

    with open(obj, "r", errors="ignore") as f:
        for line in f:
            if line[:2] != "v ":
                continue

            n += 1

            if sample > 1 and (n % sample):
                continue

            parts = line.split()

            try:
                xs.append(float(parts[1]))
                zs.append(float(parts[3]))
            except (IndexError, ValueError):
                pass

    if not len(xs):
        return None, None

    # the model -> world frame (X mirrored): this is the true extent
    rect = (-LEVEL_SCALE * max(xs), LEVEL_SCALE * min(zs),
            -LEVEL_SCALE * min(xs), LEVEL_SCALE * max(zs))

    W, H = size
    img = Image.new("L", size, 0)
    px = img.load()
    x0, z0, x1, z1 = rect
    sx = W / float(x1 - x0)
    sz = H / float(z1 - z0)
    kept = 0

    for i in range(len(xs)):
        ix = int((-LEVEL_SCALE * xs[i] - x0) * sx)
        iz = int((LEVEL_SCALE * zs[i] - z0) * sz)

        if 0 <= ix < W and 0 <= iz < H:
            v = px[ix, iz]

            if v < 255:
                px[ix, iz] = min(255, v + 50)

            kept += 1

    img = img.convert("RGB")

    try:
        img.save(png)
        json.dump({"city": city.upper(), "rect": list(rect), "verts": n,
                   "sampled": len(xs), "plotted": kept,
                   "obj_size": os.stat(obj).st_size,
                   "obj_mtime": int(os.stat(obj).st_mtime)},
                  open(side, "w"), indent=1)

        if verbose:
            print("cached %s (%d of %d verts plotted; world rect %s)"
                  % (os.path.basename(png), kept, n, rect))
    except Exception as e:
        if verbose:
            print("(could not write the cache: %s - using it for this session only)" % e)

    return img, rect


def cells_world_rect(cw, ch, cell=2048):
    """The world rectangle a WxH cell grid covers, centred on the origin
    (DriverLevelTool prints 'Level dimensions [W H], cell size: 2048')."""
    hw = cw * cell // 2
    hh = ch * cell // 2
    return (-hw, -hh, hw, hh)


# ---------------------------------------------------------------------------
# headless render
# ---------------------------------------------------------------------------
def render_png(arenas, path, bg=None, bg_rect=None, size=(1100, 800)):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", size, (18, 18, 22))
    dr = ImageDraw.Draw(img)

    # the view frames the arena; the background is placed under it, so a zoomed
    # view still lines the picture up with the world coordinates
    wr = _bounds(arenas)
    view = View(wr)
    view.scale = min(size[0] / (wr[2] - wr[0]), size[1] / (wr[3] - wr[1])) * 0.92
    view.ox, view.oy = size[0] / 2.0, size[1] / 2.0

    if bg is not None and bg_rect is not None:
        p0 = view.world_to_screen(bg_rect[0], bg_rect[1])
        p1 = view.world_to_screen(bg_rect[2], bg_rect[3])
        w = max(1, int(abs(p1[0] - p0[0])))
        h = max(1, int(abs(p1[1] - p0[1])))
        img.paste(bg.resize((w, h)), (int(min(p0[0], p1[0])), int(min(p0[1], p1[1]))))
        dr = ImageDraw.Draw(img)

    def S(x, z):
        return view.world_to_screen(x, z)

    for a in arenas:
        color = (90, 160, 255)
        if a.region:
            x0, z0, x1, z1 = a.region
            p0 = S(x0, z0); p1 = S(x1, z1)
            dr.rectangle([p0, p1], outline=(255, 200, 60), width=2)
        for i, (x, z, h, y) in enumerate(a.spawns):
            sx, sy = S(x, z)
            c = (120, 255, 120) if i == 0 else (255, 120, 120)
            r = 6
            dr.ellipse([sx - r, sy - r, sx + r, sy + r], fill=c, outline=(0, 0, 0))
            dx, dy = _heading_vec(h)
            dr.line([sx, sy, sx + dx * 26, sy + dy * 26], fill=c, width=2)
            dr.text((sx + 8, sy + 8), "%d" % i, fill=(255, 255, 255))
        for p in a.pickups:
            sx, sy = S(p["x"], p["z"])
            c = (120, 255, 120) if p["type"] == "health" else (255, 210, 120)
            dr.rectangle([sx - 6, sy - 6, sx + 6, sy + 6], outline=c, width=2)
            dr.text((sx + 8, sy - 16), "H" if p["type"] == "health" else "W",
                    fill=c)
        # a label in the corner
        dr.text((8, 8 + arenas.index(a) * 16),
                "%s  [%s]" % (describe(a), a.path or "<new>"), fill=(230, 230, 230))

    img.save(path)
    return path


# ---------------------------------------------------------------------------
# interactive editor (tkinter)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# the interactive editor: menu bar + toolbar + canvas + inspector + status bar
# ---------------------------------------------------------------------------
# Each tool is (id, toolbar label, status-bar hint).
TOOLS = (
    ("select", "Select", "Select and drag spawns or region corners. Middle-drag pans, the wheel zooms."),
    ("spawn", "Add spawn", "Click the map to add a spawn there. Spawn 1 is the player, the rest opponents."),
    ("delete", "Delete", "Click a spawn or a pickup to delete it."),
    ("region", "Region", "Click one corner, then the opposite corner, to set the play boundary."),
)

TOOL_KEYS = {"1": "select", "2": "spawn", "3": "delete", "4": "region"}

# grid steps to choose from, in world units (a cell is 2048)
GRID_STEPS = (512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 4194304)

SPAWN_COL = "#7dff7d"      # spawn 1 = the player
OPP_COL = "#ff7d7d"        # the opponents
SEL_COL = "#ffffff"
REGION_COL = "#ffc83c"
PICK_HEALTH = "#66e0a0"
PICK_WEAPON = "#ffb347"
GRID_COL = "#23232e"
GRID_TEXT = "#4a4a5c"
BG_TINT = "#9a9a9a"


class EditorApp:
    """The arena editor window.

    The whole UI lives here, so it can be built, driven and checked without
    entering mainloop - see --uitest, which pumps the window and exercises the
    commands.
    """

    def __init__(self, root, arenas, bg=None, bg_rect=None, hint=""):
        import tkinter as tk
        from tkinter import ttk

        self.tk = tk
        self.ttk = ttk
        self.root = root
        self.arenas = list(arenas) if arenas else [Arena("new_arena", "New Arena")]
        self.idx = 0
        self.sel = -1
        self.sel_pick = -1
        self.mode = "select"
        self.undo_stack = []
        self.show_grid = True
        self.show_labels = True
        self.bg_caller = bg              # what the caller supplied (--map / --obj)
        self.bg_caller_rect = bg_rect
        self.bg = bg                     # the picture actually drawn
        self.bg_rect = bg_rect
        self.bg_photo = None
        self.pending_corner = None
        self.pan_from = None
        self.drag = None
        self.exit_code = 0
        self.view = View(_bounds(self.arenas))

        root.title("Caine's Crossfire - arena editor")

        self.var_mode = tk.StringVar(value=self.mode)
        self.var_grid = tk.BooleanVar(value=True)
        self.var_labels = tk.BooleanVar(value=True)
        self.var_city = tk.StringVar(value="CHICAGO")
        self.var_layout = tk.StringVar(value="0")
        self.var_mp = tk.BooleanVar(value=True)
        self.var_pick_kind = tk.StringVar(value="health")

        self._build_menu()
        self._build_toolbar()
        self._build_body()
        self._build_status()
        self._bind()

        self.fit()
        self.refresh()
        self.redraw()
        self._say(hint or "ready")
        self.root.after(900, self._poll)

    # -- helpers ------------------------------------------------------------
    def cur(self):
        return self.arenas[self.idx]

    def _say(self, msg):
        try:
            self.lbl_msg.configure(text=msg)
        except Exception:
            pass

    def _mode_changed(self):
        self.mode = self.var_mode.get()
        for tid, _l, hint in TOOLS:
            if tid == self.mode:
                self._say(hint)
        self.redraw()

    # -- window chrome ------------------------------------------------------
    def _build_menu(self):
        tk = self.tk
        m = tk.Menu(self.root)

        f = tk.Menu(m, tearoff=0)
        f.add_command(label="New arena", accelerator="Ctrl+N", command=self.new_arena)
        f.add_command(label="Open\u2026", accelerator="Ctrl+O", command=self.open_file)
        f.add_command(label="Save", accelerator="Ctrl+S", command=self.save)
        f.add_command(label="Save as\u2026", command=self.save_as)
        f.add_separator()
        f.add_command(label="Reload from disk", accelerator="Ctrl+R", command=self.reload)
        f.add_separator()
        f.add_command(label="Quit", accelerator="Ctrl+Q", command=self.root.destroy)
        m.add_cascade(label="File", menu=f)

        e = tk.Menu(m, tearoff=0)
        e.add_command(label="Undo", accelerator="Ctrl+Z", command=self.undo)
        e.add_command(label="Delete selected", accelerator="Del", command=self.delete_selected)
        e.add_separator()
        e.add_command(label="Deselect", command=self.deselect)
        m.add_cascade(label="Edit", menu=e)

        v = tk.Menu(m, tearoff=0)
        v.add_checkbutton(label="Grid", variable=self.var_grid, command=self.redraw)
        v.add_checkbutton(label="Spawn labels", variable=self.var_labels, command=self.redraw)
        v.add_separator()
        v.add_command(label="Fit to arena", accelerator="F", command=self.fit_view)
        v.add_command(label="Zoom in", command=lambda: self.zoom(1.25))
        v.add_command(label="Zoom out", command=lambda: self.zoom(1 / 1.25))
        v.add_separator()
        v.add_command(label="Background: cached level map", command=self.use_cached_map)
        v.add_command(label="Background: build level map\u2026", command=self.build_map)
        v.add_command(label="Background: none", command=self.clear_bg)
        m.add_cascade(label="View", menu=v)

        h = tk.Menu(m, tearoff=0)
        h.add_command(label="Shortcuts", accelerator="F1", command=self.show_help)
        m.add_cascade(label="Help", menu=h)

        self.root.config(menu=m)

    def _build_toolbar(self):
        ttk = self.ttk
        bar = ttk.Frame(self.root, padding=(6, 4))
        bar.pack(side="top", fill="x")

        def sep():
            ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=6)

        ttk.Button(bar, text="New", command=self.new_arena).pack(side="left", padx=1)
        ttk.Button(bar, text="Open", command=self.open_file).pack(side="left", padx=1)
        ttk.Button(bar, text="Save", command=self.save).pack(side="left", padx=1)
        sep()

        for tid, label, _h in TOOLS:
            ttk.Radiobutton(bar, text=label, value=tid, variable=self.var_mode,
                            command=self._mode_changed).pack(side="left", padx=1)
        sep()

        ttk.Button(bar, text="Fit", command=self.fit_view).pack(side="left", padx=1)
        ttk.Checkbutton(bar, text="Grid", variable=self.var_grid,
                        command=self.redraw).pack(side="left", padx=2)
        ttk.Checkbutton(bar, text="Labels", variable=self.var_labels,
                        command=self.redraw).pack(side="left", padx=2)
        sep()

        ttk.Label(bar, text="arena:").pack(side="left")
        self.cb_arena = ttk.Combobox(bar, width=22, state="readonly")
        self.cb_arena.pack(side="left", padx=2)
        self.cb_arena.bind("<<ComboboxSelected>>", self.on_arena_pick)

    def _build_body(self):
        tk, ttk = self.tk, self.ttk
        body = ttk.Frame(self.root)
        body.pack(side="top", fill="both", expand=True)

        self.canvas = tk.Canvas(body, background="#0f0f14", highlightthickness=0)
        self.canvas.pack(side="left", fill="both", expand=True)

        side = ttk.Frame(body, padding=(5, 4))
        side.pack(side="right", fill="y")

        self._box_arena(side)
        self._box_region(side)
        self._box_spawns(side)
        self._box_pickups(side)
        self._box_bg(side)

    def _box_arena(self, side):
        ttk = self.ttk
        b = ttk.LabelFrame(side, text="Arena", padding=6)
        b.pack(fill="x", pady=2)
        ttk.Label(b, text="arena:").grid(row=0, column=0, sticky="w")
        self.e_internal = ttk.Entry(b, width=18)
        self.e_internal.grid(row=0, column=1, sticky="we")
        ttk.Label(b, text="name:").grid(row=1, column=0, sticky="w")
        self.e_display = ttk.Entry(b, width=18)
        self.e_display.grid(row=1, column=1, sticky="we")
        ttk.Label(b, text="city:").grid(row=2, column=0, sticky="w")
        self.cb_city = ttk.Combobox(b, textvariable=self.var_city, width=16, state="readonly",
                                    values=[CITIES[i] for i in sorted(CITIES)])
        self.cb_city.grid(row=2, column=1, sticky="we")
        ttk.Checkbutton(b, text="mp map (the small layout)", variable=self.var_mp).grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(2, 0))
        ttk.Label(b, text="layout:").grid(row=4, column=0, sticky="w")
        ttk.Combobox(b, textvariable=self.var_layout, width=4, state="readonly",
                     values=["0", "1"]).grid(row=4, column=1, sticky="w")
        ttk.Button(b, text="Apply", command=self.apply_arena).grid(
            row=5, column=0, columnspan=2, sticky="we", pady=(5, 0))

    def _box_region(self, side):
        ttk = self.ttk
        b = ttk.LabelFrame(side, text="Region (keep spawns inside it)", padding=6)
        b.pack(fill="x", pady=2)
        self.region_entries = {}
        for i, k in enumerate(("x0", "z0", "x1", "z1")):
            ttk.Label(b, text=k + ":").grid(row=i // 2, column=(i % 2) * 2, sticky="w")
            e = ttk.Entry(b, width=10)
            e.grid(row=i // 2, column=(i % 2) * 2 + 1, sticky="we", padx=(0, 6))
            self.region_entries[k] = e
        ttk.Button(b, text="Apply", command=self.apply_region).grid(
            row=2, column=0, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Clear", command=self.clear_region).grid(
            row=2, column=1, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Fit to it", command=self.fit_region).grid(
            row=2, column=2, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Use the view's edges", command=self.region_from_view).grid(
            row=3, column=0, columnspan=3, sticky="we", pady=(3, 0))

    def _box_spawns(self, side):
        ttk = self.ttk
        b = ttk.LabelFrame(side, text="Spawns (1 = player)", padding=6)
        b.pack(fill="both", expand=True, pady=2)
        cols = ("no", "x", "y", "z", "head")
        self.tv_spawns = ttk.Treeview(b, columns=cols, show="headings", height=6,
                                      selectmode="browse")
        for c, w in zip(cols, (30, 64, 52, 64, 46)):
            self.tv_spawns.heading(c, text=c.upper())
            self.tv_spawns.column(c, width=w, anchor="e" if c != "no" else "center")
        self.tv_spawns.grid(row=0, column=0, columnspan=4, sticky="nsew")
        ttk.Button(b, text="Add", command=self.add_spawn).grid(
            row=1, column=0, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Del", command=self.delete_selected).grid(
            row=1, column=1, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Dup", command=self.dup_spawn).grid(
            row=1, column=2, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Apply", command=self.apply_spawn).grid(
            row=1, column=3, sticky="we", pady=(5, 0))
        self.sp_entries = {}
        for i, k in enumerate(("x", "y", "z", "head")):
            ttk.Label(b, text=k).grid(row=2, column=i, sticky="e")
            e = ttk.Entry(b, width=7)
            e.grid(row=3, column=i, sticky="we")
            self.sp_entries[k] = e

    def _box_pickups(self, side):
        ttk = self.ttk
        b = ttk.LabelFrame(side, text="Pickups (drive-over)", padding=6)
        b.pack(fill="x", pady=2)
        cols = ("kind", "detail", "x", "z")
        self.tv_pickups = ttk.Treeview(b, columns=cols, show="headings", height=4,
                                       selectmode="browse")
        for c, w in zip(cols, (56, 74, 58, 58)):
            self.tv_pickups.heading(c, text=c.upper())
            self.tv_pickups.column(c, width=w, anchor="w" if c in ("kind", "detail") else "e")
        self.tv_pickups.grid(row=0, column=0, columnspan=5, sticky="nsew")
        ttk.Combobox(b, textvariable=self.var_pick_kind, width=7, state="readonly",
                     values=["health", "weapon"]).grid(row=1, column=0, sticky="we", pady=(5, 0))
        self.e_pick_what = ttk.Entry(b, width=9)
        self.e_pick_what.grid(row=1, column=1, sticky="we", pady=(5, 0))
        self.e_pick_amt = ttk.Entry(b, width=6)
        self.e_pick_amt.grid(row=1, column=2, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Add at view centre", command=self.add_pickup).grid(
            row=1, column=3, sticky="we", pady=(5, 0))
        ttk.Button(b, text="Del", command=self.del_pickup).grid(
            row=1, column=4, sticky="we", pady=(5, 0))
        ttk.Label(b, text="(weapon name / health amount)", foreground="#888").grid(
            row=2, column=0, columnspan=5, sticky="w")

    def _box_bg(self, side):
        ttk = self.ttk
        b = ttk.LabelFrame(side, text="Background", padding=6)
        b.pack(fill="x", pady=2)
        self.lbl_bg = ttk.Label(b, text="none", wraplength=230, justify="left")
        self.lbl_bg.pack(anchor="w")
        ttk.Button(b, text="Use the cached level map", command=self.use_cached_map).pack(
            fill="x", pady=(4, 0))
        ttk.Button(b, text="Build level map for this city", command=self.build_map).pack(
            fill="x", pady=(2, 0))
        ttk.Button(b, text="Remove background", command=self.clear_bg).pack(
            fill="x", pady=(2, 0))

    def _build_status(self):
        ttk = self.ttk
        s = ttk.Frame(self.root, padding=(6, 3))
        s.pack(side="bottom", fill="x")
        self.lbl_pos = ttk.Label(s, text="", width=30, anchor="e")
        self.lbl_pos.pack(side="right")
        self.lbl_file = ttk.Label(s, text="", anchor="w")
        self.lbl_file.pack(side="left")
        self.lbl_msg = ttk.Label(s, text="", anchor="w")
        self.lbl_msg.pack(side="left", padx=14)

    def _bind(self):
        c = self.canvas
        c.bind("<ButtonPress-1>", self.on_press)
        c.bind("<B1-Motion>", self.on_drag)
        c.bind("<ButtonRelease-1>", self.on_release)
        c.bind("<ButtonPress-2>", self.on_pan_start)
        c.bind("<B2-Motion>", self.on_pan)
        c.bind("<ButtonRelease-2>", self.on_release)
        c.bind("<ButtonPress-3>", self.on_right)
        c.bind("<Motion>", self.on_motion)
        c.bind("<MouseWheel>", self.on_wheel)
        c.bind("<Button-4>", lambda e: self.zoom(1.15, e.x, e.y))
        c.bind("<Button-5>", lambda e: self.zoom(1 / 1.15, e.x, e.y))
        c.bind("<Configure>", lambda e: self.redraw())
        c.bind("<Key>", self.on_key)
        c.configure(takefocus=1)
        self.tv_spawns.bind("<<TreeviewSelect>>", self.on_spawn_select)
        self.tv_pickups.bind("<<TreeviewSelect>>", self.on_pickup_select)

        r = self.root
        r.bind("<Control-n>", lambda e: self.new_arena())
        r.bind("<Control-o>", lambda e: self.open_file())
        r.bind("<Control-s>", lambda e: self.save())
        r.bind("<Control-r>", lambda e: self.reload())
        r.bind("<Control-z>", lambda e: self.undo())
        r.bind("<Control-q>", lambda e: self.root.destroy())
        r.bind("<F1>", lambda e: self.show_help())

    # -- view -------------------------------------------------------------
    def S(self, x, z):
        return self.view.world_to_screen(x, z)

    def W(self, sx, sy):
        return self.view.screen_to_world(sx, sy)

    def fit(self):
        v = self.view
        w = max(1, self.canvas.winfo_width())
        h = max(1, self.canvas.winfo_height())
        r = _bounds(self.arenas)
        v.wx0, v.wz0, v.wx1, v.wz1 = r
        v.cx = (r[0] + r[2]) / 2.0
        v.cz = (r[1] + r[3]) / 2.0
        v.scale = min(w / (r[2] - r[0]), h / (r[3] - r[1])) * 0.9
        v.ox, v.oy = w / 2.0, h / 2.0

    def fit_view(self):
        self.fit()
        self.redraw()
        self._say("view fitted to the arena")

    def fit_region(self):
        a = self.cur()
        if not a.region:
            self._say("no region to fit to")
            return
        x0, z0, x1, z1 = a.region
        self._fit_rect((x0, z0, x1, z1))

    def region_from_view(self):
        v = self.view
        w = max(1, self.canvas.winfo_width())
        h = max(1, self.canvas.winfo_height())
        x0, z0 = self.W(0, 0)
        x1, z1 = self.W(w, h)
        self.push_undo()
        self.cur().region = (int(min(x0, x1)), int(min(z0, z1)),
                             int(max(x0, x1)), int(max(z0, z1)))
        self.cur().dirty = True
        self.refresh()
        self.redraw()
        self._say("region set to what is on screen")

    def _fit_rect(self, rect):
        v = self.view
        w = max(1, self.canvas.winfo_width())
        h = max(1, self.canvas.winfo_height())
        x0, z0, x1, z1 = rect
        v.cx = (x0 + x1) / 2.0
        v.cz = (z0 + z1) / 2.0
        v.scale = min(w / max(1.0, x1 - x0), h / max(1.0, z1 - z0)) * 0.9
        v.ox, v.oy = w / 2.0, h / 2.0
        self.redraw()

    def zoom(self, f, sx=None, sy=None):
        v = self.view
        if sx is None:
            sx, sy = self.canvas.winfo_width() / 2.0, self.canvas.winfo_height() / 2.0
        wx, wz = self.W(sx, sy)
        v.scale *= f
        v.ox = sx - (wx - v.cx) * v.scale
        v.oy = sy - (wz - v.cz) * v.scale
        self.redraw()

    # -- background ---------------------------------------------------------
    def _set_bg(self, img, rect, what):
        self.bg = img
        self.bg_rect = rect
        try:
            self.lbl_bg.configure(text=what)
        except Exception:
            pass
        self.redraw()

    def use_cached_map(self):
        city = CITY_NAMES.get(self.cur().city, "CHICAGO")
        img, rect = build_level_map(city, allow_build=False, verbose=False)
        if img is None:
            self._say("no cached level map for %s yet - use Build (it takes a few seconds)" % city)
            return
        self._set_bg(img, rect, "%s level map (cached)" % city)

    def build_map(self):
        city = CITY_NAMES.get(self.cur().city, "CHICAGO")
        self._say("building the %s level map\u2026" % city)
        self.root.update_idletasks()
        img, rect = build_level_map(city, rebuild=False, allow_build=True, verbose=False)
        if img is None:
            self._say("no rip for %s - DriverLevelTool/ needs a %s_LEVELMODEL.obj" % (city, city))
            return
        self._set_bg(img, rect, "%s level map (built)" % city)
        self._say("%s level map ready and cached" % city)

    def clear_bg(self):
        self.bg = self.bg_caller
        self.bg_rect = self.bg_caller_rect
        try:
            self.lbl_bg.configure(text="none" if self.bg is None else "the image you opened with")
        except Exception:
            pass
        self.redraw()

    # -- undo ---------------------------------------------------------------
    def snapshot(self):
        a = self.cur()
        return {"internal": a.internal, "display": a.display, "city": a.city,
                "mp_level": a.mp_level, "mp_arena": a.mp_arena, "region": a.region,
                "spawns": list(a.spawns), "pickups": [dict(p) for p in a.pickups]}

    def push_undo(self):
        self.undo_stack.append(self.snapshot())
        if len(self.undo_stack) > 200:
            self.undo_stack.pop(0)

    def undo(self):
        if not self.undo_stack:
            self._say("nothing to undo")
            return
        s = self.undo_stack.pop()
        a = self.cur()
        a.internal = s["internal"]
        a.display = s["display"]
        a.city = s["city"]
        a.mp_level = s["mp_level"]
        a.mp_arena = s["mp_arena"]
        a.region = s["region"]
        a.spawns = list(s["spawns"])
        a.pickups = [dict(p) for p in s["pickups"]]
        a.dirty = True
        self.sel = min(self.sel, len(a.spawns) - 1)
        self.refresh()
        self.redraw()
        self._say("undone")

    # -- the canvas ---------------------------------------------------------
    def redraw(self):
        c = self.canvas
        c.delete("all")
        a = self.cur()
        w = max(1, c.winfo_width())
        h = max(1, c.winfo_height())
        v = self.view

        if self.show_grid:
            step = GRID_STEPS[0]
            for s in GRID_STEPS:
                if s * v.scale >= 55:
                    step = s
                    break
            x0, z0 = self.W(0, 0)
            x1, z1 = self.W(w, h)
            gx = int(min(x0, x1) // step) * step
            while gx <= max(x0, x1):
                px = self.S(gx, 0)[0]
                c.create_line(px, 0, px, h, fill=GRID_COL)
                gx += step
            gz = int(min(z0, z1) // step) * step
            while gz <= max(z0, z1):
                py = self.S(0, gz)[1]
                c.create_line(0, py, w, py, fill=GRID_COL)
                gz += step
            c.create_text(w - 8, 8, anchor="ne", fill=GRID_TEXT,
                          text="grid %d units (a cell is 2048)" % step)

        if self.bg is not None and self.bg_rect:
            br = self.bg_rect
            p0 = self.S(br[0], br[1])
            p1 = self.S(br[2], br[3])
            iw = max(1, int(abs(p1[0] - p0[0])))
            ih = max(1, int(abs(p1[1] - p0[1])))
            if iw > 1 and ih > 1 and iw * ih < 40_000_000:
                from PIL import ImageTk
                self.bg_photo = ImageTk.PhotoImage(self.bg.resize((iw, ih)))
                c.create_image(min(p0[0], p1[0]), min(p0[1], p1[1]), anchor="nw",
                               image=self.bg_photo)

        if a.region:
            x0, z0, x1, z1 = a.region
            p = self.S(x0, z0)
            q = self.S(x1, z1)
            c.create_rectangle(p[0], p[1], q[0], q[1], outline=REGION_COL, width=2,
                               dash=(6, 3))
            for hx, hz in ((x0, z0), (x1, z0), (x0, z1), (x1, z1)):
                sx, sy = self.S(hx, hz)
                c.create_rectangle(sx - 4, sy - 4, sx + 4, sy + 4, fill=REGION_COL,
                                   outline="#000")
        else:
            c.create_text(12, h - 16, anchor="sw", fill="#7a5c00",
                          text="no region set - the whole level is the arena")

        for i, (x, z, hd, y) in enumerate(a.spawns):
            sx, sy = self.S(x, z)
            col = SPAWN_COL if i == 0 else OPP_COL
            if i == self.sel:
                c.create_oval(sx - 11, sy - 11, sx + 11, sy + 11, outline=SEL_COL, width=2)
                col = SEL_COL
            dx, dy = _heading_vec(hd)
            c.create_line(sx, sy, sx + dx * 26, sy + dy * 26, fill=col, width=2)
            c.create_oval(sx - 5, sy - 5, sx + 5, sy + 5, fill=col, outline="#000")
            if self.show_labels:
                label = "%d" % i if i else "P"
                c.create_text(sx + 9, sy + 9, anchor="nw", fill="#ffffff",
                              text=label, font=("TkDefaultFont", 8, "bold"))

        for i, p in enumerate(a.pickups):
            sx, sy = self.S(p["x"], p["z"])
            col = PICK_HEALTH if p["type"] == "health" else PICK_WEAPON
            if i == self.sel_pick:
                c.create_oval(sx - 10, sy - 10, sx + 10, sy + 10, outline=SEL_COL, width=2)
            c.create_rectangle(sx - 5, sy - 5, sx + 5, sy + 5, fill=col, outline="#000")

        if self.pending_corner is not None:
            sx, sy = self.S(*self.pending_corner)
            c.create_line(sx - 8, sy, sx + 8, sy, fill=REGION_COL, width=2)
            c.create_line(sx, sy - 8, sx, sy + 8, fill=REGION_COL, width=2)

        self._status_line()

    def _status_line(self):
        a = self.cur()
        what = a.path or "<unsaved: press Save>"
        star = " *unsaved*" if a.dirty else ""
        if a.region:
            where = "region %d,%d..%d,%d" % a.region
        else:
            where = "no region"
        self.lbl_file.configure(
            text="%s%s   |   %s   |   %s   |   %d spawn(s), %d pickup(s)"
                 % (what, star, a.internal, where, len(a.spawns), len(a.pickups)))

    # -- mouse --------------------------------------------------------------
    def on_press(self, ev):
        self.canvas.focus_set()
        wx, wz = self.W(ev.x, ev.y)
        a = self.cur()

        if self.mode == "spawn":
            if len(a.spawns) >= SPAWN_MAX:
                self._say("that is the maximum of %d spawns" % SPAWN_MAX)
                return
            self.push_undo()
            a.spawns.append((int(wx), int(wz), 0, None))
            self.sel = len(a.spawns) - 1
            a.dirty = True
            self.refresh()
            self.redraw()
            self._say("added spawn %d (spawn 1 is the player)" % self.sel)
            return

        if self.mode == "delete":
            i = self._pick_spawn(ev.x, ev.y)
            if i >= 0:
                self.push_undo()
                del a.spawns[i]
                a.dirty = True
                self.sel = -1
                self.refresh()
                self.redraw()
                self._say("deleted spawn %d" % i)
                return
            j = self._pick_pickup(ev.x, ev.y)
            if j >= 0:
                self.push_undo()
                del a.pickups[j]
                a.dirty = True
                self.refresh()
                self.redraw()
                self._say("deleted pickup %d" % j)
                return
            self._say("nothing there to delete")
            return

        if self.mode == "region":
            if self.pending_corner is None:
                self.pending_corner = (int(wx), int(wz))
                self._say("corner A set - click the opposite corner")
            else:
                ax, az = self.pending_corner
                self.push_undo()
                a.region = (min(ax, int(wx)), min(az, int(wz)),
                            max(ax, int(wx)), max(az, int(wz)))
                a.dirty = True
                self.pending_corner = None
                self.refresh()
                self._say("region set: %d,%d..%d,%d" % a.region)
            self.redraw()
            return

        # select mode: a spawn, a region corner, or nothing
        i = self._pick_spawn(ev.x, ev.y)
        if i >= 0:
            self.sel = i
            self.tv_spawns.selection_set(self.tv_spawns.get_children()[i])
            self.drag = ("spawn", i)
            self.refresh()
            self.redraw()
            return
        if a.region:
            x0, z0, x1, z1 = a.region
            for k, (hx, hz) in enumerate(((x0, z0), (x1, z0), (x0, z1), (x1, z1))):
                sx, sy = self.S(hx, hz)
                if abs(sx - ev.x) < 9 and abs(sy - ev.y) < 9:
                    self.drag = ("corner", k)
                    return
        j = self._pick_pickup(ev.x, ev.y)
        if j >= 0:
            self.sel_pick = j
            self.sel = -1
            self.refresh()
            self.redraw()
            return
        self.deselect()
        self.redraw()

    def on_drag(self, ev):
        if self.drag is None:
            return
        wx, wz = self.W(ev.x, ev.y)
        a = self.cur()
        kind, k = self.drag
        if kind == "spawn" and 0 <= k < len(a.spawns):
            x, z, hd, y = a.spawns[k]
            a.spawns[k] = (int(wx), int(wz), hd, y)
            a.dirty = True
            self.redraw()
        elif kind == "corner" and a.region:
            x0, z0, x1, z1 = a.region
            corners = [[x0, z0], [x1, z0], [x0, z1], [x1, z1]]
            corners[k] = [int(wx), int(wz)]
            xs = sorted(c[0] for c in corners)
            zs = sorted(c[1] for c in corners)
            a.region = (xs[0], zs[0], xs[3], zs[3])
            a.dirty = True
            self.redraw()

    def on_release(self, ev):
        if self.drag is not None:
            self.drag = None
            self.refresh()
        self.pan_from = None

    def on_pan_start(self, ev):
        self.canvas.focus_set()
        self.pan_from = (ev.x, ev.y, self.view.ox, self.view.oy)

    def on_pan(self, ev):
        if self.pan_from is None:
            return
        x0, y0, ox, oy = self.pan_from
        self.view.ox = ox + (ev.x - x0)
        self.view.oy = oy + (ev.y - y0)
        self.redraw()

    def on_right(self, ev):
        # right-click: delete whatever is under the pointer (no mode switch)
        i = self._pick_spawn(ev.x, ev.y)
        if i >= 0:
            self.push_undo()
            del self.cur().spawns[i]
            self.cur().dirty = True
            self.sel = -1
            self.refresh()
            self.redraw()
            self._say("deleted spawn %d" % i)
            return
        j = self._pick_pickup(ev.x, ev.y)
        if j >= 0:
            self.push_undo()
            del self.cur().pickups[j]
            self.cur().dirty = True
            self.refresh()
            self.redraw()
            self._say("deleted pickup %d" % j)

    def on_motion(self, ev):
        wx, wz = self.W(ev.x, ev.y)
        self.lbl_pos.configure(text="cursor  %d , %d" % (int(wx), int(wz)))

    def on_wheel(self, ev):
        self.zoom(1.15 if ev.delta > 0 else 1 / 1.15, ev.x, ev.y)

    def on_key(self, ev):
        k = ev.keysym.lower()
        if k in TOOL_KEYS:
            self.var_mode.set(TOOL_KEYS[k])
            self._mode_changed()
        elif k == "f":
            self.fit_view()
        elif k == "g":
            self.var_grid.set(not self.var_grid.get())
            self.redraw()
        elif k == "l":
            self.var_labels.set(not self.var_labels.get())
            self.redraw()
        elif k in ("delete", "backspace"):
            self.delete_selected()
        elif k == "escape":
            self.pending_corner = None
            self.deselect()
            self.redraw()

    def _pick_spawn(self, sx, sy):
        best, bestd = -1, 18.0
        for i, (x, z, _h, _y) in enumerate(self.cur().spawns):
            p = self.S(x, z)
            d = ((p[0] - sx) ** 2 + (p[1] - sy) ** 2) ** 0.5
            if d < bestd:
                best, bestd = i, d
        return best

    def _pick_pickup(self, sx, sy):
        best, bestd = -1, 16.0
        for i, p in enumerate(self.cur().pickups):
            q = self.S(p["x"], p["z"])
            d = ((q[0] - sx) ** 2 + (q[1] - sy) ** 2) ** 0.5
            if d < bestd:
                best, bestd = i, d
        return best

    # -- commands -----------------------------------------------------------
    def deselect(self):
        self.sel = -1
        self.sel_pick = -1
        try:
            self.tv_spawns.selection_remove(*self.tv_spawns.selection())
            self.tv_pickups.selection_remove(*self.tv_pickups.selection())
        except Exception:
            pass
        self.refresh()

    def add_spawn(self):
        a = self.cur()
        if len(a.spawns) >= SPAWN_MAX:
            self._say("the maximum is %d spawns" % SPAWN_MAX)
            return
        self.push_undo()
        v = self.view
        nx = int(v.cx)
        nz = int(v.cz)
        if a.spawns:
            x, z, _h, _y = a.spawns[-1]
            nx, nz = x + 2000, z + 2000
        a.spawns.append((nx, nz, 0, None))
        self.sel = len(a.spawns) - 1
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("added spawn %d" % self.sel)

    def dup_spawn(self):
        a = self.cur()
        if self.sel < 0 or self.sel >= len(a.spawns):
            self._say("select a spawn first")
            return
        if len(a.spawns) >= SPAWN_MAX:
            self._say("the maximum is %d spawns" % SPAWN_MAX)
            return
        self.push_undo()
        x, z, hd, y = a.spawns[self.sel]
        a.spawns.insert(self.sel + 1, (x, z, hd, y))
        self.sel += 1
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("duplicated the spawn")

    def delete_selected(self):
        a = self.cur()
        if self.sel_pick >= 0 and self.sel_pick < len(a.pickups):
            self.push_undo()
            del a.pickups[self.sel_pick]
            a.dirty = True
            self.sel_pick = -1
            self.refresh()
            self.redraw()
            self._say("deleted the pickup")
            return
        if self.sel < 0 or self.sel >= len(a.spawns):
            self._say("nothing selected")
            return
        self.push_undo()
        del a.spawns[self.sel]
        a.dirty = True
        self.sel = -1
        self.refresh()
        self.redraw()
        self._say("deleted the spawn")

    def apply_arena(self):
        a = self.cur()
        self.push_undo()
        a.internal = self.e_internal.get().strip() or a.internal
        a.display = self.e_display.get().strip() or a.internal
        a.city = CITY_INDEX.get(self.var_city.get(), a.city)
        a.mp_level = 1 if self.var_mp.get() else 0
        a.mp_arena = 1 if self.var_layout.get() == "1" else 0
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("arena settings applied")

    def apply_region(self):
        a = self.cur()
        try:
            x0 = int(float(self.region_entries["x0"].get()))
            z0 = int(float(self.region_entries["z0"].get()))
            x1 = int(float(self.region_entries["x1"].get()))
            z1 = int(float(self.region_entries["z1"].get()))
        except ValueError:
            self._say("the region wants four numbers (x0 z0 x1 z1)")
            return
        self.push_undo()
        a.region = (min(x0, x1), min(z0, z1), max(x0, x1), max(z0, z1))
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("region set")

    def clear_region(self):
        self.push_undo()
        self.cur().region = None
        self.cur().dirty = True
        self.refresh()
        self.redraw()
        self._say("region cleared - the whole level is the arena now")

    def apply_spawn(self):
        a = self.cur()
        if self.sel < 0 or self.sel >= len(a.spawns):
            self._say("select a spawn first")
            return
        try:
            x = int(float(self.sp_entries["x"].get()))
            z = int(float(self.sp_entries["z"].get()))
            hd = int(float(self.sp_entries["head"].get())) & (HEADING_MAX - 1)
            yraw = self.sp_entries["y"].get().strip()
            y = int(float(yraw)) if yraw else None
        except ValueError:
            self._say("x, y, z and head want numbers (y may be blank)")
            return
        self.push_undo()
        a.spawns[self.sel] = (x, z, hd, y)
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("spawn %d updated" % self.sel)

    def add_pickup(self):
        a = self.cur()
        v = self.view
        kind = self.var_pick_kind.get()
        what = self.e_pick_what.get().strip()
        amt = self.e_pick_amt.get().strip()
        x, z = int(v.cx), int(v.cz)
        if kind == "health":
            try:
                amount = int(amt) if amt else 2500
            except ValueError:
                self._say("the health amount wants a number")
                return
            p = {"type": "health", "weapon": None, "amount": amount, "x": x, "z": z}
        else:
            if not what:
                self._say("a weapon pickup wants a weapon name")
                return
            try:
                amount = int(amt) if amt else 5
            except ValueError:
                self._say("the ammo wants a number")
                return
            p = {"type": "weapon", "weapon": what, "amount": amount, "x": x, "z": z}
        self.push_undo()
        a.pickups.append(p)
        a.dirty = True
        self.refresh()
        self.redraw()
        self._say("pickup added - drag it where you want (select mode)")

    def del_pickup(self):
        a = self.cur()
        if self.sel_pick < 0 or self.sel_pick >= len(a.pickups):
            self._say("select a pickup in the list first")
            return
        self.push_undo()
        del a.pickups[self.sel_pick]
        a.dirty = True
        self.sel_pick = -1
        self.refresh()
        self.redraw()
        self._say("deleted the pickup")

    # -- files --------------------------------------------------------------
    def _confirm_discard(self):
        if not self.cur().dirty:
            return True
        from tkinter import messagebox
        return messagebox.askyesno("Unsaved changes",
                                   "This arena has unsaved changes. Throw them away?")

    def new_arena(self):
        from tkinter import simpledialog
        if not self._confirm_discard():
            return
        name = simpledialog.askstring("New arena", "arena name (letters/digits/underscore):",
                                      parent=self.root)
        if not name:
            return
        path = os.path.join(os.path.dirname(self.cur().path or "."), name + ".cca")
        a = Arena(name, name, CITY_INDEX.get(self.var_city.get(), 0), 1, 0)
        a.path = path
        self.arenas.append(a)
        self.idx = len(self.arenas) - 1
        self.sel = -1
        self.refresh()
        self.fit()
        self.redraw()
        self._say("new arena '%s' - Save to write %s" % (name, path))

    def open_file(self):
        from tkinter import filedialog
        if not self._confirm_discard():
            return
        p = filedialog.askopenfilename(title="Open an arena",
                                       filetypes=[("arena files", "*.cca"), ("all files", "*.*")])
        if not p:
            return
        a, saw = load_arena(p)
        if not saw:
            from tkinter import messagebox
            messagebox.showerror("Not an arena", "%s has no 'arena:' line." % p)
            return
        self.arenas.append(a)
        self.idx = len(self.arenas) - 1
        self.sel = -1
        self.refresh()
        self.fit()
        self.redraw()
        self._say("opened %s" % p)

    def save(self):
        a = self.cur()
        if not a.path:
            return self.save_as()
        self._write(a, a.path)
        return a.path

    def save_as(self):
        from tkinter import filedialog
        a = self.cur()
        initial = os.path.basename(a.path or (a.internal + ".cca"))
        p = filedialog.asksaveasfilename(title="Save arena as", initialfile=initial,
                                         defaultextension=".cca",
                                         filetypes=[("arena files", "*.cca")])
        if not p:
            return None
        a.path = p
        self._write(a, p)
        return p

    def _write(self, a, path):
        a.path = path
        save_arena(a)
        try:
            a.mtime = os.path.getmtime(path)
        except OSError:
            a.mtime = None
        a.dirty = False
        self.refresh()
        self.redraw()
        self._say("saved %s" % path)

    def reload(self):
        a = self.cur()
        if not a.path or not os.path.exists(a.path):
            self._say("nothing on disk to reload yet")
            return
        fresh, _ = load_arena(a.path)
        a.__dict__.update(fresh.__dict__)
        self.sel = -1
        self.undo_stack = []
        self.refresh()
        self.redraw()
        self._say("reloaded from disk")

    def show_help(self):
        from tkinter import messagebox
        messagebox.showinfo(
            "Shortcuts",
            "Tools:  1 select   2 add spawn   3 delete   4 region\n"
            "Map:    left = use the tool (or drag a spawn / region corner)\n"
            "        right-click = delete what is under the pointer\n"
            "        middle-drag = pan, wheel = zoom\n"
            "Keys:   F fit   G grid   L labels   Del delete   Esc cancel\n"
            "        Ctrl+S save   Ctrl+O open   Ctrl+N new   Ctrl+R reload\n"
            "        Ctrl+Z undo   Ctrl+Q quit\n\n"
            "Spawn 1 is the player, the rest are opponents.\n"
            "Keep every spawn inside the region: the boundary only pulls back a car\n"
            "that drove out from inside; one that starts outside is left alone.")

    # -- inspector <-> model ------------------------------------------------
    def on_arena_pick(self, _ev=None):
        i = self.cb_arena.current()
        if 0 <= i < len(self.arenas):
            self.idx = i
            self.sel = -1
            self.sel_pick = -1
            self.undo_stack = []
            self.refresh()
            self.fit()
            self.redraw()

    def on_spawn_select(self, _ev=None):
        sel = self.tv_spawns.selection()
        if not sel:
            return
        i = self.tv_spawns.index(sel[0])
        self.sel = i
        self.sel_pick = -1
        self._fill_spawn_form()
        self.redraw()

    def on_pickup_select(self, _ev=None):
        sel = self.tv_pickups.selection()
        if not sel:
            return
        self.sel_pick = self.tv_pickups.index(sel[0])
        self.sel = -1
        self.redraw()

    def _fill_spawn_form(self):
        a = self.cur()
        if 0 <= self.sel < len(a.spawns):
            x, z, hd, y = a.spawns[self.sel]
            for k, val in (("x", x), ("z", z), ("head", hd), ("y", "" if y is None else y)):
                self.sp_entries[k].delete(0, "end")
                self.sp_entries[k].insert(0, str(val))

    def refresh(self):
        a = self.cur()
        self.cb_arena.configure(values=["%d: %s%s" % (i, x.internal, " *" if x.dirty else "")
                                        for i, x in enumerate(self.arenas)])
        self.cb_arena.current(self.idx)

        self.e_internal.delete(0, "end")
        self.e_internal.insert(0, a.internal)
        self.e_display.delete(0, "end")
        self.e_display.insert(0, a.display)
        self.var_city.set(CITY_NAMES.get(a.city, "CHICAGO"))
        self.var_mp.set(bool(a.mp_level))
        self.var_layout.set("1" if a.mp_arena else "0")

        if a.region:
            for k, val in zip(("x0", "z0", "x1", "z1"), a.region):
                self.region_entries[k].delete(0, "end")
                self.region_entries[k].insert(0, str(val))
        else:
            for k in ("x0", "z0", "x1", "z1"):
                self.region_entries[k].delete(0, "end")

        self.tv_spawns.delete(*self.tv_spawns.get_children())
        for i, (x, z, hd, y) in enumerate(a.spawns):
            self.tv_spawns.insert("", "end",
                                  values=(("P" if i == 0 else str(i)), x, "" if y is None else y,
                                          z, hd))
        if 0 <= self.sel < len(a.spawns):
            kids = self.tv_spawns.get_children()
            self.tv_spawns.selection_set(kids[self.sel])
            self._fill_spawn_form()

        self.tv_pickups.delete(*self.tv_pickups.get_children())
        for p in a.pickups:
            self.tv_pickups.insert("", "end", values=(
                p["type"], p.get("weapon") or p.get("amount"), p["x"], p["z"]))

        self._status_line()

    # -- the other editor's saves -------------------------------------------
    def _poll(self):
        for i, a in enumerate(self.arenas):
            if not a.path:
                continue
            try:
                m = os.path.getmtime(a.path)
            except OSError:
                continue
            if a.mtime is None:
                a.mtime = m
                continue
            if abs(m - a.mtime) < 1e-6:
                continue
            if a.dirty:
                self._say("'%s' changed on disk - Ctrl+R to take it (you have unsaved edits)"
                          % a.internal)
            else:
                fresh, _ = load_arena(a.path)
                a.__dict__.update(fresh.__dict__)
                if i == self.idx:
                    self.refresh()
                    self.redraw()
                self._say("'%s' reloaded from disk (the game saved it)" % a.internal)
        self.root.after(900, self._poll)

def run_editor(arenas, bg=None, bg_rect=None, hint=""):
    """Open the editor window.

    Returns 0, or 2 when tkinter/Pillow is missing (with one clear line saying
    what to install, rather than a traceback).
    """
    try:
        import tkinter as tk
    except ImportError:
        print("arenaedit: the editor window needs tkinter, which this Python lacks.")
        print("  interpreter: %s" % sys.executable)
        print("  fix: install Python 3 from python.org - the Microsoft Store build")
        print("       ships without tkinter. Check with:  py -3 -c 'import tkinter'")
        print("  or use a headless mode: --check | --render OUT.png | --json")
        return 2

    try:
        from PIL import Image, ImageTk      # noqa: F401  (the canvas needs both)
    except ImportError:
        print("arenaedit: the editor window needs Pillow (PIL) for its canvas image.")
        print("  fix: py -3 -m pip install pillow")
        print("  or use a headless mode: --check | --render OUT.png | --json")
        return 2

    root = tk.Tk()
    app = EditorApp(root, arenas, bg, bg_rect, hint)
    root.minsize(1000, 640)

    # make sure the window is not lost behind the fullscreen game or the console:
    # raise it, hold it on top just long enough to appear, then let it behave
    root.update_idletasks()
    root.deiconify()
    root.lift()
    root.attributes("-topmost", True)
    root.after(500, lambda: root.attributes("-topmost", False))
    try:
        root.focus_force()
    except Exception:
        pass

    root.mainloop()
    return app.exit_code
def ui_selftest(arenas, bg=None, bg_rect=None):
    """Build the editor window, drive it through its own commands, and report.

    This is how the UI is checked without a human at the screen (and without
    entering mainloop): it pumps the window, clicks the map, uses the inspector,
    undoes, saves and reloads. Exit 0 when every step behaved.
    """
    import tempfile
    import tkinter as tk

    ok = True

    def check(label, cond):
        nonlocal ok
        print("  %-40s %s" % (label, "ok" if cond else "FAIL"))
        if not cond:
            ok = False

    print("ui selftest")

    class Ev:
        def __init__(self, x, y):
            self.x = x
            self.y = y
            self.delta = 120

    root = tk.Tk()
    app = EditorApp(root, arenas, bg, bg_rect, "ui selftest")
    root.update()

    cur = app.cur()
    n0 = len(cur.spawns)
    p0 = len(cur.pickups)
    r0 = cur.region
    check("window built (%d arena(s))" % len(app.arenas), len(app.arenas) == len(arenas))
    check("toolbar mode is select", app.var_mode.get() == "select")
    check("menubar present", root.cget("menu") != "")

    app.add_spawn()
    root.update()
    check("Add spawn button -> %d spawn(s)" % (n0 + 1), len(cur.spawns) == n0 + 1)

    cx = max(40, app.canvas.winfo_width() // 2)
    cy = max(40, app.canvas.winfo_height() // 2)
    app.mode = "spawn"
    app.on_press(Ev(cx, cy))
    root.update()
    check("click-to-add -> %d spawn(s)" % (n0 + 2), len(cur.spawns) == n0 + 2)

    app.mode = "region"
    app.on_press(Ev(cx, cy))
    app.on_press(Ev(cx + 60, cy + 60))
    root.update()
    check("two clicks set the region", cur.region != r0)

    app.undo()
    root.update()
    check("undo restored the region", cur.region == r0)

    app.sel = 0
    app._fill_spawn_form()
    app.sp_entries["x"].delete(0, "end")
    app.sp_entries["x"].insert(0, "1234")
    app.apply_spawn()
    root.update()
    check("inspector applied x=1234", cur.spawns[0][0] == 1234)

    check("spawn list has %d row(s)" % len(cur.spawns),
          len(app.tv_spawns.get_children()) == len(cur.spawns))

    app.var_pick_kind.set("health")
    app.e_pick_amt.delete(0, "end")
    app.e_pick_amt.insert(0, "1500")
    app.add_pickup()
    root.update()
    check("added a pickup -> %d" % (p0 + 1), len(cur.pickups) == p0 + 1)
    check("pickup list has %d row(s)" % len(cur.pickups),
          len(app.tv_pickups.get_children()) == len(cur.pickups))

    app.zoom(1.25)
    app.fit_view()
    root.update()
    check("zoom + fit ran", app.view.scale > 0)

    tmp = os.path.join(tempfile.gettempdir(), "_uitest_arena.cca")
    cur.path = tmp
    app.save()
    root.update()
    check("saved to disk", os.path.exists(tmp))

    back, saw = load_arena(tmp)
    check("round-trips (%d spawns, %d pickups)" % (len(back.spawns), len(back.pickups)),
          saw and back.spawns == cur.spawns and len(back.pickups) == len(cur.pickups))

    root.destroy()
    try:
        os.remove(tmp)
    except OSError:
        pass

    print("  result                                   %s" % ("OK" if ok else "PROBLEM"))
    return 0 if ok else 2


# ---------------------------------------------------------------------------
# what the tools need, in one place: --selftest, and the launcher's Check setup
# ---------------------------------------------------------------------------
def _repo_root():
    # <root>/JERICHO/MODS/cainescrossfire/tools/arenaedit.py -> <root>
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(here))))


def _find_game_exe():
    root = _repo_root()
    for cfg, name in (("Release_dev", "REDRIVER2_dev.exe"),
                      ("Release", "REDRIVER2.exe")):
        p = os.path.join(root, "src_rebuild", "bin", cfg, name)
        if os.path.exists(p):
            return p
    return None


def selftest(arena_dir):
    """Print what the editor needs and where it looks; 0 = OK, 2 = a problem."""
    ok = True

    print("arenaedit setup check")
    print("  interpreter : %s" % sys.executable)
    print("                Python %s" % sys.version.split()[0])

    try:
        import tkinter
        print("  tkinter     : OK (Tk %s)" % tkinter.TkVersion)
    except Exception as e:
        ok = False
        print("  tkinter     : MISSING (%s)" % e)
        print("                the editor window needs it - install Python from")
        print("                python.org; the Microsoft Store build has no tkinter")

    try:
        import PIL
        print("  Pillow      : OK (%s)" % PIL.__version__)
    except Exception as e:
        ok = False
        print("  Pillow      : MISSING (%s)" % e)
        print("                needed for a background image - py -3 -m pip install pillow")

    print("  arena folder: %s" % arena_dir)
    if os.path.isdir(arena_dir):
        files = sorted(glob.glob(os.path.join(arena_dir, "*.cca")))
        print("                exists, %d arena file(s)" % len(files))
        for f in files:
            print("                  %s" % os.path.basename(f))
    else:
        print("                DOES NOT EXIST yet (created on first save)")

    for city in sorted(CITIES.values()):
        obj, png, side = level_rip_paths(city)
        have = "cached map" if os.path.exists(png) else ("rip, no map yet" if os.path.exists(obj) else "-")
        print("  level %-8s: %s" % (city, have))

    exe = _find_game_exe()
    print("  game exe    : %s" % (exe if exe else
                                  "not found - the launcher's in-game option needs it"))

    print("  result      : %s" % ("OK" if ok else "PROBLEM - see above"))
    return 0 if ok else 2


def main(argv=None):
    ap = argparse.ArgumentParser(description="Caine's Crossfire arena editor (top-down).")
    ap.add_argument("files", nargs="*", help="one or more .cca arena files (globs ok); default: the mod's arenas folder")
    ap.add_argument("--dir", help="the arena folder to list when no files are given")
    ap.add_argument("--new", metavar="NAME", help="create a blank arena NAME.cca in the arena folder, then open it")
    ap.add_argument("--city", default="CHICAGO", help="city for --new (default CHICAGO)")
    ap.add_argument("--map", help="background image (PNG) stretched over --map-world")
    ap.add_argument("--map-world", nargs=4, type=int, metavar=("X0", "Z0", "X1", "Z1"),
                    help="the world rectangle the background image covers")
    ap.add_argument("--obj", help="a level .obj (DriverLevelTool) drawn as a top-down background")
    ap.add_argument("--obj-world", nargs=4, type=int, metavar=("X0", "Z0", "X1", "Z1"),
                    help="the world rectangle the .obj's own bounding box is stretched onto")
    ap.add_argument("--cells", nargs=2, type=int, metavar=("W", "H"),
                    help="level grid (DriverLevelTool 'Level dimensions [W H]'); sets --obj-world")
    ap.add_argument("--level", nargs="?", const="auto", metavar="CITY",
                    help="draw a city's DriverLevelTool rip as the top-down background, aligned "
                         "to the arena's world coordinates (omit CITY to use the arena's own). "
                         "Cached next to the .obj, so it is slow only the first time")
    ap.add_argument("--rebuild-map", action="store_true",
                    help="rebuild the cached level map even when it looks current")
    ap.add_argument("--render", metavar="OUT.png", help="render headlessly and exit")
    ap.add_argument("--check", action="store_true", help="validate and print, do not open a window")
    ap.add_argument("--json", action="store_true", help="print the parsed arenas as JSON")
    ap.add_argument("--selftest", action="store_true",
                    help="report the interpreter, tkinter/Pillow, the arena folder and the game exe, then exit")
    ap.add_argument("--uitest", action="store_true",
                    help="build the editor window, drive it through its own commands, report and exit (a headless UI check)")
    args = ap.parse_args(argv)

    arena_dir = args.dir or _default_arena_dir()

    if args.selftest:
        return selftest(arena_dir)

    paths = []

    for pat in (args.files or []):
        if any(c in pat for c in "*?["):
            paths.extend(sorted(glob.glob(pat)))
        else:
            paths.append(pat)

    if args.new:
        os.makedirs(arena_dir, exist_ok=True)
        path = os.path.join(arena_dir, args.new + ".cca")
        if os.path.exists(path):
            print("already exists, opening:", path)
        else:
            a = Arena(args.new, args.new, CITY_INDEX.get(args.city.upper(), 0), 1, 0)
            a.path = path
            save_arena(a)
            print("created", path)
        paths = [path]

    if not paths:
        paths = sorted(glob.glob(os.path.join(arena_dir, "*.cca")))
        if paths:
            print("(no files given; using %s)" % arena_dir)

    arenas = []
    for path in paths:
        if not os.path.exists(path):
            print("skip (missing):", path)
            continue
        a, saw = load_arena(path)
        if not saw:
            print("skip (no arena: line):", path)
            continue
        arenas.append(a)

    hint = ""

    if not arenas:
        print("No arena files found.")
        print("  make one:   python arenaedit.py --new chicago_docks --city CHICAGO")
        print("  or drop .cca files in: %s" % arena_dir)

        # the offline modes just report; the editor still OPENS (empty) so the
        # user gets a window with the folder it looked in and how to make one.
        # (--level is the exception: with a level map there IS something to draw,
        # so a --render builds/renders it even with no arena files.)
        if (args.json or args.check or args.render) and not args.level:
            return 0

        os.makedirs(arena_dir, exist_ok=True)
        blank = Arena("new_arena", "New Arena", CITY_INDEX.get(args.city.upper(), 0), 1, 0)
        blank.path = os.path.join(arena_dir, "new_arena.cca")
        arenas = [blank]
        hint = ("no .cca files in %s  -  press N to make one, or run: "
                "python arenaedit.py --new myarena" % arena_dir)

    if args.json:
        import json
        print(json.dumps([{
            "path": a.path, "internal": a.internal, "display": a.display,
            "city": a.city, "mp_level": a.mp_level, "mp_arena": a.mp_arena,
            "region": a.region, "spawns": a.spawns, "pickups": a.pickups,
        } for a in arenas], indent=2))
        return 0

    if args.check:
        rc = 0
        for a in arenas:
            warns = check_arena(a)
            print("%s  %s" % ("OK  " if not warns else "WARN", describe(a)))
            for w in warns:
                print("      -", w)
                rc = 1
        return rc

    # background: a prepared image, or a level .obj point cloud. `bg_rect` is the
    # world rectangle the picture covers; the view (interactive or rendered)
    # frames the arena and places the picture under it.
    bg = None
    bg_rect = tuple(args.map_world) if args.map_world else None

    if args.obj and not os.path.exists(args.obj):
        print("arenaedit: --obj wants a DriverLevelTool .obj and there is no '%s'." % args.obj)
        print("  for a city's rip drawn as an aligned background, use:  --level CITY")
        return 2

    if args.obj:
        if args.obj_world:
            wr = tuple(args.obj_world)
        elif args.cells:
            wr = cells_world_rect(args.cells[0], args.cells[1])
        else:
            wr = cells_world_rect(448, 576)   # a common DriverLevelTool grid
            print("note: no --obj-world/--cells; assuming a 448x576 grid for the obj mapping")
        bg_rect = wr
        bg = load_obj_points(args.obj, wr)
    elif args.map:
        try:
            from PIL import Image
        except ImportError:
            print("arenaedit: --map needs Pillow (PIL) to read the background image.")
            print("  fix: py -3 -m pip install pillow")
            return 2
        if not os.path.exists(args.map):
            # by far the most common mistake: `--map RIO` meaning "use the RIO
            # level map". That is --level; --map is for a plain image file.
            print("arenaedit: --map wants an IMAGE FILE (png/jpg) and there is no '%s'." % args.map)
            print("  for a city's level rip as the background, use:  --level %s" % args.map.upper())
            print("  (or just --level with no value, to use the arena's own city)")
            return 2

        bg = Image.open(args.map).convert("RGB")
        if bg_rect is None:
            bg_rect = _bounds(arenas)
    else:
        # a level rip: an explicit --level CITY builds or loads it; otherwise an
        # already-cached one for the arena's city is used, and NOT built - so
        # opening never stalls on a 60-second parse of a 185 MB .obj
        city = None

        if args.level:
            if args.level == "auto":
                city = CITY_NAMES.get(arenas[0].city if arenas else 0)
            else:
                city = args.level
        elif arenas:
            city = CITY_NAMES.get(arenas[0].city)

        if city:
            img, rect = build_level_map(city, rebuild=args.rebuild_map,
                                        allow_build=bool(args.level),
                                        verbose=bool(args.level) or args.rebuild_map)

            if img is not None:
                bg, bg_rect = img, rect

    if args.uitest:
        return ui_selftest(arenas, bg, bg_rect)

    if args.render:
        render_png(arenas, args.render, bg, bg_rect)
        print("wrote", args.render)
        return 0

    return run_editor(arenas, bg, bg_rect, hint=hint)


if __name__ == "__main__":
    sys.exit(main())
