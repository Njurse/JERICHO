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
    python arenaedit.py --rip CHICAGO                # export a city's level model
    python arenaedit.py --rip                        # ... or all four cities
    python arenaedit.py myarena.cca --level RIO       # the city, textured, underneath
    python arenaedit.py --level RIO --style points   # ... or the fast vertex cloud

The view is in WORLD units (the units the .cca stores and the game uses), so no
calibration is needed to place things. A background is optional and purely
decorative. Give it as an IMAGE stretched over a world rectangle (--map
--map-world), or as a level .obj from DriverLevelTool (--obj), whose own
bounding box is stretched onto --obj-world (or the centered cell grid from
--cells). Best is a city's own rip (`--level CITY`), which is aligned to the
game's world coordinates for you: by default it is a real textured top-down
RENDER of the level's faces, cached beside the .obj (`--style points` gives the
fast, texture-free vertex cloud instead). Without a background you get a
coordinate grid.

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


def _repo_root_from(start):
    """The repo root at or above `start`, or None.

    `start` may be .../JERICHO/MODS/cainescrossfire/tools (the repo copy) or
    .../src_rebuild/bin/<cfg>/JERICHO/MODS/.../tools (the build's copy), so walk
    up looking for the folder that holds BOTH the repo mod tree and src_rebuild.
    """
    d = os.path.abspath(start)
    for _ in range(12):
        if (os.path.isdir(os.path.join(d, "JERICHO", "MODS", "cainescrossfire")) and
                os.path.isdir(os.path.join(d, "src_rebuild"))):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def repo_root():
    """The repo root, whichever copy of this script is running."""
    here = os.path.dirname(os.path.abspath(__file__))
    return _repo_root_from(here) or os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.dirname(here))))


def _stale_mirror_note():
    """A warning line when this is the build's COPY of the script and the repo
    copy has moved on - the trap that makes a fix 'not work'."""
    here = os.path.abspath(__file__)
    root = _repo_root_from(os.path.dirname(here))
    if not root:
        return None
    repo_copy = os.path.join(root, "JERICHO", "MODS", "cainescrossfire", "tools",
                             "arenaedit.py")
    if os.path.abspath(repo_copy) == here:
        return None
    try:
        with open(repo_copy, "rb") as f1, open(here, "rb") as f2:
            if f1.read() == f2.read():
                return None
    except OSError:
        return None
    return repo_copy


def _default_arena_dir():
    """Where arenas live: the mod's own arena folder.

    This script normally sits in <repo>/JERICHO/MODS/cainescrossfire/tools/, but
    the Windows build also copies the whole mod into the game tree
    (<repo>/src_rebuild/bin/<cfg>/JERICHO/MODS/...). Running THAT copy used to
    write arenas into the mirror - where a dev-build game never looks, because
    the game resolves its arena folder to the repo copy (CD2_ARENA_DIR). So
    resolve it the same way the game does: the repo folder when there is one.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    local = os.path.normpath(os.path.join(here, "..", "arenas"))

    root = _repo_root_from(here)
    if root:
        cand = os.path.join(root, "JERICHO", "MODS", "cainescrossfire", "arenas")
        if os.path.isdir(cand):
            return cand

    return local


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


def level_rip_paths(city):
    """(obj, png, json) for a city's rip in DriverLevelTool/ - any may be missing."""
    base = os.path.join(repo_root(), "DriverLevelTool", "%s_LEVELMODEL" % city.upper())
    return base + ".obj", base + ".topdown.png", base + ".topdown.json"


def driverleveltool_dir():
    return os.path.join(repo_root(), "DriverLevelTool")


def find_tool_exe():
    """DriverLevelTool.exe, or None. It is the only way to make a rip."""
    p = os.path.join(driverleveltool_dir(), "DriverLevelTool.exe")
    return p if os.path.exists(p) else None


def _cfg_order():
    """Build configs, the one the game exe lives in first."""
    order = []
    exe = _find_game_exe()
    if exe:
        order.append(os.path.basename(os.path.dirname(exe)))
    for c in ("Release_dev", "Release", "Debug"):
        if c not in order:
            order.append(c)
    return order


def find_city_lev(city):
    """The `.LEV` to rip for `city`, or None.

    RIO was ripped from DRIVER2/LEVELS/RIO.LEV (the full single-player city, not
    the small mp map), so look there - in the config the game runs from first,
    then the others, then beside the tool (where RIO.LEV sits).
    """
    city = city.upper()
    root = repo_root()
    cands = [os.path.join(driverleveltool_dir(), "%s.LEV" % city)]
    for cfg in _cfg_order():
        cands.append(os.path.join(root, "src_rebuild", "bin", cfg, "DRIVER2",
                                  "LEVELS", "%s.LEV" % city))
    cands.append(os.path.join(root, "src_rebuild", "bin", "Release_dev",
                              "DRIVER2 original - Copy", "LEVELS", "%s.LEV" % city))
    for p in cands:
        if os.path.exists(p):
            return p
    return None


def rip_level(city, force=False, verbose=True, cancel=None):
    """Export a city's level model with DriverLevelTool, so the editor can draw it.

    `DriverLevelTool.exe <CITY>.LEV -world 1 -textures 1`, run in
    DriverLevelTool/ with the `.LEV` copied in beside it (that is how RIO was
    done). The tool writes `<CITY>_LEVELMODEL.obj`, a `<CITY>_textures/` folder
    of pages and a `.mtl`. Returns the `.obj` path, or None with the reason
    printed. Already-ripped cities are skipped unless `force`.

    Traps: this build REJECTS `-format` (it auto-detects the LEV); and
    `DriverLevelTool/` is gitignored, so a rip is a local artifact - that is why
    only RIO exists until someone runs this.

    `cancel` is an optional threading.Event; while the tool runs it is polled,
    and on cancel the child is terminated.
    """
    import shutil
    import subprocess

    city = city.upper()
    obj, _png, _side = level_rip_paths(city)

    if os.path.exists(obj) and not force:
        return obj

    exe = find_tool_exe()
    if not exe:
        if verbose:
            print("cannot rip %s: no DriverLevelTool.exe in %s"
                  % (city, driverleveltool_dir()))
            print("  (put the tool there, or use --obj/--map with a picture you have)")
        return None

    lev = find_city_lev(city)
    if not lev:
        if verbose:
            print("cannot rip %s: no %s.LEV (looked in DriverLevelTool/ and"
                  " src_rebuild/bin/*/DRIVER2/LEVELS/)" % (city, city))
            print("  build the game first, or copy a %s.LEV next to the tool" % city)
        return None

    work = driverleveltool_dir()
    local_lev = os.path.join(work, "%s.LEV" % city)
    if os.path.abspath(lev) != os.path.abspath(local_lev):
        try:
            shutil.copyfile(lev, local_lev)
        except OSError as e:
            if verbose:
                print("cannot copy %s into %s: %s" % (lev, work, e))
            return None

    argv = [exe, os.path.basename(local_lev), "-world", "1", "-textures", "1"]
    if verbose:
        print("ripping %s: %s" % (city, " ".join(argv)))
        print("  (DriverLevelTool opens a window and takes a few minutes)")

    try:
        proc = subprocess.Popen(argv, cwd=work)
    except OSError as e:
        if verbose:
            print("cannot run DriverLevelTool: %s" % e)
        return None

    while True:
        try:
            rc = proc.wait(timeout=0.25)
            break
        except subprocess.TimeoutExpired:
            if cancel is not None and cancel.is_set():
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                if verbose:
                    print("rip of %s cancelled" % city)
                return None

    if not os.path.exists(obj):
        if verbose:
            print("DriverLevelTool exited %s but wrote no %s"
                  % (rc, os.path.basename(obj)))
        return None

    if verbose:
        print("ripped %s -> %s (%.0f MB)"
              % (city, os.path.basename(obj), os.stat(obj).st_size / 1048576.0))
    return obj


# ---------------------------------------------------------------------------
# the top-down render: the rip's FACES, textured (the `textured` style)
# ---------------------------------------------------------------------------
# The rip carries real geometry - v + vt + f, over a million of them - and an
# .mtl naming a texture page per material. So the background can be the city as
# it looks from above instead of a cloud of its vertices: every triangle is
# filled with its texture's colour, and a height buffer keeps the topmost
# surface, so a roof wins over the street under it. `points` (the old vertex
# cloud) is kept as the fast, texture-free fallback.
GRID_MAX = 64           # the largest triangle bbox (in pixels) the fill handles
ELEM_BUDGET = 1000000   # pixels per vectorised chunk (keeps memory ~100 MB)


def _read_mtl(path):
    """{material name: texture file} from a DriverLevelTool .mtl."""
    pages = {}
    cur = None
    try:
        f = open(path, "r", errors="ignore")
    except OSError:
        return pages
    with f:
        for line in f:
            low = line.strip().lower()
            if low.startswith("newmtl "):
                cur = low.split(None, 1)[1].strip()
            elif low.startswith("map_kd ") and cur is not None:
                pages[cur] = line.strip().split(None, 1)[1].strip()
    return pages


def _scan_obj(obj, cancel=None):
    """One streaming pass over a rip -> (V, UV, TRI, TRIUV, SLOTS, materials, rect).

    The export re-emits every group's vertices, so the OBJ indices are global and
    cumulative - the arrays just grow. Faces are quads (fanned 0-2) or triangles,
    and the last `usemtl` before a face is that face's material.

    The world rect is taken from EVERY vertex. The old code derived it from the
    1-in-`sample` vertices it plotted, so the extent under-covered and the whole
    picture was scaled and shifted off the arena's coordinates.
    """
    import numpy as np
    from array import array

    vx = array("f"); vy = array("f"); vz = array("f")
    uu = array("f"); vv = array("f")
    ti = array("i"); tu = array("i"); ts = array("i")
    materials = ["none"]        # slot 0: an untextured face
    slot = {"none": 0}
    cur = 0
    min_x = min_z = 1e30
    max_x = max_z = -1e30

    with open(obj, "r", errors="ignore") as f:
        for line in f:
            c = line[:2]

            if c == "v ":
                p = line.split()
                try:
                    x = float(p[1]); y = float(p[2]); z = float(p[3])
                except (IndexError, ValueError):
                    x = y = z = 0.0
                vx.append(x); vy.append(y); vz.append(z)
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if z < min_z: min_z = z
                if z > max_z: max_z = z

            elif c == "vt":
                p = line.split()
                try:
                    uu.append(float(p[1])); vv.append(float(p[2]))
                except (IndexError, ValueError):
                    uu.append(0.0); vv.append(0.0)

            elif c == "us":
                parts = line.split(None, 1)
                name = parts[1].strip().lower() if len(parts) > 1 else "none"
                cur = slot.get(name)
                if cur is None:
                    cur = slot[name] = len(materials)
                    materials.append(name)

            elif c == "f ":
                toks = line.split()
                if len(toks) < 4:
                    continue
                try:
                    corners = [(int(t.split("/")[0]),
                                int(t.split("/")[1]) if "/" in t and t.split("/")[1] else 0)
                               for t in toks[1:]]
                except ValueError:
                    continue
                a0, t0 = corners[0]
                for k in range(1, len(corners) - 1):
                    b, tb = corners[k]
                    cc, tc = corners[k + 1]
                    ti.extend((a0 - 1, b - 1, cc - 1))
                    tu.extend((t0 - 1 if t0 else 0,
                               tb - 1 if tb else 0,
                               tc - 1 if tc else 0))
                    ts.append(cur)

            if cancel is not None and cancel.is_set():
                return None

    if not len(vx) or not len(ti):
        return None

    nv = len(vx)
    V = np.empty((nv, 3), dtype=np.float32)
    V[:, 0] = np.frombuffer(vx, dtype=np.float32)
    V[:, 1] = np.frombuffer(vy, dtype=np.float32)
    V[:, 2] = np.frombuffer(vz, dtype=np.float32)

    if len(uu):
        UV = np.empty((len(uu), 2), dtype=np.float32)
        UV[:, 0] = np.frombuffer(uu, dtype=np.float32)
        UV[:, 1] = np.frombuffer(vv, dtype=np.float32)
    else:
        UV = np.zeros((1, 2), dtype=np.float32)

    TRI = np.frombuffer(ti, dtype=np.int32).reshape(-1, 3)
    TRIUV = np.frombuffer(tu, dtype=np.int32).reshape(-1, 3)
    SLOTS = np.frombuffer(ts, dtype=np.int32).reshape(-1)

    # indices are 1-based; a bad one would read garbage, so clamp (np.clip
    # returns a writable copy, which the read-only frombuffer views are not)
    TRI = np.clip(TRI, 0, len(V) - 1)
    TRIUV = np.clip(TRIUV, 0, max(0, len(UV) - 1))

    # the model -> world frame (X mirrored) over the TRUE extent
    rect = (-LEVEL_SCALE * max_x, LEVEL_SCALE * min_z,
            -LEVEL_SCALE * min_x, LEVEL_SCALE * max_z)
    return V, UV, TRI, TRIUV, SLOTS, materials, rect


def _build_atlas(material_pages, obj_dir):
    """Stack every referenced page into one padded RGBA array for a single gather.

    Returns (flat, base, widths, heights, wmax, npages, loaded, missing). Slot 0
    (an untextured face) is a flat grey, so "no texture" still draws something.
    """
    import numpy as np
    from PIL import Image

    n = len(material_pages)
    loaded = 0
    missing = []
    imgs = []

    for i, path in enumerate(material_pages):
        im = None
        if i > 0 and path:
            cand = path if os.path.isabs(path) else os.path.join(obj_dir, path)
            for c in (cand, os.path.join(obj_dir, os.path.basename(path))):
                if os.path.exists(c):
                    try:
                        im = Image.open(c).convert("RGBA")
                        loaded += 1
                        break
                    except Exception:
                        im = None
            if im is None:
                missing.append(path)
        if im is None:
            im = Image.new("RGBA", (4, 4), (150, 150, 150, 255))
        imgs.append(np.asarray(im, dtype=np.uint8))

    wmax = max(a.shape[1] for a in imgs)
    hmax = max(a.shape[0] for a in imgs)
    atlas = np.zeros((n, hmax, wmax, 4), dtype=np.uint8)
    base = np.zeros(n, dtype=np.int64)
    widths = np.zeros(n, dtype=np.int64)
    heights = np.zeros(n, dtype=np.int64)

    for i, a in enumerate(imgs):
        h, w = a.shape[0], a.shape[1]
        atlas[i, :h, :w] = a
        base[i] = i * hmax * wmax
        widths[i] = w
        heights[i] = h

    return (atlas.reshape(-1, 4), base, widths, heights, wmax,
            n, loaded, missing)


def _split_big(PX, PY, PH, PU, PV, SLOTS, limit, rounds=8):
    """Subdivide triangles whose screen bbox is bigger than `limit`.

    A triangle larger than the fill's grid was previously SKIPPED, which punched
    holes in the picture wherever the level had a big polygon (the fill reported
    them as `faces_skipped`). Splitting at the edge midpoints keeps every
    barycentric attribute exact - a midpoint's value is the mean of its ends - so
    the pieces fill normally and the image has no holes.
    """
    import numpy as np

    for _ in range(rounds):
        w = np.maximum(np.maximum(PX[:, 0], PX[:, 1]), PX[:, 2]) \
            - np.minimum(np.minimum(PX[:, 0], PX[:, 1]), PX[:, 2])
        h = np.maximum(np.maximum(PY[:, 0], PY[:, 1]), PY[:, 2]) \
            - np.minimum(np.minimum(PY[:, 0], PY[:, 1]), PY[:, 2])
        big = np.maximum(w, h) > limit
        if not big.any():
            break

        idx = np.nonzero(big)[0]
        small = np.nonzero(~big)[0]

        def expand(P):
            m01 = 0.5 * (P[:, 0] + P[:, 1])
            m12 = 0.5 * (P[:, 1] + P[:, 2])
            m20 = 0.5 * (P[:, 2] + P[:, 0])
            kids = np.stack([P[:, 0], m01, m20,
                             m01, P[:, 1], m12,
                             m20, m12, P[:, 2],
                             m01, m12, m20], axis=1)
            kids = kids.reshape(len(P), 4, 3)[idx].reshape(-1, 3)
            return np.concatenate([P[small], kids], axis=0)

        PX = expand(PX); PY = expand(PY); PH = expand(PH)
        PU = expand(PU); PV = expand(PV)
        SLOTS = np.concatenate([SLOTS[small], np.repeat(SLOTS[idx], 4)])

    return PX, PY, PH, PU, PV, SLOTS


def _build_textured(city, obj, size, sample, verbose, uv_flip=False,
                    progress=None, cancel=None):
    """The textured top-down render. Returns (PIL image, rect, stats)."""
    import numpy as np
    from PIL import Image

    if verbose:
        print("  reading %s (%.0f MB, this is the slow part) ..."
              % (os.path.basename(obj), os.path.getsize(obj) / 1048576.0))

    scan = _scan_obj(obj, cancel=cancel)
    if scan is None:
        return None, None, {}

    V, UV, TRI, TRIUV, SLOTS, materials, rect = scan
    ntri = len(TRI)

    if sample and sample > 1:
        keep = np.arange(0, ntri, sample)
        TRI = TRI[keep]; TRIUV = TRIUV[keep]; SLOTS = SLOTS[keep]
        ntri = len(TRI)

    # --- the texture pages -------------------------------------------------
    pages = _read_mtl(os.path.splitext(obj)[0] + ".mtl")
    material_pages = [pages.get(m) for m in materials]
    flat, base, tw, th, wmax, npages, loaded, missing = _build_atlas(
        material_pages, os.path.dirname(obj))

    if verbose:
        print("  %s: %d triangles, %d texture pages (%d loaded)"
              % (city.upper(), ntri, npages - 1, loaded))
        if missing:
            print("  (no page for: %s)" % ", ".join(missing[:4]))

    W, H = size

    # screen space (x right, y down) and the height the z-buffer sorts on
    corners = V[TRI].astype(np.float64)
    x0, z0, x1, z1 = rect
    sx = W / float(x1 - x0)
    sz = H / float(z1 - z0)

    PX = ((-LEVEL_SCALE * corners[:, :, 0] - x0) * sx).astype(np.float32)
    PY = ((LEVEL_SCALE * corners[:, :, 2] - z0) * sz).astype(np.float32)
    PH = (LEVEL_SCALE * corners[:, :, 1]).astype(np.float32)
    PU = np.stack([UV[TRIUV[:, 0], 0], UV[TRIUV[:, 1], 0],
                   UV[TRIUV[:, 2], 0]], axis=1).astype(np.float32)
    PV = np.stack([UV[TRIUV[:, 0], 1], UV[TRIUV[:, 1], 1],
                   UV[TRIUV[:, 2], 1]], axis=1).astype(np.float32)

    # --- orientation -------------------------------------------------------
    # A face's signed area in screen space and in (u, v) must have the SAME sign
    # if its texture is not mirrored when seen from above. That is independent of
    # the model's winding convention, so it is a measurement, not a guess: the
    # export negates X to land on the world frame, and mirroring the model
    # mirrors u with it. `--uv-flip` is the escape hatch for a rip that disagrees
    # (it is the same correction, a quarter-turn apart).
    area_s = (PX[:, 1] - PX[:, 0]) * (PY[:, 2] - PY[:, 0]) \
        - (PX[:, 2] - PX[:, 0]) * (PY[:, 1] - PY[:, 0])
    area_t = (PU[:, 1] - PU[:, 0]) * (PV[:, 2] - PV[:, 0]) \
        - (PU[:, 2] - PU[:, 0]) * (PV[:, 1] - PV[:, 0])
    live = (area_s != 0.0) & (area_t != 0.0)
    # A raw majority over every face is noisy: the level is mostly sub-pixel
    # slivers, where a sign is meaningless (that reads ~0.35). Restricting to
    # faces with a well-determined area on screen AND in uv space gives a
    # decisive verdict (0.95-1.00 across all four cities) - so this measures
    # rather than guesses. Widen the net only if too few faces qualify.
    sel = (np.abs(area_s) > 16.0) & (np.abs(area_t) > 0.02)
    if int(sel.sum()) < 32:
        sel = (np.abs(area_s) > 1.0) & (np.abs(area_t) > 1e-3)
    if not sel.any():
        sel = live
    njudged = int(sel.sum())
    agree = float(np.mean((area_s[sel] > 0) == (area_t[sel] > 0))) if njudged else 1.0
    mirrored_u = agree < 0.5
    if mirrored_u:
        PU = 1.0 - PU
        agree = 1.0 - agree
    if uv_flip:
        PV = 1.0 - PV

    if verbose:
        print("  uv orientation: %.1f%% of %d faces agree (u %s)"
              % (agree * 100.0, njudged,
                 "mirrored to match" if mirrored_u else "kept"))

    # --- fill --------------------------------------------------------------
    PX, PY, PH, PU, PV, SLOTS = _split_big(
        PX.astype(np.float32), PY.astype(np.float32), PH.astype(np.float32),
        PU.astype(np.float32), PV.astype(np.float32), SLOTS, GRID_MAX)
    pieces = len(PX)

    if verbose:
        print("  filling %d pieces (%.2f px each) ..."
              % (pieces, float(np.maximum(
                  np.maximum(PX[:, 0], PX[:, 1]), PX[:, 2]).mean()
                  - np.minimum(np.minimum(PX[:, 0], PX[:, 1]), PX[:, 2]).mean())))

    cx0 = np.clip(np.floor(np.minimum(np.minimum(PX[:, 0], PX[:, 1]), PX[:, 2])).astype(np.int64), 0, W)
    cy0 = np.clip(np.floor(np.minimum(np.minimum(PY[:, 0], PY[:, 1]), PY[:, 2])).astype(np.int64), 0, H)
    cx1 = np.clip(np.ceil(np.maximum(np.maximum(PX[:, 0], PX[:, 1]), PX[:, 2])).astype(np.int64) + 1, 0, W)
    cy1 = np.clip(np.ceil(np.maximum(np.maximum(PY[:, 0], PY[:, 1]), PY[:, 2])).astype(np.int64) + 1, 0, H)

    vis = (cx1 > cx0) & (cy1 > cy0) & np.isfinite(PX[:, 0]) & np.isfinite(PY[:, 0])
    g = np.maximum(cx1 - cx0, cy1 - cy0)
    vis &= g <= GRID_MAX            # _split_big guarantees this, but be safe
    total = int(vis.sum())

    out = np.zeros(W * H, dtype=np.int64)

    def fill(sub, S):
        ox = np.arange(S, dtype=np.float64)[None, None, :]
        oy = np.arange(S, dtype=np.float64)[None, :, None]
        bx = cx0[sub].astype(np.float64)[:, None, None] + ox
        by = cy0[sub].astype(np.float64)[:, None, None] + oy

        xs = PX[sub].astype(np.float64)[:, :, None, None]
        ys = PY[sub].astype(np.float64)[:, :, None, None]
        x0s, x1s, x2s = xs[:, 0], xs[:, 1], xs[:, 2]
        y0s, y1s, y2s = ys[:, 0], ys[:, 1], ys[:, 2]

        d = (y1s - y2s) * (x0s - x2s) + (x2s - x1s) * (y0s - y2s)
        degen = np.abs(d) < 1e-12
        d = np.where(degen, 1e-12, d)

        # edge functions. The barycentric WEIGHTS are n/d (affine either way, so
        # they interpolate whatever the winding), but INSIDE is where the sign of
        # n matches the sign of d - a level mesh is not uniformly wound, and
        # testing only one sign silently dropped every clockwise face.
        n0 = (y1s - y2s) * (bx - x2s) + (x2s - x1s) * (by - y2s)
        n1 = (y2s - y0s) * (bx - x2s) + (x0s - x2s) * (by - y2s)
        n2 = d - n0 - n1
        sgn = np.where(d < 0.0, -1.0, 1.0)

        # CONSERVATIVE test: include a pixel when the pixel SQUARE touches the
        # triangle, i.e. its centre is up to half a pixel outside an edge. A
        # plain centre test drops every sub-pixel triangle - the level is mostly
        # sub-pixel triangles at this scale, so it left the picture speckled with
        # pinholes. (|n|/|edge| is the centre-to-edge distance.)
        len0 = np.hypot(x1s - x2s, y1s - y2s)
        len1 = np.hypot(x2s - x0s, y2s - y0s)
        len2 = np.hypot(x0s - x1s, y0s - y1s)

        ins = ((n0 * sgn) >= -0.5 * len0)
        ins &= ((n1 * sgn) >= -0.5 * len1)
        ins &= ((n2 * sgn) >= -0.5 * len2)
        ins &= ~degen
        ins &= bx < cx1[sub].astype(np.float64)[:, None, None]
        ins &= by < cy1[sub].astype(np.float64)[:, None, None]
        if not ins.any():
            return 0

        w0 = n0 / d
        w1 = n1 / d
        w2 = n2 / d

        hs = PH[sub].astype(np.float64)[:, :, None, None]
        us = PU[sub].astype(np.float64)[:, :, None, None]
        vs = PV[sub].astype(np.float64)[:, :, None, None]

        hh = w0 * hs[:, 0] + w1 * hs[:, 1] + w2 * hs[:, 2]
        uu = w0 * us[:, 0] + w1 * us[:, 1] + w2 * us[:, 2]
        vv = w0 * vs[:, 0] + w1 * vs[:, 1] + w2 * vs[:, 2]

        sl = SLOTS[sub][:, None, None]
        pw = tw[sl]; ph = th[sl]
        col = np.clip((uu * pw).astype(np.int64), 0, pw - 1)
        row = np.clip((vv * ph).astype(np.int64), 0, ph - 1)
        texel = flat[base[sl] + row * wmax + col]           # (n, S, S, 4)

        ins &= texel[..., 3] > 0
        if not ins.any():
            return 0

        rgb = ((texel[..., 0].astype(np.int64) << 16)
               | (texel[..., 1].astype(np.int64) << 8)
               | texel[..., 2].astype(np.int64))
        # height first in the key, so the topmost sample wins the pixel
        hq = np.clip(np.rint(hh), -8388607, 8388607).astype(np.int64) + 8388608
        key = (hq << 24) | rgb
        flatpx = by.astype(np.int64) * W + bx.astype(np.int64)

        np.maximum.at(out, flatpx[ins], key[ins])
        return int(ins.sum())

    for S in (1, 2, 4, 8, 16, 32, 64):
        if S == 1:
            sel = vis & (g <= 1)
        else:
            sel = vis & (g > S // 2) & (g <= S)
        idx = np.nonzero(sel)[0]
        if not len(idx):
            continue
        step = max(1, ELEM_BUDGET // (S * S))
        for a in range(0, len(idx), step):
            sub = idx[a:a + step]
            fill(sub, S)
            if progress is not None:
                progress(min(total, a + step), total)
            if cancel is not None and cancel.is_set():
                return None, None, {}

    # --- decode ------------------------------------------------------------
    img = np.zeros((H, W, 3), dtype=np.uint8)
    img[:, :] = (26, 24, 30)                 # the void, so holes are visible
    seen = out != 0
    if seen.any():
        rgb = out[seen] & 0xFFFFFF
        img.reshape(-1, 3)[seen] = np.stack(
            ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255), axis=1).astype(np.uint8)

    stats = {"tris": int(ntri), "pieces": int(pieces), "plotted": int(seen.sum()),
             "covered_pct": round(100.0 * float(seen.sum()) / (W * H), 2),
             "uv_agree": round(agree, 4), "u_mirrored": bool(mirrored_u),
             "uv_flip": bool(uv_flip),
             "pages": int(npages - 1), "pages_loaded": int(loaded)}

    return Image.fromarray(img), rect, stats


def _build_points(obj, size, sample, verbose):
    """The old vertex-cloud background: fast, no textures, `points` style."""
    from array import array
    from PIL import Image

    xs = array("f")
    zs = array("f")
    n = 0
    min_x = min_z = 1e30
    max_x = max_z = -1e30

    with open(obj, "r", errors="ignore") as f:
        for line in f:
            if line[:2] != "v ":
                continue

            n += 1
            parts = line.split()

            try:
                x = float(parts[1])
                z = float(parts[3])
            except (IndexError, ValueError):
                continue

            # the extent comes from EVERY vertex, not just the plotted 1-in-N:
            # sampling it under-covered the world rect and shifted the picture off
            # the arena's coordinates
            if x < min_x: min_x = x
            if x > max_x: max_x = x
            if z < min_z: min_z = z
            if z > max_z: max_z = z

            if sample > 1 and (n % sample):
                continue

            xs.append(x)
            zs.append(z)

    if not len(xs):
        return None, None, {}

    # the model -> world frame (X mirrored)
    rect = (-LEVEL_SCALE * max_x, LEVEL_SCALE * min_z,
            -LEVEL_SCALE * min_x, LEVEL_SCALE * max_z)

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
    stats = {"verts": n, "sampled": len(xs), "plotted": kept}
    if verbose:
        print("  %d of %d verts plotted" % (kept, n))
    return img, rect, stats


def build_level_map(city, size=(2000, 2000), sample=None, rebuild=False,
                    allow_build=True, allow_rip=False, verbose=True,
                    style="textured", uv_flip=False, progress=None, cancel=None):
    """The top-down background for a city, from its DriverLevelTool rip.

    Returns (PIL image, world_rect), or (None, None) when there is no rip (or no
    cache and allow_build is off). The picture is CACHED as
    `<CITY>_LEVELMODEL.topdown.png` plus a `.json` sidecar with the world rect and
    the style, so the slow pass over a ~240 MB .obj happens once and later opens
    are instant - and the PNG alone is shareable, since the sidecar carries the
    alignment.

    `style` is "textured" (the rip's faces, filled with their texture - the
    default and what the map "looks like") or "points" (just the vertices, no
    textures, much faster). The cache is keyed on the style, so switching
    rebuilds instead of serving the other one.

    `sample` keeps every Nth item: every Nth vertex for "points" (the cloud is
    millions of points; every 4th is plenty) or every Nth triangle for
    "textured" (default: all of them - dropping triangles leaves holes).

    `uv_flip` flips the texture's V axis for a rip whose UVs disagree with the
    world frame (the render measures and reports that itself).
    """
    import json
    from PIL import Image

    if sample is None:
        sample = 4 if style == "points" else 1

    obj, png, side = level_rip_paths(city)

    def _cached():
        if not (os.path.exists(png) and os.path.exists(side)):
            return None

        try:
            meta = json.load(open(side))
            rect = tuple(meta["rect"])
        except Exception:
            return None

        # a cache in the OTHER style (or the old un-keyed point cloud) is not
        # this; and uv_flip is part of the picture, so it is part of the key
        if meta.get("style", "points") != style:
            if verbose:
                print("level map cache for %s is the %s style - rebuilding as %s"
                      % (city.upper(), meta.get("style", "points"), style))
            return None
        if bool(meta.get("uv_flip", False)) != bool(uv_flip):
            return None

        # invalidate only when the .obj is present AND has changed; a PNG on its
        # own (the obj deleted, or a shared cache) is still good
        if os.path.exists(obj) and not rebuild:
            st = os.stat(obj)

            if meta.get("obj_size") != st.st_size or meta.get("obj_mtime") != int(st.st_mtime):
                if verbose:
                    print("level map cache for %s is stale - rebuilding" % city.upper())
                return None

        img = Image.open(png)
        img.load()
        img = img.convert("RGB")
        return img, rect

    if not rebuild:
        got = _cached()

        if got is not None:
            return got

    # a missing rip is the difference between "RIO works" and "only RIO works":
    # the .obj is a gitignored local artifact, so make it on demand when asked.
    if not os.path.exists(obj) and allow_rip:
        if rip_level(city, verbose=verbose) is None:
            return None, None

    if not os.path.exists(obj):
        if verbose:
            print("no level rip for %s (looked for %s)" % (city.upper(), obj))
            print("  make one:  python arenaedit.py --rip %s" % city.upper())
        return None, None

    if not allow_build:
        if verbose:
            print("no cached level map for %s yet - run with --level %s once to build it"
                  % (city.upper(), city.upper()))
        return None, None

    if verbose:
        print("building the %s level map (%s style, one pass over %s, then cached)"
              % (city.upper(), style, os.path.basename(obj)))

    if style == "points":
        img, rect, stats = _build_points(obj, size, sample, verbose)
        renderer = "points"
    else:
        img, rect, stats = _build_textured(city, obj, size, sample, verbose,
                                           uv_flip=uv_flip, progress=progress,
                                           cancel=cancel)
        renderer = "textured"

        # a rip with no faces (or one whose textures are all missing) still has
        # vertices, so fall back rather than showing nothing. The cache key stays
        # the REQUESTED style, so this is not re-attempted on every open.
        if img is None and not (cancel is not None and cancel.is_set()):
            if verbose:
                print("  no faces to texture - falling back to the vertex cloud")
            img, rect, stats = _build_points(
                obj, size, sample if sample and sample > 1 else 4, verbose)
            renderer = "points (fallback)"

    if img is None:
        if verbose:
            print("could not render %s (cancelled, or a rip with no faces)" % city.upper())
        return None, None

    try:
        img.save(png)
        meta = {"city": city.upper(), "rect": list(rect), "style": style,
                "renderer": renderer, "uv_flip": bool(uv_flip),
                "obj_size": os.stat(obj).st_size,
                "obj_mtime": int(os.stat(obj).st_mtime)}
        meta.update(stats)
        json.dump(meta, open(side, "w"), indent=1)

        if verbose:
            print("cached %s (%s; world rect %s)"
                  % (os.path.basename(png),
                     ", ".join("%s=%s" % (k, stats[k]) for k in sorted(stats)),
                     tuple(round(v) for v in rect)))
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
        self._bg_key = None              # what the cached background tile shows
        self._redraw_job = None          # a coalesced redraw, if one is pending
        self._last_configure = None      # the canvas size of the last <Configure> we acted on
        self._labels = {}                # last text per label, so we only rewrite changes
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

        # Fixed width, or the inspector's text re-flows the layout on every redraw:
        # the canvas resizes -> <Configure> -> redraw -> text changes -> ... which
        # never lets Tk drain and freezes the editor. Content must not size us.
        side = ttk.Frame(body, padding=(5, 4), width=360)
        side.pack(side="right", fill="y")
        side.pack_propagate(False)

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
        # fixed widths for the same reason as the inspector above: variable-length
        # status text must not resize the window under us
        self.lbl_pos = ttk.Label(s, text="", width=30, anchor="e")
        self.lbl_pos.pack(side="right")
        self.lbl_file = ttk.Label(s, text="", width=74, anchor="w")
        self.lbl_file.pack(side="left")
        self.lbl_msg = ttk.Label(s, text="", width=34, anchor="w")
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
        # NOTE: no redraw binding on <Configure>. Drawing can change the window's own
        # layout (the status line carries a long path), so canvas resize -> <Configure>
        # -> redraw -> resize was an endless loop that froze the editor: update() and
        # mainloop never returned. Debouncing it only made the loop slower. Every real
        # action redraws (pan/zoom/fit/tool clicks and the file poll), so the only cost
        # of leaving it out is that a window resize repaints at the next click or Fit.
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
        img, rect = build_level_map(city, rebuild=False, allow_build=True,
                                    allow_rip=True, verbose=False)
        if img is None:
            self._say("no level map for %s - rip it first: python arenaedit.py --rip %s"
                      % (city, city))
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
        self._redraw_job = None
        c = self.canvas
        c.delete("all")
        a = self.cur()
        w = max(1, c.winfo_width())
        h = max(1, c.winfo_height())
        self._drawn_size = (w, h)
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
            self._draw_bg(c, w, h)

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
            if not (-40.0 <= sx <= w + 40.0 and -40.0 <= sy <= h + 40.0):
                continue                       # off screen: cull it
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
            if not (-30.0 <= sx <= w + 30.0 and -30.0 <= sy <= h + 30.0):
                continue                       # off screen: cull it
            col = PICK_HEALTH if p["type"] == "health" else PICK_WEAPON
            if i == self.sel_pick:
                c.create_oval(sx - 10, sy - 10, sx + 10, sy + 10, outline=SEL_COL, width=2)
            c.create_rectangle(sx - 5, sy - 5, sx + 5, sy + 5, fill=col, outline="#000")

        if self.pending_corner is not None:
            sx, sy = self.S(*self.pending_corner)
            c.create_line(sx - 8, sy, sx + 8, sy, fill=REGION_COL, width=2)
            c.create_line(sx, sy - 8, sx, sy + 8, fill=REGION_COL, width=2)

        self._status_line()

    def _redraw_soon(self):
        """Coalesce a burst of motion events into a single redraw.

        Panning and dragging fire many events per frame; redrawing on each one
        was most of the lag.
        """
        if self._redraw_job is None:
            # a TIMER, not after_idle: an idle callback that provokes more work can
            # starve Tk's idle queue, and update() then never returns
            self._redraw_job = self.root.after(16, self.redraw)

    def _draw_bg(self, c, w, h):
        """Draw the level map, but only the part on screen, and only re-rasterise
        it when that part or the window changes.

        Resizing the whole 2000x2000 map on every redraw - which is what happened
        while panning or dragging - is what made the editor crawl. Cropping to the
        visible window first keeps every frame canvas-sized.
        """
        from PIL import Image, ImageTk

        br = self.bg_rect
        p0 = self.S(br[0], br[1])
        p1 = self.S(br[2], br[3])
        sx0, sx1 = min(p0[0], p1[0]), max(p0[0], p1[0])
        sy0, sy1 = min(p0[1], p1[1]), max(p0[1], p1[1])
        span_w = sx1 - sx0
        span_h = sy1 - sy0
        if span_w <= 1 or span_h <= 1:
            return

        # the part of the map that is actually on screen
        vx0 = max(0.0, sx0)
        vy0 = max(0.0, sy0)
        vx1 = min(float(w), sx1)
        vy1 = min(float(h), sy1)
        dst_w = int(vx1 - vx0)
        dst_h = int(vy1 - vy0)
        if dst_w < 1 or dst_h < 1:
            return

        # ...the same window in source pixels
        iw, ih = self.bg.size
        bx0 = max(0, int((vx0 - sx0) * iw / span_w))
        by0 = max(0, int((vy0 - sy0) * ih / span_h))
        bx1 = min(iw, max(bx0 + 1, int((vx1 - sx0) * iw / span_w)))
        by1 = min(ih, max(by0 + 1, int((vy1 - sy0) * ih / span_h)))

        key = (id(self.bg), bx0, by0, bx1, by1, dst_w, dst_h)
        if self._bg_key != key:
            tile = self.bg.crop((bx0, by0, bx1, by1))
            if tile.size != (dst_w, dst_h):
                resample = Image.NEAREST if dst_w < tile.size[0] else Image.BILINEAR
                tile = tile.resize((dst_w, dst_h), resample)
            self.bg_photo = ImageTk.PhotoImage(tile)
            self._bg_key = key
        c.create_image(vx0, vy0, anchor="nw", image=self.bg_photo)

    def _label(self, widget, text):
        """Set a label's text only when it changed - configuring on every mouse
        move was its own small cost."""
        key = str(widget)
        if self._labels.get(key) != text:
            self._labels[key] = text
            widget.configure(text=text)

    def _status_line(self):
        a = self.cur()
        what = a.path or "<unsaved: press Save>"
        star = " *unsaved*" if a.dirty else ""
        if a.region:
            where = "region %d,%d..%d,%d" % a.region
        else:
            where = "no region"
        self._label(self.lbl_file,
                    "%s%s   |   %s   |   %s   |   %d spawn(s), %d pickup(s)"
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
            self._redraw_soon()
        elif kind == "corner" and a.region:
            x0, z0, x1, z1 = a.region
            corners = [[x0, z0], [x1, z0], [x0, z1], [x1, z1]]
            corners[k] = [int(wx), int(wz)]
            xs = sorted(c[0] for c in corners)
            zs = sorted(c[1] for c in corners)
            a.region = (xs[0], zs[0], xs[3], zs[3])
            a.dirty = True
            self._redraw_soon()

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
        self._redraw_soon()

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
        self._label(self.lbl_pos, "cursor  %d , %d" % (int(wx), int(wz)))

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

    # --- the background is the expensive part: culled + cached ---------------
    try:
        import time
        from PIL import Image

        big = Image.new("RGB", (2000, 2000), (35, 35, 40))
        for k in range(0, 2000, 40):        # some texture, so it is not a flat block
            big.paste((130, 130, 130), (k, 0, k + 18, 2000))

        # (a) a map about the size of the view: the old code resized the WHOLE
        #     image on every redraw, which is what made panning crawl
        cx0, cz0 = cur.spawns[0][0], cur.spawns[0][1]
        app._set_bg(big, (cx0 - 4000, cz0 - 4000, cx0 + 4000, cz0 + 4000), "test map")
        root.update()
        n = 25
        t0 = time.perf_counter()
        for _ in range(n):
            app.view.ox += 4                # pan a little, so it must re-crop
            app.redraw()
        ms = (time.perf_counter() - t0) * 1000.0 / n
        print("  %-40s %.1f ms/frame" % ("pan + redraw, map ~= view", ms))
        check("pan+redraw under 100 ms/frame", ms < 100.0)

        # (b) a map far bigger than the view: the old code silently SKIPPED it
        #     (its guard refused anything over 40M pixels)
        app._set_bg(big, (-300000, -300000, 300000, 300000), "huge map")
        root.update()
        app.redraw()
        check("a huge map still draws (culled, not skipped)", app.bg_photo is not None)

        # (c) nothing moved -> the tile is reused, not rebuilt
        k1 = app._bg_key
        app.redraw()
        check("the background tile is cached", app._bg_key == k1)
        app.clear_bg()
        root.update()
    except ImportError:
        print("  (no Pillow: skipped the background timing checks)")

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
    return repo_root()


def _find_game_exe():
    root = _repo_root()
    for cfg, name in (("Release_dev", "REDRIVER2_dev.exe"),
                      ("Release", "REDRIVER2.exe")):
        p = os.path.join(root, "src_rebuild", "bin", cfg, name)
        if os.path.exists(p):
            return p
    return None


def rip_main(token):
    """`--rip [CITY]` - export the level model(s) with DriverLevelTool, then exit.

    0 when every requested city has a rip afterwards, 2 otherwise. Already-ripped
    cities are skipped (that is why RIO is quick here).
    """
    if token == "auto":
        cities = sorted(CITIES.values())
    else:
        c = _city_from_token(token)
        if c is None:
            print("unknown city '%s' - pick one of: %s"
                  % (token, ", ".join(sorted(CITIES.values()))))
            return 2
        cities = [CITY_NAMES[c]]

    if not find_tool_exe():
        print("cannot rip: no DriverLevelTool.exe in %s" % driverleveltool_dir())
        print("  the tool (and the rips it makes) are gitignored, so neither is")
        print("  in the checkout - drop DriverLevelTool.exe there first")
        return 2

    print("ripping %d city(ies): %s" % (len(cities), ", ".join(cities)))
    print("  DriverLevelTool is slow (minutes each) and writes ~185 MB per city")
    print()

    rc = 0
    for city in cities:
        if rip_level(city) is None:
            rc = 2
        print()

    return rc


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

    tool = find_tool_exe()
    print("  DriverLevelTool: %s" % (tool if tool else
          "not found in %s" % driverleveltool_dir()))
    if not tool:
        print("                needed to rip a city; the tool and its rips are gitignored")

    for city in sorted(CITIES.values()):
        obj, png, side = level_rip_paths(city)
        if os.path.exists(png):
            have = "cached map"
        elif os.path.exists(obj):
            have = "ripped, no map yet (--level %s builds it)" % city
        elif tool and find_city_lev(city):
            have = "no rip - run: --rip %s" % city
        else:
            have = "no rip (no %s.LEV to make one from)" % city
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
    ap.add_argument("--style", choices=("textured", "points"), default="textured",
                    help="how to draw a level map: 'textured' (the default) fills the rip's "
                         "faces with their textures - the city as it looks from above; "
                         "'points' is the fast, texture-free vertex cloud")
    ap.add_argument("--rip", nargs="?", const="auto", metavar="CITY",
                    help="export the city's level model with DriverLevelTool so it can be drawn "
                         "(omit CITY to rip every city), then exit. Slow, and local: the .obj is "
                         "gitignored, which is why only RIO ships ripped")
    ap.add_argument("--no-rip", action="store_true",
                    help="with --level: never run DriverLevelTool; fail if that city has no rip")
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

    _stale = _stale_mirror_note()
    if _stale:
        print("note: this is the build's COPY of arenaedit.py and the repo copy differs.")
        print("      run the repo one instead:  py -3 \"%s\"" % _stale)
        print()

    arena_dir = args.dir or _default_arena_dir()

    if args.selftest:
        return selftest(arena_dir)

    if args.rip:
        return rip_main(args.rip)

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
                                        allow_rip=bool(args.level) and not args.no_rip,
                                        style=args.style,
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
