# Ped animations & movement — how it actually works

The authoritative map of the player-ped animation/movement pipeline, so mods
can override pieces without guessing. All line numbers are approximate for
`src_rebuild/Game/C` at the time of writing; the names are stable.

This is a **corrected** revision: the earlier version recommended posing the
arm through `vCurrPos` and claimed `HEAD.pvRotation` was a rotation channel —
both were wrong and caused the "arm/head only sometimes rotate correctly"
symptoms. The two facts that matter most:

- **`pvRotation` sits on the PARENT joint.** `newRotateBones` builds bone
  *i*'s matrix from `parent.pvRotation`, so a bone's own `pvRotation` is
  consumed by its CHILDREN (never itself). `HEAD.pvRotation` is a leaf and
  is **never read** — the head's yaw is `NECK.pvRotation.vy - head_rot`.
- **`vOffset` is the armature-correct position channel.** It is
  parent-relative, re-derived every draw by `SetupTannerSkeleton`, and
  rotated by the parent chain in `newRotateBones`. `vCurrPos` phase-0
  writes are a model-frame poke that bypasses the chain.

## 1. Input -> movement

- Pad bits (`pad.h:38-45`): `TANNER_PAD_ACTION = MPAD_TRIANGLE`,
  `GOFORWARD = MPAD_CROSS | D_UP`, `GOBACK = MPAD_SQUARE | D_DOWN`,
  `TURNLEFT = D_LEFT`, `TURNRIGHT = D_RIGHT`.
- `main.c` assembles the bits, fires `JER_EVENT_PED_INPUT`
  (args in `jer_events.h`; the module may rewrite the pad AND the movement
  bits AND `lp->dir`), then calls `ProcessTannerPad(pPed, t0, t1, t2)`.
- `ProcessTannerPad` (`pedest.c:2603`) stores the bits into the global
  `tannerPad`, ground-clamps `position.vy = -130 - MapHeight`, handles the
  ACTION button, then dispatches the **state function**:
  `fpAgitatedState ? (*fpAgitatedState)(pPed) : (*fpRestState)(pPed)`.
- The personality table `fpPedPersonalityFunctions[]` (`pedest.c:66`):
  `[0]=PedDoNothing, [1]=PedUserWalker, [2]=PedUserRunner, ...`.
- The state is **promoted in `PedDoNothing`** (`pedest.c:795`):
  `GOFORWARD` -> `SetupRunner` (`speed = 40`, `type = PED_ACTION_RUN`,
  `fpAgitatedState = [2]`); `GOBACK` -> `SetupBack` (`speed = -10`, WALK,
  `[1]`); turn bits integrate `doing_turn` into `dir.vy` and zero
  `head_rot`; idle runs `PedCarryOutAnimation`.

### Speed

- `pPed->speed` is a `char` (`dr2types.h:973`). **Positive = forward,
  negative = backward.**
- `PedUserRunner` re-arms `speed = 40 - (tannerDeathTimer >> 1)` **every
  frame** (`pedest.c:916`) then calls `AnimatePed`; `PedUserWalker` sets
  `-10` or `0`.

### The position advance — `AnimatePed` (`pedest.c:609`)

1. Ground clamp (except SIT).
2. `JER_EVENT_PED_MOVE` fires (`pedest.c:632`) — **the ONLY place a module
   speed write survives** (the runner re-arms 40 before this, and the
   advance below uses the final value).
3. `speed < 0` branch (`641`): moves with `dir.vy` UNOFFSET, **subtracting**:
   `pos -= FIXED(speed * RSIN(dir))` etc.
4. `speed >= 0` branch (`648`): moves with `dir.vy - 2048`, **adding**:
   `pos += FIXED(speed * RSIN(dir))` etc.
5. `frame1` animation frame advance (16 frames per cycle; 31 for some
   types), footstep SFX, then the player position is copied from the ped.

So: `dir.vy` is the only direction consumed, `speed` is the only magnitude,
and the sign of `speed` picks the forward vs backward movement/animation
path — the engine's ready-made "backpedal" branch.

## 2. The skeleton

```c
struct BONE {                       // motion_c.c:62
    LIMBS id;
    BONE* pParent;
    char numChildren;
    BONE* pChildren[3];
    SVECTOR_NOPAD* pvOrigPos;       // bind/root translation data
    SVECTOR*       pvRotation;      // per-frame rotation data (motion bytes)
    VECTOR  vOffset;                // parent->child delta (re-derived every draw)
    VECTOR  vCurrPos;               // vOffset after the parent chain (composed)
    MODEL** pModel;                 // model drawn at this joint
};
```

- `LIMBS` (`motion_c.c:27`): `ROOT=0, LOWERBACK=1, JOINT_1=2, NECK=3,
  HEAD=4, LSHOULDER=5, LELBOW=6, LHAND=7, LFINGERS=8, RSHOULDER=9,
  RELBOW=10, RHAND=11, RFINGERS=12, HIPS=13, ... RTOE=21, JOINT=22`.
- Hierarchy (`Skel[]`, `motion_c.c:160`): `LOWERBACK -> ROOT`,
  `JOINT_1 -> LOWERBACK`, `NECK -> JOINT_1`, `HEAD -> NECK`, both shoulders
  -> `JOINT_1`, `HIPS -> ROOT`. **`JOINT_1` is the upper-body root: rotate
  it and the torso, both arms and the head all follow; the hips/legs don't.**

### The motion buffer layout (`motion_c.c:928-950`)

`pPed->motion` points into `MotionCaptureData[pPed->type]` — a **shared,
per-type** buffer (every Tanner shares the TANNER motion; every cop the cop
motion). Layout:

```
offset 0     14-byte lump header (unused by the skeleton)
offset 14    bind positions (pvOrigPos) for bones 1..22, 22 x SVECTOR_NOPAD
             (STATIC — not per frame; the rest pose)
offset 146   frame 0: ROOT pvOrigPos (6) | ROOT pvRotation (6)
             | bones 1..22 pvRotation (22 x 6)          == 144 bytes
offset 290   frame 1: same 144-byte block
...
frame f block = motion + 146 + f*144
  ROOT pvOrigPos      = block + 0
  ROOT pvRotation     = block + 6
  bone i pvRotation   = block + 12 + 6*(i-1)   (i = 1..22)
```

`SetupTannerSkeleton` re-points EVERY `pvRotation`/`pvOrigPos` from the
frame block addressed by the CURRENT `frame1` each draw, then recomputes
`store[i] = pvOrigPos[child] - pvOrigPos[parent]` and resets
`vCurrPos = vOffset = store` for all bones. **Consequences:**

- `vOffset` is re-derived every draw -> a module write to `vOffset` is
  frame-local and needs NO restore.
- `pvRotation` slots live in the SHARED buffer -> a module write to a slot
  persists in that frame's slot until restored (the walk cycle READS the
  buffer, it never writes), and the write is visible to every ped sharing
  the type. Restore discipline is mandatory (see §7).

## 3. The transform chain — the critical ordering

Per frame, `DrawTanner` (`motion_c.c:1676`) runs:

1. `SetSkelModelPointers` + `SetupTannerSkeleton` — re-points the motion
   slots for `frame1`, resets `vOffset`/`vCurrPos`.
2. **`JER_EVENT_PED_POSE`** (`motion_c.c:1690`) — the ONLY window where a
   per-bone `pvRotation` write takes effect: `newRotateBones`, the sole
   reader, runs immediately after. `vOffset` writes made here are also
   consumed by `newRotateBones` the same frame.
3. `newRotateBones` (`motion_c.c:1357`) — builds the root matrix from
   `pPed->dir` (`RotMatrixYXZ`), then for each bone in route order:
   - `_sMatrix = RotMatrixZYX_gte(parent->pvRotation)` (parent's vx is
     negated; the HEAD branch additionally does
     `parent->vy -= pPed->head_rot`),
   - `_sMatrix.t = child->vOffset`,
   - `_oMatrix = mStore[parent] * _sMatrix`,
   - `vCurrPos = _oMatrix * vOffset` (the child's world position),
   - `mStore[child] = _oMatrix`, and the posed model verts are baked in
     place.
   So: **`childWorld = parentWorld x Rot(parent.pvRotation) x
   Translate(child.vOffset)`** — a standard parent-relative armature, with
   the rotation "on the parent joint" and the translation parent-relative.
4. `gte_SetRotMatrix(&inv_camera_matrix)`.
5. `newShowTanner` (`motion_c.c:1064`):
   - **phase-0 hook** (`1098`, `JER_EVENT_PED_SKELETON` — the position-poke
     hook),
   - the accumulation loop (`1143`): `vJPos[id] = vJPos[parent] + vCurrPos`
     (RAW addition — a phase-0 `vCurrPos` override is NOT re-rotated),
   - **phase-1 hook** (`1175` — read joint positions / draw at the hand),
   - the render: each bone's model drawn at
     `vJPos[id] + playerPos - cameraPos` under the camera rotation.

### The clobber map (what survives)

| Write | Where | Survives? |
|---|---|---|
| `vOffset` (position) | `JER_EVENT_PED_POSE` | YES — composed by `newRotateBones` through the parent chain (armature-correct); re-derived next draw so it never persists |
| `pvRotation` (rotation) | `JER_EVENT_PED_POSE` | YES — the only effective rotation channel; persists in the shared slot until restored (§7) |
| `pvRotation` (rotation) | `PED_SKELETON` phase-0 | **NO** — the only reader is `newRotateBones`, which already ran |
| `vCurrPos` (position) | `PED_SKELETON` phase-0 | YES — the accumulation uses it; but it is a MODEL-FRAME poke (raw addition, no parent rotation) and is reset next draw |
| `pPed->speed` | `PED_MOVE` (`AnimatePed:632`) | YES — the runner re-armed it earlier in the same frame; this is the last word |
| `pPed->dir.vy` | `PED_INPUT` | YES — feeds both the movement direction and the whole-body yaw |
| `head_rot` | `PED_INPUT` / `CAMERA_LOOK` | YES — consumed in `newRotateBones` (HEAD yaw) |

## 4. Head turn — the dead channel

- `TurnHead` (`camera.c:393`) — the L2/R2 look buttons set
  `pPlayerPed->head_rot = 512 / -512 / 0`; `JER_EVENT_CAMERA_LOOK` fires
  there and may suppress/override. `lp->headPos` easing is camera-only.
- **`HEAD.pvRotation` is NEVER read.** `newRotateBones` builds the HEAD
  matrix from `NECK.pvRotation.vy - pPed->head_rot` (`motion_c.c:1419-1420`)
  — the head's effective yaw is the NECK slot minus `head_rot`, composed on
  top of the JOINT_1 twist. Writing `HEAD.pvRotation.vy = 0` (as d2pl once
  did) does nothing.
- `head_rot` is consumed ONLY in `newRotateBones:1419` (HEAD yaw) and the
  sprite-head path `DoCivHead`.
- `head_pos` is NOT a look angle — it is the head joint's world Y used as
  the viewer height.
- **An aim-mode head lock must therefore:** write the NECK slot
  (`NECK.pvRotation.vy = 0`, slot-restored) and set `pPed->head_rot = 0` so
  the stock look can't fight it. The JOINT_1 twist already orients the
  whole chain; the head then points exactly where the torso aims.

## 5. Models

- Model<->limb map (via `SetSkelModelPointers`): `JOINT_1 -> pmTannerModels[0]`
  (torso), `HEAD -> [1]`, `NECK -> [14]`, `LELBOW -> [8]`, `LHAND -> [9]`,
  `LFINGERS -> [10]`, `RELBOW -> [2]`, `RHAND -> [3]`, `RFINGERS -> [4]`,
  legs as named. LSHOULDER/RSHOULDER/LOWERBACK/HIPS/ROOT have NULL models
  (pure joints).
- Bone model verts are pose-rotated **in place** by `newRotateBones`; they
  are re-posed next frame, so a mod must never persist vertex edits.

## 6. Practical hook guidance (corrected)

- Aim/pose (rotation): mutate `*Skel[i].pvRotation` in `JER_EVENT_PED_POSE`
  — the only effective rotation channel. Slots must be snapshot+restored
  (§7).
- Aim/pose (translation, armature-correct): write `Skel[i].vOffset` in
  `JER_EVENT_PED_POSE` — `newRotateBones` rotates it by the full parent
  chain (body yaw + torso twist + animation), so the arm follows the aim
  naturally. No restore needed.
- Direct position pokes (no chain rotation): `Skel[i].vCurrPos` in
  `PED_SKELETON` phase-0 — only for small deltas where the parent rotation
  is negligible; NOT the arm-pose channel.
- Movement speed: `JER_EVENT_PED_MOVE` (survives; the negative-speed branch
  gives the backpedal for free).
- Movement direction / tank overrides: `JER_EVENT_PED_INPUT` (rewrite the
  pad bits + `lp->dir`).
- Head: write the `NECK` slot + set `pPed->head_rot = 0` while aiming.

## 7. The rotation slots — semantics + restore discipline

### What the numbers mean

Every transform a module writes is LOCAL (parent-relative); the engine
composes the chain as:

    world(child) = RootMatrix(pPed->dir)
                   x Rot(ROOT.pvRotation)   [the torso's parent chain]
                   x Rot(LOWERBACK.pvRotation)
                   x Rot(JOINT_1.pvRotation)  [the upper-body root]
                   x Rot(NECK.pvRotation)     [head only]
                   x Translate(child.vOffset)

- `pPed->dir.vy` is the WHOLE-BODY yaw (the root matrix). The engine's
  body heading is `pPed->dir.vy`; the MOVEMENT heading is `dir.vy - 2048`
  (the AnimatePed forward branch moves along `RSIN(dir.vy - 2048)`).
- A bone's `pvRotation` is consumed by ITS CHILDREN (the rotation "sits on
  the parent joint"). Writing `JOINT_1.pvRotation.vy = twist` rotates the
  shoulders, neck and head (its children) by `twist` relative to the
  LOWERBACK frame — the upper-body aim.
- **Composed torso yaw** (what the upper body actually faces) is
  `dir.vy + ROOT.pvRotation.vy + LOWERBACK.pvRotation.vy` (`bReverseYRotation`
  is 0 in normal play, so the ROOT yaw is not negated). The aim twist must
  be measured against THIS, not the raw `dir.vy` — the walk/run cycle's own
  torso sway otherwise leaves the aim axis a few degrees off. Use
  `jer_anim_torso_yaw()` / `jer_anim_twist_to_aim()` (jer_anim.h).
- A body that FACES the aim runs at `dir.vy = aimYaw - 2048` (the model
  face is -z), so:
  `twist = DIFF_ANGLES(composedTorsoYaw, aimYaw - 2048)` — i.e.
  `wrap((aimYaw - 2048) - composedTorsoYaw)`, clamped to the spine limit
  (±796 = ~70 deg).
- `vOffset`/`vCurrPos` are parent->child POSITION deltas in the ped's
  local (render, Y-down) frame. `vOffset` is composed by the chain;
  `vCurrPos` phase-0 writes are not.

### The restore discipline (d2pl's slot-tracked pattern)

Because the `pvRotation` slots live in the SHARED per-type motion buffer
and the animation advances `frame1` every frame, a posing module must:

1. **Restore every slot it dirtied LAST frame** (the saved pointers ARE
   the exact motion-buffer addresses, so the restore is always correct even
   when the animation frame advanced or the motion buffer swapped),
2. **Snapshot the CURRENT frame's slots** (the pure animation values),
3. **Write the pose values**,
4. **On the first non-posing frame, restore every saved slot.**

d2pl touches exactly four slots while aiming: `JOINT_1` (twist), `NECK`
(head lock), the weapon-side `LSHOULDER`/`LELBOW` or `RSHOULDER`/`RELBOW`
(identity, so the walk cycle's arm swing can't rotate the posed hand).
Known limitation: two peds sharing a type AND the same `frame1` in the same
frame (multiplayer) interleave their writes on the same slots; the restore
is per-module, so multiplayer Tanners need per-ped ownership (out of scope
for single-player).

## 8. Diagnostics

The `[d2pl] bone:` log (d2pl_weapon.c, PED_POSE) prints
`pedDir / aim / diff / JOINT1 rot / NECK rot / HEAD rot` every 90 frames
while aiming, so the relative chain is observable end-to-end: with a
correct lock, `diff` should equal the written JOINT1 yaw and NECK should
read 0.
