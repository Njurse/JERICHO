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
- **Base FOV override** (default ON, 82° horizontal) with a pitch-linked
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
- Joystick Look — right-stick orbit/look control (ON by default; turn it
  off to fall back to the stock look buttons)
- Foot Distance / Foot Offset / Foot Height — on-foot camera framing
- Car Distance / Car Offset (%) — in-car framing
- Shoulder — which shoulder the camera (and the aiming arm) sits on
- Invert Look X / Invert Look Y
- Base FOV — target horizontal FOV (55–90°) + on/off
- Laser Sight — Off / Red / Green / Blue / White
- Camera Mod — ON/OFF kill-switch (disables the mod's camera + movement,
  back to the stock camera — use it to isolate a broken view)

Everything persists to **`JERICHO/CONFIG/d2pl.ini`** via the JERICHO config
API and is restored on the next boot (a config version bump resets stale
values from older builds).

## Testing on foot: `-onfoot`

Boot args spawn the player in a car; add `-onfoot` to imperceptibly swap
that spawn car out for the player walking (the d2pl module activates the
pedestrian, switches control, and pings the car out once the level is
playable):

```
REDRIVER2_dev.exe -nointro -nofmv -level chicago -onfoot
```

## Camera behaviour (GTA IV-style)

- **Two states**: Natural (the camera eases toward a blended natural heading
  — for cars that's a speed-weighted blend of the car body heading and its
  ground velocity, with a slip-angle boost during drifts) and View
  Manipulation (right stick drives the orbit directly; the pan is a pure
  spherical arc at a frozen radius, focal snapped to the car). Release
  holds for a moment, then settles back lazily.
- **On foot**: the camera sits high and far (~1040 back, ~300 up) looking
  down at the player; movement is camera-relative and the player ALWAYS
  runs forward toward the stick heading (never walks backward) — from a
  standstill he pivots almost instantly, while running he arcs around
  (even a 180). While the right stick is held, the movement reference
  frame freezes so panning can't bend the running line.
- **Speed lens**: smoothed speed subtly pulls the car camera back, raises
  it, and widens the FOV (base FOV default 82°).

## Ped vs vehicle scale

Driver 2 anchors the player ped ~130 units above the ground (`AnimatePed`
in pedest.c) — the whole ped is ~278 units tall (measured from the skeleton
extent), while a car is roughly 4× taller. The on-foot camera works in the
ped's own scale at a GTA-like distance — ~450 units back / ~90 to the side
/ ~60 below the body centre (~1.6 ped-heights behind) — so on foot and in
a car feel cohesive instead of the on-foot camera being parked 10× too
close.

## Camera diagnostics

While in-game the module logs the camera state to **`REDRIVER2.log`** every
60 frames (and immediately on a collision push):

```
[d2pl] cam: pos=(x,y,z) ang=(vx,vy) scr_z=290 collide=0
```

- `pos` / `ang` — the final camera position and angle; sanity-check them
  (huge values or wildly changing angles = a broken transform)
- `scr_z` — the projection distance (≈216–290 with the FOV override on)
- `collide` — 0 = clear, 1 = a clip push happened, 2 = could not clear

If the view is solid black, check these lines: a `collide` that stays 1-2
or a `pos` stuck at the player's coordinates points at the clip logic;
wild `ang` values point at the orbit math. The **Camera Mod** toggle
isolates the mod camera from the stock one.

## Tuning

All defaults live at the top of `d2pl.c` (the `gS` settings block and the
`#define` constants). Values changed in-game are written back to the config
file; hand-edit the file (while the game is closed) for fine tuning.
