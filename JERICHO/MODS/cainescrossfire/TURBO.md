# Turbo

The Felony bar is gone: in its place is a turbo meter, and double-tapping a drive
button lights it.

## The trigger

Press a drive button, then press it again within **`CD2_TURBO_TAP_GRACE`** (12
frames, 0.4s at 30Hz) and turbo latches **on** for that car. The reverse button does
the same for a reverse boost.

It then holds until **either** the meter runs out **or** the driver lets that button
go. So a boost can be stopped early by releasing, and it cannot be left running by
accident. Edge detection happens in the module's pad hook, ahead of its control
remap, because a double tap is a driver gesture rather than a button binding.

`turbo:<0|1>` in the debug driver raises one programmatically, which is how a
headless run can watch the meter; the same hold is how a scripted or AI driver
would hold one deliberately.

## The meter

Starts full, and is **`CD2_TURBO_METER_FRAMES`** (600 frames = 20 seconds of boost).
It drains *only* while boosting, and it never trickles back up - it refills on the
events in `CD2_TURBO_REFILL_*` (respawn, level start). That is what makes spending
it a decision rather than a wait.

It is filled the first time a car is seen, so there is no level-start hook to depend
on, and the drain is frame-exact: a scripted run engaged at frame 60, showed
`360/600` at frame 300 and `200/600` after a release at 460 - 240 frames drained for
240 frames elapsed, 160 for 160.

## What a boost does

| | |
|---|---|
| top speed | ×**`CD2_TURBO_SPEED_PCT`** (125) |
| acceleration | ×**`CD2_TURBO_ACCEL_PCT`** (125) |
| rev ceiling | +**`CD2_TURBO_OVERREV_PCT`** (25) |
| engine note | +**`CD2_TURBO_PITCH_BOOST`** (420 SPU pitch units) |
| the car itself | a shove, plus a knock (see `KNOCK.md`) |
| the exhaust | `SMOKE_FIRE` out of the back while it lasts |
| the bar | white, pulsing white ↔ red twice a second while being spent |

Both speed and acceleration are raised in `cd2GetStats`, last, because that is where
the stats finally settle: a car's own derivation runs above it and must not
overwrite the boost, and every cap and acceleration in the sim reads those two
fields. The debug dump reports them rather than the flag, because that is what can
be checked - `220 -> 275` top speed and `16 -> 20` acceleration across an
engagement, exactly 1.25×.

The **over-rev** is the other half: the speed cap alone would raise the ceiling in
silence, so the gearbox is allowed to wind `CD2_TURBO_OVERREV_PCT` past its own
limit while boosting, and the engine note is lifted on top of that. One without the
other would either sound boosted without being it, or be boosted in silence.

The **shove** is a real push along the heading, `CD2_TURBO_KICK_FORCE_PCT` of top
speed, applied the moment the boost starts - throttle or not, which is the point of
a shove - and before the rest of the frame's speed maths.

## The bar

The Felony bar is *replaced*, not supplemented. `FelonyBar` is exported, so the
module takes over its position, colour and tag and lets the engine draw it in the
usual place: no engine render code, and the bar lands where the player already looks
for one. Its pulse is `CD2_TURBO_BAR_PULSE_FRAMES` (15 frames - twice a second).

The crime value behind the bar is deliberately untouched: it still feeds the felony
checks, it just is not what the bar shows any more.

## Where the numbers came from

Twisted Metal (2012) is the reference the module's handling model was already
written against, so the turbo was checked against it. What the research found, and
what is actually documented:

- **Double-tap gas for turbo** - documented, and the trigger we use (though chosen
  here before the research landed).
- **A boost is a depleting meter, not ammo** - documented. *Where* it refills is our
  one deliberate departure: TM recharges over time, this refills on an event, which
  makes a boost a decision instead of a wait.
- **TM's turbo is about 2× top speed for a fixed budget per vehicle** (Darkside:
  60 mph normal, 120 turbo, "Turbo Length 50 secs"). Ours is 1.25×, which is a much
  gentler boost on purpose.
- **Bodies animated separately from the wheels, with a small upward nudge and a
  damped settle** - inferred for TM (there is no public post-mortem), but it is the
  established convention for arcade racers, and it is what `KNOCK.md` implements.
- **Juice: camera shake, a one-shot engage layered under a sustained loop, particles
  at the exhaust, effects held ~0.3-0.5s** - inferred from genre convention. Sound
  layering and exhaust particles are in; **camera shake is not implemented yet**.

Sources: PS.Blog multiplayer hands-on (Jaffe on the cars being "fighter jets");
IGN's control layout and wiki (double-tap gas, Turbo Dash); the Twisted Metal wiki
entry and Darkside's stat table (turbo lengths, recharge rates).
