# Caine's Crossfire — the motion layers

A car in this mod is drawn with a procedural offset on top of its physics transform:
it shudders when it is standing still, its nose comes up under power and dips under the
brakes. All of it is a costume. **Nothing here can reach the handling model** — the
offset is applied to the car's RENDER matrix, in the car-draw hook, and the physics
matrix is never touched.

It is a system rather than a set of animations because the layers are separate and
composite: each one fills a `CD2_VISUAL_OFFSET` and they are **summed**, in one place,
then applied once.

    knock/knock.c    the impact layer: an impulse, a ceiling, a settle
    motion/          the procedural layers (this file)
    cars.c           the render matrix, the wheel pass, rigidWheels

## The layers

### Layer 1 — the idle fidget

A car at rest shudders. There is no keyframe anywhere: it is three sine waves per axis
at 1.2, 1.7 and 2.3 Hz, summed 0.5/0.3/0.2, plus a vertical bob on its own faster
waves (2.6/3.5/4.4 Hz, deliberately shallower — a slow deep rise and fall reads as a
boat, an engine trembles quickly and shallowly).

Three frequencies that do not share a period is the whole trick. One sine is a wobble
you can see repeating; three that never line up again inside a human attention span
read as an engine. On top of that a slow envelope (about four seconds) breathes the
amplitude between half and full, so a car left standing never looks like a loop.

The three weights total 4096, which is not decoration: it makes the summed output
**structurally** incapable of exceeding the class amplitude. Adding a fourth wave
cannot breach a ceiling.

Cars used to take a sudden jolt of about 1.4 degrees on one axis every 0.8 to 2.5
seconds. **That is gone.** It was specified as an occasional impulse spike, but on screen
it was the most visible thing in the layer and it read as the car snapping: measured as
instantaneous jumps of 10-14 units (about a degree) at 24-92 frame intervals, which is
exactly the cadence of the timer that drove it. Moving the idle's own amplitude makes no
difference to it either way - a jolt is an impulse, not a wave. What is left below is the
continuous fidget, which is the whole of the idle now. If a jolt is ever wanted again it
belongs in `cd2MotionApply` as a `cd2KnockAdd` call, sized through
`CD2_KNOCK_IMPULSE_TO` so the number means an angle rather than a raw impulse.

### Layer 2 — the pitch-back, the squat, and the nose-bob

Under power the nose comes up; on release it comes back **through** level before
settling. This layer is HELD — the nose stays up while the car is under power — which
is why it is a real spring-damper (`accel = (target - pos) * stiffness - vel * damping`)
and not the knock's two-phase motion: the knock cannot hold anything, it carries an
angle to a ceiling and settles it.

The target has two halves:

- the **thrust**, sustained: under power, the class ceiling. Reversing is the same
  power pointed the other way and gets the opposite pitch at half amplitude.
- the **delta**, transient: the change in speed per step. This is what makes the layer
  interesting. A hard stop dips hard because a hard stop *is* a large negative delta,
  and a turbo lurches because a turbo *is* a large positive one — neither is a special
  case, and the amplitude of a wheelie or a stoppie falls straight out of it.

The squat is derived from the spring's own position (a shift along the car and a
downward bob), so it cannot drift out of step with the angle that caused it: nose up,
weight back, body sitting down on the rear.

The nose-bob comes free. The class damping is deliberately below critical, so the
spring overshoots on arrival and swings back past neutral on release. Neither overshoot
is coded — they are what an underdamped spring does, which is why the class numbers are
a stiffness and a damping rather than a scripted curve.

Measured on a real level: rise to the ceiling in about 0.2 s, arrival peak 109-112
against the 102 ceiling of a MEDIUM car (+9%, "overshoots slightly"), release return in
about 0.17 s, and the pitch going negative on 158 samples for one car — the flip back
past level.

## Where it runs, and when

The layers are driven from two hooks, and the split matters:

- **JER_EVENT_CAR_STEP** (the physics rate) records what the car is doing: its speed,
  the change since the last step, and the applied thrust. The delta has to be a real
  per-step change, not a function of how often a car happened to be drawn.
- **JER_EVENT_CAR_DRAW** (the draw) evaluates the layers and composes the offset. Only
  cars that are DRAWN are advanced, which is cheap and invisible — but it does mean an
  off-screen car's phases pause rather than running in the background, and its idle
  will differ from a car that stayed on screen.

The layers are gated to the **racer set**: the player, or a car this mod's own AI is
driving (`cd2MotionIsRacer`). That is the same test the AI uses to find the cars in a
race, so traffic, parked cars and the world's police are excluded by construction
rather than by a filter that has to remember to exclude them. A queue of traffic
shuddering in unison looks like a bug, not a world.

Unlike the knock, they set **`rigidWheels`**, so the wheels move with the body. The
engine normally rotates the body model alone and leaves the wheels on the un-rotated
matrix, which is right for a body lean and wrong for anything that moves the whole car.

## The composition, and the one clamp

`cd2MotionCompose` builds the offset — the knock first, then the layers adding to it —
and clamps it. The rule: **the knock always lands in full**, and a layer may put its own
ceiling on top of that, but not more. Without it a knock arriving during a wheelie would
stack two full-amplitude rotations, and — worse — the impact would be damped by whatever
the driving layer happened to be doing at the time.

Layers 2+ carry their own ceilings, separate from the knock's deliberate ~5 degrees. An
impact is a snap and stays small; a wheelie is held and can be much larger. They are
never the same number, which is the point.

## Vehicle classes

Feel is per class, not per car: `LIGHT` / `MEDIUM` / `HEAVY`, with the pitch ceiling,
spring stiffness and damping, release overshoot, and the idle amplitudes.

Which class a car is in comes from `cd2MotionModelClass[]`, keyed by car MODEL, with a
derived fallback for a model the table has not been told about. The fallback is
deliberately blunt, and that is a measured decision rather than a lazy one: dumped from
two real levels, **every** resident car reported `mass=4096` — which is 1.0 in fixed
point, the engine's default — so a mass-based rule put the whole pool in one class. The
fields that do differ are the collision box (351..396 long, 129..145 wide) and the wheel
size, and the spread is only about 12%.

So the boundaries sit OUTSIDE the observed range: they catch a genuine outlier and
everything ordinary comes out MEDIUM. It caught one immediately on Chicago — model 7,
659 long with 74 wheels against 375..396 and 49..53 for the cars — a truck-sized vehicle,
correctly HEAVY. The table is the real lever.

The class line is written once per car per run, deliberately verbosely, so the table can
be filled from evidence:

    [cainescrossfire] motion car=0 model=3 slot=3 class=MEDIUM (derived) racer=1 mass=4096 power=4096 pw=4096 hnd=0 len=382 wide=129 wheel=49

`model` and `slot` came back equal on every car observed, so a table entry keyed by
model is also keyed by resident slot.

## Switches and instrumentation

| key / variable | default | what it does |
|---|---|---|
| `motion` | 1 | master switch for the whole layer system |
| `motion_idle` | 1 | the idle fidget alone |
| `motion_accel` | 1 | the pitch-back alone |
| `CC_MOTION=0\|1` | - | run-only master switch, never written back to the ini |

The master switch is a **true off**: with it off the composed offset is exactly the
knock's, i.e. what a car did before any of this existed. That matters for a bisect —
"off" has to mean "as if it were not there".

`CC_MOTION_LOG=<frames>` samples the layers every N frames into `REDRIVER2.log`, a
run-only override like `CC_OPPONENTS`:

    [cainescrossfire] idle car=0 pitch=6 roll=-1 yaw=-3 bob=0 scale=4096 speed=0 class=MEDIUM
    [cainescrossfire] accel car=0 pitch=102 vel=6 delta=17 thr=1 shift=-26 bob=-23 speed=148 class=MEDIUM
    [cainescrossfire] composed car=0 pitch=110 roll=8 yaw=31 bob=49 shift=55

`composed` is what the renderer actually got, clamp included — which is what makes the
clamp checkable rather than a claim. `scale` is how present the idle is: 4096 at a
standstill, fading to nothing by `CD2_IDLE_SPEED_ZERO`.

## What to change for a given look

| want | change |
|---|---|
| how big the shudder is | the class `idlePitch/Roll/Yaw` |
| the vertical bob | the class `idleBob` - **1 world unit is the floor**, see below |
| a different shudder character | `CD2_IDLE_FREQ`, `CD2_IDLE_WEIGHT`, `CD2_IDLE_BOB_FREQ` |
| how long it "breathes" | `CD2_IDLE_DRIFT_FREQ` |
| when the idle goes away | `CD2_IDLE_SPEED_FULL/ZERO`, `CD2_IDLE_SCALE_LERP` |
| how big a wheelie/stoppie | the class `pitchMax`, or `CD2_MOTION_DELTA_GAIN` |
| how fast it gets there | the class `stiffness` |
| how much it rings / how much nose-bob | the class `damping` (lower = more) |
| how hard a hit is | `CD2_KNOCK_*` in `knock/knock.h` — the impact layer |
| a different car's class | `cd2MotionModelClass[]` |

## Known limits, honestly

- **Only drawn cars advance.** By design (it is where the offset has to be built), but
  it means an off-screen car's state is not running.
- **A run with opponents is not bit-reproducible.** The same `-seed` gave one car
  `speed=32` in one run and `speed=0` in another by frame 14, with identical idle values
  at that point — so the divergence is the simulation's, not this layer's. With no
  opponents the whole run reproduces exactly, which is where determinism was proven.
- **The idle's advance is one per step per car**, confirmed by sampling every frame over
  300 frames (297 samples): the engine steps at 30 FPS, so the frequencies above and the
  PSX angle units per frame agree.
- **The arrival overshoot exceeds the class ceiling** by about 9% for a few frames. That
  is the spring passing a held target, not a bug — but it is why the ceilings read lower
  than the angles seen.
- **The look and the sound of this are unverified.** Headless runs prove the rows, the
  ranges and the timing; they cannot say whether the shudder reads as an engine or the
  flame reads as a flame. Everything in the table above is a dial.
