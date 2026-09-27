# Arenas

An **arena** is a bounded place a match happens on. Where a [vehicle
profile](PROFILES.md) says *what car* you fight in, an arena says *where*: which
city, which mission layout, the **region** that keeps the fight in (the
barrier), and the **spawn points** (position + heading) the cars start at.

The registry is the same shape as the vehicle one (`profiles/registry.c`): a
manifest of rows indexed by an id, in `arenas/registry.c`. The four **built-in**
arenas are the cities' small multiplayer maps — identity only. Every **custom**
arena is an authored data file under `JERICHO/MODS/cainescrossfire/arenas/` that the game
loads at boot, so a new arena needs no rebuild — only a file the editor writes.
A file whose `arena:` name matches a built-in (e.g. `chicago`) *replaces* it, so
the built-ins are themselves editable.

## Making and opening arenas

The easy way is the launcher, which keeps the mod's arena folder and the copy
the game reads in step, and can make a new arena, open one in the Python editor,
or drop you into the in-game editor:

```
JERICHO\MODS\cainescrossfire\tools\arena_menu.bat
```

Double-click it on Windows. By hand:

- **New** — drop a `.cca` into `arenas/` (or `python tools/arenaedit.py --new my_arena`).
- **Reaching the game** — `arenas/` is part of the mod. A **dev build's** game
  resolves its arena folder to **this repo copy**, so an edit here is live (that
  is what makes the two editors pseudo-realtime); an installed mod uses its own
  mirrored copy, which a normal build keeps in step.
- **Run the repo copy of the tools.** The Windows build also copies the whole mod
  into the game tree (`src_rebuild\bin\<cfg>\JERICHO\MODS\...`), so an old script
  can linger there and make a fix look like it did not work. Both copies now
  resolve the arena folder to the repo's `arenas/`, and running the build's copy
  prints a note when the repo copy has moved on.
- **Open top-down** — `python tools/arenaedit.py <the .cca>` (no arguments opens
  every arena in the folder).
- **Edit in-game** — the Python editor's **Launch in game** button (or `--launch`)
  boots straight into the open arena in editor mode; or launch with `-cceditor`
  and pick an arena (see below).

## The four built-ins

| internal | on-screen | city | layout |
|---|---|---|---|
| `chicago` | Chicago | CHICAGO | the city's mp map (arena 0) |
| `havana`  | Havana  | HAVANA  | the city's mp map (arena 0) |
| `vegas`   | Las Vegas | VEGAS | the city's mp map (arena 0) |
| `rio`     | Rio     | RIO     | the city's mp map (arena 0) |

They are **unbounded** (no barrier) and author **no spawns**: the match falls
back to the engine's player start and the module's player-relative opponent
placement, and a short on-screen notice says so (`No spawn points in this arena
- using fallback`).

## The arena file

One arena per file, `JERICHO/MODS/cainescrossfire/arenas/<name>.cca`. A line-based, diffable
text format — written by hand, by the in-game editor, or by the Python top-down
editor (`tools/arenaedit.py`), all three reading and writing exactly this.

```
# a comment runs to the end of the line
arena: dockyard_duel      # internal name (the file key; also overrides a built-in)
name: Dockyard Duel       # on-screen name (default: the internal name)
city: CHICAGO             # CHICAGO | HAVANA | VEGAS | RIO, or 0..3
mp: 1 0                   # <load the small mp map?> <which layout 0|1>. 'full' = whole city
region: -20000 -30000 20000 30000   # the barrier, world units: x0 z0 x1 z1. 'none' = whole level
spawn: -5000 2000 1024 30  # x z heading [y]. Repeatable; the FIRST is the player, the rest the opponents
spawn: 5000 2000 2048 30
spawn: 3000 -6000 0 30     # y is optional but recommended - see below
# pickup: weapon <name> x z [ammo]   |   pickup: health x z [amount]
pickup: weapon missile 5000 2000 5   # a drive-over crate: grants the named weapon + ammo
pickup: health -3000 1000 2500       # a repair: this many damage units off totalDamage
```

- **`region`** is world-space XZ (the same units the game uses), an
  axis-aligned rectangle. `x0<x1` and `z0<z1` after load (the loader swaps if
  needed). Cars the module owns (the player and the opponents) are kept inside
  it: a car at the edge is put back on it and its **outward velocity is
  cancelled**, so the edge reads as a wall. Civ traffic is free to wander out.
  Omit it (or `none`) for no barrier.
- **Keep every `spawn` INSIDE the `region`.** The barrier only pulls back a car
  near the edge — one that drove out from inside. A car sitting FAR outside it
  (which is what a spawn outside the region produces) is deliberately **left
  alone** rather than teleported across the map into the void: the game warns at
  GAME_START (`spawn N (x,z) is OUTSIDE the region`) and on screen, the barrier
  says `car=N sits far outside the region ... left alone`, and
  `arenaedit.py --check` flags it. So if a car will not stay put, look at the
  region first.
- **`spawn`** is a world position, a heading in PSX angle units (0..4095), and an
  optional **height** `y`. Spawn 1 is the player; 2.. are the opponents **in
  order** (opponent 0 is the second `spawn:` line, and so on). Up to 16. A
  `spawn` inside scenery is the author's to fix — the editor shows where they are,
  and authored spawns are trusted (no clear-line probe).
- **`y`** is what keeps cars out of the void. With no `y`, the game keeps whatever
  height the *level* started the player at — right for a flat area, wrong on a
  hill or a raised road, where the car drops through the world. The in-game editor
  records the car's height as you place, so authoring by driving is always right;
  placing by hand, run the arena once and read the `spawned at (x,y,z)` log line
  for the height to paste. (A spot with no ground under it at all still falls —
  place on the map.)
  trusted (no clear-line probe).
- **`pickup`** places a drive-over item: `pickup: weapon <name> x z [ammo]` gives the
  named weapon (its short code name, e.g. `missile`, `MG`, `shotgun` — or its
  display name) plus `ammo` rounds (default 5), and `pickup: health x z [amount]`
  removes `amount` damage units from the car's `totalDamage` (default 2500). A
  module-owned car (the player or an opponent) driving within **320 units** collects
  it; it then disappears and comes back after **900 frames (30s)**. Civ traffic
  drives over them freely. Watch the name: it is matched against the weapon's
  **short code name** (`CD2_WEAPON_DEF.name`), so the homing weapon is `SEEKER`, not
  `homing` — a name that matches nothing is dropped without a word.
- **how a pickup LOOKS** (all optional, and the defaults are what you want unless you
  are doing something specific):
  - `pickupspin: <PSX angle units per frame>` — how fast the plane turns. Default
    **48**, i.e. a full turn every ~2.8s at 30fps; it goes edge-on twice a turn,
    which is what reads as spinning.
  - `pickupbob: <world units>` — how far it floats up and down. Default **40**, `0`
    to keep it still.
  - `pickupsize: <world units>` — the plane's half-size. Default **220** (440 wide).

  An arena that sets none of them is written back without them, so existing files
  are untouched. Each pickup is drawn as an upright plane showing its icon
  (`textures/icons/health.tga` and `textures/icons/wid_<weapon>.tga` — see
  `tools/icons.py`), turning about the world vertical and bobbing, always on top so
  the road cannot hide it. A weapon with no icon falls back to the old
  bar-and-diamond: **green for health, amber for a weapon**.
- **`city` + `mp`** decide the level: `city` picks the city, `mp 1 0` loads that
  city's small multiplayer map, `mp 0 0` (or `mp: full`) loads the **full city
  map** — which, with a `region`, is how you cordon off a corner of a
  single-player level into a small arena of your own.
- Unknown keys are ignored (forward-compatible). A file with no `arena:` line
  does not register.

### Where the files are read

`JERICHO/MODS/cainescrossfire/arenas/*.cca` — the mod's own folder. The game
scans its **mirror** of that folder, `bin\<config>\JERICHO\MODS\cainescrossfire\arenas\`,
which the build (or `arena_menu.bat`'s *Sync*) keeps in step; the in-game editor
writes there too, and the launcher copies edits back to the mod folder. With
`-ccmenu` (or the main menu's Deathmatch entry) the arena menu lists every
registered arena; picking one loads its city/layout and makes it the match's
current arena, so its spawns and barrier apply.

## Coordinates (what the numbers mean)

An arena's `region` and `spawn` values are **world units** — exactly what
`CAR_DATA.hd.where.t[]` holds (`x z`, plus `y` for the height). They are *not*
grid cells and *not* the PlayStation fixed-point scale, and the editor shows the
same numbers the game logs.

- **Map grid:** a *cell* is **2048** units and a *region* is 32 cells =
  **65536** units (`MAP_CELL_SIZE` / `MAP_REGION_SIZE`, both level-header fields
  — zero until a level is loaded). A small arena is therefore a few cells per
  side: tens of thousands of units, not millions. The shipped examples
  (`-8000 -70000 8000 -55000`) are about 8×7 cells.
- **`ONE == 4096`** (`dr2math.h`) is the engine's fixed-point unit for trig,
  velocities and the **exported level model** — not for `hd.where`. The rigid
  body keeps `fposition = hd.where.t << 4` (16 sub-units); the inverse is
  `hd.where.t = fposition >> 4`.
- **DriverLevelTool's `.obj`** is the level model at **1/4096 scale, X
  mirrored**: `world_x = -4096 * obj_x`, `world_z = +4096 * obj_z`. So the RIO
  vertex `x=55.7991` is world `x ≈ -228553`. The Python editor's
  `--obj/--cells` mode instead stretches the obj's bounding box onto the cell
  rectangle — a rough backdrop; use the 4096 formula when you need the picture to
  register exactly with authored coordinates.
- **The easy way to get a coordinate** is to drive there: the in-game editor's
  readout and the `player car N spawned at (x,y,z)` log line give you real world
  numbers, including the height that keeps a car out of the void.

## Two editors, one live file (pseudo-realtime)

The game reads *and writes* the arena file **in the mod folder**
(`JERICHO/MODS/cainescrossfire/arenas/`; an installed mod uses its own copy) —
the same file `tools/arenaedit.py` edits — and both sides watch it:

- **Save in the Python editor** → the running game re-reads the `.cca` within a
  second: the region barrier and the ghost markers update live, and the HUD says
  *arena reloaded from disk*. Unsaved in-game edits are never clobbered — the
  game logs that it kept them instead.
- **Save in-game** (SELECT) → the Python editor notices the file changed and
  reloads it; if you have unsaved top-down edits it tells you rather than
  overwriting them.

So you can drive in the game and adjust the same arena top-down (or the other
way round) without restarting either one.

Verified both ways (2026-09): a save through the Python API while the game was
running logged `arena: reloaded 'live' from disk (2 spawns, 0 pickups)`; an
in-game save (a scripted L1 place + SELECT) moved the file's mtime, and the
Python side's poll reloaded the new spawn — or refused, when there were unsaved
top-down edits.

## The in-game editor

Start a match with `-cceditor` (or `CC_EDITOR=1`) and the player's car becomes
the cursor: drive to a spot and press a button. The change shows live — ghost
markers and the region rectangle are drawn every frame — and **SAVE** writes the
same `.cca` the Python editor reads *and* updates the live arena, so the barrier
follows the edit immediately. It is a build/debug tool, not a play mode.

| button (pad 0) | action |
|---|---|
| **L1** | place / move the *selected* spawn at the car (its position and heading) |
| **R1** | cycle which spawn slot is selected |
| **L2** | delete the spawn nearest the car |
| **R2** | mark the region: first press = corner A, second = the rect (the car is the other corner) |
| **TRIANGLE** | warp the car to the cursor (the freecam's aim point, or its own spot) |
| **CROSS (X)** | toggle **noclip** — hold the car's altitude (no gravity, no fall) so you can drive/flit through the air to place a spawn precisely without dropping out of the world |
| **SELECT** | save to `MODS/cainescrossfire/arenas/<name>.cca` (and update the live arena) |
| **START** | reload the arena from its file |

**Noclip** (CROSS) is flight for positioning: the car IS the editor cursor, so
fly it (drive in x/z — the handling is point-mass, so it works in the air) to the
spot, adjust the height with the **d-pad** (up = rise, down = drop, ~15000
units/s), then **L1** drops a spawn there. Noclip starts holding whatever
altitude the car had when you switched it on and no longer falls. The HUD line
shows `noclip ON/off`. It resets off on each level start.

**While the editor is ON the module disables weapons and sends the mounted crew
back inside**, so the shoulder buttons belong to the editor — no shot fires and
no driver/gunner leans out while you place (the log notes it once: `arena editor:
weapons disabled and crew retracted`). A scripted run can drive the editor
headlessly through the debug driver's `pad:` step with `JERICHO_CC_INJECT=1` —
the editor reads that injected mask alongside the live pad:

```
5:pad:4      # L1  -> place a spawn at the car
7:pad:0
12:pad:8     # R1  -> next slot
20:pad:2     # R2  -> corner A ... and again for the rect
28:pad:256   # SELECT -> save
```

## Freecam (F7) as the editor cursor

Press **F7** (the engine's freecam — `game.freeCamera=1` in `data/config.ini` is
what wires that key up in a release build) and the editor's cursor leaves the
car: every action then uses **the point the camera is looking at**, about 6000
units in front of it, so an arena can be laid out from above instead of by
driving. Turn the freecam off and the cursor snaps straight back to the car.

The readout says which is live (`cursor car` / `cursor FREECAM (F7)`) and a cyan
marker is drawn at the freecam cursor. **Aim at the ground** where you want the
spawn: the cursor's height is what is written as the spawn's `y`, which is what
keeps the car out of the void. Headless, or without a keyboard, the debug
driver's `freecam:1` / `freecam:0` step forces it on and off.

**Getting the car there:** press **TRIANGLE** while the freecam is on and the car
is dropped at the cursor — fly over an area, tap it, then drive around to feel the
arena out without restarting the match. With the freecam off it does nothing (the
car is already the cursor).

## The Python editor (the window)

`arenaedit.py` (or the launcher's *2) Open…*) is a normal editor window, not a
bare canvas:

- **Menu bar** — File (new / open / save / save as / reload / quit), Edit (undo,
  delete selected), View (grid, labels, fit, zoom, background), Help (shortcuts).
- **Toolbar** — the tools, as radio buttons: **Select** (drag spawns and region
  corners), **Add spawn**, **Delete**, **Region** (click a corner, then the
  opposite one) — plus Fit, Grid and Labels toggles and the arena picker.
- **Canvas** — a grid with its step printed (a cell is 2048), the region as a
  dashed rectangle with draggable corners, spawns numbered (`P` = the player),
  pickups as squares, middle-drag to pan, wheel to zoom.
- **Inspector** (right) — the arena's fields (name, city, mp map/layout), the
  region's four numbers with Apply / Clear / Fit / "use the view's edges", the
  **spawn list** with an edit form (x, y, z, heading — leave y blank for "none"),
  the **pickup list** with an add form, and the **Background** box: the map style
  (`textured` / `points`), *Rip this city*, *Use the cached level map*, *Build
  level map for this city* and *Remove background*.
- **Status bar** — the file and its unsaved state, the arena, the region, the
  counts, the last action, and the cursor's world position.
- **Launch in game** — the toolbar button (also *File ▸ Launch in game*, `F5`,
  or `--launch`) saves the arena and starts the game **straight into it** in the
  in-game editor, zero opponents. The game runs **detached**, so this window
  stays usable. It boots the engine's own frontend bypass
  (`-level <city> [-mp <layout>] -car slot1`) with `CC_EDITOR=1` and
  `CC_FORCE_ARENA_NAME=<arena>`; `--launch-dry` prints that command instead of
  starting anything. The exe is the build output, or `--exe PATH` / `CC_GAME_EXE`
  (reported by `--selftest`).

### Long jobs (a rip, a level-map render)

Both of those take tens of seconds to minutes, so they run **off the Tk thread**
and the window says so rather than freezing: the Background box grows a progress
bar and a **Stop** button, the background buttons go grey, and the status line
counts the render's progress ("building the RIO level map (textured): 47%").
Stop, `Ctrl+Q` and closing the window all cancel the job — for a `--rip` that
means the `DriverLevelTool` child is terminated — and the waiting is bounded, so
the window never becomes unkillable. A job that raises is reported in the status
line instead of vanishing.

(That is also why this was worth doing: the load and the folder listing used to
run straight from the button handler, so the window was dead for the whole
render and the OS greyed it out as "not responding".)

Keys: `1`–`4` pick a tool, `F` fit, `G` grid, `L` labels, `Del` delete, `Esc`
cancel, `Ctrl+S/O/N/R/Z/Q`, `F1` shortcuts. Right-click deletes whatever is under
the pointer; **undo** is `Ctrl+Z`.

`--uitest` builds the window, drives it through its own commands (clicks the map,
uses the inspector, undoes, saves, reloads), exercises a background job (starts
one, cancels one, fails one) and reports — so the UI is checked without a human
at the screen. It keeps the window **withdrawn**: it drives the widgets, it does
not need to be seen, and leaving it mapped made Tk paint every canvas item.

## Designing against a level map (the Python editor)

A city needs a **rip** for the editor to draw it, and a rip is a **local
artifact**: `DriverLevelTool/` is gitignored, so only what you have exported
yourself is there. That is why RIO used to be the only city you could draw — the
other three had never been ripped, and `--level CHICAGO` had nothing to load.

```
python arenaedit.py --rip                        # export all four cities' rips
python arenaedit.py --rip HAVANA                 # ... or just one
python arenaedit.py myarena.cca --level RIO      # build/load the map, aligned
python arenaedit.py --level RIO --rebuild-map    # force a rebuild
arena_menu.bat  ->  8) Build the level map       # the same, from the menu
```

`--rip` runs `DriverLevelTool.exe <CITY>.LEV -world 1 -textures 1` in
`DriverLevelTool/` (copying the `.LEV` in beside it, from
`src_rebuild/bin/<cfg>/DRIVER2/LEVELS/`). It is slow — minutes per city, ~120–240 MB
of `.obj` — so it is a one-off per machine, and an already-ripped city is skipped.
`--level CITY` **rips on demand** when that city has no rip yet (`--no-rip`
refuses instead), and `--selftest` prints each city's state, so "which cities can
I draw?" is one command. This build of `DriverLevelTool` rejects `-format` — it
detects the LEV itself — so the tool is not passed one.

The picture is drawn **aligned to the game's world coordinates** (a rip is the
level model at 1/4096 with X mirrored, so `world_x = -4096*obj_x`,
`world_z = +4096*obj_z`) and **cached** beside the `.obj` as
`<CITY>_LEVELMODEL.topdown.png` plus a `.json` sidecar carrying the world rect
(and the style) — so only the first build costs anything and the PNG is
shareable on its own, without the rip. With no `--level`, a map already cached
for the arena's city is used automatically.

### What the map is (`--style`)

```
python arenaedit.py --level RIO --style textured   # the default: the city, textured
python arenaedit.py --level RIO --style points     # the fast vertex cloud
```

The rip carries real geometry — ~1.2M textured faces for a city — and an `.mtl`
naming a texture page per material, so the default **`textured`** style is a real
top-down **render** of the city, not a wireframe: every triangle is filled with
its texture's colour, and a height buffer keeps the topmost surface, so a roof
wins over the street under it. It costs ~10–30 s and ~1–1.7M triangles per city,
once, into the cache. **`points`** is the old vertex cloud: instant, no textures,
and still the automatic fallback for a rip with no faces.

Two things worth knowing:

* **Which way up the texture is** is *measured*, not assumed. A face's signed
  area on screen and in `(u, v)` must match if its texture is not mirrored when
  seen from above; the build counts that over the faces where the sign is well
  determined and reports the percentage (all four cities land at 88–95%, i.e.
  keep `u` as it is). `--uv-flip` is the escape hatch if a rip disagrees.
* The tool writes a page file only when it has one, so a few materials name a
  `PAGE_n.tga` that does not exist (0–9 per city). Those faces draw flat grey —
  measured at 93 px in the whole RIO map, so it is not worth chasing.

Changing `--style` **rebuilds** the map rather than serving the other one: the
sidecar keys the cache on the style (`renderer` records what actually drew it).

Asking for a map and not getting one is never silent: the reason is printed
*and* `--render` exits non-zero, so the launcher's menu 8 can tell (it used to
report "the build failed" only on an earlier, unrelated error).

`tools/rendercheck.py` is the headless guard for all of this — a synthetic rip
whose answer is known (ground, a raised patch that must win the height buffer,
and a quad too big for the fill grid, which must be subdivided rather than
skipped) plus `--city CITY` / `--all` on the real maps:

```
python rendercheck.py --all        # every ripped city renders with structure
```

## Test levers

- `CC_FORCE_ARENA=<arena id>` — launch straight into an arena (id 0..N-1 in
  registry order: 0..3 are the built-ins, then files in load order). Pairs with
  `CC_FORCE_CAR=<0..CD2_VEH_COUNT-1>` and `CC_FORCE_OPPONENTS=<0..CD2_AI_MAX>`;
  a padless run launches from the menu-open frame.
- The boot log prints one line per arena (`cd2ArenaDump`), and a run logs the
  player's spawn, each opponent's authored spawn, the first barrier hit per car,
  and the no-spawns fallback — so a headless run shows the arena data it
  actually loaded:

```
[cainescrossfire] arenas: 5 registered (4 built-in)
[cainescrossfire]   [4] test_docks (Test Docks) city=0 mp=1/0 region=0,0,4000,4000 spawns=3
[cainescrossfire] arena 'test_docks': player car 0 at spawn (-500,-500) heading 0
[cainescrossfire] arena spawn: opponent 0 at (1000,1000) heading 1024
[cainescrossfire] arena barrier: car=0 clamped to (0,0)
```

## Which map an arena loads (single-player city vs the small mp map)

The `mp:` line is the biggest lever in an arena and the least obvious:

| `mp:` | what loads |
|---|---|
| `0 0` | the **full single-player city** (mission M50..M57) |
| `1 0` | the city's **small multiplayer map**, layout 0 (M58..M65) |
| `1 1` | the city's **small multiplayer map**, layout 1 (M498..M505) |

The engine picks this in `glaunch.c` (GAME_TAKEADRIVE): base `50` when it is the
full city, base `58` for the multiplayer map, plus `GameLevel * 2 + night +
subGame * 440`. `tools/arena_menu.bat` asks for it when it makes an arena
("`1` = the city's small mp map, `0` = the full city") and the Python editor has
the same switch in its Arena box.

So "I picked arena X and it loaded the normal multiplayer map" means the arena's
file says `mp: 1 ...`. New arenas default to the mp map unless you answer `0`.

## Where a spawn gets its height (and when it falls into the void)

A spawn's `y` is optional: `spawn: x z heading [y]`.

* authored `y` — used as-is;
* no `y` — the module asks the engine (`MapHeight`, the same call antfarm uses)
  for the **ground under that spawn**, so a distant spawn no longer inherits the
  player's height and drop through the world.

### The player's spawn streams its own region

The engine streams the world as a 2×2 window of **regions** and only pre-loads
*neighbours*, so a region the player **hops** into — a spawn far from the level's
own start — is never placed in a barrel and its cells never load: the world
around the car is a void. The module now fixes that: when it places the player at
`spawns[0]` it calls **`jer_map_spool_to`** (`JERICHO/docs/map-streaming.md`),
which makes the engine load the spawn's region (and its neighbours) before the
car needs the ground. The log shows it:

```
[jer_map] streamed region 144 for (-186527,82690)
```

Only the **player's** spawn is streamed. The engine keeps just four regions, so
streaming one per spawn would evict the ones just loaded — keep the opponents
inside the same part of the map as the player (see the spawn verdict below).

### The spawn verdict (logged a second after the drop)

A second after placing the player the game logs one line per spawn — a
`jer_map`-aware verdict, not a bare `MapHeight`:

```
arena 'pracinhas': spawn 0 (-186527,82690) - the engine gave no ground height here (MapHeight is not always reliable)
arena 'pracinhas': spawn 2 (-198135,146918) is in region 157, which is not resident (not streamed) - a car dropped there falls into the void. Keep spawns inside the arena you are playing in.
```

**"not resident — a car dropped there falls into the void" is the thing to act
on.** The engine only spools four regions around the player, so a spawn a long
way from the player has no ground. This is why spawns should sit inside the part
of the map the arena is played in.

Two caveats about the engine's height answer, both measured:

* `MapHeight` reads the map through a barrel indexed by cell **parity**, not by
  region number, so for a point whose region is *not* resident it returns a
  **neighbouring** region's height — a confident-looking number that is the wrong
  place. `cd2ArenaGroundY` therefore refuses to answer unless the point's region
  is resident.
* even for a resident region it is not always reliable (it returned `0x7FFFFFFF`
  for a region a car was standing on), so a missing height is *logged*, not
  alarmed — and a spawn with no authored `y` that gets no answer simply keeps the
  height the level gave the car.

Set `CC_YLOG=1` to print the player's height and vertical velocity for the first
90 frames — the way to tell "settling on the ground" from "falling out of it".

## Run the launcher from anywhere

`tools/arena_menu.bat` finds the repo by walking up for the folder holding both
`JERICHO\MODS\cainescrossfire` and `src_rebuild`, and uses that copy's arenas and
editor even when you run the build's own copy of the script. That matters: the
build mirrors `MODS` into the output folder, and a launcher that resolved its
paths by counting directory levels up from itself put arenas in the mirror, where
the game never looks.

## Source layout

| file | what |
|---|---|
| `arenas/profile.h` | the `CD2_ARENA_PROFILE` row (region + spawns) and the registry API |
| `arenas/registry.c` | the manifest: the four built-ins + the file scan |
| `arenas/arenafile.c` | the `.cca` reader/writer |
| `arenas/arena.c` | the runtime: player placement, the opponent spawn lookups, the barrier, the fallback notice |
