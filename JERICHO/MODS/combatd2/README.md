# Combat D2

Twisted Metal: Black style arcade handling for REDRIVER2, built as a JERICHO
deep mod. The feel is *engineered, not simulated* — responsive and forgiving
(TMB's north star) — on top of a point-mass rigid body that replaces the
stock wheel/suspension sim:

- **Direct velocity control** — throttle accelerates to `topSpeed`, brake
  decelerates (fast & proportional: strong scrub at speed, smooth taper so it
  never jars to a stop) and reverses up to `reverseSpeed`.
- **Direct yaw control** — steering sets a target yaw rate; turning works at
  zero speed and steering authority never drops mid-slide.
- **Tight Turn** — an acute forced pivot on its own yaw authority, *not*
  steering amplification. It bleeds a little speed so gas + pivot produces a
  short drift-slide; at low speed it spins nearly in place.
- **TMB button layout** (optional, default ON) — with PlayStation face
  buttons as the reference: **Square = Gas, Circle = Brake, X/Cross = Tight
  Turn**, Triangle left unbound for the car (get in/out stays on L3). The
  layout only applies while driving a car, never on foot. If your pad labels
  the *left* face button "X" (Xbox-style), flip **Tight Turn Button** in the
  menu to put the pivot on that button and Gas on X/Cross.
- **Brief, forgiving skids** — high base grip that only sags ~30% at full
  slip, so you never spiral out of control.
- **Short, punchy gears with a tall top** — the rev/gear model is retuned per
  player car so low gears are short and shift quickly, while top gear is tall
  enough that the pitch levels out at the car's top speed instead of revving
  away (tunable via the `CD2_GEAR_*` / `CD2_REV_CEILING` macros in `combatd2.h`).
- **Engine-audio tuners** — pitch/volume scaling for the rev + idle engine
  channels live in `combatd2.h` as `CD2_SND_*` macros (SPU pitch 4096 = normal,
  PSX volumes; all neutral by default).
- **Walls absorb momentum** — scenery hits are a hard stop, not a bounce.
- **Weight & Control spread** — derived from each chassis' power-to-weight and
  mass: light cars are instant and agile, trucks are slow but heavy and coast
  longer; collisions stay mass-based so heavies push.

Out of scope (no combat layer in Driver 2): turbo meter, energy attacks and
ram-damage bonuses. Button layout: the optional TMB layout remaps the face
buttons while driving (see above), replacing the original car
binds while it is active; the *physical* button → engine-binding
stays in the engine's own `config.ini`, so every bind remains rebindable
there — combatd2 never hard-wires a physical key.

## How it works

All physics runs in one hook, `JER_EVENT_CAR_TORQUE` (the tail of
`StepOneCar`): the module zeros the stock horizontal force and yaw torque,
then writes `linearVelocity[0..2]` and `angularVelocity[1]` directly.
`JER_EVENT_CAR_STEP` snapshots the raw throttle before the stock handbrake
code zeroes it, `JER_EVENT_CAR_PAD` (new engine hook, fired inside
`ProcessCarPad`) lets the module take over the car's pedal semantics for the
TMB layout — the stock face-button assignment is skipped that frame, so the
original binds never double-fire alongside the new ones; `JER_EVENT_CAR_GEARBOX`
retunes the gamesnd rev model (short gears, tall top) and
`JER_EVENT_CAR_ENGINE_SOUND` scales the engine channel pitch/volume — and
`JER_EVENT_GET_WALL_RESTITUTION` (new engine query) makes
walls absorb momentum — the stock collision path is unchanged when combatd2
is off. Vertical motion + roll/pitch stay stock so the car still rides terrain.

## Tuning

All constants live at the top of `combatd2.h` as `CD2_*` macros (speed =
world-units/frame, yaw = PSX angle units where 4096 = 360°). Live sliders —
Top Speed, Acceleration, Braking, Handling, Grip, Tight Pivot, TMB Buttons
(on/off + tight-button position), Telemetry Log, and three presets — are in
the pause menu under **Modules → Combat D2** (settings persist to
`JERICHO/CONFIG/combatd2.ini`).

## Validation checklist

- TMB layout: while driving, Square gasses, Circle brakes, X/Cross pivots;
  on foot the ped controls are unchanged. Flip Tight Turn Button if the pad
  labels the left face button "X".
- Tight Turn: hard pivot, spins nearly in place at low speed, gas + pivot
  slides instead of stopping dead.
- High-speed stop: brakes scrub quickly but taper — no jarring dead stop.
- Wall hit at speed: hard stop, minimal bounce, no wall-spin; still controllable.
- Vehicle spread: a light car out-accelerates and out-turns a heavy one.
