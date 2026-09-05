# combatd2 — handling model

This documents how the combatd2 car feels, and how it maps onto the
Twisted Metal 2 / Twisted Metal: Black handling post-mortem. It is the
"so I know how to tune it" reference for `combatd2.h`.

## Target

The goal is TM's *arcade combat* feel, not realism:

- **TM2** is a nearly-planar rigid body: **direct yaw control**, very high
  lateral grip, a **binary grip/slide threshold**, and **collision
  forgiveness** (a wall kills only the *into-wall* velocity and lets you
  scrape along it).
- **TM Black** adds a real 4-point suspension, weight transfer, and a
  continuous slip-angle tire curve.

REDRIVER2 already ships a 4-point suspension (`wheelforces.c`). combatd2
keeps that suspension for **visual** pitch/roll/life, but drives the car's
**horizontal** motion with a TM2-style arcade model: it is the post-mortem's
"ideal hybrid" — direct yaw authority + suspension, tuned so slides are easy
to enter and easy to recover from.

## What combatd2 actually owns

The engine runs the full suspension and calls a `JER_EVENT_CAR_TORQUE` hook
after it. combatd2's handler then:

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
| brake decel → reverse | brake pass | `CD2_BRAKE`, `CD2_REVERSE_SPEED` |
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
