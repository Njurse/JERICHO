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
work. Remove this package from `--with-mods` and the game is vanilla denting —
no other changes needed.

Build id: `crumple` (must match `mods/crumple/` in `--with-mods`).

Tuning: `CRUMPLE_PARAMS gCrumpleParams` at the top of `crumple.c`.
