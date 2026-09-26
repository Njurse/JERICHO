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
- **Reaching the game** — `arenas/` is part of the mod, so a normal build mirrors
  it to `bin\...\MODS\cainescrossfire\arenas\`, which is what the game scans; the
  launcher's *Sync* does it without a build.
- **Open top-down** — `python tools/arenaedit.py <the .cca>` (no arguments opens
  every arena in the folder).
- **Edit in-game** — launch with `-cceditor` and pick the arena (see below).

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
  named weapon (its short code name, e.g. `missile`, `homing`, `shotgun` — or its
  display name) plus `ammo` rounds (default 5), and `pickup: health x z [amount]`
  removes `amount` damage units from the car's `totalDamage` (default 2500). A
  module-owned car (the player or an opponent) driving within **320 units** collects
  it; it then disappears and comes back after **900 frames (30s)**. Each pickup
  draws as a bar-and-diamond: **green for health, amber for a weapon**. Civ traffic
  drives over them freely.
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
| **SELECT** | save to `MODS/cainescrossfire/arenas/<name>.cca` (and update the live arena) |
| **START** | reload the arena from its file |

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

## Source layout

| file | what |
|---|---|
| `arenas/profile.h` | the `CD2_ARENA_PROFILE` row (region + spawns) and the registry API |
| `arenas/registry.c` | the manifest: the four built-ins + the file scan |
| `arenas/arenafile.c` | the `.cca` reader/writer |
| `arenas/arena.c` | the runtime: player placement, the opponent spawn lookups, the barrier, the fallback notice |
