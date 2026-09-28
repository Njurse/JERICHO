#!/usr/bin/env python3
"""view3d.py - the arena editor's 3D viewport renderer.

A small software perspective rasterizer (numpy + Pillow) with a real DEPTH
BUFFER, so the editor can show the cell a spawn/pickup sits in - and its eight
neighbours - the way it will look in game. No GPU, no new dependency: it reuses
the level-geometry cache (arenaedit.LevelGeom) for the triangles and the same
texture atlas the top-down map uses (`arenaedit._build_atlas`).

The world frame is the engine's own (`world = (-4096*obj_x, +4096*obj_y,
+4096*obj_z)` - arenaedit.LEVEL_SCALE), the SAME frame the `.cca` spawn x/z live
in, so nothing needs calibrating.

    from view3d import Camera, render_viewport
    img = render_viewport("CHICAGO", focus=(x, y, z), rect=(x0, z0, x1, z1),
                          level_geom=geom, size=(480, 360))

Geometry is two-sided (the level mesh is not uniformly wound) and lit flat, so a
wall reads from either side.
"""

import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BG = (26, 24, 30)          # "nothing here", the same colour rendercheck uses
NEAR = 24.0                # near plane (world units; a car is ~750)
FOV_Y = 52.0               # vertical field of view, degrees
HEADING_MAX = 4096         # PSX angle units for a full turn (spawn headings)
MAP_CELL = 2048            # MAP_CELL_SIZE (map.h) - one streaming map cell
LIGHT = (0.35, 0.86, 0.36)  # a fixed key light, world-space (down-ish from above)
SHADE_LO, SHADE_HI = 0.55, 1.0


class Camera:
    """An orbit camera around `target`."""

    def __init__(self, target, yaw=35.0, pitch=30.0, dist=3200.0, fov=FOV_Y):
        self.target = (float(target[0]), float(target[1]), float(target[2]))
        self.yaw = float(yaw)          # degrees, about world Y
        self.pitch = float(pitch)      # degrees above the horizon
        self.dist = float(dist)
        self.fov = float(fov)

    def basis(self, W, H):
        """(eye, right, up, fwd, f) for a WxH viewport."""
        ty = math.radians(self.yaw)
        tp = math.radians(self.pitch)
        # direction from the target out to the eye
        dx = math.cos(tp) * math.sin(ty)
        dy = math.sin(tp)
        dz = math.cos(tp) * math.cos(ty)
        t = self.target
        eye = np.array([t[0] + self.dist * dx, t[1] + self.dist * dy,
                        t[2] + self.dist * dz], dtype=np.float64)

        fwd = np.array([-dx, -dy, -dz], dtype=np.float64)          # eye -> target
        world_up = np.array([0.0, 1.0, 0.0])
        right = np.cross(fwd, world_up)
        n = np.linalg.norm(right)
        if n < 1e-9:                       # looking straight down: pick any right
            right = np.array([1.0, 0.0, 0.0])
        else:
            right = right / n
        up = np.cross(right, fwd)

        f = (H / 2.0) / math.tan(math.radians(self.fov) / 2.0)
        return eye, right, up, fwd, f


def _project(verts, eye, right, up, fwd):
    """World verts (n,3) -> view-space (vx, vy, vz) with vz = depth in front."""
    d = verts - eye[None, :]
    vx = d @ right
    vy = d @ up
    vz = d @ fwd
    return vx, vy, vz


def _clip_near(poly, attrs, near=NEAR):
    """Sutherland-Hodgman clip of one polygon against vz >= near.

    `poly` is a list of (vx, vy, vz); `attrs` a parallel list of attribute tuples
    (each interpolated linearly, which is right for u/z, v/z and 1/z)."""
    out_p, out_a = [], []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        aa, ab = attrs[i], attrs[(i + 1) % n]
        ain = a[2] >= near
        bin_ = b[2] >= near
        if ain:
            out_p.append(a)
            out_a.append(aa)
        if ain != bin_:
            t = (near - a[2]) / (b[2] - a[2])
            out_p.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), near))
            out_a.append(tuple(aa[k] + t * (ab[k] - aa[k]) for k in range(len(aa))))
    return out_p, out_a


def _screen(view, W, H, f):
    """View-space (vx, vy, vz) -> (sx, sy, 1/vz) with vz > 0."""
    vx, vy, vz = view
    sx = W / 2.0 + f * vx / vz
    sy = H / 2.0 - f * vy / vz
    return sx, sy, 1.0 / vz


def _draw_tri(img, zbuf, X, Y, iz, uvz, colour, atlas=None, slot=0):
    """One screen-space triangle. `uvz` is (3,2) of (u/z, v/z) for a textured tri
    (else None); `colour` is the flat (r,g,b) for an untextured one. `atlas` is
    (flat, base, widths, heights, wmax). Depth-tested, two-sided, conservative."""
    H, W = zbuf.shape
    fx0, fy0 = float(X.min()), float(Y.min())
    fx1, fy1 = float(X.max()), float(Y.max())
    x0 = max(int(math.floor(fx0)) - 1, 0)
    y0 = max(int(math.floor(fy0)) - 1, 0)
    x1 = min(int(math.ceil(fx1)) + 1, W)
    y1 = min(int(math.ceil(fy1)) + 1, H)
    if x1 <= x0 or y1 <= y0:
        return

    xs = np.arange(x0, x1, dtype=np.float64)[None, :]
    ys = np.arange(y0, y1, dtype=np.float64)[:, None]

    x0v, x1v, x2v = float(X[0]), float(X[1]), float(X[2])
    y0v, y1v, y2v = float(Y[0]), float(Y[1]), float(Y[2])

    d = (y1v - y2v) * (x0v - x2v) + (x2v - x1v) * (y0v - y2v)
    if abs(d) < 1e-12:
        return

    n0 = (y1v - y2v) * (xs - x2v) + (x2v - x1v) * (ys - y2v)
    n1 = (y2v - y0v) * (xs - x2v) + (x0v - x2v) * (ys - y2v)
    n2 = d - n0 - n1
    sgn = -1.0 if d < 0.0 else 1.0

    # conservative: a pixel counts when its square touches the tri (half-pixel
    # slack), so shared edges do not leave seams. The depth test cleans up the
    # overdraw.
    len0 = math.hypot(x1v - x2v, y1v - y2v)
    len1 = math.hypot(x2v - x0v, y2v - y0v)
    len2 = math.hypot(x0v - x1v, y0v - y1v)
    ins = (n0 * sgn >= -0.5 * len0) & (n1 * sgn >= -0.5 * len1) & (n2 * sgn >= -0.5 * len2)
    if not ins.any():
        return

    w0, w1, w2 = n0 / d, n1 / d, n2 / d
    izi = w0 * iz[0] + w1 * iz[1] + w2 * iz[2]

    zreg = zbuf[y0:y1, x0:x1]
    win = ins & (izi > zreg)
    if not win.any():
        return

    if uvz is None:
        rgb = np.empty((y1 - y0, x1 - x0, 3), dtype=np.float64)
        rgb[...] = colour
    else:
        # only the WINNING pixels matter: the conservative test reaches up to half
        # a pixel past the edges, where the barycentric sum (izi) can be ~0, so the
        # perspective divide has to be guarded or it produces inf/nan
        den = np.where(win, izi, 1.0)
        u = (w0 * uvz[0, 0] + w1 * uvz[1, 0] + w2 * uvz[2, 0]) / den
        v = (w0 * uvz[0, 1] + w1 * uvz[1, 1] + w2 * uvz[2, 1]) / den
        u = np.where(win, u, 0.0)
        v = np.where(win, v, 0.0)
        rgb = _sample_atlas(atlas, slot, u, v)

    zreg[win] = izi[win]
    img[y0:y1, x0:x1][win] = np.clip(rgb[win], 0, 255).astype(np.uint8)


def _sample_atlas(atlas, slot, u, v):
    """Nearest-sample the atlas at (u, v) in [0,1] -> (h, w, 3) float."""
    flat, base, widths, heights, wmax = atlas
    pw = int(widths[slot]) or 1
    ph = int(heights[slot]) or 1
    col = np.clip((u * pw).astype(np.int64), 0, pw - 1)
    row = np.clip((v * ph).astype(np.int64), 0, ph - 1)
    texel = flat[int(base[slot]) + row * wmax + col]           # (h, w, 4)
    out = texel[..., :3].astype(np.float64)
    out[texel[..., 3] == 0] = BG                               # transparent -> bg
    return out


def _shade_faces(world_tri):
    """Per-face flat shade in [SHADE_LO, SHADE_HI] from the world-space normal."""
    e1 = world_tri[:, 1] - world_tri[:, 0]
    e2 = world_tri[:, 2] - world_tri[:, 0]
    nrm = np.cross(e1, e2)
    ln = np.linalg.norm(nrm, axis=1)
    ln[ln < 1e-9] = 1.0
    nrm = nrm / ln[:, None]
    lam = np.abs(nrm @ np.array(LIGHT))
    return SHADE_LO + (SHADE_HI - SHADE_LO) * lam


_ATLAS_CACHE = {}


def city_atlas(city, materials=None):
    """(atlas, materials) for a city's rip pages, cached. Atlas layout matches
    arenaedit._build_atlas: (flat, base, widths, heights, wmax)."""
    import arenaedit as ae

    obj, _png, _side = ae.level_rip_paths(city)
    key = (city.upper(), obj)
    try:
        stamp = os.stat(obj).st_mtime
    except OSError:
        return None, None
    got = _ATLAS_CACHE.get(key)
    if got and got[0] == stamp:
        return got[1], got[2]

    # the material slot order must match the geometry cache's, so use the same
    # list the scan produced (passed in, or loaded once here)
    if materials is None:
        g = ae.load_level_geom(city, verbose=False)
        if g is None:
            return None, None
        materials = g.materials

    pages = ae._read_mtl(os.path.splitext(obj)[0] + ".mtl")
    material_pages = [pages.get(m) for m in materials]
    flat, base, tw, th, wmax, npages, loaded, missing = ae._build_atlas(
        material_pages, os.path.dirname(obj))
    atlas = (flat, base, tw, th, wmax)
    _ATLAS_CACHE[key] = (stamp, atlas, materials)
    return atlas, materials


def draw_mesh(img, zbuf, verts, tris, cam, W, H, colours=None, atlas=None,
              uv=None, slots=None, bias=0.0):
    """Rasterize one world-space mesh into (img, zbuf).

    `verts` (n,3) WORLD, `tris` (t,3). `colours` (t,3) flat rgb (untextured) OR
    `atlas` + `uv` (t,3,2) in [0,1] + `slots` (t,) (textured)."""
    if not len(tris):
        return
    eye, right, up, fwd, f = cam.basis(W, H)
    vx, vy, vz = _project(verts, eye, right, up, fwd)

    world_tri = verts[tris]
    shade = _shade_faces(world_tri)

    for i, t in enumerate(tris):
        idx = (int(t[0]), int(t[1]), int(t[2]))
        poly = [(vx[k], vy[k], vz[k]) for k in idx]
        if atlas is not None and uv is not None:
            attrs = [((uv[i, k, 0] / vz[idx[k]] if vz[idx[k]] > NEAR else 0.0),
                      (uv[i, k, 1] / vz[idx[k]] if vz[idx[k]] > NEAR else 0.0))
                     for k in range(3)]
        else:
            attrs = [(0.0, 0.0)] * 3

        if all(p[2] < NEAR for p in poly):
            continue
        if any(p[2] < NEAR for p in poly):
            poly, attrs = _clip_near(poly, attrs)
            if len(poly) < 3:
                continue

        sch = shade[i]
        if colours is not None:
            col = tuple(min(255.0, c * sch) for c in colours[i])
        else:
            col = None

        # fan the (possibly clipped) polygon
        for k in range(1, len(poly) - 1):
            tri_p = [poly[0], poly[k], poly[k + 1]]
            tri_a = [attrs[0], attrs[k], attrs[k + 1]]
            X = np.empty(3); Y = np.empty(3); IZ = np.empty(3)
            for j in range(3):
                X[j], Y[j], IZ[j] = _screen(tri_p[j], W, H, f)
            if IZ.min() <= 0.0:
                continue
            if atlas is not None and uv is not None:
                uvz = np.array([[tri_a[0][0], tri_a[0][1]],
                                [tri_a[1][0], tri_a[1][1]],
                                [tri_a[2][0], tri_a[2][1]]], dtype=np.float64)
                _draw_tri(img, zbuf, X, Y, IZ, uvz, None, atlas, int(slots[i]))
            else:
                _draw_tri(img, zbuf, X, Y, IZ, None, col, None)


PICKUP_ICON = {                 # the .cca weapon NAME -> the icon asset stem
    "MG": "wid_mg", "MISSILE": "wid_missile", "SEEKER": "wid_homing",
    "CLUSTER": "wid_cluster", "ZOOMY": "wid_zoomy", "FREEZE": "wid_freeze",
    "SHOTGUN": "wid_shotgun", "SMG": "wid_smg", "MINE": "wid_mine",
}


def icon_dir():
    """The mod's textures/icons folder (this file lives in the mod's tools/)."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), "textures", "icons")


def icon_asset(kind, weapon):
    """The icon asset stem for a pickup: `health`, or `wid_<weapon>`. SEEKER is the
    only name whose asset differs from its lowercased self (the homing weapon's
    asset is `wid_homing` - CD2_WID_HOMING, weapon.h + arenas/pickupdraw.c)."""
    if kind == "health":
        return "health"
    w = (weapon or "").strip()
    if not w:
        return "wid_unspecified"
    return PICKUP_ICON.get(w.upper(), "wid_" + w.lower())


_ICON_CACHE = {}


def icon_atlas(name):
    """A one-page atlas (flat, base, widths, heights, wmax) for an icon, cached."""
    got = _ICON_CACHE.get(name)
    if got is not None:
        return got
    path = os.path.join(icon_dir(), name + ".tga")
    try:
        im = Image.open(path).convert("RGBA")
    except Exception:
        im = Image.new("RGBA", (8, 8), (200, 60, 60, 255))
    a = np.asarray(im, dtype=np.uint8)
    h, w = a.shape[0], a.shape[1]
    atlas = (a.reshape(-1, 4), np.zeros(1, np.int64), np.array([w], np.int64),
             np.array([h], np.int64), w)
    _ICON_CACHE[name] = atlas
    return atlas


def car_blob(city, pos, heading, model=1, geom=None, palette=None):
    """The placeholder car as a world-space blob: car model `model`'s geometry,
    rotated to the spawn `heading` (PSX units) and put at `pos`, flat-coloured from
    palette 0. None when the city has no such model.

    The model faces -z (its headlights are at z=-351, brake lights at +368 -
    CHICAGO.LCF cosmetics, cars.c), and a heading of 0 must point at world -z, so
    the rotation is -heading."""
    import levgeom

    if geom is None:
        geom, palette = levgeom.load_car(city, model, verbose=False)
        if geom is None:
            return None
    if palette is None:
        palette = {}

    a = heading / float(HEADING_MAX) * 2.0 * math.pi
    ca, sa = math.cos(-a), math.sin(-a)
    x, y, z = geom.verts[:, 0], geom.verts[:, 1], geom.verts[:, 2]
    wx = pos[0] + (x * ca + z * sa)
    wz = pos[2] + (-x * sa + z * ca)
    wy = pos[1] + y
    verts = np.stack([wx, wy, wz], axis=1)

    colours = np.empty((len(geom.tris), 3), dtype=np.float64)
    for i in range(len(geom.tris)):
        c = palette.get((int(geom.sets[i]), int(geom.texids[i])))
        colours[i] = c if c else (110, 110, 115)
    return {"verts": verts, "tris": geom.tris, "colours": colours}


def ground_y(geom, x, z, radius=768):
    """A stand-in ground height at (x,z): the low quartile of the nearby terrain
    (roads are flat, buildings rise above), so a pickup sits on the street."""
    for r in (radius, radius * 4, radius * 16):
        VW, _TRI, _u, _s = geom.window((x - r, z - r, x + r, z + r))
        if len(VW):
            return float(np.percentile(VW[:, 1], 25))
    return 0.0


def billboard(center, size, cam, W, H, atlas):
    """A camera-facing quad at `center`, `size` world units a side."""
    _eye, right, up, _fwd, _f = cam.basis(W, H)
    c = np.asarray(center, dtype=np.float64)
    r = right * (size / 2.0)
    u = up * (size / 2.0)
    verts = np.array([c - r - u, c + r - u, c + r + u, c - r + u], dtype=np.float64)
    tris = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.int32)
    uv = np.array([[[0.0, 1.0], [1.0, 1.0], [1.0, 0.0]],
                   [[0.0, 1.0], [1.0, 0.0], [0.0, 0.0]]], dtype=np.float64)
    return {"verts": verts, "tris": tris, "uv": uv,
            "slots": np.zeros(2, np.int32), "atlas": atlas}


def object_blob(city, geom, obj, cam, W, H, car_model=1, icon_size=500.0):
    """The world-space blob for one arena object (duck-typed: kind/x/z/y/heading/
    weapon). A spawn -> the placeholder car; a pickup -> its icon billboard."""
    kind = getattr(obj, "kind", None)
    x, z = float(obj.x), float(obj.z)

    if kind in ("player_spawn", "opponent_spawn", None):
        y = getattr(obj, "y", None)
        if y is None:
            y = ground_y(geom, x, z)
        return car_blob(city, (x, float(y), z), float(getattr(obj, "heading", 0) or 0),
                        model=car_model)

    if kind == "weapon":
        name = icon_asset("weapon", getattr(obj, "weapon", None))
    else:
        name = "health"
    gy = ground_y(geom, x, z)
    return billboard((x, gy + 250.0, z), icon_size, cam, W, H, icon_atlas(name))


def project_points(cam, pts, W, H):
    """Project world points (n,3) -> (sx, sy, ok): screen coords and whether each
    is in front of the near plane. For overlays (the cell mark)."""
    eye, right, up, fwd, f = cam.basis(W, H)
    P = np.asarray(pts, dtype=np.float64).reshape(-1, 3)
    vx, vy, vz = _project(P, eye, right, up, fwd)
    ok = vz > NEAR
    safe = np.where(ok, vz, 1.0)
    sx = W / 2.0 + f * vx / safe
    sy = H / 2.0 - f * vy / safe
    return sx, sy, ok


def cell_mark(img, cam, x, z):
    """Draw the map CELL the object sits in as a projected outline, so the 3x3
    window's centre cell is obvious. Uses MAP_CELL (2048), the engine's cell."""
    from PIL import ImageDraw

    cx = int(math.floor(x / MAP_CELL))
    cz = int(math.floor(z / MAP_CELL))
    x0, x1 = cx * MAP_CELL, (cx + 1) * MAP_CELL
    z0, z1 = cz * MAP_CELL, (cz + 1) * MAP_CELL
    y = 6.0                       # just above the ground, so it is not buried
    corners = [(x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)]

    W, H = img.width, img.height
    sx, sy, ok = project_points(cam, corners, W, H)
    dr = ImageDraw.Draw(img)
    for i in range(4):
        j = (i + 1) % 4
        if ok[i] and ok[j]:
            dr.line([float(sx[i]), float(sy[i]), float(sx[j]), float(sy[j])],
                    fill=(120, 200, 255), width=2)
    return img


def render_viewport(city, focus, rect, size=(480, 360), cam=None, level_geom=None,
                    blobs=(), bg=BG):
    """Render the 3D viewport -> a PIL RGB image.

    `rect` is the world window (x0, z0, x1, z1) - the 9-cell box around the
    focused object. `blobs` are extra world-space meshes drawn on top (the car,
    pickups); each is a dict with `verts`, `tris` and either `colours`, or
    `uv`+`slots` with the level atlas."""
    W, H = size
    if cam is None:
        cam = Camera(focus)

    img = np.empty((H, W, 3), dtype=np.uint8)
    img[...] = bg
    zbuf = np.zeros((H, W), dtype=np.float64)

    if level_geom is None:
        import arenaedit as ae
        level_geom = ae.load_level_geom(city, verbose=False)

    if level_geom is not None:
        VW, TRI, TRIUV, SLOTS = level_geom.window(rect)
        if len(TRI):
            atlas, _mats = city_atlas(city, level_geom.materials)
            if atlas is not None:
                # the geometry stores UV INDICES into the uv array; expand to [0,1]
                uv = level_geom.uv[TRIUV]
                draw_mesh(img, zbuf, VW, TRI, cam, W, H, atlas=atlas, uv=uv,
                          slots=SLOTS)
            else:
                flat = (128, 120, 112)
                draw_mesh(img, zbuf, VW, TRI, cam, W, H,
                          colours=np.tile(np.array(flat, float), (len(TRI), 1)))

    for b in blobs:
        draw_mesh(img, zbuf, b["verts"], b["tris"], cam, W, H,
                  colours=b.get("colours"), atlas=b.get("atlas"),
                  uv=b.get("uv"), slots=b.get("slots"))

    return Image.fromarray(img, "RGB")


def main():
    import argparse
    import arenaedit as ae

    ap = argparse.ArgumentParser(description="Render the arena editor's 3D viewport "
                                             "for one world point.")
    ap.add_argument("city")
    ap.add_argument("x", type=float)
    ap.add_argument("z", type=float)
    ap.add_argument("--y", type=float, default=None)
    ap.add_argument("--out", default="viewport.png")
    ap.add_argument("--size", type=int, default=480)
    ap.add_argument("--yaw", type=float, default=35.0)
    ap.add_argument("--pitch", type=float, default=30.0)
    ap.add_argument("--dist", type=float, default=3200.0)
    args = ap.parse_args()

    geom = ae.load_level_geom(args.city, verbose=True)
    if geom is None:
        print("no geometry for %s" % args.city)
        return 1

    y = args.y
    if y is None:
        VW, TRI, _u, _s = geom.window((args.x - 3072, args.z - 3072, args.x + 3072, args.z + 3072))
        y = float(np.median(VW[:, 1])) if len(VW) else 0.0

    rect = (args.x - 3072, args.z - 3072, args.x + 3072, args.z + 3072)
    cam = Camera((args.x, y, args.z), yaw=args.yaw, pitch=args.pitch, dist=args.dist)
    img = render_viewport(args.city, (args.x, y, args.z), rect,
                          size=(args.size, int(args.size * 0.75)), cam=cam, level_geom=geom)
    img.save(args.out)
    a = np.asarray(img)
    colour = float(np.mean(np.any(a != np.array(BG, np.uint8), axis=2)))
    print("wrote %s (%d x %d, %.1f%% covered)" % (args.out, img.width, img.height, colour * 100))
    return 0


if __name__ == "__main__":
    sys.exit(main())
