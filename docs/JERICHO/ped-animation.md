# Ped animations & movement — how it actually works

The authoritative map of the player-ped animation/movement pipeline, so mods
can override pieces without guessing. All line numbers are approximate for
`src_rebuild/Game/C` at the time of writing; the names are stable.

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
struct BONE {                       // motion_c.c:61
    LIMBS id;
    BONE* pParent;
    char numChildren;
    BONE* pChildren[3];
    SVECTOR_NOPAD* pvOrigPos;       // bind/root translation data
    SVECTOR*       pvRotation;      // per-frame rotation data (motion bytes)
    VECTOR  vOffset;                // rest pose parent->child delta (const)
    VECTOR  vCurrPos;               // world-oriented joint delta (per frame)
    MODEL** pModel;                 // model drawn at this joint
};
```

- `LIMBS` (`motion_c.c:26`): `ROOT=0, LOWERBACK=1, JOINT_1=2, NECK=3,
  HEAD=4, LSHOULDER=5, LELBOW=6, LHAND=7, LFINGERS=8, RSHOULDER=9,
  RELBOW=10, RHAND=11, RFINGERS=12, HIPS=13, ... RTOE=21, JOINT=22`.
- Hierarchy (`Skel[]`, `motion_c.c:146`): `LOWERBACK -> ROOT`,
  `JOINT_1 -> LOWERBACK`, `NECK -> JOINT_1`, `HEAD -> NECK`, both shoulders
  -> `JOINT_1`, `HIPS -> ROOT`. **`JOINT_1` is the upper-body root: rotate
  it and the torso, both arms and the head all follow; the hips/legs don't.**
- `pvRotation` points into the raw motion-capture frame block
  (`motion + frame1*144 + 152/158`); `pvOrigPos` likewise (bind positions).
- Per-frame `SetupTannerSkeleton` (`motion_c.c:899`) computes
  `store[i] = pvOrigPos[child] - pvOrigPos[parent]` and resets
  `vCurrPos = vOffset = store`.

## 3. The transform chain — the critical ordering

Per frame, `DrawTanner` (`motion_c.c:1671`) runs:

1. `SetupTannerSkeleton` — resets `vCurrPos = vOffset` (all bones).
2. `newRotateBones` (`1343`) — builds the root matrix from `pPed->dir`,
   then for each bone in route order: reads the **parent's** `pvRotation`
   (HEAD additionally gets `vy -= head_rot`), accumulates the parent
   matrix, transforms `vOffset` -> **overwrites `vCurrPos`** (translation
   only), and bakes the posed model verts in place.
3. `gte_SetRotMatrix(&inv_camera_matrix)`.
4. `newShowTanner` (`1050`):
   - **phase-0 hook** (`1084`, `JER_EVENT_PED_SKELETON` — the pose hook),
   - the accumulation loop (`1110`): `vJPos[id] = vJPos[parent] + vCurrPos`,
   - **phase-1 hook** (`1161` — read joint positions / draw at the hand),
   - the render (`1176`): each bone's model drawn at
     `vJPos[id] + playerPos - cameraPos` under the camera rotation.

### The clobber map (what survives)

| Write | Where | Survives? |
|---|---|---|
| `vCurrPos` (position) | `PED_SKELETON` phase-0 | YES — the accumulation uses it; but it is reset next frame by `SetupTannerSkeleton`, so re-apply every draw |
| `pvRotation` (rotation) | `PED_SKELETON` phase-0 | **NO** — the only reader is `newRotateBones`, which already ran |
| `pvRotation` (rotation) | raw motion bytes before `newRotateBones` | YES — this is the only effective rotation channel |
| `pPed->speed` | `PED_MOVE` (`AnimatePed:632`) | YES — the runner re-armed it earlier in the same frame; this is the last word |
| `pPed->dir.vy` | `PED_INPUT` | YES — feeds both the movement direction and the whole-body yaw |
| `head_rot` | `PED_INPUT` / `CAMERA_LOOK` | YES — consumed in `newRotateBones` (HEAD yaw) |

**There is no hook between `SetupTannerSkeleton` and `newRotateBones`**
today. `jer_anim` adds one (`JER_EVENT_PED_POSE`) so mods can mutate the
per-bone rotations where they actually take effect.

## 4. Head turn

- `TurnHead` (`camera.c:393`) — the L2/R2 look buttons set
  `pPlayerPed->head_rot = 512 / -512 / 0`; `JER_EVENT_CAMERA_LOOK` fires
  there and may suppress/override. `lp->headPos` easing is camera-only.
- `head_rot` is consumed ONLY in `newRotateBones:1405` (HEAD bone yaw) and
  the sprite-head path `DoCivHead`.
- `head_pos` is NOT a look angle — it is the head joint's world Y used as
  the viewer height.
- NECK/HEAD are children of the JOINT_1 upper-body chain, so upper-body
  rotation overrides move the head too — an aim-mode head lock should set
  `head_rot` to keep the head aligned with the (rotated) torso.

## 5. Models

- Model<->limb map (via `SetSkelModelPointers`): `JOINT_1 -> pmTannerModels[0]`
  (torso), `HEAD -> [1]`, `NECK -> [14]`, `LELBOW -> [8]`, `LHAND -> [9]`,
  `LFINGERS -> [10]`, `RELBOW -> [2]`, `RHAND -> [3]`, `RFINGERS -> [4]`,
  legs as named. LSHOULDER/RSHOULDER/LOWERBACK/HIPS/ROOT have NULL models
  (pure joints).
- Bone model verts are pose-rotated **in place** by `newRotateBones`; they
  are re-posed next frame, so a mod must never persist vertex edits.

## 6. Practical hook guidance

- Aim/pose (translation): write `Skel[i].vCurrPos` in `PED_SKELETON`
  phase-0 (the arm-hold pose does this today).
- Aim/pose (rotation): mutate `*Skel[i].pvRotation` in `JER_EVENT_PED_POSE`
  (the new pre-`newRotateBones` hook) — the only effective channel.
- Movement speed: `JER_EVENT_PED_MOVE` (survives; the negative-speed branch
  gives the backpedal for free).
- Movement direction / tank overrides: `JER_EVENT_PED_INPUT` (rewrite the
  pad bits + `lp->dir`).
- Head: `head_rot` (or `JER_EVENT_CAMERA_LOOK` suppress).
## 7. Relative transforms (what the numbers mean)

Every transform the mods write is LOCAL (parent-relative); the engine
composes the chain as:

    world = RootMatrix(pPed->dir) x boneLocal[ROOT..i](pvRotation) x vCurrPos

- `pPed->dir.vy` is the WHOLE-BODY yaw (the root matrix). The engine's
  body heading is `pPed->dir.vy`; the MOVEMENT heading is `dir.vy - 2048`
  (the AnimatePed forward branch moves along `RSIN(dir.vy - 2048)`).
- Each bone's `pvRotation` is that bone's OWN yaw RELATIVE TO ITS PARENT
  (in the same 4096/360 angle scale). Writing `JOINT_1.vy = diff` where
  `diff = (bodyYaw - aimYaw) & 0xfff` (normalized to -2048..2048 by
  `jer_anim_aim_diff`) rotates the whole upper body (torso + arms + head,
  via the NECK chain) by exactly that much off the body heading.
- `vOffset`/`vCurrPos` are parent->child POSITION deltas in the ped's
  local frame (render frame, Y down). The phase-0 arm pose writes these.
- The raw motion-frame rotation bytes (`pvRotation` targets) are the
  STOCK per-bone rotations for the current `frame1`; a PED_POSE write
  overrides them for that frame. Because they point into the SHARED
  per-type motion buffer, a module must snapshot + restore them when it
  stops posing (see the d2pl `gPoseSaved` pattern), or the walk cycle
  keeps the twist.

Diagnostic: the `[d2pl] bone:` log (d2pl_weapon.c, PED_POSE) prints
`pedDir / aim / diff / JOINT1 rot / HEAD rot` every 90 frames while
aiming, so the relative chain is observable end-to-end.
