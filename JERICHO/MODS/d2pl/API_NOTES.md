# d2pl — hook and API notes

Working notes for Driver 2 Parallel Lines (d2pl), the on-foot ped module.
Two goals: keep the addon a **runtime DLL** (no game rebuild), and shrink the
event surface it has to register.

This module replaced the old camera/weapon d2pl. That implementation is in git
history; only the on-foot path was carried forward.

## Hooks d2pl uses today

| Hook | Why |
|---|---|
| `JER_EVENT_CAMERA_LOOK` | own the yaw/pitch; always suppress stock look and grip the orbit |
| `JER_EVENT_CAMERA` | the on-foot rig: placement, aim blend, pitch orbit, terrain/collision cap, cache the view for the aim ray |
| `JER_EVENT_PED_INPUT` | right/left stick split, camera-relative heading, pad rewrite; zero the in/out steer |
| `JER_EVENT_PED_MOVE` | set ped heading and analog speed after the runner re-arm (player/owned-ped gate) |
| `JER_EVENT_PED_POSE` | right-arm chain (shoulder/elbow/hand) + torso/head yaw and pitch toward the aim (the only rotation channel; DrawTanner + DrawCharacter) |
| `JER_EVENT_PED_SKELETON` | phase 0 arm pose; phase 1 hand position for the laser |
| `JER_EVENT_FRAME` | mouse poll, weapon tick (aim/fire/reload), laser, marker |
| `JER_EVENT_DRAW_OVERLAY` | ammo readout |
| `JER_EVENT_GAME_START` | forget the previous level's ped pointer and saved rotations |

## The on-foot camera rig

The camera is module-owned on foot (`cameraEnabled`), not a set of deltas on
the stock chase cam:

- `CAMERA_LOOK` keeps the owned yaw (`gD2plViewYaw`) and pitch, always sets
  `suppress` + `gripOrbit`, and mirrors the yaw into `PLAYER.cameraAngle`
  (yaw − 2048, the engine's offset direction).
- `CAMERA` places the camera at `base − forward(yaw)·dist + right·lateral`,
  height `−base[1] − height`. The aim blend lerps distance 700 → 380, height
  240 → 190 and shoulder 0 → 105.
- Pitch is a spherical orbit while not aiming and a view tilt while aiming,
  cross-blended by the same aim factor.
- The rig has no `CheckScenalyCollisions`; it clamps the y to the host
  `map_height` terrain line and caps the distance to the engine's own
  collision-checked camera distance (pull-in only). The final position is
  smoothed, with a context-jump snap for level/spawn changes.
- The camera looks at the chest/lateral target, so the player sits beside the
  crosshair while aiming and the shot is not hidden by the body.

## Settings page

`d2pl_menu.c` registers a `JER_PAUSE_MENU` page under **Continue → JERICHO
Addons** with id `d2pl`:

- `Look Sens X` / `Look Sens Y` — left/right adjust, 4..128 in steps of 4,
  dynamic value labels.
- `Invert Look X` / `Invert Look Y` — CROSS toggles ON/OFF.

The rows write the live `gS` values and persist all four keys
(`sens_x`, `sens_y`, `invert_x`, `invert_y`) through `jer_config`. The old
profile's `invert_h`/`invert_v` keys used the opposite vertical convention and
are deliberately ignored, as is `camera_enabled`, so the new profile starts
normal.

## Right-arm aim (PED_POSE)

The position channel (`PED_SKELETON` phase 0) places the right elbow and hand
on the shoulder→aim-point ray. The rotation channel (`PED_POSE`) then turns
the upper arm from its rest vector to the aim direction expressed in the
upper-body frame; the elbow and hand are kept straight, and the torso/head
follow the aim in yaw and pitch. `D2PL_RSHOULDER_YAW_OFFSET` /
`D2PL_RSHOULDER_PITCH_OFFSET` are the calibration knobs; the whole-skeleton
save/restore keeps the shared motion buffer clean.

To stop the arm wiggling, `PED_POSE` uses the DRAWN ped's own `dir` (the new
`JER_ARGS_PED_POSE.dir` field), not the movement ped's facing, and the exact
shoulder→aim direction cached by `PED_SKELETON` phase 0. The upper-arm pitch
is expressed relative to the torso's pitch, and the cached direction is
lightly smoothed. Rotation and position therefore share one ray instead of
fighting each other.

The snapshot/restore covers only the five bones the pose writes (`JOINT_1`,
`HEAD`, `RSHOULDER`, `RELBOW`, `RHAND`); the legs/lower body are left to the
engine's locomotion animation. A whole-skeleton restore made the legs look
frozen, especially under mp where the drawn ped and movement ped differ.

The upper-body limits, IK switches and support-arm grip come from the
equipped weapon's holdtype (`D2plHoldtype`, `d2pl_holdtype.c`; see
HOLDTYPES.md), selected by `D2plActiveHoldtype()` - there is no separate hold
table or `D2plSetHold()` any more. The twist is
`jer_anim_aim_diff(aimYaw, bodyYaw)` with `bodyYaw = (PED_POSE.dir - 2048)`
(positive = aim to the right, 0 = straight ahead; no extra 180 degrees).

Fire feedback: each shot kicks `gD2plRecoil` (pistol full pulse, rifle a
softer kick refreshed per MG round; decays 5/8 per tick). The pose turns it
into an arm buckle (upper arm drops, forearm climbs) and a torso recoil
(JOINT_1 leans back and twists the weapon-hand shoulder back). Impacts call
the engine's `jer_fx_impact` (car-collision sparks/debris + a black smoke
puff) on a ped hit or where the shot meets the ground.

## Drawn ped vs movement ped

Under mp/pedhacks the **drawn** player ped can be a different pointer from the
movement ped. d2pl tracks them separately: `gD2plDrawPed0` is learned from the
draw hooks for `PED_POSE`/`PED_SKELETON`, while `gD2plPlayerPed0` is the
`PED_MOVE` ped. Reusing the movement pointer for the pose hooks makes the pose
stop after the character swap (observed: draw ped `...65C0`, movement ped
`...6550`).

## Ped handle trap

`jer_npc_*` handles **are** the ped pointer: `jer_npc_spawn` returns
`(JerNpc*)pPed` and every setter casts the handle straight back to
`LPPEDESTRIAN`. Do not wrap the ped in a stack `JerNpc` and pass `&wrapper` —
the writes land in the wrapper and the ped never turns. d2pl's `PED_MOVE` uses
`ped = (JerNpc*)a->ped` directly.

Movement/pose/skeleton hooks are gated on the shared JERICHO ped ownership
rule (player `padId >= 0`, or `jer_npc_owned`), **not** on `TANNER_MODEL`, so
mp's `OTHER_MODEL` player character is driven too.

## Redundant / combinable hooks

These are candidates for a shared handler or a common "ped frame" event. They
are listed roughly in order of how much they would simplify a ped addon.

1. **`PED_POSE` + `PED_SKELETON` (phase 0)** — both fire inside
   `DrawTanner`/`DrawCharacter`, one right after the other, and both write the
   same skeleton on two
   different channels (rotation before `newRotateBones`, position after it).
   A single `PED_DRAW` event with the stage order made explicit would remove
   the "which channel wins?" trap that has bitten every weapon/pose mod.
   Phase 1 (read joint positions) can stay a separate post-accumulation stage
   or be a flag on the same event.

2. **`PED_INPUT` + `PED_MOVE`** — both are movement. `PED_INPUT` carries the
   pad/sticks and is where you decide a heading; `PED_MOVE` fires a few lines
   later and is the only place a speed/dir write survives. Adding `dir` and
   `speed` to the `PED_MOVE` args (in/out) would make it a complete movement
   event and let `PED_INPUT` stay a pure pad filter. d2pl caches the stick in
   `PED_INPUT` and writes in `PED_MOVE`.

3. **`CAMERA` + `CAMERA_LOOK`** — `CAMERA_LOOK` fires in `TurnHead` (input),
   `CAMERA` at the end of `InitCamera` (placement). They are two phases of the
   same camera frame. A single `CAMERA` event with `stage = LOOK|PLACE`
   (or a `lookDelta` field) would remove the "my angle was overwritten"
   class of bug and halve the registration list.

4. **`FRAME` / `PRE_SIM` / `DEBUG_TICK`** — three per-frame ticks at different
   points in the step. `DEBUG_TICK` is already close to `FRAME`; one event
   with a phase argument would be simpler for modules that use several.

5. **The car family** (`CAR_PAD`, `CAR_ENGINE`, `CAR_FRICTION`, `CAR_STEP`,
   `CAR_TORQUE`, `CAR_DRAW`, `CAR_DRAW_COLOR`, ...) — same pattern:
   per-car, per-frame, different sites. One `CAR_STEP` with a stage id (and
   `pad`/`torque`/`draw` sub-structs) would be a much smaller contract.

6. **`GAME_START` / `INIT` / `LEVEL_LAUNCH` / `FRONTEND_ENTERED`** — d2pl only
   needs one "a level is starting" signal; several are registered in different
   modules just to reset the same state.

## What keeps this from being a "clean" DLL

d2pl is compiled in today. These are the gaps that would have to close before
it could move to a pure DLL addon:

1. **Ped heading + speed.** The event args do not carry writable movement
   values, and a DLL cannot deref `PEDESTRIAN`. The DLL version called the
   exported `jer_npc_face()` / `jer_npc_set_speed()` on the player ped from
   `PED_MOVE`, which works but is named for NPCs and was not exported in a
   release build. **Ask:** add `int dir` and `int speed` (in/out) to
   `JER_ARGS_PED_MOVE`, or host `player->set_ped_dir/set_ped_speed`.

2. **Aim point / camera pitch.** `JER_ARGS_CAMERA` hands `cameraAngle` as
   `void*` (an engine `SVECTOR`), so the addon casts a local 3-short struct.
   The host camera API exposes `cameraAngle` (the orbit) but not
   `camera_angle` (the view). **Ask:** a `JER_CAMERA_STATE.viewAngle` field
   and `set_view_angle()`; then the aim ray needs no event cast.

3. **Mouse look.** SDL2 is linked into the exe, not exported, so the addon
   resolves `SDL_GetRelativeMouseState` from `SDL2.dll` with `GetProcAddress`.
   That breaks if SDL is ever linked statically or renamed. **Ask:** an input
   subtable in `JER_HOST_API` (`mouse_delta`, `mouse_buttons`) alongside the
   pad accessors.

4. **`-onfoot` / module-side boots.** The engine only sets `gBootOnFoot`; a
   module has to perform the swap. A DLL cannot read that global or call
   `ActivatePlayerPedestrian` / `ChangeCarPlayerToPed`. `d2pl_boot.c`
   therefore does it in the compiled-in build. **Ask:** a
   `game->on_foot_requested()` + `player->go_on_foot()` pair.

Smaller gaps, not blockers:

- `jer_anim_bone_parent()` and `jer_anim_bone_model()` exist in the SDK but
  are not exported from a release exe, so a DLL cannot call them. The arm pose
  hardcodes the documented shoulder chain instead.
- No host API to draw a model at a bone, so the debug pistol has no hand
  mesh (the data has no pistol model anyway). `jer_anim_bone_model()` would
  make attaching one possible once a model exists.
- `FESound()` is exported, but the mod family disables weapon audio because
  swapping the SFX bank mid-shot corrupts the bank state. A safe
  `host->ui->sound_play(sample, channel)` would fix that for good.
- `JER_ARGS_PED_SKELETON.jointPos` is a `void*` to `SVECTOR[]`; the 4-short
  stride is an engine detail every pose addon has to know. An accessor
  (`host->ped->joint_pos(skel, limb)`) would remove the last cast.

## Why the arm pose was broken (and what fixed it)

The old pose built elbow/hand offsets in the ped's local frame and rotated
them by a heading — but passed `dir.vz` (roll) instead of the yaw, and the
position channel is world-oriented by the time `PED_SKELETON` phase 0 runs.
The result was an arm pinned to one world direction, and never at the aim.

The fix computes the shoulder's world position (documented
`ROOT → LOWERBACK → JOINT_1 → RSHOULDER` chain), takes the normalized
direction to the camera-centre aim point, and lays the forearm and hand out
along that ray using their own rest lengths. No local frame, no facing
convention, and it points at the laser at any body/aim angle.
