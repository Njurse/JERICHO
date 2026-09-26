#!/usr/bin/env python3
"""arenaedit.py - the Caine's Crossfire ARENA editor, top-down.

Edits the `.cca` arena files (see ../ARENAS.md) - the region (the barrier) and
the spawn points (position + heading) - in a top-down view. It reads and writes
exactly the format the game reads, so it and the in-game editor (arenas/arena.c)
are two views of one file: what one saves, the other loads.

    python arenaedit.py arenas/*.cca                 # open the editor
    python arenaedit.py chicago.cca --map city.png \
        --map-world -450000 -580000 450000 580000    # with a background
    python arenaedit.py chicago.cca --render out.png # headless snapshot
    python arenaedit.py chicago.cca --check          # validate, print, exit

The view is in WORLD units (the units the .cca stores and the game uses), so no
calibration is needed to place things. A background IMAGE is optional and purely
decorative - it is stretched over the world rectangle you give it with
--map-world x0 z0 x1 z1. Without one you get a coordinate grid.

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
import os
import sys

SPAWN_MAX = 16
HEADING_MAX = 4096
CITIES = {0: "CHICAGO", 1: "HAVANA", 2: "VEGAS", 3: "RIO"}
CITY_INDEX = {v: k for k, v in CITIES.items()}


# ---------------------------------------------------------------------------
# the data model + the file format (mirrors arenas/arenafile.c)
# ---------------------------------------------------------------------------
class Arena:
    def __init__(self, internal="", display="", city=0, mp_level=1, mp_arena=0,
                 region=None, spawns=None):
        self.internal = internal
        self.display = display
        self.city = city
        self.mp_level = mp_level
        self.mp_arena = mp_arena
        self.region = region            # None, or (x0, z0, x1, z1)
        self.spawns = list(spawns or [])  # list of (x, z, heading)
        self.path = None

    def clone(self, internal):
        return Arena(internal, internal, self.city, self.mp_level, self.mp_arena,
                     self.region, self.spawns)


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
                    a.spawns.append((x, z, h & (HEADING_MAX - 1)))
    if not a.display:
        a.display = a.internal
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
    lines.append("# spawn: x z heading  (first = player, rest = opponents)")
    for (x, z, h) in a.spawns:
        lines.append("spawn: %d %d %d" % (x, z, h))
    with open(a.path, "w") as f:
        f.write("\n".join(lines) + "\n")


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
    for i, (x, z, h) in enumerate(a.spawns):
        if a.region:
            x0, z0, x1, z1 = a.region
            if not (x0 <= x <= x1 and z0 <= z <= z1):
                w.append("spawn %d (%d,%d) is OUTSIDE the region" % (i, x, z))
        if not (0 <= h < HEADING_MAX):
            w.append("spawn %d heading %d out of range" % (i, h))
    if len(a.spawns) == 0:
        w.append("no spawns (the game falls back to its own placement)")
    return w


def describe(a):
    who = "player" if a.spawns else "-"
    opp = max(0, len(a.spawns) - 1)
    reg = "none" if not a.region else "%d,%d..%d,%d" % a.region
    return ("%s (%s) city=%s mp=%d/%d region=%s spawns=%d (%s + %d opp)"
            % (a.internal, a.display, CITIES.get(a.city, a.city), a.mp_level,
               a.mp_arena, reg, len(a.spawns), who, opp))


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
# headless render
# ---------------------------------------------------------------------------
def render_png(arenas, path, map_path=None, map_world=None, size=(1100, 800)):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", size, (18, 18, 22))
    dr = ImageDraw.Draw(img)

    wr = map_world if map_world else _bounds(arenas)
    view = View(wr)
    view.scale = min(size[0] / (wr[2] - wr[0]), size[1] / (wr[3] - wr[1])) * 0.92
    view.ox, view.oy = size[0] / 2.0, size[1] / 2.0

    if map_path and os.path.exists(map_path):
        bg = Image.open(map_path).convert("RGB").resize(size)
        img.paste(bg, (0, 0))

    def S(x, z):
        return view.world_to_screen(x, z)

    for a in arenas:
        color = (90, 160, 255)
        if a.region:
            x0, z0, x1, z1 = a.region
            p0 = S(x0, z0); p1 = S(x1, z1)
            dr.rectangle([p0, p1], outline=(255, 200, 60), width=2)
        for i, (x, z, h) in enumerate(a.spawns):
            sx, sy = S(x, z)
            c = (120, 255, 120) if i == 0 else (255, 120, 120)
            r = 6
            dr.ellipse([sx - r, sy - r, sx + r, sy + r], fill=c, outline=(0, 0, 0))
            dx, dy = _heading_vec(h)
            dr.line([sx, sy, sx + dx * 26, sy + dy * 26], fill=c, width=2)
            dr.text((sx + 8, sy + 8), "%d" % i, fill=(255, 255, 255))
        # a label in the corner
        dr.text((8, 8 + arenas.index(a) * 16),
                "%s  [%s]" % (describe(a), a.path or "<new>"), fill=(230, 230, 230))

    img.save(path)
    return path


# ---------------------------------------------------------------------------
# interactive editor (tkinter)
# ---------------------------------------------------------------------------
def run_editor(arenas, map_path=None, map_world=None):
    import tkinter as tk
    from PIL import Image, ImageTk

    if not arenas:
        print("no arena files given")
        return 1

    state = {"idx": 0, "sel": -1, "mode": None, "bg": None, "bgimg": None}

    root = tk.Tk()
    root.title("Caine's Crossfire arena editor")
    canvas = tk.Canvas(root, width=1100, height=760, background="#121216",
                       highlightthickness=0)
    canvas.pack(fill="both", expand=True)

    def cur():
        return arenas[state["idx"]]

    wr = map_world if map_world else _bounds(arenas)
    view = View(wr)
    state["view"] = view

    def fit():
        v = state["view"]
        w = max(1, canvas.winfo_width())
        h = max(1, canvas.winfo_height())
        r = map_world if map_world else _bounds(arenas)
        v.wx0, v.wz0, v.wx1, v.wz1 = r
        v.cx = (r[0] + r[2]) / 2.0
        v.cz = (r[1] + r[3]) / 2.0
        v.scale = min(w / (r[2] - r[0]), h / (r[3] - r[1])) * 0.92
        v.ox, v.oy = w / 2.0, h / 2.0
        if map_path and state["bg"] is None:
            try:
                state["bg"] = Image.open(map_path).convert("RGB")
            except Exception:
                state["bg"] = False

    def S(x, z):
        return state["view"].world_to_screen(x, z)

    def redraw():
        canvas.delete("all")
        v = state["view"]
        a = cur()
        if state["bg"]:
            w = max(1, canvas.winfo_width()); h = max(1, canvas.winfo_height())
            state["bgimg"] = ImageTk.PhotoImage(state["bg"].resize((w, h)))
            canvas.create_image(0, 0, anchor="nw", image=state["bgimg"])

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
        for i, (x, z, h) in enumerate(a.spawns):
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
            "%s   [%s]\nfile %d/%d   selected spawn %d\n"
            "n new  d delete  r region  s save  Tab file  q quit"
            % (describe(a), a.path or "<new>", state["idx"] + 1, len(arenas),
               state["sel"])))

    def pick_spawn(sx, sy):
        best, bestd = -1, 18.0
        for i, (x, z, h) in enumerate(cur().spawns):
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
                a.spawns.append((int(wx), int(wz), 0))
                state["sel"] = len(a.spawns) - 1
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
            x, z, h = a.spawns[state["sel"]]
            a.spawns[state["sel"]] = (int(wx), int(wz), h)
            redraw()
        elif isinstance(state["mode"], tuple) and a.region:
            x0, z0, x1, z1 = a.region
            corners = [[x0, z0], [x1, z0], [x0, z1], [x1, z1]]
            k = state["mode"][1]
            corners[k] = [int(wx), int(wz)]
            xs = sorted([c[0] for c in corners]); zs = sorted([c[1] for c in corners])
            a.region = (xs[0], zs[0], xs[3], zs[3])
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
            redraw()
        elif k == "r":
            a.region = None if a.region else (-2000, -2000, 2000, 2000)
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

    canvas.bind("<ButtonPress>", on_press)
    canvas.bind("<B1-Motion>", on_drag)
    canvas.bind("<B3-Motion>", on_drag)
    canvas.bind("<Motion>", lambda e: None)
    canvas.bind("<MouseWheel>", on_wheel)
    root.bind("<Key>", on_key)
    root.bind("<Configure>", lambda e: (fit(), redraw()))

    fit()
    redraw()
    root.mainloop()
    return 0


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Caine's Crossfire arena editor (top-down).")
    ap.add_argument("files", nargs="+", help="one or more .cca arena files")
    ap.add_argument("--map", help="background image (PNG) stretched over --map-world")
    ap.add_argument("--map-world", nargs=4, type=int, metavar=("X0", "Z0", "X1", "Z1"),
                    help="the world rectangle the background image covers")
    ap.add_argument("--render", metavar="OUT.png", help="render headlessly and exit")
    ap.add_argument("--check", action="store_true", help="validate and print, do not open a window")
    ap.add_argument("--json", action="store_true", help="print the parsed arenas as JSON")
    args = ap.parse_args(argv)

    arenas = []
    for path in args.files:
        if not os.path.exists(path):
            print("skip (missing):", path)
            continue
        a, saw = load_arena(path)
        if not saw:
            print("skip (no arena: line):", path)
            continue
        arenas.append(a)

    if args.json:
        import json
        print(json.dumps([{
            "path": a.path, "internal": a.internal, "display": a.display,
            "city": a.city, "mp_level": a.mp_level, "mp_arena": a.mp_arena,
            "region": a.region, "spawns": a.spawns,
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

    if args.render:
        mw = tuple(args.map_world) if args.map_world else None
        render_png(arenas, args.render, args.map, mw)
        print("wrote", args.render)
        return 0

    mw = tuple(args.map_world) if args.map_world else None
    return run_editor(arenas, args.map, mw)


if __name__ == "__main__":
    sys.exit(main())
