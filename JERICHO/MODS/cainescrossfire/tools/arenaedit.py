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
        for (x, z, _h) in a.spawns:
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
def run_editor(arenas, bg=None, bg_rect=None):
    import tkinter as tk
    from PIL import Image, ImageTk

    if not arenas:
        print("no arena files given")
        return 1

    state = {"idx": 0, "sel": -1, "mode": None, "bg": bg, "bg_rect": bg_rect,
             "bgimg": None, "status": ""}

    root = tk.Tk()
    root.title("Caine's Crossfire arena editor")
    canvas = tk.Canvas(root, width=1100, height=760, background="#121216",
                       highlightthickness=0)
    canvas.pack(fill="both", expand=True)

    def cur():
        return arenas[state["idx"]]

    def touch():
        # any local edit marks the arena dirty, so the file watcher knows not to
        # reload over it from disk
        cur().dirty = True

    view = View(_bounds(arenas))
    state["view"] = view

    def fit():
        v = state["view"]
        w = max(1, canvas.winfo_width())
        h = max(1, canvas.winfo_height())
        r = _bounds(arenas)
        v.wx0, v.wz0, v.wx1, v.wz1 = r
        v.cx = (r[0] + r[2]) / 2.0
        v.cz = (r[1] + r[3]) / 2.0
        v.scale = min(w / (r[2] - r[0]), h / (r[3] - r[1])) * 0.92
        v.ox, v.oy = w / 2.0, h / 2.0

    def S(x, z):
        return state["view"].world_to_screen(x, z)

    def redraw():
        canvas.delete("all")
        v = state["view"]
        a = cur()
        if state["bg"] and state["bg_rect"]:
            br = state["bg_rect"]
            p0 = S(br[0], br[1]); p1 = S(br[2], br[3])
            w = max(1, int(abs(p1[0] - p0[0])))
            h = max(1, int(abs(p1[1] - p0[1])))
            state["bgimg"] = ImageTk.PhotoImage(state["bg"].resize((w, h)))
            canvas.create_image(min(p0[0], p1[0]), min(p0[1], p1[1]),
                                anchor="nw", image=state["bgimg"])

        # region
        if a.region:
            x0, z0, x1, z1 = a.region
            p0 = S(x0, z0); p1 = S(x1, z1)
            canvas.create_rectangle(p0[0], p0[1], p1[0], p1[1],
                                    outline="#ffc83c", width=2, tags="region")
            for (hx, hz) in [(x0, z0), (x1, z0), (x0, z1), (x1, z1)]:
                sx, sy = S(hx, hz)
                canvas.create_rectangle(sx - 5, sy - 5, sx + 5, sy + 5,
                                        fill="#ffc83c", tags=("knob", (hx, hz)))

        # spawns
        for i, (x, z, h, y) in enumerate(a.spawns):
            sx, sy = S(x, z)
            color = "#7dff7d" if i == 0 else "#ff7d7d"
            idx_tag = ("spawn", i)
            dx, dy = _heading_vec(h)
            canvas.create_line(sx, sy, sx + dx * 28, sy + dy * 28,
                               fill=color, width=2, tags=idx_tag)
            r = 7 if i == state["sel"] else 5
            canvas.create_oval(sx - r, sy - r, sx + r, sy + r, fill=color,
                               outline="#000", width=2, tags=idx_tag)
            canvas.create_text(sx + 10, sy + 12, text=str(i), fill="#fff",
                               anchor="nw", tags=idx_tag)

        canvas.create_text(10, 10, anchor="nw", fill="#e8e8e8", text=(
            "%s   [%s]%s\nfile %d/%d   selected spawn %d\n"
            "n new  d delete  r region  s save  L reload  Tab file  q quit"
            % (describe(a), a.path or "<new>", "  *unsaved*" if a.dirty else "",
               state["idx"] + 1, len(arenas), state["sel"])))
        if state.get("status"):
            canvas.create_text(10, canvas.winfo_height() - 24, anchor="nw",
                               fill="#ffd35c", text=state["status"])

    def pick_spawn(sx, sy):
        best, bestd = -1, 18.0
        for i, (x, z, h, y) in enumerate(cur().spawns):
            p = S(x, z)
            d = ((p[0] - sx) ** 2 + (p[1] - sy) ** 2) ** 0.5
            if d < bestd:
                best, bestd = i, d
        return best

    def on_press(ev):
        a = cur()
        if ev.num == 2:  # middle: pan
            state["mode"] = "pan"
            return
        wx, wz = state["view"].screen_to_world(ev.x, ev.y)
        # a region knob?
        if a.region:
            x0, z0, x1, z1 = a.region
            for k, (hx, hz) in enumerate([(x0, z0), (x1, z0), (x0, z1), (x1, z1)]):
                p = S(hx, hz)
                if abs(p[0] - ev.x) < 8 and abs(p[1] - ev.y) < 8:
                    state["mode"] = ("knob", k)
                    return
        i = pick_spawn(ev.x, ev.y)
        if ev.num == 3:  # right: add or delete
            if i >= 0:
                del a.spawns[i]
                state["sel"] = -1
            elif len(a.spawns) < SPAWN_MAX:
                a.spawns.append((int(wx), int(wz), 0, None))
                state["sel"] = len(a.spawns) - 1
            touch()
            redraw()
            return
        state["sel"] = i
        state["mode"] = "drag" if i >= 0 else None
        redraw()

    def on_drag(ev):
        a = cur()
        wx, wz = state["view"].screen_to_world(ev.x, ev.y)
        if state["mode"] == "pan":
            state["view"].ox += 0  # pan handled by motion delta below
            return
        if state["mode"] == "drag" and 0 <= state["sel"] < len(a.spawns):
            x, z, h, y = a.spawns[state["sel"]]
            a.spawns[state["sel"]] = (int(wx), int(wz), h, y)
            touch()
            redraw()
        elif isinstance(state["mode"], tuple) and a.region:
            x0, z0, x1, z1 = a.region
            corners = [[x0, z0], [x1, z0], [x0, z1], [x1, z1]]
            k = state["mode"][1]
            corners[k] = [int(wx), int(wz)]
            xs = sorted([c[0] for c in corners]); zs = sorted([c[1] for c in corners])
            a.region = (xs[0], zs[0], xs[3], zs[3])
            touch()
            redraw()

    def on_wheel(ev):
        v = state["view"]
        w0x, w0z = v.screen_to_world(ev.x, ev.y)
        f = 1.15 if ev.delta > 0 else 1 / 1.15
        v.scale *= f
        # keep the world point under the cursor
        v.ox = ev.x - (w0x - v.cx) * v.scale
        v.oy = ev.y - (w0z - v.cz) * v.scale
        redraw()

    def on_key(ev):
        a = cur()
        k = ev.keysym.lower()
        if k in ("q", "escape"):
            root.destroy()
        elif k == "n":
            n = a.internal + "_copy"
            arenas.append(a.clone(n))
            arenas[-1].path = os.path.join(os.path.dirname(a.path or "."), n + ".cca")
            state["idx"] = len(arenas) - 1
            redraw()
        elif k == "d" and 0 <= state["sel"] < len(a.spawns):
            del a.spawns[state["sel"]]
            state["sel"] = -1
            touch()
            redraw()
        elif k == "r":
            a.region = None if a.region else (-2000, -2000, 2000, 2000)
            touch()
            redraw()
        elif k == "l":
            # force a reload from disk, discarding local edits
            if a.path:
                reload_one(state["idx"])
                state["status"] = "reloaded '%s' from disk" % a.internal
            redraw()
        elif k in ("s",) or (ev.state & 0x4 and k == "s"):
            if a.path:
                save_arena(a)
                print("saved", a.path)
            redraw()
        elif k == "tab":
            state["idx"] = (state["idx"] + (-1 if ev.state & 0x1 else 1)) % len(arenas)
            state["sel"] = -1
            redraw()

    def reload_one(i):
        a = arenas[i]
        if not a.path:
            return
        fresh, _ = load_arena(a.path)
        a.__dict__.update(fresh.__dict__)

    def poll():
        # the other side of the loop: the game saves the .cca, so pick it up here
        for i, a in enumerate(arenas):
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
                state["status"] = ("'%s' changed on disk - press L to reload "
                                   "(you have unsaved edits)" % a.internal)
            else:
                reload_one(i)
                state["status"] = "reloaded '%s' from disk" % a.internal
                if i == state["idx"]:
                    redraw()
        root.after(900, poll)

    canvas.bind("<ButtonPress>", on_press)
    canvas.bind("<B1-Motion>", on_drag)
    canvas.bind("<B3-Motion>", on_drag)
    canvas.bind("<Motion>", lambda e: None)
    canvas.bind("<MouseWheel>", on_wheel)
    root.bind("<Key>", on_key)
    root.bind("<Configure>", lambda e: (fit(), redraw()))

    fit()
    redraw()
    root.after(900, poll)
    root.mainloop()
    return 0


# ---------------------------------------------------------------------------
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
    ap.add_argument("--render", metavar="OUT.png", help="render headlessly and exit")
    ap.add_argument("--check", action="store_true", help="validate and print, do not open a window")
    ap.add_argument("--json", action="store_true", help="print the parsed arenas as JSON")
    args = ap.parse_args(argv)

    arena_dir = args.dir or _default_arena_dir()
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

    if not arenas:
        print("No arena files found.")
        print("  make one:   python arenaedit.py --new chicago_docks --city CHICAGO")
        print("  or drop .cca files in: %s" % arena_dir)
        return 0

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
        from PIL import Image
        bg = Image.open(args.map).convert("RGB")
        if bg_rect is None:
            bg_rect = _bounds(arenas)

    if args.render:
        render_png(arenas, args.render, bg, bg_rect)
        print("wrote", args.render)
        return 0

    return run_editor(arenas, bg, bg_rect)


if __name__ == "__main__":
    sys.exit(main())
