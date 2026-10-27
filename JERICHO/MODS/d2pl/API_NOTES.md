# d2pl — hook and API notes

Working notes for Driver 2 Parallel Lines (d2pl), the on-foot ped module.
Two goals: keep the addon a **runtime DLL** (no game rebuild), and shrink the
event surface it has to register.

This module replaced the old camera/weapon d2pl. That implementation is in git
history; only the on-foot path was carried forward.

## Hooks d2pl uses today

| Hook | Why |
|---|---|
| `JER_EVENT_CAMERA_LOOK` | right-stick + mouse orbit; freeze the orbit while aiming |
| `JER_EVENT_CAMERA` | aim-camera pull/shoulder, pitch tilt, cache the view for the aim ray |
| `JER_EVENT_PED_INPUT` | right/left stick split, camera-relative heading, pad rewrite |
| `JER_EVENT_PED_MOVE` | set ped heading and analog speed after the runner re-arm |
| `JER_EVENT_PED_POSE` | torso/neck rotation toward the aim (the only rotation channel) |
| `JER_EVENT_PED_SKELETON` | phase 0 arm pose; phase 1 hand position for the laser |
| `JER_EVENT_FRAME` | mouse poll, weapon tick (aim/fire/reload), laser, marker |
| `JER_EVENT_DRAW_OVERLAY` | ammo readout |
| `JER_EVENT_GAME_START` | forget the previous level's ped pointer and saved rotations |

## Redundant / combinable hooks

These are candidates for a shared handler or a common "ped frame" event. They
are listed roughly in order of how much they would simplify a ped addon.

1. **`PED_POSE` + `PED_SKELETON` (phase 0)** — both fire inside `DrawTanner`,
   one right after the other, and both write the same skeleton on two
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
