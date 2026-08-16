# CRUMPLE

The flagship JERICHO package: impact-driven car damage for REDRIVER2.

- **Body**: impact-driven vertex deformation (dents, crumples) layered on the
  original damaged-model system — the engine's texture/UV damage still works.
- **Wheels**: per-wheel bend (camber / toe / steering deviation), compounding
  directional damage, and body slump toward a vertically-bent wheel.
- **Physics**: wheel-damage-aware raycasts (bounded), lateral scrub, and
  surface-roughness amplification on grass scaled by cumulative wheel damage.
- **Debug**: the **Crumple Debug** pause submenu (D-Pad Deform, Buddha Mode,
  Repair Car) and the overlay impact readout, via `JER_EVENT_PAUSE_MENU` /
  `JER_EVENT_GET_IMPACT_INFO`.

## How it plugs in

The engine never calls CRUMPLE directly. It fires JERICHO events
(`JER_EVENT_COLLISION`, `JER_EVENT_DENT_PASS`, `JER_EVENT_GET_WHEEL_BEND`, ...)
and this module's handlers (registered in `jer_module_crumple_entry`) do the
work. Remove this package from `JERICHO/MODS` and the game is vanilla denting —
no other changes needed.

Build id: `crumple` (the folder `JERICHO/MODS/crumple/` is auto-discovered by premake).

Tuning: `CRUMPLE_PARAMS gCrumpleParams` at the top of `crumple.c`.

All module output goes to **`REDRIVER2.log`** (via the JERICHO logger,
wired at boot); look for the `[crumple]` lines right after the JERICHO
boot inventory (which lists this module's ~12 hooks).
