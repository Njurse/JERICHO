# Mounted crew — the driver and gunner leaning out to fire

Some weapons are fired by a **crew member leaning out of the car**, not by the
vehicle itself: the driver hangs out of their window to fire one, the gunner
(the passenger) hangs out of the other. Others — the machine gun, the missile,
the seeker, the mine, the freeze — fire **from the vehicle directly**. This
document is the map of that split and of the ped lifecycle that draws it.

Source: `weapons/core/crew.c` / `crew.h`, plus the muzzle helpers in
`weapons/core/weapons.c`.

## 1. Which weapon leans whom (`CD2_WEAPON_DEF.leanOut`)

`leanOut` is a **bitmask** — which crew member leans out *while this weapon is
firing*:

| bit | value | crew |
|---|---|---|
| 0 | `1` (`CD2_CREW_DRIVER`) | the driver — the **left** window |
| 1 | `2` (`CD2_CREW_GUNNER`) | the gunner — the **right** window |
|   | `3` | both (a single muzzle uses the driver's window) |
|   | `0` | nobody (the vehicle fires it) |

Assignments today:

| weapon | leanOut | who leans |
|---|---|---|
| `SHOTGUN` | 1 | driver |
| `CLUSTER` | 2 | gunner |
| `ZOOMY`   | 2 | gunner |
| `MG`, `MISSILE`, `HOMING`, `MINE`, `FREEZE` | 0 | the vehicle |

Both crew members use **Tanner's model** (ped model 0). That is not a
preference: it is the only model that is always loaded and renders through the
engine's *skeleton* path. Civilians render through the sprite path (`DrawCiv`),
which reads a different motion format, cannot play the get-out pose, and gets
recycled by the ambient-pedestrian system.

## 2. The request: firing **or** selected

Every weapon fire — the player's trigger and the AI's — goes through
`cd2WpnTryFire`, which calls `cd2CrewNotifyFire(car, leanOut)`. Two things feed
the same request:

- **Selected** — `cd2WpnOnFrame` calls `cd2CrewArmed(car, selectedDef->leanOut)`
  every frame, so **picking** a leaning weapon brings its ped out and *holds it
  out* while that weapon stays selected. Switching to a non-leaning weapon stops
  refreshing the hold and the ped gets back in.
- **Fired** — `cd2CrewNotifyFire` refreshes the hold at the moment of a shot.
  This is what drives the **AI**, which has no "selection": it leans a ped out
  when it fires a leaning weapon.

The flags **OR** together and each set side gets its hold (24 frames) re-armed.
So a side that any firing/selected leaning weapon wants out **stays out**, even
while the base machine gun (leanOut 0) fires: the MG contributes nothing and can
never clear another weapon's side.

## 3. The ped lifecycle

Driven off the request, one ped per side (`weapons/core/crew.c`):

1. **out** — a side's hold goes live: spawn a parked `TANNER_MODEL` ped beside
   the car's door (`padId = -1`) and climb it out with `PED_ACTION_GETOUTCAR`.
   The animation advances `CD2_CREW_ANIM_STEP` (3) frames per tick (the
   engine's own get-out steps one per tick, which reads sluggish) and stops at
   the side's end frame: the **driver at 9**, still leaning out of the window,
   the **gunner at 14**, fully out.
2. **held** — the ped settles into its resting pose and is **re-placed from the
   car's transform every frame** so it rides the car:
   * **driver** — holds the mid-climb lean (frame 9) with his right arm forced
     into a weapon-holding reach (see *The poses* below);
   * **gunner** — the **same** motion at the **same** frame, raised
     `CD2_CREW_SILL_RAISE` so he perches *on* the windowsill. Both sides use one
     animation and one frame on purpose: a later frame stands the ped up, and
     raising *that* just floats him.
3. **in** — the hold lapses (or the weapon is deselected): play
   `PED_ACTION_GETINCAR` 0→15 (same step rate) and `DestroyPedestrian` the ped
   as it disappears. This is done by the module, **never** through the engine's
   `PedGetInCar` (that calls `ChangePedPlayerToCar` and would hijack the
   player's car).

If fire resumes mid-retract the side flips straight back to *out*.

### Placement

`cd2CrewPlace` hangs the ped on the door: lateral = `colBox.vx * 1.08` out to
the side (a *per-side* sign), forward = `colBox.vz / 4` (a touch *ahead* of
centre, not the rear), **riding the car** in all three axes (see *3D placement*
below), and rotated to `hd.direction + CD2_CREW_YAW_OFFSET`.

**Per-vehicle offsets.** A body whose doors sit somewhere the default does not
fit (a limo, a truck, a very wide car) moves the mount with its profile's
`CD2_VEH_CREW` block — `lat`/`fwd`/`up`/`yaw` per side, plus a `sillRaise` and an
`armScale`, each a delta with `CD2_VEH_INHERIT` (0) meaning "leave the default".
`cd2CrewOffsets` reads it; `cd2CrewPlace` and `cd2CrewPoseArm` apply it. See
[`PROFILES.md`](PROFILES.md) § *Crew offsets*.

**Facing — measured, and it is one number.** The crew's body yaw is
`hd.direction + CD2_CREW_BODY_YAW`, with `CD2_CREW_BODY_YAW = 2048`.

That 2048 is not a fudge. A live dump of a crew ped's bones next to its car
(`cd2CrewDumpPose`, below) shows the drawn body's forward sitting at world angle

    3072 - yaw          (measured: the toe axis lands on it, ~±40 units)

against the car's own forward at

    1024 - direction     (the car's local +Z, straight off its matrix)

so a ped handed `yaw = direction` is drawn facing **back down the car** — both
crew read as backwards, which is exactly the bug. `+2048` cancels it. The flee
path had already found the same thing independently (`cd2CrewFleeSide` sets
`dir.vy = head + 2048` to make a fleeing ped face where it runs), which is the
corroboration that convinced me it is the model, not the arithmetic.

`CD2_CREW_BODY_YAW` is still the single knob: `2048` = both face forward along
the car, `2048 ∓ 1024` = each turned a quarter to face out of his own door (the
engine's own get-out pairing is `-1024` left / `+1024` right on top of the
model's half turn).

**Mirroring is the get-out flag, not a yaw flip.** The gunner is the driver's
mirror via `bReverseYRotation` (driver 0, gunner 1) — the engine's own mechanism
for the opposite door. This does *not* disturb the facing: both sides still
measure `dToe ≈ 0`. Flipping the *yaw* per side instead leaves one door 180° out,
because the model's front is not aligned with the yaw vector in the first place
(that half turn above). Only the lateral offset, the arm's side, and that flag
are per-side.

The placement runs on **`JER_EVENT_CAMERA`** (fired from `InitCamera`, just
before `DrawAllPedestrians`), *not* only on `FRAME`. `FRAME` fires before
`StepCars()`, so a ped placed there is a whole step behind the car at speed —
plainly visible as lag. The `CAMERA` pass uses the cars' final transforms for
the frame.

### The poses — the driver leans and aims, the passenger sits on the sill

Both sides start from the engine's own `GETOUTCAR` motion — no new art, no
invented pose (`PED_ACTION_SIT` was tried and reverted: its legs dangle ~95 below
the hip and land ~13 units *inboard* of it, so they hung through the door panel).
On top of that motion each side is posed differently and, both of them, **aim at
the car's current target**.

**Where "the target" comes from.** An AI car's crew aim at whatever its own brain
is chasing (`cd2AiTargetPos`, published by `cd2AiDrive`); the local player's crew
aim at the radar lock (`cd2LockOnTarget`). No target = the arm falls back to a
fixed "out of the window" reach. See `AI.md` §10.

* **the arms AIM (POSITION channel, `PED_SKELETON` phase 0).** `cd2CrewAim` turns
  the target into a direction in the car's frame (`RSIN`/`RCOS` of the target
  yaw, clamped to `CD2_CREW_ARM_CONE` so a target behind does not fold the arm
  through the ped) and `cd2CrewPoseArm` extends the forearm and hand along it,
  with a fixed outward bias (`CD2_CREW_ARM_OUT_BIAS`) so the arm still leaves the
  window rather than crossing the chest.
  Re-applied every draw. The subtlety that made this look broken for a while:
  **the position channel is world-oriented** — `motion_c.c` has already rotated
  the chain by the body matrix by the time phase 0 runs, so `vCurrPos` is not a
  local offset any more. Writing a ped-local offset into it raw, or rotating it
  by the yaw alone, pins the arm to one world direction: it stops following the
  car. `cd2CrewLocalToWorld` turns the offset with the body's full rotation —
  which for a crew ped *is* the car's — and that is what makes the reach track
  the car exactly.
  The other half of it: the shoulder is now **left alone**. It is already in the
  world frame where the motion (and the torso aim below) put it, and assigning
  the local rest value into it (as the first cut did) yanked the whole arm to a
  fixed spot.
  The offsets are ped-local: `+x` out of the ped's right, `-y` up (render frame
  is Y-down), `+z` the way he faces. The driver's `out` is `-x` and the gunner's
  `+x` — the car's local `+x` runs toward the gunner's door, since the crew are
  placed at `-`/`+` lateral.
* **the torso and head aim (ROTATION channel, `PED_POSE`).** `cd2CrewPoseTorso`
  rolls the **driver's** upper body out of his own window (`CD2_CREW_DRIVER_LEAN`
  on `JOINT_1` — the upper-body root, so both arms and the head follow) and turns
  his head toward the target (`CD2_CREW_HEAD_AIM`). The **passenger** is the other
  pose: his body is turned `CD2_CREW_SILL_YAW` (180°) at the mount so he sits on
  the sill with his legs in the cabin and his back to the window, and `JOINT_1` is
  twisted `CD2_CREW_SILL_TORSO` (180°) back toward the car's forward so he still
  looks where the car goes; his head follows the target too. All single knobs —
  the rendered look still wants a play-test.
  A rotation write lands in the **shared per-type motion buffer**, so it leaks
  into every ped playing that motion frame: `jer_anim_save_rotations` snapshots it
  in `PED_POSE` and the skeleton phase-1 pass puts it back with
  `jer_anim_restore_rotations` — the same hand-back the mirror flag below uses.
* **the mirror flag** — the one that decides which way each ped is drawn. For a
  get-out the engine sets the shared `bReverseYRotation` (`SetupGetOutCar`) and
  `newRotateBones` **mirror-flips the ped's root rotation** when it is set. Both
  crew play that same motion at the same frame, so the second door needs the
  flip to read as a mirror image: `cd2CrewOnPedPose` sets it per side (driver =
  unmirrored, gunner = mirrored) and the skeleton phase-1 pass hands the game's
  value straight back. That is the only effective *rotation* channel.

### Seeing it for yourself: `cd2CrewDumpPose`

Two rounds of "that looks wrong" were spent guessing at the interactions above
because they are invisible in the source. They are not invisible at runtime.
With `debug_log = 1` the first few crew draws print the ped's whole orientation
next to its car's:

    [crew] car  dir=-91  fwdAng=1115  m00=4057 m02=-567 m20=567 m22=4057
    [crew] ped  dir=(2,2048,0)  pos=(6077,-80,-222363)
    [crew] bone hips=(0,0,0) j1=(-4,-33,60) Lsh=(-27,5,-8) Rsh=(-28,7,0)
    [crew] axis right=(55,-2,5) up=(7,24,-64) fwd=(8,3555,1334)
    [crew] ang  carFwd=1115  pedFwd=1100  dPed=4081  lToe=1113  dToe=4094
    [crew] arm  reach=(-34,9,0) ang=2048  dCar=933

How to read it:

* `dToe` (the toe axis against the car's forward) is the reliable facing measure
  — **0 means the ped faces forward along the car**. The `pedFwd`/`dPed` pair is
  the right×up cross-product, which the *bent climbing pose* skews, so trust the
  toe.
* `reach` is the hand's offset from the shoulder in world space: it should sit on
  the ped's own side (`-x` driver, `+x` gunner) and **rotate as `m00`/`m20` do**
  — that is the arm following the car.

Its companion, **`cd2CrewDumpBones`** (`PED_POSE`, so the values are both ours
and readable), prints the whole held pose as a paste-ready rotation table — the
baseline to author a pose from outside the code (see
[`CREW_POSES.md`](CREW_POSES.md)):

    [crew] static const JER_BONE_ROT side1[JER_LIMB_COUNT] = {
    [crew] 	{     0,  2048,   140 },	/*  2 JOINT_1 */
    [crew] 	{     0,   300,     0 },	/*  4 HEAD */
    ...


Reaching a module's own ped is what the **ownership gate** is for: the ped-pose
hooks (`PED_POSE`, `PED_SKELETON`) fire for the player ped **or any ped a module
owns**. Spawning through `jer_npc_spawn*` marks the ped (`jer_npc_owned`),
despawning unmarks it, and the query also checks `pUsedPeds` — so a ped the
engine destroyed behind our back never matches a recycled slot. Ambient
pedestrians are never posed, and the engine pays nothing for them.

> Two channels, and they are not interchangeable (full map:
> `JERICHO/docs/ped-animation.md`): **positions** take from `PED_SKELETON`
> phase 0, **rotations** only from `JER_EVENT_PED_POSE` (before
> `newRotateBones`).

### 3D placement — the crew rides the car, not the map

Two bugs lived here, both from treating a mounted ped as a grounded one:

* **Y** came from `-MapHeight(x,z)` — the map under the ped, which is *not*
  attached to the car. A car in the air (or tossed by a wreck) left its crew
  standing on the road below. Now `y = -cp->hd.where.t[1] - CD2_CREW_BODY_LIFT`,
  riding the car. Note the conventions: the car matrix is **Y-UP** while a ped's
  `position` is **Y-DOWN** (`camera.c` proves it: `where.t[1] == -cameraPos.vy`),
  hence the negation. `CD2_CREW_BODY_LIFT` (99) reproduces the old on-ground
  offset, measured with the car level.
* **body rotation** was the 2D `hd.direction` with the body always upright.
  `newRotateBones` builds a ped's root with `RotMatrixYXZ(dir)` over **all
  three** of `dir.vx/vy/vz`, so pitch and roll are settable.
  `cd2CrewMatrixEuler` inverts `RotMatrixYXZ` (PsyCross `LIBGTE.C`) into the
  car's YXZ triple and `jer_npc_set_orient()` applies it, so the crew banks and
  pitches with the car — and the arm, posed in the ped's own frame, follows.
  The extracted yaw is **identical to `hd.direction`** (verified), so it feeds
  the facing directly (`CD2_CREW_YAW_OFFSET` = 0 = face forward); the pitch/roll
  sign is the one `CD2_CREW_TILT_SIGN` knob.

## 4. Firing from the window

`cd2WpnShotMuzzle(def, car, side, out)` sources a shot: when the weapon leans
(`cd2WpnMuzzleSide` maps `leanOut` → −1 driver / +1 gunner) it returns
`cd2WpnWindowMuzzle` — the door plane (`colBox.vx`) and up toward the roof
(`65% colBox.vy`) — so the pellets/missile visibly leave from the window the ped
hangs out of. Non-leaning weapons keep the centred body muzzle. Used by the
shotgun scatter, the cluster and the zoomy burst (and the shotgun's sound).

## 5. AI contestants

So "all contestant cars" lean, `ai/opponent.c` fires the leaning weapons too:
SHOTGUN (driver) up close, CLUSTER/ZOOMY (gunner) at range. Each is its own
`if (!launched …)` so a cooling-down weapon doesn't block the next, and the
missile/seeker primaries still fire in between. The AI goes through the same
`cd2WpnTryFire → cd2CrewNotifyFire` path, so its cars get crew for free.

## 6. Wreck bail-out (run away on fire)

When a car the module drives is destroyed (`cd2CarTotaled`), both crew sides
**bail out**:

- a ped already leaning converts to fleeing; a side that wasn't out is spawned
  at the door first;
- it runs **away from the wreck**: base heading = `ratan2` of the ped's offset
  from the wreck point, swung by a sine each step (a wavy path), stepping with
  the engine's own `AnimatePed` forward math and a 16-frame `PED_ACTION_RUN`
  cycle, for ~3s before it is cleaned up;
- it is painted **flat black** through the engine's `JER_EVENT_PED_DRAW` hook
  (matched back to the crew by ped pointer) and `SMOKE_FIRE` is set up at its
  origin every few frames — reading as running from the wreckage in flames.
  The black is the whole ped, and getting there took two separate engine fixes
  because a pedestrian is not drawn the way a car is:

  1. a pedestrian is drawn with `PLOT_NO_SHADE`, and that path takes its colour
     from the plot context's `combo`, *not* from `planeColours` — so holding
     `planeColours` (which is all the hook used to do) had no effect on a ped at
     all. The colour now travels as `plotContext.flatColour` under
     `PLOT_FLAT_COLOUR`, which every flat-shaded plot path honours;
  2. the **skin** needed one more: the head is a *model*, and `RenderModel`
     assigns `plotContext.flags = flags` on entry, which wiped the flat bit — so
     the shaded body polys went black (they read `f4colourTable`, built from
     `planeColours`) while the face, which takes the `PLOT_NO_SHADE` branch, was
     still lit by `combo`. Measured: 611 face polys at `combo=00808080` → all
     `colour=2c000000` after. `RenderModel` now preserves the bit, and the
     limb/skin quads in `DrawBodySprite` (which never go through the plotter at
     all, and take `combointensity` or a hardcoded 254 for the shadow) have their
     own branch for it.

## 7. Player death: hold the blast point, then pan up slowly

While the local player's car is a wreck the crew module takes over the camera
(`JER_EVENT_CAMERA`, `override = 1`), so the death plays without the stock view
sliding around. Rather than freeze it dead, the held view **pans up** over the
respawn window (`gCd2Cfg.respawnDelay`, ~150 frames / 5s):

- it **holds the spot the camera was at when the car blew up** — the anchor
  (`sCamFocus`) is the camera's own position at the blast, not the wreck's, so there
  is no lateral drift and the wreckage the camera was looking at stays where it is in
  frame. Jaret: *"lets do a slower pan up and focus on the point the camera was at when
  it blew up"*. (It used to pull back along (camera − wreck) up to
  `CD2_DEATHCAM_ZOOM` 700, which slid the camera away from the car it was watching; that
  term and its constant are gone — to bring the drift back, anchor the focus on the car
  and re-add it.)
- it **rises** up to `CD2_DEATHCAM_RISE` (260) — engine camera Y is *down*, so "up" is a
  smaller `vy` — and the ramp is **eased in** (`t²`), so the pan starts almost still and
  gathers. Was a linear 420, which read as a lurch; measured over a 150-frame window the
  eased rise is 41 units at 40% and 166 at 80%.
- the captured **angle** is held unchanged, so the view keeps the framing it had.

It is released (stock camera resumes) when the car respawns. Constants live in
`weapons/core/crew.c`; the pan is observable per 60 ticks as
`crew: deathcam tick=N/M rise=R (held at the blast point)`.

## 8. The engine hook

`JER_EVENT_PED_DRAW` (`JER_ARGS_PED_DRAW { ped, flatBlack, tintR/G/B }`) is the
ped analogue of `JER_EVENT_CAR_DRAW_COLOR`. It is fired in `newShowTanner`
before a ped's bones are drawn; a module may set `flatBlack` (or a tint) and the
engine draws that ped's polys in that flat colour, restoring the plot context
afterwards (the palette `jer_ped_palette_*` machinery is what carries a team
TINT, and it stays independent — a tinted ped keeps its own palette and the
module's colour is applied on top).

Note the mechanism, because it is not the obvious one and it has two halves:

- a pedestrian is drawn with `PLOT_NO_SHADE`, whose colour comes from the plot
  context's `combo`, not from `planeColours` — so the flat colour is passed as
  `plotContext.flatColour` under `PLOT_FLAT_COLOUR` and honoured in all three
  flat-shaded plot paths (`draw.c`). Setting `planeColours` alone (what this
  hook did originally) is a no-op for a ped;
- the bit has to *survive the draw*: the head is a model, and `RenderModel`
  assigns `plotContext.flags = flags` on entry, which dropped it — leaving the
  face (the `PLOT_NO_SHADE` polys) lit at `combo` while the shaded body went
  black. `RenderModel` preserves `PLOT_FLAT_COLOUR` across that assignment, and
  `DrawBodySprite` has its own flat-colour branch for the limb quads.

With no handler the ped renders stock. It is appended at the end of the event
enum so existing ids are unchanged, and mirrored into `JERICHO/sdk/include/`.

## 9. Guards

- **Distance** — no crew is spawned beyond ~2000 units from the camera.
- **Ped pool** — `MAX_PEDESTRIANS` (28) is shared with ambient civs; a full pool
  is a silent skip.
- **Stale pointers** — every stored ped is re-validated against `pUsedPeds`
  each frame (a cutscene/level reset can destroy it).
- **Level reset** — `JER_EVENT_GAME_START` clears all crew state *without*
  touching the pool, which `InitPedestrians` has already rebuilt by then.

## 10. Verifying headlessly

`cd2debug.c` drives the whole thing from `JERICHO/CONFIG/cc_debug.txt`
(frames from GAME_START, 30 fps):

```
60:grant                 # the whole regular arsenal (a special stays its car's own)
70:select:shotgun        # driver leans out - no firing needed
90:crew                  # dump: 'crew frame=.. sel=SHOTGUN player car=0 driver=1 ..'
130:select:missile       # non-leaning: the driver gets back in
160:crew
170:muzzle               # centred vs left/right window muzzles
180:fire:cluster         # gunner leans + fires
190:killplayer:self      # wreck -> both bail out, camera held
```

Poses, the flat-black tint, the sine path and the fire are **not** verifiable
headlessly — they need a play-test. What a run *does* prove: the request/mask
bits, the spawn/hold/retract/despawn lifecycle (ped count returns to 0), the
window muzzles, that AI contestants fire leaning weapons, the wreck bail-out and
the camera hold/release, with no crash and no ped leak.
