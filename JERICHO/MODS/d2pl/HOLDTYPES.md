# d2pl holdtypes, weapons and custom animation — where to start

A starting guide for extending the on-foot weapon pose. File map first, then
the three things people add (holdtypes, weapons, sounds), then the limits.

## File map

| File | Owns |
|---|---|
| `d2pl_holdtype.c` | the holdtype registry and the built-ins (`none`, `pistol`, `rifle`) |
| `d2pl_pose.c` | `PED_POSE`: torso/neck twist, right-arm IK, the holdtype pose hook. `PED_SKELETON`: read-only (shoulder, root-yaw calibration, muzzle) |
| `d2pl_weapon.c` | the weapon table, fire modes, reload, laser, HUD, weapon cycle |
| `d2pl_cam.c` | movement, aim camera, the backpedal hook (`D2PL_BACKPEDAL`) |
| `src_rebuild/Game/C/jer_sound.c` | `jer_sound_request_menu_bank` / `jer_sound_menu_play` (placeholder SFX) |
| `src_rebuild/Game/C/JERICHO/docs/ped-animation.md` | the engine's bone transform chain: read section 3 before touching the pose |

## The one rule: write rotations in PED_POSE

`newRotateBones` reads each bone's `pvRotation` once, right after
`JER_EVENT_PED_POSE`, and bakes the meshes with it. A write made later (in
`PED_SKELETON` phase 0) moves bone *positions* without re-orienting the mesh,
which is exactly what made the old aim arm crinkle. So:

- **pose = rotations, in `PED_POSE`**;
- `PED_SKELETON` is for reading results (joint positions, the muzzle).

`pvRotation` points into the shared per-ped-type motion buffer. `D2plOnPedPose`
saves every rotation before it writes and restores them at the start of the
next pose, so overrides never leak into the clip. Keep any new write inside
that save/restore window (that is, inside `D2plOnPedPose` or a pose hook).

Bone matrices follow the engine exactly: `M[b] = M[parent] * Rz*Ry*Rx(-x, y, z)`
of the *parent's* rotation, with the root `RotYXZ(ped dir)`. So the rotation on
`RSHOULDER` orients the upper arm (the `RELBOW` offset), and the rotation on
`RELBOW` orients the forearm (the `RHAND` offset). `M3FromBone`/`M3ToBone` in
`d2pl_pose.c` convert between the two forms.

## Holdtypes

A holdtype (`D2plHoldtype`, `d2pl.h`) says how the upper body carries a weapon:

| Field | Meaning |
|---|---|
| `mask` | bit per `JER_LIMB_*` the holdtype may write |
| `aimIk` | while aiming: right-arm IK at the aim point |
| `idleIk`, `idlePitch`, `idleWeight` | weapon out, not aiming: blend the arm toward forward-and-down by `idlePitch`, `idleWeight`/4096 of the way |
| `torsoLimit`, `headLimit` | the aim yaw twist: the spine (`JOINT_1`) takes up to `torsoLimit`, the neck the rest up to `headLimit`. Both are added to the clip's own rotation |
| `allowBackpedal` | lets the backpedal hook run for this holdtype |
| `poseHook`, `poseUser` | your code, run last in `PED_POSE` (see "Custom animation") |

Add one by calling `D2plHoldtypeRegister(&h)` in `D2plHoldtypeInit` (or from
any init after it). It returns the id; weapons refer to it by id, and
`D2plHoldtypeFind("name")` looks one up. Ids 0–2 are fixed (`D2PL_HOLD_*`).

The pose applies the holdtype of the **equipped weapon**
(`D2plActiveHoldtype()`): the aim branch while L1 is held, the idle-hold branch
otherwise.

## Weapons

`gWeaponDefs[]` in `d2pl_weapon.c`: one row per weapon.

```c
{ "MG", D2PL_HOLD_RIFLE, D2PL_FIRE_AUTO, 6, 30, 120, 60, JER_MENU_SFX_ACCEPT, JER_MENU_SFX_BACK },
/* name, holdtype,  fire mode,     rounds/s, clip, reserve, reload frames, fire sfx, reload sfx */
```

- Fire modes: `D2PL_FIRE_SEMI` (one shot per press), `D2PL_FIRE_AUTO`
  (`roundsPerSec` while held, credit-based so it averages exactly at the 30 Hz
  tick), `D2PL_FIRE_EVERY_FRAME`.
- `clipSize = 0` means endless ammo and no reload.
- A shot is `D2plFireShot`: ammo, sound, the impact marker. Damage, tracers and
  hit tests go there.
- In game: **L1** aims, **R1** fires, **X** reloads, and **R2** cycles
  PISTOL → MG → DEBUG. The HUD shows the weapon name and ammo.

## Sounds

These are placeholders for now. `D2plWeaponInit` calls
`jer_sound_request_menu_bank()`. After that, every level load appends the
frontend menu bank (`VOICES2.BLK` lump `SBK_ID_MENU`) to the SFX bank, before
the car-sound memtop is saved, so car imports can't overwrite it.
`jer_sound_menu_play(JER_MENU_SFX_ACCEPT)` (Cross) is the fire sound and
`JER_MENU_SFX_BACK` (Triangle) is the reload sound.

How to swap in real SFX:
1. Build a sound bank lump with the gun samples, in the same format as the
   other `VOICES2.BLK` lumps. JERICHO already loads embedded banks for Driver 1
   cars (`LoadEmbeddedCarSoundForSlot`, `gamesnd.c`); follow that pattern for a
   mod-shipped bank.
2. Load it from `jer_sound_load_requested_banks` in the same way, record its
   base index, and add a play call. Use `Start3DSoundVolPitch` at the hand
   position (`gD2plHand`) so it's positional.
3. Point `fireSfx`/`reloadSfx` at the new ids.

## Custom animation

There are two levels:
- **Keyframe overrides**: set a holdtype `poseHook`. It runs in `PED_POSE` after
  the twist and IK, with the skeleton. Write `jer_anim_bone_rotation(skel, limb)`
  for bones in your `mask`. This covers a fixed grip, a reload gesture driven
  by the weapon's `reloading` counter, or a recoil kick on fire.
- **Custom clips**: a clip is per-bone rotations over time. Store it as
  `JER_BONE_ROT frames[N][JER_LIMB_COUNT]` and sample it in a pose hook, using
  your own frame counter (or `api->game->frame()`). Blend per bone toward the
  clip value with the mask. Nothing in the engine has to change for this.
  Upper-body clips layer over the stock legs naturally, because the hook only
  writes the masked bones.

Swapping the stock *leg* clip (for example, a backpedal cycle) has to change
the ped's action, not its rotations. `jer_npc_set_action`/`jer_npc_set_gait`
are the starting points.

## Known limits / next steps

- **Pitch**: the twist is yaw only. The arm IK does follow the aim pitch, but
  the spine doesn't lean. Next step: share the aim pitch between `LOWERBACK`
  and `JOINT_1` (`vx`) in the aim branch.
- **Backpedal**: on by default (`D2PL_BACKPEDAL` 1, runtime toggle in the
  pause page). While aiming with the stick more than ~100° off the view,
  PED_INPUT sends the stock walk-back (`GOBACK`, `SetupBack`) instead of
  forward, PED_MOVE keeps the body facing the aim and backs up at speed
  -10 to -20 (stick scaled), and `jer_npc_set_reverse_anim` sets ped flag 4
  so `AnimatePed` plays the walk frames in reverse. d2pl clears the bit only
  when it set it: on stopping, releasing aim, turning forward, entering a car
  or pressing the car-entry button (the engine uses the same bit for the
  door animation). Known hitch: the reverse wraps at frame 15 but the forward
  cycle at 16, so one frame per loop repeats. A real backpedal clip would
  remove it; that needs a leg-clip swap (`jer_npc_set_action`).
- **Hips**: the whole body still turns as one (`pPed->dir`). A hips-versus-
  upper-body split beyond the spine twist needs the root yaw separated from
  `LOWERBACK`.
- **Left hand**: there's no left-arm IK yet, so the rifle is held one-handed.
  Add a second `PoseArm` for `LSHOULDER`/`LELBOW` that targets a point on the
  weapon.
- **Root yaw**: the IK learns the offset between the host's player `dir` and
  the ped root yaw from the engine's own result (logged once as `[d2pl] root
  yaw offset measured`). If the arm ever points the wrong way, check that line.
- **SFX**: these are placeholder menu samples, not positional.
