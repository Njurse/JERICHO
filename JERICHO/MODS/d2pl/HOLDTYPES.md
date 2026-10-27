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
| `pitchLimit` | spine pitch: `JOINT_1.vx` takes `D2PL_PITCH_SHARE`/4096 of the aim elevation, clamped to this. `D2PL_PITCH_SIGN` flips it |
| `twoHand`, `gripForward`, `gripElbow[3]` | two-handed hold: while aiming, the left arm points at a fore-grip `gripForward` world units ahead of the right hand along the aim, then `gripElbow` (x, y, z) is added to `LELBOW` to fold the reach into a support arm. Needs the left-arm bits in `mask`. The rifle uses 45 and (0, 0, 200); tune in game |
| `poseHook`, `poseUser` | your code, run last in `PED_POSE` (see "Custom animation") |

Add one by calling `D2plHoldtypeRegister(&h)` in `D2plHoldtypeInit` (or from
any init after it). It returns the id; weapons refer to it by id, and
`D2plHoldtypeFind("name")` looks one up. Ids 0–2 are fixed (`D2PL_HOLD_*`).

The pose applies the holdtype of the **equipped weapon**
(`D2plActiveHoldtype()`): the aim branch while L1 is held, the idle-hold branch
otherwise.

**Keeping the twist sane.** The clamps are tight (pistol: spine 40°, neck
30°; rifle: 35° and 25°). Standing and aiming, the whole body turns toward the
aim once the twist passes 20° (`D2PL_TURN_START`, up to 14° a frame). Moving
and aiming more than 80° off the view backpedals instead. Past both clamps
the aim IK fades out, so the arms never stick out sideways from a twisted
torso.

**The built-ins.** *pistol*: one hand, the arm hangs low at the side when
idle (light blend), the arm locks on the target when aiming, the left arm
stays free. *rifle*: two hands, carried across the body with the muzzle
low-forward when idle (firm blend), shouldered with the support hand on the
fore-grip when aiming, a smaller twist so the body turns sooner, and more
lean.

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
- A shot is `D2plFireShot`: ammo, sound, the impact marker, then the hit test.
- **Damage stub**: `D2plHitScan` walks the shot from the right hand toward the
  aim point (up to 8000 units, in 120-unit steps) and asks
  `jer_npc_nearest` for a non-player ped within 120 units. The first hit takes
  the weapon's `damage` (pistol 25, MG 10, debug 2) from a d2pl-side 100 HP
  (the engine's civilians have no health), the marker jumps onto the hit and
  the HUD shows `HIT n%` or `DOWN`. Nothing reacts yet: a knockdown or
  death needs an engine ped action.
- In game: **L1** aims, **R1** fires, **X** reloads, and **Select** cycles
  PISTOL → MG → DEBUG (R2 stays the stock look-right). Select is **Space** on the keyboard and
  **Back/View** on a controller by default. The small lower-left HUD shows
  the weapon, the ammo and the last hit (world x y z and the ped's HP %).

## Sounds

**Optional WAVs.** If `MODS/d2pl/sounds/<name>.wav` exists it plays instead
of the placeholder (`jer_sound_mod_wav_play`, desktop only, master volume,
non-positional; the repo copy `JERICHO/MODS/d2pl/sounds/` is checked first in
a dev build). Names: `pistol_fire`, `pistol_reload`, `mg_fire`, `mg_reload`,
`debug_fire`. A missing file is looked for once, then the FE sound plays.
These are loose mod files, not LEV-bank assets; how game-ready gun sounds
should live in the LEV banks is still to be decided.

**Placeholders.** `D2plWeaponInit` calls
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

- **Pitch**: the spine leans with the aim (`pitchLimit`) on `JOINT_1` only;
  the sign is unverified in game (`D2PL_PITCH_SIGN`).
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
- **Left hand**: the support arm is aim-and-bend, not true two-bone IK, and
  uses last frame's positions, so it can lag a frame at speed.
- **Root yaw**: the IK learns the offset between the host's player `dir` and
  the ped root yaw from the engine's own result (logged once as `[d2pl] root
  yaw offset measured`). If the arm ever points the wrong way, check that line.
- **SFX**: these are placeholder menu samples, not positional.
