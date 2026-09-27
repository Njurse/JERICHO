#!/usr/bin/env python3
"""paletteedit.py - a live palette editor for Caine's Crossfire car colours.

Why this exists
---------------
Which CLUT entry paints which part of a car is not something you can read off the data:
a car model's polys each carry their own clut reference, the palette rows are SHARED, and
which entries a part samples depends on the texture pixels it covers. The fast answer is
to change an entry and look at the car.

So this is a two-way tool:

  * it READS  JERICHO/CONFIG/cc_palette_map.txt - written live by the running game
    (palette/paletteedit.c). That gives every live car's (row, texid, palette) -> CLUT id,
    VRAM address, how many of the model's polys draw from it, and all 16 entries in RGB.

  * it WRITES JERICHO/CONFIG/cc_palette_overrides.txt - which the game re-reads while you
    play, every 15 frames, and applies to those CLUT rows in place. In place means no new
    VRAM is allocated at all, so it works with the CLUT strip completely full (it is).

Usage
-----
    python paletteedit.py                     # read the mod's CONFIG folder in the dev build
    python paletteedit.py --bin <path>        # ... of another build (i.e. where JERICHO/ is)
    python paletteedit.py --map mymap.txt --out myover.txt
    python paletteedit.py --launch            # start the game (with the map dump enabled)
    python paletteedit.py --topmost           # float above the game
    python paletteedit.py --offline           # no game: just show/edit a saved map

The workflow it is built for: pick a car, pick a CLUT row, then SOLO or FLASH an entry to
find out what it paints, and drag the wheel until it is the colour you want. Then note the
(row, texid, palette, entry, colour) - that is what an authored car colour is built from.
See PALETTES.md.

In-place edits are GLOBAL to the row: every car that resolves to the same CLUT id changes
with it. Good for finding out what a row paints, not what you want for one car's paint -
that is the per-instance dye, a different mechanism.
"""

import argparse
import colorsys
import os
import re
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ---------------------------------------------------------------------------
# paths
# ---------------------------------------------------------------------------

HERE = os.path.dirname(os.path.abspath(__file__))
# .../JERICHO/MODS/cainescrossfire/tools -> the repo root is five levels up
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
DEV_BIN = os.path.join(REPO, "src_rebuild", "bin", "Release_dev")

MAP_NAME = "cc_palette_map.txt"
# NB: must be the file palette/paletteedit.c READS (it re-reads CONFIG/cc_palette.txt every
# 15 frames). Two names here would look right and do nothing.
OVR_NAME = "cc_palette.txt"


def config_dir(bin_dir):
    return os.path.join(bin_dir, "JERICHO", "CONFIG")


# ---------------------------------------------------------------------------
# colours
# ---------------------------------------------------------------------------

def rgb_to_555(r, g, b):
    """The PSX word: stp | b<<10 | g<<5 | r, five bits a channel (blue HIGH)."""
    return ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3)


def rgb555_to_rgb(c):
    return ((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)


def quantise(r, g, b):
    """What the PSX will actually store, shown next to the pick."""
    return rgb555_to_rgb(rgb_to_555(r, g, b))


def hex6(r, g, b):
    return "#%02x%02x%02x" % (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))


# ---------------------------------------------------------------------------
# the map file
# ---------------------------------------------------------------------------

class ClutRow:
    """One (carid, texid, palette) -> one CLUT row of 16 entries.

    `carid` is the civ_clut ROW the model's polys were baked for - NOT the car's slot.
    Those are different numberings (the row comes from which car page the model's polys
    name), so rows are attached to the car they were dumped under, by order.
    """

    def __init__(self, carid, texid, palette, clut, vx, vy, polys, entries, car_index=-1, ymin=0, ymax=0):
        self.carid = carid
        self.texid = texid
        self.palette = palette
        self.clut = clut
        self.vx = vx
        self.vy = vy
        self.polys = polys
        self.entries = entries          # list of 16 (r,g,b)
        self.car_index = car_index      # which of PalMap.cars this row was dumped under
        self.ymin = ymin                # model-space Y extent: where on the car this part sits
        self.ymax = ymax

    @property
    def key(self):
        return (self.carid, self.texid, self.palette)

    def label(self):
        where = "clut %04x @(%d,%d)" % (self.clut, self.vx, self.vy) if self.clut else "offline"
        height = "y %d..%d" % (self.ymin, self.ymax) if (self.ymin or self.ymax) else "y ?"
        return "row %d  texid %d  palette %d   %s   %s   %d poly(s)" % (
            self.carid, self.texid, self.palette, where, height, self.polys)


class PalMap:
    def __init__(self):
        self.cars = []                  # list of dicts: slot, model, palette, role
        self.rows = []                  # list of ClutRow
        self.stamp = None

    @staticmethod
    def parse(path):
        m = PalMap()
        ncar = -1
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if parts[0] == "car" and len(parts) >= 5:
                    m.cars.append({"slot": int(parts[1]), "model": int(parts[2]),
                                   "palette": int(parts[3]), "role": parts[4]})
                    ncar = len(m.cars) - 1
                elif parts[0] == "clut" and len(parts) >= 10:
                    carid, texid, palette = int(parts[1]), int(parts[2]), int(parts[3])
                    clut = int(parts[4], 16)
                    vx, vy, polys = int(parts[5]), int(parts[6]), int(parts[7])
                    # <ymin> <ymax> follow the poly count - model-space Y, the part's height
                    ymin, ymax = int(parts[8]), int(parts[9])
                    nums = [int(x) for x in parts[10:]]
                    entries = []
                    for i in range(0, min(len(nums), 48), 3):
                        entries.append((nums[i], nums[i + 1], nums[i + 2]))
                    while len(entries) < 16:
                        entries.append((0, 0, 0))
                    m.rows.append(ClutRow(carid, texid, palette, clut, vx, vy, polys, entries[:16],
                                          ncar, ymin, ymax))
        return m


def offline_level_map(level_path, city):
    """Build a map from a level's own palette LUMP_PALLET, with no game running.

    Reuses tools/levpalette.py rather than re-parsing the format: it is the tool that
    already owns that reader (and it resolves the `clut_number` back-references for us).

    What an offline map CANNOT have, and why the columns are blank: the CLUT ids and their
    VRAM addresses are assigned when the level loads, and the poly counts come from the
    BUILT model. Neither exists outside a run. What it does have is everything the override
    file is keyed on - (row, texid, palette, entry) and the colours - so you can work out a
    palette offline and carry the lines into a run.
    """
    import levpalette as lp

    with open(level_path, "rb") as f:
        blob = f.read()

    off, size = lp.find_pallet(blob)

    if not off:
        raise ValueError("no LUMP_PALLET in %s" % level_path)

    _, records = lp.parse_pallet(blob, off, size)

    m = PalMap()
    m.cars.append({"slot": -1, "model": -1, "palette": 0, "role": "level (offline)"})

    for palette, texnum, tpage, clut in records:
        carid = lp.carid_of(city, tpage)

        if carid is None or clut is None:
            continue                      # not one of the city's car pages

        entries = [lp.psx_to_rgb(v) for v in clut]

        # The lump's `palette` is the engine's `palette + 1` column - the number the draw
        # reads and the override file uses, so no translation here.
        m.rows.append(ClutRow(carid, texnum, palette + 1, 0, 0, 0, 0, entries, 0))

    return m


# ---------------------------------------------------------------------------
# the override file
# ---------------------------------------------------------------------------

class Overrides:
    """What we want the game to do: a set of per-entry colours, or a probe mode."""

    def __init__(self):
        self.sets = {}                  # (carid, texid, palette, entry) -> (r,g,b)
        self.probe = None               # (mode, carid, texid, palette, entry)

    def set(self, row, texid, palette, entry, rgb):
        self.sets[(row, texid, palette, entry)] = rgb
        self.probe = None

    def solo(self, row, texid, palette, entry):
        self.probe = ("solo", row, texid, palette, entry)

    def flash(self, row, texid, palette, entry):
        self.probe = ("flash", row, texid, palette, entry)

    def clear(self):
        self.sets.clear()
        self.probe = None

    def text(self):
        out = ["# written by tools/paletteedit.py - the game re-reads this while you play",
               "# set   <row> <texid> <palette> <entry> <rrggbb>",
               "# solo  <row> <texid> <palette> <entry>      (everything else black)",
               "# flash <row> <texid> <palette> <entry>",
               ""]
        if self.probe:
            mode, row, texid, palette, entry = self.probe
            out.append("%s %d %d %d %d" % (mode, row, texid, palette, entry))
        for (row, texid, palette, entry), (r, g, b) in sorted(self.sets.items()):
            out.append("set %d %d %d %d %02x%02x%02x" % (row, texid, palette, entry, r, g, b))
        if not self.probe and not self.sets:
            out.append("clear")
        out.append("")
        return "\n".join(out)


# ---------------------------------------------------------------------------
# the wheel
# ---------------------------------------------------------------------------

class ColourWheel(tk.Canvas):
    """An HSV disc: angle = hue, radius = saturation, and a value slider beside it.

    Rendered smoothly (one image) when Pillow is present, because a wheel drawn as a
    handful of arcs bands badly in saturation - and the whole point of a wheel is judging
    a colour by eye. Without Pillow it falls back to Canvas arcs, which still works.

    The marker is a separate canvas item on top, so it survives a re-render.
    """

    RINGS = 24              # fallback only
    WEDGES = 180            # fallback only

    def __init__(self, master, size=200, on_pick=None, **kw):
        super().__init__(master, width=size, height=size, highlightthickness=1,
                         highlightbackground="#444", **kw)
        self.size = size
        self.on_pick = on_pick
        self.value = 1.0
        self.rgb = (255, 255, 255)
        self._photo = None              # NB: keep a reference or Tk drops the image
        self._rendered_value = None
        try:
            from PIL import Image, ImageTk  # noqa: F401
            self._pil = True
        except Exception:
            self._pil = False
        self.bind("<Button-1>", self._click)
        self.bind("<B1-Motion>", self._click)
        self.bind("<Configure>", self._resize)
        self.redraw()

    # -- rendering ---------------------------------------------------------

    def _render_pil(self):
        """A per-pixel HSV disc. ~40k pixels, so it is only redone when the value changes."""
        import math
        from PIL import Image
        import numpy as np

        n = self.size
        cx = cy = (n - 1) / 2.0
        r_out = n / 2.0 - 2

        yy, xx = np.mgrid[0:n, 0:n]
        dx = xx - cx
        dy = yy - cy
        dist = np.sqrt(dx * dx + dy * dy)
        ang = (np.arctan2(dy, dx) / (2 * math.pi)) % 1.0
        sat = np.clip(dist / r_out, 0, 1)

        hsv = np.stack([ang, sat, np.full_like(ang, self.value)], axis=-1)
        rgb = np.zeros((n, n, 4), dtype=np.uint8)

        # vectorised HSV -> RGB (standard six-sector form)
        h6 = hsv[..., 0] * 6.0
        i = np.floor(h6).astype(int) % 6
        f = h6 - np.floor(h6)
        p = hsv[..., 2] * (1 - hsv[..., 1])
        q = hsv[..., 2] * (1 - hsv[..., 1] * f)
        t = hsv[..., 2] * (1 - hsv[..., 1] * (1 - f))
        v = hsv[..., 2]

        r = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [v, q, p, p, t, v])
        g = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [t, v, v, q, p, p])
        b = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [p, p, t, v, v, q])

        rgb[..., 0] = (r * 255).astype(np.uint8)
        rgb[..., 1] = (g * 255).astype(np.uint8)
        rgb[..., 2] = (b * 255).astype(np.uint8)
        rgb[..., 3] = np.where(dist <= r_out, 255, 0)

        img = Image.fromarray(rgb, "RGBA")
        return img

    def redraw(self):
        self.delete("all")

        if self._pil:
            from PIL import ImageTk
            img = self._render_pil()
            self._photo = ImageTk.PhotoImage(img)
            self.create_image(0, 0, anchor="nw", image=self._photo)
            self._rendered_value = self.value
        else:
            self._redraw_arcs()

        self._place_marker()

    def _redraw_arcs(self):
        import math
        cx = cy = self.size / 2.0
        r_out = self.size / 2.0 - 2
        for ring in range(self.RINGS):
            sat0 = ring / float(self.RINGS)
            sat1 = (ring + 1) / float(self.RINGS)
            r_in = r_out * sat0
            r_mid = r_out * (sat0 + sat1) / 2.0
            w = max(1, int(r_out - r_in))
            for i in range(self.WEDGES):
                a0 = 360.0 * i / self.WEDGES
                a1 = 360.0 * (i + 1) / self.WEDGES
                r, g, b = [int(255 * c) for c in
                           colorsys.hsv_to_rgb(a0 / 360.0, r_mid / r_out, self.value)]
                self.create_arc(cx - r_out, cy - r_out, cx + r_out, cy + r_out,
                                start=a0, extent=a1 - a0, fill=hex6(r, g, b),
                                outline="", style=tk.ARC, width=w)
        del math

    def _resize(self, ev):
        # keeps the disc square if the window is resized; cheap because the render is cached
        pass

    def _place_marker(self):
        self.delete("marker")
        import math
        r, g, b = self.rgb
        h, s, _ = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        cx = cy = self.size / 2.0
        rr = (self.size / 2.0 - 2) * s
        x = cx + rr * math.cos(2 * math.pi * h)
        y = cy + rr * math.sin(2 * math.pi * h)
        self.create_oval(x - 6, y - 6, x + 6, y + 6, outline="#000", width=4, tags="marker")
        self.create_oval(x - 6, y - 6, x + 6, y + 6, outline="#fff", width=2, tags="marker")

    def set_value(self, v):
        """Brightness changed: re-render the disc (it is one image, so this is a redraw)."""
        self.value = max(0.0, min(1.0, float(v)))
        if self._pil:
            self.redraw()
        else:
            self._redraw_arcs()
            self._place_marker()

    def _click(self, ev):
        import math
        cx = cy = self.size / 2.0
        r_out = self.size / 2.0 - 2
        dx, dy = ev.x - cx, ev.y - cy
        dist = math.hypot(dx, dy)
        if dist > r_out:
            return
        h = (math.atan2(dy, dx) / (2 * math.pi)) % 1.0
        s = dist / r_out
        r, g, b = [int(round(255 * c)) for c in colorsys.hsv_to_rgb(h, s, self.value)]
        self.set_rgb(r, g, b, notify=False)
        if self.on_pick:
            self.on_pick((r, g, b))

    def set_rgb(self, r, g, b, notify=True):
        self.rgb = (int(r), int(g), int(b))
        _, _, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        newval = v if v > 0 else self.value
        if abs(newval - self.value) > 1e-6 and self._pil:
            self.value = newval
            self.redraw()               # the disc's brightness changed: re-render once
        else:
            self.value = newval
            self._place_marker()
        if notify and self.on_pick:
            self.on_pick(self.rgb)


# ---------------------------------------------------------------------------
# the app
# ---------------------------------------------------------------------------

class PaletteEditor(tk.Tk):
    def __init__(self, map_path, out_path, topmost=False, offline=False, launch=None, prebuilt=None):
        super().__init__()
        self.title("Caine's Crossfire - palette editor")
        self.map_path = map_path
        self.out_path = out_path
        self.offline = offline
        self.launch_cmd = launch
        self.prebuilt = prebuilt

        self.map = PalMap()
        self.ovr = Overrides()
        self.selected = None            # the ClutRow
        self.entry = 1                  # the entry inside it
        self.last_stamp = None

        if topmost:
            try:
                self.attributes("-topmost", True)
            except tk.TclError:
                pass

        self._build()

        if self.prebuilt is not None:
            # offline from a level's palette lump: no file to poll, no game to talk to
            self.map = self.prebuilt
            self._fill_tree()
            self.say("offline: %d CLUT row(s) from the level's palette lump - edits go to %s"
                     % (len(self.map.rows), self.out_path))
        else:
            self.reload_map()
            if not offline:
                self.after(900, self._poll)

    # -- ui ----------------------------------------------------------------

    def _build(self):
        bar = ttk.Frame(self)
        bar.pack(fill="x", padx=4, pady=3)

        ttk.Button(bar, text="Reload map", command=self.reload_map).pack(side="left", padx=1)
        ttk.Button(bar, text="Solo entry", command=self.do_solo).pack(side="left", padx=1)
        ttk.Button(bar, text="Flash entry", command=self.do_flash).pack(side="left", padx=1)
        ttk.Button(bar, text="Clear all", command=self.do_clear).pack(side="left", padx=1)
        ttk.Button(bar, text="Copy value", command=self.copy_value).pack(side="left", padx=1)
        if self.launch_cmd is not None:
            ttk.Button(bar, text="Launch game", command=self.do_launch).pack(side="left", padx=6)

        self.live = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="write overrides as I drag", variable=self.live).pack(side="left", padx=8)

        body = ttk.Frame(self)
        body.pack(fill="both", expand=True, padx=4, pady=3)

        # left: the map
        left = ttk.Frame(body)
        left.pack(side="left", fill="both", expand=True)

        ttk.Label(left, text="live palette map  (poly count = how much of the model uses it)").pack(anchor="w")
        self.tree = ttk.Treeview(left, columns=("polys", "clut", "vram", "height"), show="tree headings")
        self.tree.heading("#0", text="car / row")
        self.tree.heading("polys", text="polys")
        self.tree.heading("clut", text="clut")
        self.tree.heading("vram", text="vram")
        self.tree.heading("height", text="model y")
        self.tree.column("#0", width=360)
        self.tree.column("polys", width=50, anchor="e")
        self.tree.column("clut", width=60, anchor="e")
        self.tree.column("vram", width=90, anchor="e")
        self.tree.column("height", width=80, anchor="e")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        # right: the entries + the wheel
        right = ttk.Frame(body)
        right.pack(side="left", fill="y", padx=(8, 0))

        ttk.Label(right, text="entries in the selected CLUT row").pack(anchor="w")
        self.grid = tk.Canvas(right, width=16 * 22, height=44, highlightthickness=0)
        self.grid.pack(anchor="w")
        self.grid.bind("<Button-1>", self._on_grid_click)

        ttk.Label(right, text="colour wheel (drag on it)").pack(anchor="w", pady=(6, 0))
        self.wheel = ColourWheel(right, 200, on_pick=self._on_wheel)
        self.wheel.pack(anchor="w")

        self.valslider = ttk.Scale(right, from_=0.0, to=1.0, orient="horizontal")
        self.valslider.pack(fill="x")

        # the preview swatches must exist BEFORE the slider is set: setting a Scale fires
        # its command, and the command updates them
        prev = ttk.Frame(right)
        prev.pack(fill="x", pady=4)
        ttk.Label(prev, text="pick").pack(side="left")
        self.sw_pick = tk.Canvas(prev, width=40, height=22, highlightthickness=1, highlightbackground="#444")
        self.sw_pick.pack(side="left", padx=4)
        ttk.Label(prev, text="5-5-5").pack(side="left")
        self.sw_555 = tk.Canvas(prev, width=40, height=22, highlightthickness=1, highlightbackground="#444")
        self.sw_555.pack(side="left", padx=4)
        self.lbl = ttk.Label(prev, text="")
        self.lbl.pack(side="left", padx=6)

        self.valslider.config(command=self._on_value)
        self.valslider.set(1.0)

        self.status = ttk.Label(self, text="", anchor="w")
        self.status.pack(fill="x", padx=4, pady=(0, 3))

    def say(self, s):
        self.status.config(text=s)

    # -- map ----------------------------------------------------------------

    def reload_map(self):
        try:
            stamp = os.path.getmtime(self.map_path)
        except OSError:
            stamp = None

        if stamp is None:
            self.say("no map yet at %s%s" % (
                self.map_path, "" if self.offline else "  (start the game with the dump enabled)"))
            return

        self.last_stamp = stamp

        try:
            self.map = PalMap.parse(self.map_path)
        except Exception as e:                      # a half-written file while the game dumps
            self.say("map unreadable right now (%s)" % e)
            return

        self._fill_tree()

        self.say("map: %d car(s), %d CLUT row(s) - updated %s" % (
            len(self.map.cars), len(self.map.rows), time.strftime("%H:%M:%S", time.localtime(stamp))))

    def _fill_tree(self):
        self.tree.delete(*self.tree.get_children())
        bycar = {}
        for r in self.map.rows:
            bycar.setdefault(r.car_index, []).append(r)

        for ci, car in enumerate(self.map.cars):
            rows = bycar.get(ci, [])
            node = self.tree.insert("", "end", text="car slot %d  model %d  palette %d  (%s)  %d row(s)" % (
                car["slot"], car["model"], car["palette"], car["role"], len(rows)), open=True)
            # sorted highest-first: the parts whose Y reaches the top of the model are the
            # roof/upper body, which is what 'which part is which' usually means
            for r in sorted(rows, key=lambda r: (-r.ymax, -r.polys)):
                self.tree.insert(node, "end", iid=str(id(r)), text="    " + r.label(),
                                 values=(r.polys or "", "%04x" % r.clut if r.clut else "",
                                         ("%d,%d" % (r.vx, r.vy)) if r.clut else "offline",
                                         "%d..%d" % (r.ymin, r.ymax)))

    def _poll(self):
        try:
            stamp = os.path.getmtime(self.map_path)
        except OSError:
            stamp = None
        if stamp is not None and stamp != self.last_stamp:
            self.reload_map()
        self.after(900, self._poll)

    def _row_of_iid(self, iid):
        for r in self.map.rows:
            if str(id(r)) == iid:
                return r
        return None

    def _on_select(self, _ev):
        sel = self.tree.selection()
        if not sel:
            return
        r = self._row_of_iid(sel[0])
        if r is None:
            return
        self.selected = r
        self.entry = min(self.entry, 15)
        self.draw_grid()
        rgb = r.entries[self.entry]
        self.wheel.set_rgb(*rgb, notify=False)
        self._update_preview(rgb)
        self.say("selected %s   entry %d" % (r.label(), self.entry))

    # -- the entry strip ---------------------------------------------------

    def draw_grid(self):
        self.grid.delete("all")
        if self.selected is None:
            return
        for i in range(16):
            x0 = i * 22
            r, g, b = self.selected.entries[i]
            unused = (r == 0 and g == 0 and b == 0)
            # A black entry is the closest thing to "unused" that can be said without
            # looking at the texture pixels a poly covers: a car palette normally has no
            # black in it, and an entry the model never samples stays at whatever the
            # upload left. It is a hint, not a fact - Solo is what makes it a fact.
            fill = hex6(r, g, b) if not unused else "#101010"
            self.grid.create_rectangle(x0 + 1, 1, x0 + 21, 41, fill=fill,
                                       outline="#ff0" if i == self.entry else "#333",
                                       width=2 if i == self.entry else 1)
            self.grid.create_text(x0 + 11, 20, text=str(i),
                                  fill="#555" if unused else ("#000" if sum((r, g, b)) > 380 else "#fff"))

    def _on_grid_click(self, ev):
        i = ev.x // 22
        if 0 <= i < 16:
            self.entry = i
            self.draw_grid()
            if self.selected:
                rgb = self.selected.entries[i]
                self.wheel.set_rgb(*rgb, notify=False)
                self._update_preview(rgb)

    # -- colour ------------------------------------------------------------

    def _on_wheel(self, rgb):
        self._update_preview(rgb)
        self.apply_colour(rgb)

    def _on_value(self, _v):
        import colorsys as cs
        self.wheel.set_value(float(self.valslider.get()))
        r, g, b = self.wheel.rgb
        h, s, _ = cs.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
        nr, ng, nb = [int(round(255 * c)) for c in cs.hsv_to_rgb(h, s, self.wheel.value)]
        self.wheel.set_rgb(nr, ng, nb, notify=False)
        self._update_preview((nr, ng, nb))
        self.apply_colour((nr, ng, nb))

    def _update_preview(self, rgb):
        q = quantise(*rgb)
        self.sw_pick.delete("all")
        self.sw_pick.create_rectangle(0, 0, 40, 22, fill=hex6(*rgb), outline="")
        self.sw_555.delete("all")
        self.sw_555.create_rectangle(0, 0, 40, 22, fill=hex6(*q), outline="")
        self.lbl.config(text="%d,%d,%d  ->  15-bit %d,%d,%d" % (rgb[0], rgb[1], rgb[2], q[0], q[1], q[2]))

    def apply_colour(self, rgb):
        if self.selected is None:
            return
        r = self.selected
        self.ovr.set(r.carid, r.texid, r.palette, self.entry, rgb)
        # show it locally too: the map file only refreshes every 90 frames, and a swatch
        # that lags a drag by 3 seconds is useless
        r.entries[self.entry] = tuple(rgb)
        self.draw_grid()
        if self.live.get():
            self.write_overrides()

    # -- probe actions -----------------------------------------------------

    def do_solo(self):
        if self.selected is None:
            return self.say("pick a row first")
        r = self.selected
        self.ovr.solo(r.carid, r.texid, r.palette, self.entry)
        self.write_overrides(force=True)
        self.say("soloing entry %d of %s - every other entry is black, so you can see what it paints"
                 % (self.entry, r.label()))

    def do_flash(self):
        if self.selected is None:
            return self.say("pick a row first")
        r = self.selected
        self.ovr.flash(r.carid, r.texid, r.palette, self.entry)
        self.write_overrides(force=True)
        self.say("flashing entry %d of %s" % (self.entry, r.label()))

    def do_clear(self):
        self.ovr.clear()
        self.write_overrides(force=True)
        self.say("cleared - the game will put every row it changed back")

    def copy_value(self):
        if self.selected is None:
            return
        r = self.selected
        rgb = self.wheel.rgb
        txt = "row %d texid %d palette %d entry %d #%02x%02x%02x   (profile: palette %d)" % (
            r.carid, r.texid, r.palette, self.entry, rgb[0], rgb[1], rgb[2], r.palette)
        self.clipboard_clear()
        self.clipboard_append(txt)
        self.say("copied: " + txt)

    # -- files -------------------------------------------------------------

    def write_overrides(self, force=False):
        if not self.live.get() and not force:
            return
        try:
            os.makedirs(os.path.dirname(self.out_path), exist_ok=True)
            tmp = self.out_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(self.ovr.text())
            # replace, so the game's content hash never sees a half-written file
            os.replace(tmp, self.out_path)
        except OSError as e:
            self.say("could not write %s: %s" % (self.out_path, e))

    def do_launch(self):
        if not self.launch_cmd:
            return
        env = dict(os.environ)
        env["JERICHO_PAL_MAP"] = "1"        # so the game writes the map every 90 frames
        try:
            subprocess.Popen(self.launch_cmd, shell=True, env=env, cwd=DEV_BIN)
            self.say("launched: %s   (map dump enabled via JERICHO_PAL_MAP)" % self.launch_cmd)
        except OSError as e:
            self.say("launch failed: %s" % e)


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="Caine's Crossfire live palette editor.")
    ap.add_argument("--bin", default=DEV_BIN,
                    help="the build folder that has JERICHO/ (default: the dev build)")
    ap.add_argument("--map", help="the palette map file (default: <bin>/JERICHO/CONFIG/%s)" % MAP_NAME)
    ap.add_argument("--out", help="the override file (default: <bin>/JERICHO/CONFIG/%s)" % OVR_NAME)
    ap.add_argument("--topmost", action="store_true", help="float above the game window")
    ap.add_argument("--offline", action="store_true", help="do not poll for a live map")
    ap.add_argument("--launch", nargs="?", const="REDRIVER2_dev.exe", metavar="CMD",
                    help="start the game (with the map dump enabled). Default command: the dev exe")
    ap.add_argument("--level", metavar="LEV",
                    help="work OFFLINE from a level's palette lump (DriverLevelTool/*.LEV) - "
                         "no game needed. Shows the colours but not the CLUT ids, VRAM "
                         "addresses or poly counts, which only exist in a run")
    ap.add_argument("--city", help="the city for --level (default: its filename stem)")
    a = ap.parse_args(argv)

    cfg = config_dir(a.bin)
    map_path = a.map or os.path.join(cfg, MAP_NAME)
    out_path = a.out or os.path.join(cfg, OVR_NAME)

    prebuilt = None

    if a.level:
        city = a.city or os.path.basename(a.level).split(".")[0]
        try:
            prebuilt = offline_level_map(a.level, city)
            print("offline: %d CLUT row(s) from %s as %s" % (len(prebuilt.rows), a.level, city))
        except Exception as e:
            print("could not read %s: %s" % (a.level, e))
            return 2
    elif not a.offline and not os.path.exists(map_path):
        print("note: no map at %s yet - start the game (with JERICHO_PAL_MAP=1) and it will appear." % map_path)
    if a.launch is not None and not os.path.exists(os.path.join(a.bin, a.launch.split()[0])):
        print("note: '%s' not found in %s - pass --launch with a command that is." % (a.launch, a.bin))

    app = PaletteEditor(map_path, out_path, topmost=a.topmost, offline=a.offline,
                        launch=a.launch, prebuilt=prebuilt)
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
