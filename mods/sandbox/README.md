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

Open: Pause → Debug Options → **Sandbox Menu**.

```
SANDBOX
DAMAGE: 0
FELONY: 0
> Spawn Car          (spawns a copy of your current car model)
  Spawn Object       (page: level objects — cones, barriers, phone boxes...)
  Spawn AI Car       (spawns a civ car)
  Repair Car         (full repair incl. CRUMPLE reset)
  Make Car Upright   (Back on Wheels)
  Set Damage         (select, then Left/Right to change, Cross to leave)
  Set Felony
  Set Time of Day    (Dawn → Day → Dusk → Night)
  Set Weather        (Dry → Rain → Wet)
  Set Cheats         (invincibility cheat toggle)
  Close              (or Triangle)
```

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
