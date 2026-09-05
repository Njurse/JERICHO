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
- **Tight Turn** (Triangle / handbrake) — an acute forced pivot on its own yaw
  authority, *not* steering amplification. It bleeds a little speed so gas +
  pivot produces a short drift-slide; at low speed it spins nearly in place.
  Trigger is rebindable (Handbrake / Wheelspin / Off in the pause menu).
- **Brief, forgiving skids** — high base grip that only sags ~30% at full
  slip, so you never spiral out of control.
- **Walls absorb momentum** — scenery hits are a hard stop, not a bounce.
- **Weight & Control spread** — derived from each chassis' power-to-weight and
  mass: light cars are instant and agile, trucks are slow but heavy and coast
  longer; collisions stay mass-based so heavies push.

Out of scope (no combat layer in Driver 2): turbo meter, energy attacks,
ram-damage bonuses and TMB's three button layouts. Remapping the physical
buttons is done in the engine's own `config.ini` (`[kbcontrols_game]` /
controller binds); combatd2 only picks *which action* the mapped input feeds.

## How it works

All physics runs in one hook, `JER_EVENT_CAR_TORQUE` (the tail of
`StepOneCar`): the module zeros the stock horizontal force and yaw torque,
then writes `linearVelocity[0..2]` and `angularVelocity[1]` directly.
`JER_EVENT_CAR_STEP` snapshots the raw throttle before the stock handbrake
code zeroes it, and `JER_EVENT_GET_WALL_RESTITUTION` (new engine query) makes
walls absorb momentum — the stock collision path is unchanged when combatd2 is
off. Vertical motion + roll/pitch stay stock so the car still rides terrain.

## Tuning

All constants live at the top of `combatd2.h` as `CD2_*` macros (speed =
world-units/frame, yaw = PSX angle units where 4096 = 360°). Live sliders —
Top Speed, Acceleration, Braking, Handling, Grip, Tight Pivot + Tight Input,
and three presets — are in the pause menu under **Modules → Combat D2**
(settings persist to `JERICHO/CONFIG/combatd2.ini`).

## Validation checklist

- Tight Turn: hard pivot, spins nearly in place at low speed, gas + pivot
  slides instead of stopping dead.
- High-speed stop: brakes scrub quickly but taper — no jarring dead stop.
- Wall hit at speed: hard stop, minimal bounce, no wall-spin; still controllable.
- Vehicle spread: a light car out-accelerates and out-turns a heavy one.
