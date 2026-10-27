# d2pl — Driver 2 Parallel Lines

Driver 2 Parallel Lines (d2pl) is the on-foot ped module: camera-relative
movement, dual-stick/mouse look, an aim arm pose that points at the aim point,
a laser sight and a baked-in debug pistol.

This is the module that replaced the previous camera/weapon d2pl
implementation; that code remains in git history. It is **compiled in** today
(no `runtime` key in `mod.toml`). [`API_NOTES.md`](API_NOTES.md) lists the
hook-consolidation ideas and the host API gaps that still stop it being a pure
DLL addon.

## What it does

- **Movement** — left stick is camera-relative, always run-forward, with an
  analog speed lerp. The stock D-pad/keyboard path is left alone when the
  stick is centred.
- **Look** — right stick and mouse orbit the camera; pitch tilts the view.
  Releasing the stick lets the engine settle the camera behind the ped.
  While aiming the orbit is held so the shot does not drift.
- **Aim camera** — pulls in over the shoulder and lifts to shoulder height.
- **Arm pose** — while aiming, the right arm is laid out along the world ray
  from the shoulder to the camera-centre aim point, so it always points at
  what the laser points at (see `API_NOTES.md` for the old bug).
- **Laser sight** — depth-sorted red line from the right hand to the aim
  point while aiming.
- **Debug pistol** — 12 rounds + 60 reserve, semi-auto, no damage model, no
  hand mesh (the data has no pistol model). `L1` aim, `R1` fire, `X` reload.
  Ammo is shown bottom-left; a black X marks the last impact point.

## Files

- `d2pl.h` — shared state, tuning, pad bits, hook declarations
- `d2pl_cam.c` — look, movement, aim camera, mouse poll
- `d2pl_pose.c` — aim point, arm pose, torso/neck aim rotation
- `d2pl_weapon.c` — debug pistol, laser, ammo HUD
- `d2pl_boot.c` — the `-onfoot` spawn swap (compiled-in only)
- `d2pl.c` — module entry and hook registration

## Build

```
JERICHO\build_game.bat          (or: python JERICHO\build.py game all)
```

`d2pl` is linked into the exe; enable it in Options → JERICHO (it is off by
default) or force it for one boot with `-mod d2pl`.

## Test

```
JERICHO_dev.exe -nointro -nofmv -level 0 -onfoot -frames 300 -mod d2pl
```

`-onfoot` is implemented by this module (`d2pl_boot.c`): it activates the ped
and removes the spawn car. Add `D2PL_TEST_AIM=1` to hold aim without a pad,
which exercises the arm pose and the laser. The boot log then carries a
one-shot line when the pose runs:

```
[d2pl] aim pose: shoulder=(...) aim=(...) reach=... arm=.../...
```

## Debug notes

- The aim point is the camera-centre ray (30,000 units). The laser and the
  arm both use it, so if the arm looks off, the laser should look off the
  same way — that separates a camera/aim issue from a pose issue.
- No pistol model exists in the shipped data. A hand mesh needs an exported
  bone-model/draw accessor (see `API_NOTES.md`).
