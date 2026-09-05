# Combat D2

Twisted Metal 2 (1996) style arcade handling for REDRIVER2, built as a
JERICHO deep mod. Unlike COLLISIONDEVIL (which scales the stock wheel/suspension
sim), Combat D2 replaces the car's **horizontal** motion with a point-mass
rigid body:

- **Direct velocity control** — throttle accelerates straight to `topSpeed`,
  brake decelerates then reverses to `reverseSpeed`. No engine/gear/suspension.
- **Direct yaw control** — steering sets a target yaw rate (`handling`); yaw
  accelerates toward it at `angularAccel`. Turning works at zero speed
  (rotate in place).
- **Controlled drift** — lateral velocity is damped by a grip force that falls
  off (up to 70%) when you steer hard at speed, so sharp turns slide instead
  of spin out.

## How it works

The override lives in one hook, `JER_EVENT_CAR_TORQUE`, which fires at the very
end of `StepOneCar` — after the stock wheel forces are computed but before the
engine integrates velocity + orientation. The module zeros the stock horizontal
force (`cp->hd.acc[0..2]`) and yaw torque (`cp->hd.aacc[1]`), then writes
`cp->st.n.linearVelocity[0..2]` and `cp->st.n.angularVelocity[1]` directly.

Vertical motion (gravity + ground lift) and roll/pitch stay stock so the car
still rides the terrain. Collisions stay stock (mass-based push); the grip +
drag make recovery forgiving, which is the TM2 feel.

## Tuning

All constants live at the top of `combatd2.h` as `CD2_*` macros, expressed in
the game's native units (speed = world-units/frame, yaw = PSX angle units
where 4096 = 360°). The TM2 SI reference values (20 m/s, 120°/s, …) are quoted
in the comments there. Per-vehicle variety is derived from the existing chassis
stats — `powerRatio` scales accel/top-speed, `traction` scales grip — so every
car works with no per-car table.

Live sliders (Top Speed / Acceleration / Handling / Grip) plus three presets
are available in the pause menu under **Modules → Combat D2**; settings persist
to `JERICHO/CONFIG/combatd2.ini`.

## Validation checklist

- High-speed turn: wide arc with a sideways slide, no spin-out.
- Zero-speed turn: rotates in place at the handling rate.
- Wall hit: bounces slightly, loses speed, stays controllable.

## Known limitation

Collision *material* properties (restitution 0.2–0.4, friction 0.0–0.1) live in
the engine's collision code (`handling.c` car-car impulse, `bcollide.c`
car-world) and are out of a module's reach. Combat D2 leaves them stock and
relies on grip + drag for the forgiving recovery instead.
