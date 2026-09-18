# Per-instance pedestrian palettes (the Tanner body)

How to give one Tanner instance its own colours without touching the others, and
what the geometry actually costs. Written for the team-colour work in `combatd2`
(see `team_palette` there), but the lever is generic.

## What the geometry actually is

Measured with the built-in probe (`JER_PALETTE_PROBE=1`, `JerichoProbeTannerPalette`
in `pedest.c`, which walks `pmTannerModels[0..16]` exactly the way the plotter
does). On a Havana load, all 16 present bones:

| model | polys | textured | | model | polys | textured |
|---|---|---|---|---|---|---|
| TORSO | 32 | 4 | | U_ARM_LEFT | 10 | 10 |
| HEAD | 47 | 9 | | L_ARM_LEFT | 10 | 10 |
| U_ARM_RIGHT | 10 | 10 | | HAND_LEFT | 5 | 3 |
| L_ARM_RIGHT | 10 | 10 | | THIGH_LEFT | 14 | 12 |
| HAND_RIGHT | 5 | 3 | | CALF_LEFT | 14 | 12 |
| THIGH_RIGHT | 14 | 12 | | FOOT_RIGHT | 6 | 6 |
| CALF_RIGHT | 14 | 12 | | NECK | 6 | 6 |
| FOOT_LEFT | 6 | 6 | | HIPS | 14 | 12 |

`BAG` is not present in every level. `HEAD` is drawn by a **separate** call
(`DoCivHead`, `motion_c.c:2243`), not in the body loop.

The whole skeleton therefore uses:

```
set=2 id=13
set=2 id=2
1 distinct texture page
```

**One page, but two CLUT entries.** That second number is the one that matters:

## Why a single `plotContext.clut` is not enough

The body is drawn as one `RenderModel(...)` **per bone** (`newShowTanner`,
`motion_c.c:1376`, flags `PLOT_NO_SHADE`), through `PlotModelSubdivNxN`
(`draw.c:1031`), which takes its CLUT per polygon:

```c
if ((pc->flags & PLOT_CUSTOM_PALETTE) == 0)
    pc->clut = (*pc->ptexture_cluts)[polys->texture_set][polys->texture_id];   // draw.c:1112
```

`PLOT_CUSTOM_PALETTE` + `plotContext.clut` (`motion_c.c:2232-2235`, the
pedestrian **head** does exactly this) replaces **every** polygon's CLUT with the
one row. That is right for the head, whose model is a single CLUT entry, and
wrong for the body: the body's `(2,13)` and `(2,2)` polygons would both be forced
to the same palette, so whichever entry is garments would be painted with the
skin row. It only works if the two source rows are near-identical, which is not
something this code should assume.

## The lever we use

Retype the CLUT **table entry** for the duration of one instance's draw, instead
of overriding the plot context:

```c
u_short saved = texture_cluts[set][id];
texture_cluts[set][id] = teamRow;

... newShowTanner(...) ...          /* plot only; see below */

texture_cluts[set][id] = saved;
```

This is safe because plotting only **captures** the address: `pc->clut` is copied
into the primitive at plot time (`draw.c:1112`), and the primitive is what gets
drawn later from the ordering table. Patching the table therefore affects exactly
the polygons plotted while it is patched — one instance — and the primitives of
every other pedestrian keep the address they captured.

It also needs **no new engine surface**: no `struct _pct` growth (it sits at a
fixed scratchpad address, `draw.c:115`, with `static_assert`s guarding its budget
at `motion_c.c:1137/1177`), no new plotter branch, and no assumption that the
body stays on one page — the number of pairs is data, not a constant.

The engine part is therefore: a one-time walk recording the body's
`(set, id)` pairs plus their stock values, a helper that allocates a recoloured
row per pair, and a begin/end bracket used around the bone loop. Per team that is
**2 rows** (one per pair), not 1.

## CLUT allocation headroom

Rows are handed out by `clutpos` in `texture.c`, a 16-wide cursor moving along
`x` by `IncrementClutNum` (`texture.c:125`), wrapping at `x >= 1024` to
`x = 960, y += 1`. It starts at `(960, 256)` and the engine's own palettes (font,
`ProcessPalletLump`, the civ palettes) consume the first few rows of the strip.

So new rows come from the `960..1023` column at `y >= 256` — that is **4 rows per
scanline and a few hundred scanlines**.

Measured at `InitTanner` on a Havana load: `clutpos` sits at **`(960, 428)`**,
i.e. **688 of the 1024 rows used and 336 free**, allocatable from `(960, 428)`
onward. At two rows per team that is capacity for ~168 teams — negligible, and
`texture_cluts[128][32]` is a plain `u_short` table so patching and restoring is
a single assignment.

A row is prepared by copying the source CLUT's 16 entries (256 bytes), remapping
each colour toward the team colour, and uploading it with `LoadImage` +
`GetClut` — the same shape as `load_civ_palettes` and `ProcessPalletLumpForCity`
(`cars.c:1456`).

## Colour order

Colours in these palettes are packed `B << 16 | G << 8 | R` — **red is the low
byte** (see `JERICHO-colour-byte-order`). Getting this backwards silently swaps
red and blue.

## The probe

`JerichoProbeTannerPalette()` in `pedest.c`, called from the end of
`InitTanner()`, logs the per-bone counts, the distinct `(set, id)` pairs and the
page count. It is gated behind the `JER_PALETTE_PROBE` environment variable so
ordinary runs print nothing, and it must run **before** anything draws the
models: `ConvertPolygonTypes` rewrites poly ids in place, so the walk is only
faithful on a fresh model.
