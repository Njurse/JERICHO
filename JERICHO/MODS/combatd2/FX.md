# Combat D2 — Explosions & weapon FX

Every weapon impact in Combat D2 goes through a small **parametric explosion
library** (`weapons/fx/`) instead of the engine's three fixed bangs. A profile
resizes, recolours and spins the stock explosion, and decides whether the blast
pushes/damages cars — so a hit reads as the weapon that caused it.

Nothing here adds new geometry: the library is a friendly front-end for the
JERICHO explosion hooks the engine now fires around its own explosion code
(`job_fx.c` / `bomberman.c`, inspired by the original `bomberman.c`).

## The engine side (three hooks)

| Event | Fired in | What a module can do |
|---|---|---|
| `JER_EVENT_EXPLOSION_SPAWN` | `AddExplosion` (`job_fx.c`) | attach a profile: `speed`/`hscale`/`rscale`, `tintR/G/B`, `yawRate`, `collide`, `colScale`, and rewrite `type` |
| `JER_EVENT_EXPLOSION_DRAW` | `DrawExplosion` (`job_fx.c`) | tint/spin the stock mesh, or `override` and draw your own |
| `JER_EVENT_EXPLOSION_COLLIDE` | `ExplosionCollisionCheck` (`bomberman.c`) | query: may this explosion push/damage this car, at what box scale |

A profile is passed to `AddExplosion` as a **custom explosion type** (an id
`>= CD2_FX_BASE`, 1000). The `SPAWN` handler resolves it, writes the parameters
onto the new `EXOBJECT`, and rewrites the type to the profile's stock base bang
so the engine's sound + collision branches keep working. The `DRAW` and
`COLLIDE` hooks then read those stored fields back automatically — that is why
`fx.c` only needs the `SPAWN` handler.

`MAX_EXPLOSION_OBJECTS` was raised from 5 to **16** so a barrage plus other
impacts can't starve the pool.

## Profiles (`weapons/fx/fx.c`)

| id | kind | size (`hscale`) | colour | collide |
|---|---|---|---|---|
| `CD2_FX_DEFAULT` | fallback bang | 4096 | stock | **1** |
| `CD2_FX_MISSILE` | missile warhead | 4700 | orange (255,170,110) | 0 |
| `CD2_FX_MINE` | mine | 4200 | red-orange (255,140,60) | 0 |
| `CD2_FX_SEEKER` | seeker | 2600 (smaller) | purple (175,90,255) | 0 |
| `CD2_FX_CLUSTER` | cluster parent | 4096 | deep orange (255,110,20) | 0 |
| `CD2_FX_BOMBLET` | cluster sub-blast | 1600 | orange (255,150,40) | 0 |
| `CD2_FX_WRECK` | dying car | 6000 | stock fire | 0 |
| `CD2_FX_ZOOMY` | zoomy missile | 950 | cool blue (140,230,255) | 0 |

- `collide = 0` means the explosion is **visual only** — the weapon's own damage
  + `cd2WpnKnock` stay authoritative, so a car is never pushed twice. The
  mission bangs (`CD2_FX_DEFAULT`) keep `collide = 1`.
- `tintR/G/B = -1` keeps the stock colour; otherwise each channel scales the
  engine's computed colour (0..255).
- `yawRate` is extra spin in PSX angle units per frame.

### Dying cars are spectacular but harmless

A totaled car explodes on the `CD2_FX_WRECK` profile: big and fiery (collapse
6000) but `collide = 0`, and `cd2FxBarrage` is called with `radius = 0` /
`damage = 0`. So the wreck goes up in flames without hurting or shoving
anything around it (`combatd2combat.c`).

## The barrage sequencer

`cd2FxBarrage(at, car, fxId, count, interval, jitter, radius, damage, skip)`
schedules `count` blasts `interval` frames apart, each jittered by up to
`+/-jitter` world units:

- **`car != NULL`** → the burst **sticks** to the car: `at` is captured as an
  offset in the car's local frame and re-projected every frame, so the blasts
  ride a moving target.
- **`car == NULL`** → the blasts stay at the world position.

`cd2FxStep` (run from the `JER_EVENT_FRAME` hook) fires each due blast through
`cd2AoeBlast`.

## Weapons

| weapon | body colour | impact profile | burst |
|---|---|---|---|
| MISSILE | orange | `CD2_FX_MISSILE` | — |
| MINE | red | `CD2_FX_MINE` | — |
| SEEKER | purple | `CD2_FX_SEEKER` (smaller) | — |
| CLUSTER | deep orange (255,90,0) | `CD2_FX_CLUSTER` | 5 × `CD2_FX_BOMBLET`, 4 frames apart, ±60, sticks to a hit car |
| ZOOMY | pale cyan (140,230,255) | `CD2_FX_ZOOMY` (small) | 10 shots, 6 frames apart, weak homing; all-ten-land bonus |

## Volley weapons (the burst launcher)

A projectile weapon with `burstCount > 0` fires a staggered BURST instead of
a single shot. The projectile pool runs it (`cd2ProjectileBurst`): it spawns
`burstCount` shots `burstInterval` frames apart, re-aiming each from the car so
the volley trails the launcher.

If EVERY shot in the volley lands on a car, the shot that lands last deals
`volleyBonusDamage` extra damage plus a `volleyBonusKnock`-sized shove. A shot
that hits scenery/ground/its range retires the volley — so a single miss
forfeits the bonus. (Per-shot damage is deliberately tiny; the volley is the
reward for good aim.)

**ZOOMY MISSILES** (`weapons/projectile/zoomy.c`) is the example: 10 fast shots
6 frames apart, `homingRate = 40` (extremely weak — the seeker uses 340), 40
damage each, all small explosions; land all ten and the last one adds 900
damage + a hard shove. The pool's volley tracking uses a generation counter so
a recycled group slot can't miscount a stale shot.

### Adding a kind

1. Add an id to the `CD2_FX_*` enum in `fx.h`.
2. Add a row to `gFxDefs` in `fx.c`.
3. Point a weapon's `CD2_WEAPON_DEF.impactFx` at it (and, for a burst, set
   `barrageCount` / `barrageInterval` / `barrageJitter` / `barrageFx` /
   `barrageRadius` / `barrageDamage` / `barrageStick`).

A weapon with `impactFx = 0` keeps its stock `explosionEffect`, so untuned
weapons are unaffected.
