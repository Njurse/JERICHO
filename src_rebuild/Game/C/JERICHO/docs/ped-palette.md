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

`jer_ped_palette_init()` in `pedest.c`, called from the end of `InitTanner()`, is
the production walk: it records the body's `(set, id)` pairs and their stock clut
words so a team palette can be swapped in. It also logs the measured footprint
(per-bone counts, the pairs, the CLUT cursor) when `JER_PALETTE_PROBE` is set, so
ordinary runs print nothing.

It must run **before** anything draws the models: `ConvertPolygonTypes` rewrites
poly ids in place, so the walk is only faithful on a fresh model. It also runs
once per level, which is what makes it correct — the pairs are not constant.
Measured on Havana they are `(2,13)`+`(2,2)` in one scenario and `(2,12)`+`(2,2)`
in another, so anything that hardcoded them would be wrong.

## The API

`JERICHO/include/jer_ped_palette.h`:

| call | when |
|---|---|
| `jer_ped_palette_init()` | once per level, from `InitTanner` |
| `jer_ped_palette_team(r, g, b, strength)` | once per team; returns a handle (rows are cached, so repeat calls are free) |
| `jer_ped_palette_select(handle)` | from a `JER_EVENT_PED_DRAW` handler, for the ped being drawn (`-1` = stock) |
| `jer_ped_palette_enter()` / `_leave()` | the host brackets the draw in `newShowTanner` — a module never calls these |

`strength` is 0..256: it scales how far each entry moves toward the team colour.

Two more decisions shape the look, and both are measured rather than guessed:

**Only the outfit is recoloured.** The body's two rows are identified by content:
the outfit is neutral (`r ≈ g ≈ b`) and skin is warm. On Havana the outfit row
averages **-0.2** per entry for `r - (g+b)/2` and the skin row **+4.0**, so the
2.5 threshold in `PedPalRowIsOutfit` is nowhere near either. Skin keeps its own
colours, so a team gets a team-coloured suit and a natural face. The rows are
classified at init, which is what makes this survive the `(2,12)`/`(2,13)`
variation — the *content* is what is stable, not the id.

That row-level test is **not sufficient on its own**, and a play test caught it:
the outfit row's own entry 9 is `[20,14,12]` — a warm tone, i.e. skin (hands,
neck) sharing the suit's CLUT. So `JerichoMakeClutRow` applies the same warm test
**per entry** and leaves warm entries untouched, while the neutral greys take the
hue. Without this the hands recolour with the suit.

**The dark end is lifted** (`jer_ped_palette_set_floor`, default 10 of 31). The
outfit row is a grey ramp dominated by near-black entries — measured, 7 of its 16
entries sit at brightness l1–l7 — so a proportional remap leaves a red suit at
red 1–5 of 31, which reads as black. Lifting the floor maps each entry's
brightness to `floor + (31 - floor) * lum / 31` before the hue goes on, so those
same entries come out at red **10–13 of 31**: clearly a dark team colour, with 13
distinct shades still intact (not a flat silhouette) and the bright entries
untouched. `floor = 0` is the unlifted behaviour and `floor = 31` is flat, so the
one parameter spans the whole spectrum.

On top of that, the brightest entries drift toward **white** (weight
`(lum - 20) * 2`, scaled by `strength`), so a light colour stays light instead of
flattening to the team hue: the outfit row's `[30,30,30]` highlight comes out
`[30,20,20]` for a red team rather than a flat `[30,0,0]`. Without it a mid-dark
team colour makes the whole outfit read as one flat tone.

`jer_ped_palette_enter` logs `ped palette: LEAK ...` if the table it is about to
swap still holds a previous team's row — i.e. if a swap ever escaped its bracket.
It has never fired, including under `combatd2` with its crew drawing.
