# Crew poses — the bone contract, and how to author one outside the code

The mounted crew (the driver and the passenger who lean out to fire; see
[`MOUNTED_CREW.md`](MOUNTED_CREW.md)) is posed every frame by the module, bone
by bone, through the JERICHO `jer_anim` API. This document is the **contract**
between that pose code and anyone authoring a pose in a modelling package, plus
the workflow to get from one to the other.

It exists because the engine has **no animation importer**: there is no way to
drop a `.bvh`/`.fbx`/custom motion file into the game. A pose is numbers — per
bone, per frame — and the only place a module can put them is the two `jer_anim`
channels below.

## 1. The skeleton — 23 bones, this order

A drawn ped is 23 joints. The module names them `JER_LIMB_*` (`jer_anim.h`); the
engine's own enum is `LIMBS` in `motion_c.c`. **The order is fixed and a rig
must match it** — a dumped table is indexed by it.

| # | name | parent | # | name | parent |
|---|---|---|---|---|---|
| 0 | `ROOT` | — | 12 | `RFINGERS` | `RHAND` |
| 1 | `LOWERBACK` | `ROOT` | 13 | `HIPS` | `ROOT` |
| 2 | `JOINT_1` | `LOWERBACK` | 14 | `LHIP` | `LOWERBACK` |
| 3 | `NECK` | `JOINT_1` | 15 | `LKNEE` | `LHIP` |
| 4 | `HEAD` | `NECK` | 16 | `LFOOT` | `LKNEE` |
| 5 | `LSHOULDER` | `JOINT_1` | 17 | `LTOE` | `LFOOT` |
| 6 | `LELBOW` | `LSHOULDER` | 18 | `RHIP` | `LOWERBACK` |
| 7 | `LHAND` | `LELBOW` | 19 | `RKNEE` | `RHIP` |
| 8 | `LFINGERS` | `LHAND` | 20 | `RFOOT` | `RKNEE` |
| 9 | `RSHOULDER` | `JOINT_1` | 21 | `RTOE` | `RFOOT` |
| 10 | `RELBOW` | `RSHOULDER` | 22 | `JOINT` | `ROOT` |
| 11 | `RHAND` | `RELBOW` | | | |

Two facts that matter when rigging:

* **`JOINT_1` is the upper-body root.** Rotating it swings the torso, *both*
  arms and the head together; the legs do not follow. That is why the passenger
  pose twists `JOINT_1`, not the spine chain.
* The crew use **Tanner's model (ped model 0)** — the only model that is always
  loaded and renders through the skeleton path (`DrawCiv`'s civilians are a
  different, sprite path).

## 2. Rotation values — what to export from a package

A rotation channel value is a **`JER_BONE_ROT`**: three signed 16-bit angles,
`{vx, vy, vz}`.

* **Units:** the engine's PSX scale — **4096 per full turn** (so 90° = 1024,
  180° = 2048). `degrees * 4096 / 360`.
* **Frame:** **parent-relative**, applied as a **ZYX Euler** (`RotMatrixZYX_gte`
  in `newRotateBones`). Export each bone's rotation *relative to its parent*, not
  in world space — a package's world-space or XYZ-Euler output must be converted.
* **Pitch / yaw / roll** map to `vx` / `vy` / `vz` respectively (the Tait-Bryan
  order above is what turns a triple into the engine's matrix).

## 3. Position values (the arm reach)

The arm is additionally moved on the **position** channel: a `JER_BONE_POS` is
three 32-bit **world units**, but by the time the hook runs the values are
**world-oriented** (the engine already rotated the bone chain), so a
body-relative offset has to be turned into that frame with
`jer_anim_rotate_offset()` — or, as the crew code does, by rotating the ped-local
offset through the car's matrix. The rendered body is **Y-down**, so "up" is a
negative Y.

## 4. The two channels, and the traps

| channel | hook | write | notes |
|---|---|---|---|
| rotations (`JER_BONE_ROT`) | `JER_EVENT_PED_POSE` | `jer_anim_bone_rotation(skel, limb)` | fires between `SetupTannerSkeleton` and `newRotateBones`; `newRotateBones` is the **sole reader**, so this is the only window in which a rotation write takes effect |
| positions (`JER_BONE_POS`) | `JER_EVENT_PED_SKELETON`, **phase 0** | `jer_anim_bone_pos(skel, limb)` | the skeleton is reset to rest every frame, so re-apply on every draw; phase 1 is *after* the joints are accumulated (arm positions valid — used to dump/aim) |

Both hooks fire for the player ped **and for any ped the module owns**
(`jer_npc_owned`), which is how the crew ped (with `padId = -1`) is reached.

**Trap 1 — the shared buffer.** A bone's rotation points *into the shared
per-type motion buffer*, so a write leaks into every ped drawing that motion
frame. `jer_anim_save_rotations()` before writing and
`jer_anim_restore_rotations()` once the ped's bones are built (the skeleton
phase-1 pass) is the supported undo. The crew pose does exactly this — the same
place the engine's `bReverseYRotation` mirror flag is handed back.

**Trap 2 — the world-oriented position frame.** See §3. Writing a *local* offset
straight into `vCurrPos` pins the pose to one world direction and it stops
following the car.

**Facing.** The drawn body's forward sits at world angle `3072 - yaw` while the
car's forward is `1024 - direction`: the model's mesh front is half a turn from
the yaw vector. `CD2_CREW_BODY_YAW = 2048` cancels it. A rig that decides to face
its own way must keep this in mind — it is a measured property of the mesh, not
of the maths.

## 5. Authoring workflow

1. **Rig** 23 bones in the package, in the order and hierarchy of §1. The bone
   *rest* poses do not have to match the game's mesh exactly to be useful, but the
   closer they are the less guesswork the numbers carry.
2. **Pose** the crew member (a lean out of the window, a sit on the sill, an arm
   holding a weapon at a target direction).
3. **Export** per-bone, parent-relative, ZYX-Euler angles in degrees (a `.csv` of
   `limb, vx, vy, vz` per frame is the simplest), one row per game frame
   (**30 fps**; stock ped cycles are ~16 frames but a *held* pose is one row).
4. **Convert** to `JER_BONE_ROT` triples: `v = degrees * 4096 / 360`. A small
   BVH/CSV→C-table script is the natural tool here (none ships yet; the tables
   are tiny enough to hand-edit).
5. **Play** the table from a `PED_POSE` handler with
   `jer_anim_bone_rotation(skel, limb)->vx/vy/vz = table[limb]`, wrapped in the
   save/restore of §4.

## 6. Start from a real baseline (what the code does today)

Rather than guess, dump the pose that is already on the model. With
`debug_log = 1`, the crew prints a paste-ready table for each side (up to 3 per
side per run) from `cd2CrewDumpPose`'s companion `cd2CrewDumpBones`
(`weapons/core/crew.c`):

```
[crew] static const JER_BONE_ROT side1[JER_LIMB_COUNT] = {
[crew] 	{     0,  2048,   140 },	/*  2 JOINT_1 */
[crew] 	{     0,   300,     0 },	/*  4 HEAD */
...
[crew] };
```

Those are the applied values (the passenger's 180° torso twist + lean, the
driver's lean and head aim, the get-out motion underneath). Edit them, or feed
them to a package as the starting point — a table and a rendered pose are the
two views of the same numbers.

The pose *shape* is a handful of named knobs in `crew.c` (the
`CD2_CREW_ARM_*`, `CD2_CREW_HEAD_AIM`, `CD2_CREW_DRIVER_LEAN`,
`CD2_CREW_SILL_*` defines), so a tuning pass is a one-number change, not a code
rewrite. The rendered look still needs a play-test.

## 7. Per-vehicle offsets (a pose that fits the car)

If a body (a limo, a truck, a very wide car) puts the door somewhere the default
mount does not fit, a **vehicle profile** carries per-side crew offsets that
`cd2CrewPlace` and the pose code apply. See [`PROFILES.md`](PROFILES.md) and
`profiles/profile.h` (`CD2_VEH_CREW`).
