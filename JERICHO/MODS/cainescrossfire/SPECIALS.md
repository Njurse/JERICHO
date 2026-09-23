# Vehicle specials

Each vehicle profile owns one unique **special** weapon. They live in
`weapons/special/`, one file each, and are driven through the ordinary weapon
framework: a `CD2_WEAPON_DEF` row plus a fire function, reachable by the same
`cd2WpnTryFire` the shared weapons use (the player's trigger, the AI, and the
debug driver all get them). What a special adds beyond a shared weapon is a
**status**: a per-car timed effect with its own hooks.

- Internal ids are `CD2_WID_SPECIAL_<CAR>` — internally named **`special_<car>`**
  (`special_hornet`), separate from the on-screen name in the def's `displayName`
  (`"Spike Storm"`).
- The **recharge**, the **ammo** and the **display name** all live in the
  special's own def (`refireCooldown`, `maxAmmo`, `displayName`) — the vehicle
  profile only **names** the weapon (`specialWeapon`), so weapon data is never
  duplicated in a profile.
- Ammo is **per car** (`weapons.c`, `gCarAmmo[MAX_CARS][CD2_WID_COUNT]`), so each
  contestant's special is its own. A car is handed its special when its profile
  is first assigned, and the **player starts on it**.

Firing one headlessly (the debug driver):

```
60:select:hornet        # arm the special (the ring comes up)
90:fire:hornet          # launch
150:fire:avalanche
180:fire:corvo
210:fire:bruxa
240:fire:highwayman
270:fire:deadstar
```

## The eleven

| internal | display | car | recharge | capacity |
|---|---|---|---|---|
| `special_hornet` | Spike Storm | Hornet | 600 (20s) | 3 |
| `special_avalanche` | Monster Crush | Avalanche | 900 (30s) | 2 |
| `special_corvo` | Siren's Wrath | Corvo | 540 (18s) | 3 |
| `special_bruxa` | Double Boom | Bruxa | 450 (15s) | 4 |
| `special_highwayman` | Breath of Fire | Highwayman | 660 (22s) | 3 |
| `special_deadstar` | Death Dash | Deadstar | 750 (25s) | 2 |
| `special_obelisk` | Missile Barrage | Obelisk | 900 (30s) | 1 |
| `special_bootlegger` | Lead Hail | Bootlegger | 600 (20s) | 2 |
| `special_invocada` | Cyclone | Invocada | 780 (26s) | 2 |
| `special_fixer` | Laser Lock | Fixer | 150 (5s) | 2 |
| `special_wheelman` | Shockwave | Wheelman | 700 (23s) | 2 |

### Hornet — Spike Storm

The ring deploys while the special is **armed**: eight spikes stand up around
the car (one per compass direction — a bright vertical spike with a faint spoke
back to the car), and any other car inside the ring takes contact damage and a knockback
every 15 frames. The ring is measured out to what the victim's own body
reaches, so a car whose flank is in the spikes is caught even with its centre
outside them. Pressing **fire launches** the eight outward in their eight
facing directions — weakly homing, a narrow lock cone — exploding on whatever
they hit (world or car). The launch spends the charge and starts the recharge.

### Avalanche — Monster Crush

A 5-second surge of max normal speed and raised grip. During it, ramming another
car makes Avalanche **climb on top**: the attacker is held over the victim, the
victim's velocity is pinned (trapped), the attacker's tyres spin and it can still
be steered, then it shoves the victim off and drops back.

The held victim is pinned **flat on the road**: its ride height is held where it
was before the crush and its tumble is stopped, so the press cannot shove its
underside down through the surface. The attacker is held **level with the
victim's own origin** — the collision solver decides the final height, so a
positive lift only gave it more to push back up (which is what floated the truck
before). Crunching plays throughout on the engine's own heavy-crash sample.

### Wheelman — Shockwave

**PLACEHOLDER.** A big bang goes off where Wheelman is standing and everything
within 2200 units takes falling damage from it: 1800 at the centre, nothing at
the rim. It is a bomb he is inside - short range, awkward to place, very good
when it lands.

The bang itself has **no collision**: its FX profile (`SHOCK`) is `collide = 0`,
so the engine's own explosion push/damage branch never runs for it and the
explosion is pure presentation. That is what stops the blast double-dipping -
the special's own radial damage is the whole of it, once. Wheelman is skipped as
its victim (he paid the charge to be at the centre of it) but is still thrown up
by it, so it reads as a detonation rather than a decal.

### Bootlegger — Lead Hail

**PLACEHOLDER.** Five seconds of machine gun out of the gunner's side, wound up
from a 9-frame interval to a 2-frame one over the first second, so the gun reads
as spinning up rather than starting at full chatter. Each bullet is nearly
nothing (55); the stream is the weapon. The rounds are the ordinary RAYCAST
class, so they travel, hit and stop on scenery for free - only the def differs.

### Invocada — Cyclone

Six seconds of storm. The car goes **solid black** - body flat black and wheels
hidden, so it is a silhouette rather than a car - with big black smoke puffs set
around a circle that turns as the storm runs (the `SMOKE` FX profile: a bang
with the colour taken out and the expansion slowed).

Anything within 1500 units is dragged toward the eye and pushed across it, so
victims are pulled in and left orbiting rather than flung away, with the pull
scaled by how deep in they are and a dead zone right at the car so nothing
judders on top of it. Damage is a slow tick (220 every 15 frames) - the storm is
a place you do not want to be, not an instant.

### Fixer — Laser Lock

**PLACEHOLDER.** While the special is *selected*, a beam runs from Fixer's gunner
to whatever it holds in its **forward cone** with a clear line to it - the same
"always on while armed" the Hornet ring uses. The beam is a lock, not a hit: a
different target resets the charge, and losing the target (out of the cone, out
of range, or scenery in the way) takes both the beam and the charge with it.

It goes **white → yellow → red** as the same car is held: white for the first
two seconds with the turn eased so the fade is quick at the end of it, yellow
turned at ~2s, red by ~4s, full charge at ~5.5s. Firing spends it: damage scales
400 → 2600 and the twist away 200 → 1400 across the charge, and there are five
seconds between shots, so a charge cannot simply be sat on.

### Obelisk — Missile Barrage

A three-second salvo. Twice a volley — a pair at a time, out of **both flanks at
once** and from the **midsection** rather than the nose — cheap homing missiles
pour out, and the weave flips every volley, so the swarm snakes toward whatever
it is chasing instead of flying in a straight line. Individually they barely
scratch (55 a hit against a 55-frame hold's worth of them); there are a great
many, and together they are a barrage.

### Corvo — Siren's Wrath

A 4-second siren. The **police siren** (the engine's siren sample, `SOUND_BANK_VOICES`
0 — what its siren cars play) wails on a loop, and the **siren light is forced onto
the car** for the whole window: Corvo is the model-0 car, but the engine only draws
a siren light for cop/pursuer cars (`cars.c`, `CarHasSiren` + controlType), so the
special drives `AddCopCarLight` itself. A lightning bolt revolves around the car and,
every 20 frames, arcs onto the nearest other car in range: moderate damage, a shaky
twist (angular velocity) that reads as electrocution, and a small vertical jump.

### Bruxa — Double Boom

The repurposed double shotgun: **40 pellets** (twice the demo weapon's 20), a
hard recoil shoved back into the shooter, and a loud, pitched-down boom. Target
recoil rides the per-pellet knock, so more pellets landing = a harder shove.

### Highwayman — Breath of Fire

A 4-second narrow cone of flame pours from the nose (short-range raycast
particles). While it burns, tapping fire launches a **homing fireball** that
seeks a target and explodes for extra damage.

### Deadstar — Death Dash

Instant acceleration to turbo top speed for 2.5 seconds, with the horn blaring
twice (0.75s apart). While dashing, the collision damage Deadstar deals is
**x4**, and a near-90-degree hit into a car's side lands an extra 15%.

The dash is a RAM, so the car-vs-car **aggressor rule** (`cd2OnCarVsCar`) matters
here: the car driving into the other deals the damage and takes none of the
exchange itself. Before that rule, ramming a heavy opponent could kill Deadstar
with its own dash. See `HANDLING.md` § *The damage model*.

## What is a first cut

The mechanics are the described ones, built on the framework's primitives, but a
few are approximations to iterate on:

- Hornet's ring is a damage/knock aura drawn with the engine's line primitives,
  not a modelled spike rack (a real model is the later pass).
- Avalanche's crush holds positions directly (a rigged climb animation is later).
- Corvo's bolt, Highwayman's flame, and the ring are line/particle visuals.
- The flame/blob/pellet sounds are sample + pitch, not bespoke audio.
