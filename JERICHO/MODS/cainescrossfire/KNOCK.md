# The vehicle knock

> **This is the impact layer.** Cars also have procedural motion — an engine-idle
> shudder, a pitch-back under power and its squat — which composites with the knock
> through one shared offset. See [`MOTION.md`](MOTION.md). The knock keeps its own
> deliberately small ceilings (~5 degrees) because an impact is a snap, while the
> held layers are allowed to be much larger; the two are never the same number.

Bucking and rocking a car **for effect, without touching the physics**.

A knock is a rotation plus a small lift applied to a car's *render* matrix
(`cd2KnockApply`, called from the car-draw path). The whole car rolls with it -
wheels included - while the handling model carries on exactly as before. Hitting
something should shove and sound like it hurt, without the physics having to agree
about it.

That is the point of having it as a convention rather than a one-off: anything can
raise a knock, and they all land in the same place, so the car has one way of being
hit instead of several.

## Who raises one

| source | where | what it does |
|---|---|---|
| turbo engaging | `turbo/turbo.c` (the latch) | one firm nose-up wheelie, plus a lift |
| turbo running | - | deliberately nothing: repeated small impulses read as a wobble |
| a collision | `cainescrossfire.c` `cd2OnCollision` | both cars buck, the other at half strength |

```
cd2KnockAdd(carId, pitchImpulse, rollImpulse, yawImpulse, lift);
```

## How it moves

Two phases, and neither of them oscillates:

```
cd2KnockTick(carId)   -- once per frame per car
  phase 1 (the impulse still carries):
      angle    += velocity
      velocity *= CD2_KNOCK_DECAY / 4096        (so a big impulse ARRIVES at the
                                                 ceiling rather than approaching it)
  phase 2 (the velocity is spent):
      angle    -= angle * CD2_KNOCK_SETTLE / 4096   (eases straight back to level)
```

A **spring/damper pair was tried first and read as wobbling**: a spring between an
angle and zero *is* an oscillator, so the car rocked back and forth instead of doing
one firm movement and settling. That is why there is no spring here. An impulse
that has to be carried there cannot be stiff either, which is the other half of why
this is the shape it is.

A useful consequence of phase 1: an impulse of about `CD2_KNOCK_MAX_PITCH *
(1 - CD2_KNOCK_DECAY/4096)` reaches the ceiling, so "how hard was that" is one
number against the max. The turbo's engagement impulse is deliberately a little
over that - a wheelie should arrive at the top, not creep toward it.

Each axis is clamped on its own (`CD2_KNOCK_MAX_*`), so however hard the hit the
car cannot end up spinning on its side. The lift settles on a softer pair of rates
and is clamped to `CD2_KNOCK_MAX_LIFT`, in one direction only: a knock may raise the
body to clear what it is leaning on, never lower it into it.

A car at rest costs nothing - `cd2KnockTick` returns immediately when every channel
is zero, which is the common case. The top of each movement is logged
(`knock car=N peak pitch=X of Y`), which is how an impulse can be checked against
the ceiling it was meant to reach.

## Tuning

Everything is in `knock/knock.h`:

- `CD2_KNOCK_DECAY` / `CD2_KNOCK_SETTLE` - how fast the impulse carries, and how
  fast the settle eases out. Lower decay is a shorter, sharper movement.
- `CD2_KNOCK_SETTLE_FRAMES` - the settle's deadline, 15 frames (0.5s). An
  exponential approaches zero forever, so without this "eased back" takes a second
  and a half to look finished; at the deadline what is left is snapped away.
- `CD2_KNOCK_IMPULSE_TO(maxAngle)` - the impulse that exactly reaches a ceiling.
  **Size impulses with this**, not by hand: the displacement an impulse buys is
  `impulse / (1 - decay/4096)`, so changing the rates silently resizes every
  hand-picked impulse. The turbo's is 120% of it, because a wheelie should arrive
  at the top rather than creep toward it.
- `CD2_KNOCK_MAX_PITCH` / `_ROLL` / `_YAW` - the ceilings per axis.
- `CD2_KNOCK_MAX_LIFT` / `CD2_KNOCK_LIFT_PER_HIT` / `CD2_KNOCK_LIFT_DECAY` /
  `_SETTLE` - how far a knock may lift, what an impulse buys, and how it comes back
  down.
- `CD2_KNOCK_HARD_SHIFT` - how a collision's `howHard` becomes an impulse.
- `CD2_KNOCK_MIN_IMPULSE` and `CD2_KNOCK_COOLDOWN` - the reasons knocks are rare.

Those last two matter more than they look. Cars rub against each other and against
walls constantly: without them a 600 frame arena run raised 1093 knocks, which is a
permanent buzz rather than a reaction. With a minimum impulse and a per-car
cooldown the same run raises 116 - all of them real hits. If the car ever looks
like it is vibrating rather than being hit, those are the two numbers.

## Why this shape

It matches how the Twisted Metal games *read*: an anti-simulation handling model
with the body animated separately from the wheels, a small upward nudge so a shove
does not put the body inside what it hit, and a damped settle back to rest. The
research behind that conclusion, and what is documented vs inferred, is in
`TURBO.md` - the same paper covers both, since the turbo is what needed it first.

## The weight shift

A knock does two things, not one. It rotates the body, and it translates it along
the car's own forward axis - the second is what makes the rotation read as weight
moving rather than the car pivoting on the spot, which is what it looked like when
only the angle was there.

    turbo engaging        weight 38 units BACK   (a squat onto the rear wheels)
    hit head-on           weight forward, half the impulse
    hit by another car    weight forward, quarter impulse

`CD2_KNOCK_MAX_SHIFT` (55 units) clamps it, so the body can never visibly separate
from its own wheels.

## The wheels go with it

By default the engine rotates the BODY model only and leaves the wheels on the
un-rotated draw matrix, so they stay level - right for a body lean. A knock is the
opposite: it moves the whole car, so the module sets `rigidWheels` on the
`JER_EVENT_CAR_DRAW` args and the wheels take the same matrix, rotation and
translation both. It is set only while a knock is actually running, so an ordinary
slide keeps its level wheels.

## The ceiling

The angles are a ceiling, not an amount: an impulse is sized with
`CD2_KNOCK_IMPULSE_TO(max)` and carries the angle up to it, so changing a decay
rate resizes every impulse that uses it rather than silently shrinking the motion.
`CD2_KNOCK_DECAY` is the dial for how hard the jolt reads: up a little puts more
frames on the up-stroke, and it still arrives at the same ceiling.

They are also deliberately TINY - pitch 57, about 5 degrees, and less on the
others. Two earlier passes had 900 (about 79 degrees) and then 240 (about 21), and
both were wrong for the same reason: a knock is not a fast rotation. Any real lean
of the model reads as the car tipping, and past a few degrees it reads as a flip
however brief it is. The motion is carried by the TRANSFORM instead - the shift
along the car, the lift, and the pivot - with the angle as an accent on top. That
composition is what makes it look like the car rocking rather than rotating on the
spot.

## The pivot

Rotating the basis turns the car about the model's origin, which is somewhere in
its middle. A wheelie has to turn about the REAR axle with the nose coming up; a
stoppie about the FRONT with the tail coming up. So the origin is moved to
compensate (`CD2_KNOCK_PIVOT_DIST`, the distance to the axle): the body rises by
the arc the far end would have swept, which is what fakes the wheelie.

The sign works out so the car only ever moves UP, whichever way it is pitching, so
a knock can never push the car down through the ground it is standing on. See
`cainescrossfire-vehicle-knock` in project memory for the whole convention.
