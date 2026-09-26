# Caine's Crossfire — handling model

This documents how the Caine's Crossfire car feels, and how it maps onto the
Twisted Metal 2 / Twisted Metal: Black handling post-mortem. It is the
"so I know how to tune it" reference for `cainescrossfire.h`.

## Target

The goal is TM's *arcade combat* feel, not realism:

- **TM2** is a nearly-planar rigid body: **direct yaw control**, very high
  lateral grip, a **binary grip/slide threshold**, and **collision
  forgiveness** (a wall kills only the *into-wall* velocity and lets you
  scrape along it).
- **TM Black** adds a real 4-point suspension, weight transfer, and a
  continuous slip-angle tire curve.

REDRIVER2 already ships a 4-point suspension (`wheelforces.c`). Caine's Crossfire
keeps that suspension for **visual** pitch/roll/life, but drives the car's
**horizontal** motion with a TM2-style arcade model: it is the post-mortem's
"ideal hybrid" — direct yaw authority + suspension, tuned so slides are easy
to enter and easy to recover from.

## What Caine's Crossfire actually owns

The engine runs the full suspension and calls a `JER_EVENT_CAR_TORQUE` hook
after it. Caine's Crossfire's handler then:

1. Replaces the horizontal velocity with a **point-mass** model (TM2):
   throttle/brake add forward force; a lateral "grip" term scrubs the
   velocity toward the car's heading; there is **no per-wheel tire slip**.
2. Drives **yaw directly** from the steering wheel (TM2), rate-limited by an
   angular acceleration (a cheap stand-in for `steering_response`).
3. Drives a **Tight Turn** as a yaw multiplier + momentum bleed (TM2 quick
   turn), and a **slide lock** for the binary grip/slide feel.
4. Leaves the suspension's vertical/roll/pitch untouched, so the body still
   rolls over bumps like Black.

The actual mapping:

| Post-mortem concept | Where it lives | Tunable |
|---|---|---|
| `engine_accel` / `top_speed` | direct drive in torque | `CD2_ACCEL`, `CD2_TOP_SPEED`, `CD2_SPEED_SCALE`, `CD2_DRAG` |
| velocity-dependent drag clamp | throttle/coast pass | `CD2_DRAG`, `CD2_TOP_SPEED` |
| brake decel → reverse | brake pass | `CD2_BRAKE`, `CD2_REVERSE_FRAC` |
| `max_yaw_rate` (direct yaw) | steering pass | `CD2_HANDLING` |
| `steering_response` (rate limit) | yaw accel | `CD2_ANGULAR_ACCEL` |
| speed-dependent yaw falloff | steering pass | `CD2_YAW_SPEED_FALLOFF` (opt-in) |
| quick turn (yaw ×, speed cut) | Tight Turn | `CD2_TIGHT_MULT`, `CD2_SLIDE_BLEED`, `CD2_SLIDE_ACCEL_FRAC` |
| lateral `static_grip` / `dynamic_grip` | grip pass | `CD2_GRIP`, `CD2_SLIP_REDUCTION` |
| `slide_threshold` (binary grip) | slide lock | `CD2_SLIDE_MIN_SPEED`, `CD2_SKID_LOCK_LAT`, `CD2_SKID_LOCK_GRIP` |
| slide recovery (snap back) | recovery pass | `CD2_RECOVER_*` |
| collision forgiveness (wall scrape) | `bcollide.c` wall hook | `CD2_WALL_KEEP` |
| anti-flip / air control | engine suspension + stock | (engine) |
| body roll / suspension | engine (kept, visual) | `CD2_ROLL_*` |

## The wall-scrape fix (collision forgiveness)

The engine's `CarBuildingCollision` used to scale **all** horizontal velocity
by the restitution, which made walls stop the car dead (TM2 would kill only
the *into-wall* component and keep the tangential scrape).

Now the `JER_EVENT_GET_WALL_RESTITUTION` hook applies `wallRest` to the
**normal (into-wall) component only**:

- `CD2_WALL_KEEP = 256` → walls absorb the impact and the car keeps its
  along-wall momentum, scraping instead of stopping.
- `4096` → stock outward impulse + spin (the engine default with no handler).

## Gaps / where TM2 vs TM Black diverge

- **Slip-angle tire curve (Black)** is not implemented; grip is a hand-rolled
  threshold/scale model (closer to TM2). Fine for the target feel.
- **`CD2_YAW_SPEED_FALLOFF`** is off by default. TM2 applies it always; set
  ~1200–1600 if top-speed turns feel too twitchy.
- **Quick-turn speed cut** (`forward_velocity *= 0.6`) is emulated by
  `CD2_SLIDE_BLEED` only while the slide is active; there is no instantaneous
  cut on the trigger press.
- **Air control** is stock (yaw only from the steering pass); roll/pitch
  authority is not added.

## Tuning quick reference

- "too twitchy" → lower `CD2_HANDLING`, raise `CD2_ANGULAR_ACCEL` (softer
  attack), or set `CD2_YAW_SPEED_FALLOFF`.
- "too drifty" → raise `CD2_GRIP` / lower `CD2_SLIP_REDUCTION` (max 6).
- "slides but then snaps hard" → soften `CD2_RECOVER_*`.
- "hits walls and stops" → lower `CD2_WALL_KEEP` (already near 0).

## Gravity, angular settling & suspension (engine physics)

Caine's Crossfire also overrides the engine's per-car physics via
`JER_EVENT_GET_PHYSICS_PARAMS`, so the whole chassis — not just the horizontal
point-mass — can be re-tuned:

| Tunable | Stock | Feeds |
|---|---|---|
| `CD2_GRAVITY` | `-7456` | vertical acceleration (`wheelforces.c`) |
| `CD2_ANGULAR_DAMPING` | `128` | `ConvertTorqueToAngularAcceleration` — pitch/roll/yaw settle rate |
| `CD2_SPRING_RATE` | `230` | suspension spring constant |
| `CD2_SPRING_DAMPING` | `100` | suspension damper constant |

- "floaty / soft suspension" → raise `CD2_SPRING_RATE` and `CD2_SPRING_DAMPING`.
- "body rocks/rolls too long after bumps" → raise `CD2_ANGULAR_DAMPING`.
- "jumps feel too floaty or too heavy" → lower/raise `CD2_GRAVITY` (more
  negative = stronger gravity).

These are compile-time macros; exposing them as config/menu sliders is a
possible follow-up.

## Wreck sounds (skidding / wheel noise stop with the car)

A destroyed car must go quiet. The tyre-screech and surface-noise handling in
`CheckCarEffects` (`handling.c`) is gated on the same damage cap the rest of the
mod uses (`MaxPlayerDamage[0]`), with one subtlety worth knowing:

- The "play skid sound" block only runs when `desired_skid != skidding.sound`.
  For a wreck `desired_skid` is `-1`, so the guard must **not** pre-set
  `skidding.sound = -1` (that made the two equal, skipped the block, and left a
  screech that was already playing ringing forever). Skipping the tyre branch
  instead lets the block see the change and `StopChannel`/`UnlockChannel` the
  live sound.
- The surface (wheel) noise is skipped for a wreck the same way, so a wreck
  with a little speed left makes no wheel noise either.
- The engine channels were already handled: `cainescrossfirecarfx.c`'s
  `cd2cOnCarEngineSound` drives rev/idle volume to `-10000` once
  `cd2CarTotaled`, and `gamesnd.c` silences them outright when the player has no
  car (after the eject).

## The damage model (how hard things bite)

Every damage path the mod touches runs through `cainescrossfiredamage.c`, and the
knobs are in the pause menu / `[cainescrossfire]` ini. The defaults below are the
post-tuning ones (cars were dying far too fast before):

- **Weapon damage — `weapon_damage`, default 50%.** EVERY weapon hit funnels
  through one choke point, `cd2WpnDamageCar` (`weapons/core/weapons.c`): direct
  hits, burst/volley splash, the AOE blasts, dropped mines and the damaging
  specials. The global percentage is applied there, so the whole arsenal's bite
  is a single number; the per-target cuts (opponent `ai_damage_taken`, the
  traffic multiplier, the profile's Armor) stack on top.
- **Scenery impact threshold — `scenery_damage_threshold`, default 122880.** The
  engine already ignores a wall/building hit below `strikeVel` 20480
  (`bcollide.c:DamageCar`); `JER_EVENT_GET_DAMAGE_SCALE` now carries that raw
  `impact` so `cd2OnDamageScale` can raise the bar: below the threshold the car
  takes **no** damage at all — a scrape is not a crash, and the map is not worth
  taking real damage over. Above it, `scenery_damage` (default 25%, or whatever
  the ini says) still scales the hit. The contact is counted *before* the
  threshold test, so the traffic tumble still sees it.

  The bar is set from what crashes actually measure. Full throttle into Havana's
  scenery (`30:thrust:1`), the impacts that arrived were **21861** (a nudge),
  **78058** and **113922** (ordinary crashes) and **290166** (a violent one) —
  so the default sits above an ordinary crash and only the violent kind is
  charged. Measured effect on that run: charged impacts 3 → 1 (the 290k one) and
  the player's total damage **14669 → 642**. Lower it to 20480 for the stock
  engine behaviour; 0 turns the gate off.
- **One scenery bite per contact — `CD2_SCENERY_HIT_COOLDOWN` (45 frames).** A car
  leaning on a wall reports an impact *every frame* it is in contact, and the
  engine's `DamageCar` has no notion of a contact already resolved — so the same
  collision used to be charged for as long as the car stayed against it, which is
  how "sticking to a light collision" added up to a write-off. After a charged
  impact the car cannot be charged again for 45 frames (~1.5s of the 30fps sim),
  which is generous next to the physics rate and shorter than a genuine second
  crash. `gCd2SceneryHits` still counts every frame of contact, so the traffic
  tumble (which reads its *change*) is unaffected.
- **A stacking budget — `CD2_SCENERY_STACK_WINDOW` (90 frames).** The cooldown limits
  how OFTEN a contact is charged, not what a run of them costs. Sliding along a wall
  passes the threshold on every contact, because the hook's `impact` is the raw strike
  VELOCITY — a fast scrape and a head-on hit look identical to it (measured while
  grinding a wall: impacts of 133k and 342k) — so a few seconds of scraping was several
  heavy bites in a row at full scale. Jaret, playing: *"sliding along scenery totally
  killed it."* Each bite after the first inside the window is therefore worth HALF the
  one before it (100%, 50%, 25%, 12.5%…), so however long the contact lasts the total is
  bounded at about twice a single hit. Logged as `scenery dmg stacked: car=N bite K of
  this window -> P% of it`.
- **Car-vs-car aggressor immunity.** In a two-car hit the car driving INTO the
  other deals the damage, and that car should not be hurt by its own attack.
  `cd2OnCarVsCar` compares each car's approach speed along the line between them
  (`st.n.linearVelocity`, 64-bit dots) and zeroes the faster-approaching car's
  `value` at the very END, after every scaling. A near-even head-on (equal
  approach) leaves both cars taking stock damage, so mutual crashes still hurt.
  This is what stopped **Deadstar** dying to its own Death Dash (a ram always
  reads as the aggressor).
- **Own-blast immunity.** The same rule for explosions: `cd2AoeBlast`
  (`weapons/aoe/aoe.c`) takes the car it directly hit as `skip` AND a second car
  to keep clear as `spare`, and the projectile pool passes the shot's own
  LAUNCHER as that `spare`. A ground/scenery/range impact always excluded the
  shooter (it was the `skip`), but a DIRECT hit passed only the struck car — so a
  shot that connected right beside its own car splashed it, with the kill
  attributed to itself. That is what the Obelisk's flank barrage needed: its
  missiles leave outward from both sides, next to their own car. Dropped mines
  deliberately pass no `spare` — a mine is your own problem.
- **The lock-on health bar.** The readout under the locked target's name
  (`hud/lockon.c`) draws `1 - totalDamage / cd2CarMaxDamage(target)` via
  `jer_hud_panel_bar`, tinted green → red.

Headless check: with `debug_log` on, a run logs `scenery dmg ignored: ... impact=N
< thresh=...`, `scenery dmg scale: ... impact=N`, `car-car aggressor: car=N
spared, other=M` and `aoe blast spared the shooter car=N (its own blast, d=… <= …)`
— enough to confirm the thresholds, the aggressor rule and the own-blast guard are
firing without a play-test.

