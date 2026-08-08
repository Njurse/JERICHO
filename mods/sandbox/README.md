# Sandbox

A demo JERICHO package proving every capability with zero vanilla edits:

- **No-damage** — a `JER_EVENT_FRAME` hook undoes the player's damage every
  frame (`totalDamage`, zone damage, `needsDenting`).
- **Time-scale** — replaces the `JER_OVERRIDE_SLOT_SIM` (the world step): at
  `4096` it runs every frame, below that every Nth frame (world frozen at 0
  while the game still renders).
- **Spawn** — every 300 frames it finds a free `car_data` slot and calls
  `InitCar` to drop a civ car a couple of car-lengths in front of the player.

Custom events: firing `JER_EVENT_MODULE_CUSTOM + 10` toggles no-damage.

Build id: `sandbox` (must match `mods/sandbox/` in `--with-mods`).
