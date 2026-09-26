# Arenas

An **arena** is a bounded place a match happens on. Where a [vehicle
profile](PROFILES.md) says *what car* you fight in, an arena says *where*: which
city, which mission layout, the **region** that keeps the fight in (the
barrier), and the **spawn points** (position + heading) the cars start at.

The registry is the same shape as the vehicle one (`profiles/registry.c`): a
manifest of rows indexed by an id, in `arenas/registry.c`. The four **built-in**
arenas are the cities' small multiplayer maps — identity only. Every **custom**
arena is an authored data file under `JERICHO/CONFIG/arenas/` that the game
loads at boot, so a new arena needs no rebuild — only a file the editor writes.
A file whose `arena:` name matches a built-in (e.g. `chicago`) *replaces* it, so
the built-ins are themselves editable.

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

One arena per file, `JERICHO/CONFIG/arenas/<name>.cca`. A line-based, diffable
text format — written by hand, by the in-game editor, or by the Python top-down
editor (`tools/arenaedit.py`), all three reading and writing exactly this.

```
# a comment runs to the end of the line
arena: dockyard_duel      # internal name (the file key; also overrides a built-in)
name: Dockyard Duel       # on-screen name (default: the internal name)
city: CHICAGO             # CHICAGO | HAVANA | VEGAS | RIO, or 0..3
mp: 1 0                   # <load the small mp map?> <which layout 0|1>. 'full' = whole city
region: -20000 -30000 20000 30000   # the barrier, world units: x0 z0 x1 z1. 'none' = whole level
spawn: -5000 2000 1024    # x z heading. Repeatable; the FIRST is the player, the rest the opponents
spawn: 5000 2000 2048
spawn: 3000 -6000 0
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
- **`spawn`** is a world position and a heading in PSX angle units (0..4095).
  Spawn 1 is the player; 2.. are the opponents **in order** (opponent 0 is the
  second `spawn:` line, and so on). Up to 16. A `spawn` inside scenery is the
  author's to fix — the editor shows where they are, and authored spawns are
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

`JERICHO/CONFIG/arenas/*.cca`, scanned at boot. With `-ccmenu` (or the main
menu's Deathmatch entry) the arena menu lists every registered arena; picking one
loads its city/layout and makes it the match's current arena, so its spawns and
barrier apply.

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
| **SELECT** | save to `CONFIG/arenas/<name>.cca` (and update the live arena) |
| **START** | reload the arena from its file |

The shoulders also carry weapon prev/next/fire while driving, so park the car
before tapping them. A scripted run can drive the editor headlessly through the
debug driver's `pad:` step with `JERICHO_CC_INJECT=1` — the editor reads that
injected mask alongside the live pad:

```
5:pad:4      # L1  -> place a spawn at the car
7:pad:0
12:pad:8     # R1  -> next slot
20:pad:2     # R2  -> corner A ... and again for the rect
28:pad:256   # SELECT -> save
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

## Source layout

| file | what |
|---|---|
| `arenas/profile.h` | the `CD2_ARENA_PROFILE` row (region + spawns) and the registry API |
| `arenas/registry.c` | the manifest: the four built-ins + the file scan |
| `arenas/arenafile.c` | the `.cca` reader/writer |
| `arenas/arena.c` | the runtime: player placement, the opponent spawn lookups, the barrier, the fallback notice |
