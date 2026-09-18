# The vehicle knock

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
| turbo engaging | `turbo/turbo.c` (the latch) | a nose-up pitch, plus a lift |
| turbo running | `turbo/turbo.c` (each 6th frame of boost) | a light sustained shove |
| a collision | `cainescrossfire.c` `cd2OnCollision` | both cars buck, the other at half strength |

```
cd2KnockAdd(carId, pitchImpulse, rollImpulse, yawImpulse, lift);
```

## How it moves

An impulse adds **velocity**, never an angle. The angle is carried there by a
spring, so it eases in, overshoots slightly and settles:

```
cd2KnockTick(carId)   -- once per frame per car
  velocity -= angle * CD2_KNOCK_SPRING / 4096     (the spring pulls it back)
  velocity *= CD2_KNOCK_DAMPING / 4096            (the damper takes the wobble out)
  angle    += velocity                             (and the angle follows)
```

Setting the angle directly is a step change, which is what made the first version
of the turbo kick read as stiff. An impulse that has to be carried there cannot be
stiff, which is exactly why this is the shape it is.

Each axis is clamped on its own (`CD2_KNOCK_MAX_*`), so however hard the hit the
car cannot end up spinning on its side. The lift springs back on a softer pair of
constants and is clamped to `CD2_KNOCK_MAX_LIFT`, in one direction only: a knock
may raise the body to clear what it is leaning on, never lower it into it.

A car at rest costs nothing - `cd2KnockTick` returns immediately when every channel
is zero, which is the common case.

## Tuning

Everything is in `knock/knock.h`:

- `CD2_KNOCK_SPRING` / `CD2_KNOCK_DAMPING` - how loose the car looks. Lower spring
  is a slower recovery; lower damping is more wobble before it settles.
- `CD2_KNOCK_MAX_PITCH` / `_ROLL` / `_YAW` - the ceilings per axis.
- `CD2_KNOCK_MAX_LIFT` / `CD2_KNOCK_LIFT_PER_HIT` - how far a knock may lift, and
  how much lift an impulse buys.
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
