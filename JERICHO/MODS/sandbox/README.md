# Sandbox — JERICHO sample package

A demo of the full JERICHO module surface with **zero vanilla edits**:

- **No-damage** — the player car's damage is reset every frame (hook `JER_EVENT_FRAME`).
- **Time-scale** — replaces the world step (`JER_OVERRIDE_SLOT_SIM`) to run the
  sim at a fraction of full speed (world frozen at 0x, half-speed, etc.).
- **Sandbox Menu** — a pause-menu overlay that *unpauses the game* (the world
  keeps running) while the menu is open. The pad drives the menu instead of
  the car (`gStopPadReads`), and the menu draws over the world
  (`JER_EVENT_DRAW_OVERLAY`).

## Sandbox Menu

Open: Pause → **Sandbox Menu** (right below Continue). With **Replace Pause
Menu** on (a toggle in the sandbox's main page), pressing START in-game opens
the sandbox overlay directly instead of the engine pause; the **Open Pause
Menu** button below it closes the overlay and opens the normal pause menu.
The toggle persists via the JERICHO config API (`JERICHO/CONFIG/sandbox.ini`).

```
SANDBOX                    (main page — categories)
DAMAGE: 0                  (current-vehicle readouts)
FELONY: 0
> Vehicle                  Vehicle:  Repair Car / Make Car Upright /
  Spawn                            Set Damage / Set Felony /
  World                            Player AI Mode (Manual→Traffic→Cop→Lead)
  Cheats                   Spawn:    Spawn Car / Spawn Object / AI Car
  Replace Pause Menu       World:    Set Time of Day / Set Weather
  Open Pause Menu          Cheats:   the real Driver 2 cheats
  Close                              (Invincibility, Immunity, Secret Car,
                                     Play as Jericho, Mini Cars, Bonus Cars)
                                     + Unlock All
                           AI Car:   Vehicle model (live preview) /
                                     Mode (Civilian/Cop/Lead) /
                                     mode parameter / Spawn
```

The menu renders in a smaller HQ-font scale behind a semi-transparent panel
(in-game UI style). L1/R1 flips pages — every submenu paginates (6 items per
page; the Spawn Object list pages 12 per page) — Triangle backs up (and closes
on the main page), Left/Right adjusts the SET items and the AI Car choices.


The right side of the screen shows a **live preview**: the player's car drawn
with its *final geometry* (CRUMPLE-deformed vertices + damage UVs), rotating
on a turntable. On the Spawn Object page the preview swaps to the selected
level object model.

**Controls while the menu is open**: D-Pad up/down moves the cursor, Cross
selects, Triangle closes. The world keeps simulating behind the menu — traffic
keeps moving, the player's car coasts to a stop (the pad is captured by the
menu).

## Spawning a car from any city

The Spawn page's **Source City** and **Car** rows decide what `Spawn Car` builds,
and they are driven by carhacks when it is installed:

| Row | What it lists |
| --- | --- |
| Source City | carhacks' cities — the four Driver 2 ones and Driver 1's five (MIAMI, FRISCO, LA, NEWYORK, NEWCASTLE). With nothing to ask it is the level you are playing, and it starts there |
| Car | that city's ROSTER — the same per-city list the frontend car screen shows (MIAMI 7 cars, NEWYORK 8, NEWCASTLE 3, …) |

The roster is listed whether or not those cars are resident here yet, because
choosing one that is not **is** the request that brings it in: `Spawn Car` asks
carhacks to import it first (the same `MP_CARQ_LOAD` request mp's Change car
makes) and only says so when it genuinely cannot be loaded. A car from another
city — Driver 1's included — therefore spawns exactly like a domestic one. (The
rows used to offer only what was already resident, which made every foreign city
read "no cars available".)

Each spawn **replaces** the last car that row spawned. The new car is dropped a
couple of car-lengths ahead, which is where the previous one is sitting, and two
overlapping cars is a collision the solver resolves by throwing them both across
the map. `Teleport In` has no such problem — it moves the player into the new car
and removes the old one itself.

Headless: `SANDBOX_TEST_SPAWN=<frames>[,<city>[,<model>]]` drives that path with
no pad — it dumps every offered city's roster and then spawns twice, the second
reusing the first's `car_data` slot, which is what proves the replacement works.

## Custom event

Firing `JER_EVENT_MODULE_CUSTOM + 10` (SANDBOX_CUSTOM_TOGGLE) flips
no-damage — any module or future console command can toggle it.

All module output goes to **`JERICHO.log`** (via the JERICHO logger,
wired at boot) — look for the `[sandbox]` lines right after the JERICHO
boot inventory in the log file.
