# Custom textures (`jer_texture.h`)

**The one way to get an image you authored onto something in the world.** A module
ships a TGA, asks the engine for a handle, and draws it. Loading, uploading, the
ordering table and the vertical-axis trap all stay on the engine side —
`Game/C/jer_texture.c` (+ `jer_texture_psx.c`).

```c
JER_TEXTURE icon = jer_texture_load("cainescrossfire", "icons/health");
...
(void)jer_texture_draw_card(icon, x, y, z, 120, 120, spin, JER_TEX_DRAW_NONE);
```

## Asset convention

```
JERICHO/MODS/<modId>/textures/<name>.tga          e.g. icons/health.tga
```

`name` is a slash-separated path under the module's `textures/` folder, extension
optional. The file is a **32-bit TGA** (uncompressed or RLE true-colour with an
8-bit alpha channel) — the format `LoadTGAImage` already reads, and the only one
that carries alpha for a cut-out icon.

The resolver prefers the *repo's* copy: a dev build runs from
`<repo>/src_rebuild/bin/<config>/JERICHO`, so the repo's `MODS` tree sits four
levels up and is chosen when it exists, falling back to the mirror next to the exe.
That is the same rule the arena loader uses, so the file a tool writes is the file
the game reads. The result is frozen to an **absolute** path, because the game
changes its working directory while loading a level.

## Drawing

Draw from `JER_EVENT_DRAW_WORLD` (mid-render, camera matrices live) or
`JER_EVENT_DRAW_OVERLAY`. A draw call outside a render pass is ignored, so a module
cannot corrupt the primitive table by calling from the wrong hook.

- `jer_texture_draw_card(tex, x, y, z, halfW, halfH, spin, flags)` — an upright,
  flat card that turns about the world vertical (`spin` in PSX angle units; it goes
  edge-on twice per turn, which is what reads as "spinning").
- `jer_texture_draw_flat(tex, x, y, z, halfW, halfL, yaw, flags)` — the same on a
  horizontal rectangle (a ground decal).

`x, y, z` is the **game's raw world frame** — the same numbers a car's
`hd.where.t[0..2]` carries. The API applies the engine's Y-flip internally. The trap
this hides: render-space Y is the *negative* of that frame (`cars.c` does
`pos.vy = -cp->hd.where.t[1]`), and camera-space +Y runs **down** the screen, so a
positive lift in the raw frame moves a card *up*.

Flags (`JER_TEX_DRAW_*`): `BILLBOARD` (yaw towards the camera instead of using
`spin` — do **not** set it on something that should spin), `MIRROR` (force the
texture flipped), `NO_OCCLUDE` (skip the depth sort so nothing hides it).

`jer_texture_draw_screen(tex, x, y, w, h, otBucket)` blits over a rectangle in the
**frame buffer** — no camera, for a menu background or any full-screen art. `otBucket`
is required rather than chosen for you: the frontend's ordering table is only
**16 entries** (`FE_OTSIZE`), so a world-sized index writes past the end of it. That
is not hypothetical — the first cut of the menu background used `OTSIZE-1` and the
menus flickered, because every frame appended primitives beyond the table. Pass the
bucket the art it replaces used (the menu background uses 11).

## JERICHO's own menu background

`JERICHO/CORE/jericho_background.tga` replaces the stock `DATA/GFX.RAW` menu art,
drawn through `jer_texture_draw_screen` so any size or format works. **On by
default.** Switch it off with `JERICHO/CONFIG/jericho.ini`:

```
custom_menu_background = 0
```

A missing or unreadable file is not an error — the stock background is drawn instead.
(`Game/C/jer_menu_bg.c`.)

## What the loader does to your image

jer_texture does not grow a format per art pipeline. **Whatever TGA you hand it is
converted, once, into what the game's own art is** — so an export from any tool
works, and authors do not have to guess:

- **RGBA.** A 24-bit TGA becomes RGBA with a fully opaque alpha (a photo or a
  background has no alpha to lose).
- **15-bit colour.** RGB is quantised to the PSX's 5-5-5, so a custom texture sits in
  the same palette world as the levels instead of reading as a photo pasted over them.
- **Capped size.** The long side is capped at 1024 and box-downscaled past that. A
  1672x941 background is needlessly heavy as a GPU texture for a 320x240-era look, and
  the draw scales it anyway — so you never have to pre-size an image. (An icon at 64x64
  is already in the right ballpark; that is what `tools/icons.py placeholders` writes.)

The converted size is what `jer_texture_size` reports.

## Two targets

| target | what it is | cost |
|---|---|---|
| `JER_TEX_TARGET_IMAGE` (default) | a real RGBA texture on the GPU, drawn via the PsyX texture override | any size, full colour, alpha; no VRAM |
| `JER_TEX_TARGET_PAGE` | the image quantised to an indexed PSX texture page + CLUT in VRAM | authentic PSX look; **VRAM is effectively full**, so a page must be given up and the load fails cleanly when none can be |

`jer_texture_target(tex)` reports which one you actually got.

## Rules the implementation enforces (and why)

- **The PsyX texture override is a global.** `DR_PSYX_TEX` applies to *every*
  textured primitive parsed after it. The three prims are therefore added to the OT
  in reverse parse order (reset, quad, override) into one bucket, so exactly one quad
  is covered. A change of texture also forces a new GPU draw split; a frame of 40
  cards measured ~80 splits against a `MAX_DRAW_SPLITS` ceiling of 4096
  (`PsyX_GPU.cpp`).
- **`z >> 3` is the only safe OT bucket.** SZ saturates at `0xFFFF`, and
  `0xFFFF >> 3 == OTSIZE-1`; `>> 2` can index past the table. The engine's own
  world prims (`debris.c`) use the same shift with a `z < 150` near-clip guard, which
  jer_texture also applies.
- **PsyX does not backface-cull**, so one quad is already visible from both sides.
  The API *auto-mirrors* the texture when the card faces away, so an icon does not
  read backwards.
- **Alpha**: `tpage 0` + `setSemiTrans(1)` selects `BM_AVERAGE`
  (`GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA`) under the RGBA override, so cut-out
  icons blend correctly.

## Lifecycle

`jer_texture_load` loads once per `(path, target)` and hands back the same handle
(handles are generation-tagged, so a freed handle is a clean no-op rather than a
stale alias). The engine re-reads every loaded texture on `JER_EVENT_GAME_START` and
on frontend entry — edit a TGA and it is picked up without a restart — and releases
everything on shutdown.

## For DLL addons: exports.def

`exports.def` is generated from the linker map, and the linker drops functions the
exe does not reference. `jer_texture.c` therefore keeps an **export anchor**
(`jerTexKeepPublicSurface`) that names every public entry point, so the whole
documented surface stays in the image and in `exports.def`. Adding an entry point
means adding a line there, then regenerating:

```
msbuild build/gen_exports.vcxproj /p:Configuration=Release_dev /p:Platform=x64
bin/Release_dev/gen_exports.exe bin/Release_dev/REDRIVER2_dev.map exports.def
```

Rebuild `gen_exports.exe` first — a stale binary emits `??_C@` string-literal
entries that fail the link with `LNK2001` under `Release_dev`.

## Testing a texture visually

`-shot <frame>` writes `SCREENSHOT.BMP` on gameplay frame N (so a visual feature can
be checked from a script rather than by pressing F12), and `JERICHO_TEX_DIAG=1`
traces each drawn card's projected rectangle to the log.

When validating the *mapping*, use a **purpose-built test image** — four quadrants of
distinct colours with a border, like
`JERICHO/MODS/cainescrossfire/textures/icons/_uvtest.tga`. A photographic icon
cannot show a swapped axis: measured on this install, a random scene patch
correlated *better* with the icon (+0.46) than the correctly rendered card did
(-0.07), because the icon is low-contrast and mostly white.
