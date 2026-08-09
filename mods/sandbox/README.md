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
The toggle persists via the JERICHO config API (`mods/config/sandbox.ini`).

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

## Custom event

Firing `JER_EVENT_MODULE_CUSTOM + 10` (SANDBOX_CUSTOM_TOGGLE) flips
no-damage — any module or future console command can toggle it.

All module output goes to **`REDRIVER2.log`** (via the JERICHO logger,
wired at boot) — look for the `[sandbox]` lines right after the JERICHO
boot inventory in the log file.
