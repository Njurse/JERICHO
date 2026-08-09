# Driver 2 Parallel Lines (d2pl)

The big camera + weapon mod for Driver 2, as a single JERICHO module.

## What it does

**Camera — a proper dual-analog third-person camera (GTA4-style):**
- Right stick **orbits** the camera around Tanner (X) and **pitches** it
  (Y), clamped to ~±60° so you can never look straight up/down.
- On release the camera holds where you left it, then eases back behind
  you after a short idle timeout (shorter in cars).
- **Reverse grace**: at a standstill in a car the camera holds completely
  still — no auto swing behind the car until you're deliberately moving
  backward.
- **Base FOV override** (default ON, 72° horizontal) with a pitch-linked
  adjustment: looking down narrows the FOV, looking up widens it.
- **On foot**: camera-relative movement — the left stick moves Tanner
  relative to where the camera looks, with limited turn rate while running
  and a fast pivot from standstill.
- **In car**: GTA4-style framing — the camera sits closer/lower and to the
  left of the car, offset scaled to ~25-30% of the car's bounding-box width,
  and pulls back as you gain speed.

**Weapons — an overridable system:**
- `WeaponBase` template (see `d2pl.h`): fire/reload/ammo/properties + arm
  pose, mesh attach, aim point — subclass and register your own.
- L1 = **aim**: camera zooms in, FOV narrows, Tanner presents the weapon.
- R1 = **primary fire**, R2 = **secondary fire** (frontend menu accept /
  backout sounds). While aiming, the stock L2/R2 look buttons are suspended.
- Ammo HUD bottom-left, **laser sight** from the hand to the aim point
  (color-cycled), and a big black **impact marker** at the shot's aim point
  for 1 second so you can trace where shots land.
- Demo pistol holds the `BOMB` model as a placeholder mesh.

## Settings

Pause → **D2PL Settings** (right below Restart):
- Look Sens X / Look Sens Y — view-change sensitivity
- Foot Distance / Foot Offset — on-foot camera framing
- Car Distance / Car Offset (%) — in-car framing
- Shoulder — which shoulder the camera (and the aiming arm) sits on
- Invert Look X / Invert Look Y
- Base FOV — target horizontal FOV (55–90°) + on/off
- Laser Sight — Off / Red / Green / Blue / White

Everything persists to **`mods/config/d2pl.ini`** via the JERICHO config
API and is restored on the next boot.

## Tuning

All defaults live at the top of `d2pl.c` (the `gS` settings block and the
`#define` constants). Values changed in-game are written back to the config
file; hand-edit the file (while the game is closed) for fine tuning.
