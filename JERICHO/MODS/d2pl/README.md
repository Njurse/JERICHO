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
  stick is centred. The heading is written through the shared JERICHO
  player/owned-ped hook gate, so it also works with mp's custom player model.
- **Look** — right stick and mouse own the orbit: the camera stays where the
  player leaves it (yaw momentum coasts on release). Vertical look is a rate
  with momentum, using the same units/frame scale as horizontal; it holds
  where you leave it (no auto-centre). While aiming the orbit is held so the
  shot does not drift.
- **Aim camera** — lerps in over the shoulder (distance 700 → 380, height
  240 → 190, shoulder 0 → 105) so the shot is visible past the player.
- **Camera rig** — the module owns the on-foot transform: placed behind the
  view yaw, terrain-clamped through the host `map_height` query, capped to the
  engine's collision-checked camera distance (pull-in only, since the rig
  gives up `CheckScenalyCollisions`), and smoothed with a context-jump snap.
- **Arm pose** — while aiming, the whole right chain is aimed at the
  camera-centre aim point: the rotation channel turns the upper arm from its
  rest vector, the forearm and hand stay straight, and the position channel
  lays the joints on the ray. The torso and head follow the aim in yaw and
  pitch. Only the upper body is snapshotted/restored (`JOINT_1`, `HEAD`,
  `RSHOULDER`, `RELBOW`, `RHAND`); the legs/lower body keep the engine's
  locomotion animation. Under mp the drawn player ped can be a different
  pointer from the movement ped; the pose tracks the drawn ped (see
  `API_NOTES.md`).
- **Hold types** — the upper-body limits and elbow/hand offsets are a data
  table per holdtype (`D2plHoldtype`, `d2pl_holdtype.c`), chosen by the
  equipped weapon (`D2plActiveHoldtype()`); see `HOLDTYPES.md`. Firing adds a
  short arm buckle + torso recoil; hits spawn sparks/debris and a black puff
  (`jer_fx_impact`).
- **Settings** — Continue → JERICHO Addons → **Driver 2 Parallel Lines**:
  `Look Sens X`, `Look Sens Y` (left/right, 4..128 in steps of 4) and
  `Invert Look X`, `Invert Look Y` (ON/OFF). Values are persisted in
  `JERICHO/CONFIG/d2pl.ini` as `sens_x`, `sens_y`, `invert_x`, `invert_y`.
  The old module's `invert_h`/`invert_v` keys used the opposite vertical
  convention and are deliberately ignored, so the new profile starts normal.
- **Laser sight** — depth-sorted red line from the right hand to the aim
  point while aiming.
- **Debug pistol** — 12 rounds + 60 reserve, semi-auto, no damage model, no
  hand mesh (the data has no pistol model). `L1` aim, `R1` fire, `X` reload.
  Ammo is shown bottom-left; a black X marks the last impact point.

## Files

- `d2pl.h` — shared state, tuning, pad bits, hook declarations
- `d2pl_cam.c` — look, movement, aim camera, mouse poll
- `d2pl_pose.c` — aim point, right-arm chain, torso/neck aim rotation
- `d2pl_menu.c` — the JERICHO Addons settings page and config persistence
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
and removes the spawn car. Test levers (all environment variables, all inert
unless set):

- `D2PL_TEST_AIM=1` — hold aim without a pad (arm pose + laser).
- `D2PL_TEST_STICK=x,y` — pin the local player's left stick (e.g. `0,127`
  for the reverse/toward-camera case, `0,-127` for forward).
- `D2PL_TEST_NOLOOK=1` — ignore right-stick/mouse look, so a desk run is
  reproducible even with a drifting controller.
- `D2PL_TEST_LOOK=x,y` — pin the right stick (e.g. `0,-127` = look up,
  `0,127` = look down) for the vertical-rate check.
- `D2PL_TEST_HOLD=0|1|2` — select the aim/rifle/hip hold preset for an
  unattended pose test.
- `D2PL_CAM_LOG=1` — log the rig/movement/pose state every 30 frames
  (`[d2pl] cam:`, `[d2pl] move:`, `[d2pl] pedmove:`, `[d2pl] pose-entry:`,
  `[d2pl] skel-entry:`, `[d2pl] arm pose:`, `[d2pl] pose:`), including the
  camera offset direction (`offsetDir` vs `yaw + 2048`), the written heading
  and the draw/move ped identities.

The boot log then carries a one-shot line when the pose runs:

```
[d2pl] aim pose: shoulder=(...) aim=(...) reach=... arm=.../...
```

## Debug notes

- The aim point is the camera-centre ray (30,000 units). The laser and the
  arm both use it, so if the arm looks off, the laser should look off the
  same way — that separates a camera/aim issue from a pose issue.
- Under mp/pedhacks the **drawn** player ped and the **movement** ped can be
  different pointers. d2pl tracks them separately (`gD2plDrawPed0` for the
  pose hooks, `gD2plPlayerPed0` for PED_MOVE); a module that reuses the
  movement pointer for pose hooks will stop posing after the character swap.
- No pistol model exists in the shipped data. A hand mesh needs an exported
  bone-model/draw accessor (see `API_NOTES.md`).

## Pose modes (D2PL_POSE_IK)

- unset / `1` (default): Tanner gets the full pose (twist, pitch lean, two-arm IK, idle carry). Other models (mp's OTHER_MODEL Jericho) get a reduced pose while aiming: yaw-only spine/head twist plus weapon-arm IK measured from the model's own rest offsets, with fire buckle; legs, hips and spine pitch are never written.
- `0`: no bone writes (stock animation).
- `2`: force the full Tanner path on every model (debug; folds OTHER_MODEL).

The engine hands PED_POSE a private per-draw copy of the bone rotations, so pose writes never touch the shared animation clip.

