# Crumple — impact-driven car damage

`crumple.c` / `crumple.h` is the vertex-deformation damage library for
REDRIVER2. It replaces the ad-hoc "move vertices toward the collision point"
block that lived in `denting.c` with a proper impact-driven deformation that
respects collision geometry, vehicle mass/health, and body structure — while
keeping the original `denting.c` logic (zone damage + UV damage levels)
intact and untouched.

All math is the engine's **4096 fixed point** (`ONE`, `FIXEDH`, `RSIN/RCOS`
from `dr2math.h`). No core engine structs (`CAR_DATA`, `APPEARANCE_DATA`,
`CAR_COSMETICS`, …) are modified.

---

## 1. Data flow

```
collision detected
   │
   ├─ car-car (handling.c GlobalTimeStep, howHard > 0)
   │     crumple_recordImpact(cp, c1, collisionpoint, normal, howHard)
   │
   └─ car-vs-world (bcollide.c CarBuildingCollision, strikeVel > 0)
         crumple_recordImpact(cp, NULL, collisionResult.hit,
                              collisionResult.surfNormal, strikeVel)
   │
   ▼
per-car pending impact stored in crumpleCarState[cp->id]
   (world point, INWARD world normal, strength, other car id)
   + wheel bends accumulated for wheels within wheelProximity
   │
   ▼
deferred denting pass (handling.c, ≤ 5 cars/frame, needsDenting flag)
   └─ denting.c DentCarDirectional
         • accumulates per-vertex zone damage (original code)
         • crumple_deform(cp, tempDamage)          ← NEW
         • updates polygon UV damage levels (original code)
   │
   ▼
crumple_deform transforms point+normal to car-local space and writes the
deformed vertices into gTempCarVertDump[cp->id], which DrawCar uses as the
car's vlist.
```

Per-car state is cleared on level load (`crumple_init()` from
`InitialiseDenting`) and on every car spawn/reset (`crumple_resetCar(cp->id)`
from `CreateDentableCar`). Pending impacts persist until overwritten or the
car resets — the same "last impact" semantics the old
`gCrumpleLastCollisionPoint` global had.

---

## 2. Coordinate spaces & the normal convention

Two things are easy to get wrong here, so they're spelled out:

- **World space** (engine physics) is y-down and matches the model space
  convention; `cp->hd.where` maps car-local → world directly (rotation in
  4096 fixed point, translation in world units). `DrawCar`/`drawCarMat`
  negate some axes only for the *renderer* — crumple never touches that path.
- The collision normal returned by `CarCarCollision3` / `bcollided2d` is an
  OBB face axis (magnitude ≈ 4096 = unit length). The engine convention is:
  **the normal points FROM the first car TOWARD the second car** (verified in
  `bcoll3d.c PointFaceCheck` and the impulse application in `handling.c`),
  and for world hits **from the car toward the wall** (`bcollide.c` negates
  and zeroes `surfNormal` before using it to push the car out).

Deformation must always be **INWARD**, so `crumple_recordImpact` stores the
flipped normal per car:

| victim            | inward normal stored           |
|-------------------|--------------------------------|
| cp0 (car-car)     | `-normal`                      |
| cp1 (car-car)     | `+normal`                      |
| car vs world      | `-surfNormal`                  |

World → car-local transforms:

- point: `crumpleWorldToLocal()` — `Rᵀ·(p − t)` (transpose rotation, subtract
  translation);
- normal: `crumpleRotateToLocal()` — `Rᵀ·n` (no translation — it's a
  direction).

Lever lengths are bounded by car size (~2k units), so all 32-bit products
stay far below overflow (audited: worst case `dist2·4096 ≈ 3.2e8`,
`g·dmgBoost ≤ 8192·8192`).

---

## 3. The deformation algorithm (Carmageddon/DETHRACE-style)

For each vertex of the clean model (`gCarCleanModelPtr[model]`), with the
collision point `P` and inward unit normal `n̂` in car-local space:

1. **Direction vector** `d = P − v` (clean vertex → collision point).
2. **Signed projection** `proj = d·n̂` (4096 fixed point).
   - `proj > 0` → the vertex is on the *attacker side* of the contact plane
     through `P` — exactly the verts that should be pushed inward.
   - `proj ≤ 0` → already inside/behind the plane: no inward push.
3. **Radial falloff** `falloff = 1 − |d|²/R²` (optionally squared for a
   smoother edge), so only nearby verts move. `R = falloffRadius` — set wide
   enough (400) that a hit **contaminates the surrounding body structure**
   (hood, fenders, doors) and not just the zone that was struck.
4. **Clamp** so a vertex can never travel past the collision point:
   `disp = min(disp, proj)` — a vertex starting `proj` units outside the plane
   stops exactly ON the plane (no stretch / overshoot).
5. **Surface noise**: mix in a small fraction (`damageModelMix`) of the
   artist-baked damaged-model delta (`gCarDamModelPtr[model] − clean`).
6. **Structural resistance** scales everything: a per-vertex table derived
   from the model AABB (see §5), folded together with impact strength, mass
   asymmetry and accumulated damage (see §4).

### Narrow vs wide objects (contact profile)

Poles and edges vs walls and car sides are told apart by the impact
normal's alignment with the car's local axes (a face-on wall/side hit is
perpendicular to the panel; a pole/edge clip usually isn't):

- **Face-on** (`align` = max(|nx|,|nz|) near 4096) → full falloff radius,
  standard depth — a wide, shallow crush.
- **Oblique** (`align` low) → the radius shrinks down to ~0.5× and the
  depth scales up to `1 + contactDeepen` — a tight, deep puncture.

Per impact, `effRadius2` / `depthScale` are recomputed each denting pass
from `align` and stored on the impact (see `CRUMPLE_IMPACT`).

### Compounding across collisions

Dents **compound on a persistent damaged mesh**: `gTempCarVertDump[cp->id]`
IS the damaged model — it is never recomputed from the clean mesh once
deformation begins, so new collisions (any zone) **add** to the existing
crumple instead of resetting it (this was the "forgets its deformation /
resets to clean when a different panel is hit" bug: the old code rebuilt the
whole mesh from clean every pass).

- `crumple_recordImpact` pushes each collision into a per-car impact ring
  (`CRUMPLE_MAX_IMPACTS = 8`). Hits within `falloffRadius/2` of an existing
  dent **merge** into it (howHard accumulates, strength saturates via the
  curve).
- Each impact is **baked once** (`applied` flag) into the damaged mesh; a
  merge clears the flag so the dent deepens — clamped by the impact plane, so
  it converges instead of overshooting.
- The mesh may never deviate from clean by more than `compoundCap` per axis
  (inversion guard).
- `crumple_resetCar` (car spawn) and **Repair Car** (see §9) restore the
  clean mesh.

Final vertex (first application of impact `i`):

```
disp_i = curve(strength_i) · massFactor · falloff_i · (softness + healthBoost)
         · depthScale_i · globalScale · maxDisplacement
disp_i = min(disp_i, maxDisplacement, proj_i)
v'     = v + disp_i·n̂_i + noiseMix·(damaged − clean)
```

`curve()` is the **impact curve** (`impactCurve`): `1` = linear
(`howHard/impactNormalize`), `2` = squared (`FIXEDH(strength²)`). Squared
makes low-speed bumps barely dent while hard/high-speed impacts deform
disproportionately more. `globalScale` (4096 = 1.0x) is the master
multiplier for exaggerating everything during tuning.

If the stored engine normal is degenerate (`|n|² < 4096`), crumple builds an
inward direction from `(nearby-vertex barycenter − P)` and normalizes it with
`crumpleNormalizeApprox` (max-component reciprocal, no sqrt). This should
never trigger in the real collision paths.

With **no recorded impacts** the mesh is left untouched (it already holds the
persistent deformation).

---

## 4. Mass, health and accumulated damage

- **Mass asymmetry** (`massFactor`): victim deformation scales with
  `otherMass / victimMass`, clamped to `[massFactorMin, massFactorMax]`.
  A bus hitting a compact car dents the compact car far more than the
  reverse. Infinite mass (`carCos->mass == 0x7fff`, cutscene cars) = rigid
  (factor 0); world/static hits use `worldImpactFactor`.
- **Health** (`healthBoost`): a car that is already damaged crumples more
  easily: `softness += (totalDamage/16)·healthInfluence`.
- **Zone damage** (`damageBoost`): per-vertex accumulated `.DEN` zone damage
  (computed by denting.c) amplifies the dent: `4096 + tempDamage·scale`.

---

## 5. Per-model structural cache

`crumpleModelData[model]` caches, per resident model index:

- the AABB of the clean model (computed once from `gCarCleanModelPtr`);
- a per-vertex **resistance** table (0..4096, higher = more rigid):

```
t    = max(|x−cx|/hx, |z−cz|/hz, |y−cy|/hy · ½)   (normalized 0..4096)
res  = cabinResistance − (cabinResistance − noseResistance)·t
```

The cabin centre is rigid; nose, tail and (at half weight) roof are soft.
The cache is keyed by the **model pointer** (`gCarCleanModelPtr[model]`), so
it survives runtime model-slot swaps (spool.c/models.c reassign slots
mid-level) as well as the `crumple_init` reset at level load.

---

## 6. Wheel damage (crooked wheels)

`crumple_recordImpact` also transforms the impact point to car-local space
and, for each of the 4 wheels, tests proximity to `carCos->wheelDisp[i]`
(radius `wheelProximity`). If the hit is hard enough (`howHard ≥
wheelMinHowHard`) **and** the per-car kick cooldown (`wheelBendCooldown`
frames) has elapsed, a per-wheel local-space bend accumulates along the
inward normal, clamped per component (`wheelBendMaxX/Y/Z`). The cooldown
stops sustained contact (scraping a wall) from grinding a wheel to maximum
bend in a fraction of a second.

The bend is consumed in two places, using the **same** local-space
`wheelDisp + bend`:

- **Physics** — `wheelforces.c AddWheelForcesDriver1`: the raycast origin
  passed to `FindSurfaceD2` is offset laterally and longitudinally (moved
  inward / toe), so the suspension reads a displaced contact point →
  asymmetric compression → scrub. To stop cars rolling onto their sides or
  back after a hit:
  - the suspension force and its torque lever arm stay on the **un-bent**
    hub (`leverPos`), so the vehicle's roll/pitch geometry never wanders;
  - the **vertical (vy) bend is excluded from the raycast entirely** —
    feeding it into the suspension jacked/unloaded a corner and rolled the
    car; it survives as a purely visual sag in `DrawCarWheels`;
  - a bent wheel gets a compression **floor** (min 4) so a damaged corner
    can never fully unload;
  - a lateral **scrub force** (∝ bend magnitude · `wheelScrubForce`)
    opposes the wheel's sideways velocity, so a crooked wheel naturally
    creates resistance while driving.
- **Draw** — `cars.c DrawCarWheels`: the wheel model is placed at the same
  bent position (including the visual sag) and a per-wheel **slant matrix**
  is composed into the wheel matrix: camber from the lateral bend (roll
  about the car's forward axis, `wheelCamberScale`), toe from the
  longitudinal bend (yaw about the vertical axis, `wheelToeScale`). The
  drawn tire therefore leans and points crooked exactly like the raycast
  behaves. Flip the scale signs to reverse the lean direction.

Wheel damage **compounds** like body damage: each wheel carries a monotonic
`wheelDamage` level plus the direction of the latest kick; the applied bend
is `wheelBend = wheelDamage · wheelDir`. Repeated hits only ever add damage
— an opposite-side hit re-points the wheel, it never cancels back toward
zero — and `crumple_resetCar` / repair clear it with the car.

Wheel index order is consistent everywhere: `0 = front-right, 1 = rear-right,
2 = front-left, 3 = rear-left` (matches `wheelDisp[]` usage in both files).

---

## 7. Integration points

| File | What changed |
|------|--------------|
| `crumple.c` / `crumple.h` | the library (this doc's subject) |
| `denting.c` | `DentCarDirectional`: vertex loop → `crumple_deform(cp, tempDamage)`; `InitialiseDenting` → `crumple_init()`; `CreateDentableCar` → `crumple_resetCar(cp->id)` |
| `handling.c` | car-car collision: `crumple_recordImpact(cp, c1, point, normal, howHard)` inside the `howHard > 0` block |
| `bcollide.c` | world collision: `crumple_recordImpact(cp, NULL, hit, surfNormal, strikeVel)` after `DamageCar` (fixes the old stale-point world dent) |
| `wheelforces.c` | wheel raycast origin offset by the bend; force/torque lever stays on the un-bent hub; bent-wheel compression floor + lateral scrub force |
| `cars.c` | `DrawCarWheels` places the wheel at the bent position and transforms a per-wheel vert copy (`crumple_transformWheelVerts`) for camber/toe |
| `pause.c` | **Crumple Debug** submenu (Debug Options): `D-Pad Deform`, `Buddha Mode` toggles + `Repair Car` |
| `handling.c` | car-car `crumple_recordImpact`; `crumple_debugTick()` once per frame before the denting pass |
| `loadsave.c` | `crumple_init()` before `LoadGameData` so restored cars start with clean crumple state |
| `overlay.c` | debug overlay prints the player car's pending impact (normal + howHard), moved up 30px |

---

## 8. Tuning guide (`CRUMPLE_PARAMS` in `crumple.c`)

| Field | Default | Effect |
|-------|---------|--------|
| `impactNormalize` | 90000 | `howHard` that counts as a full-scale dent (car-car damage starts ≈ 56k, civ cars die > 204800) |
| `impactMinHowHard` | 2048 | below this, no deformation (safety; denting only runs when damage was applied anyway) |
| `maxDisplacement` | 3000 | hard cap on per-impact vertex push (world units) — dramatic crumple at the impact point |
| `compoundCap` | 3500 | max TOTAL per-vertex push across all recorded impacts (compounding guard) |
| `damageBoostScale` | 0 | zone-damage amplification. **Keep 0**: a per-zone boost steps at zone boundaries (visible seams) and makes the dent shape depend on the last panel hit — the procedural dent should be purely impact-shaped |
| `falloffRadius` | 350 | dent radius (world units) — **concentrated**: a hit crumples locally instead of translating the whole body (was 1800) |
| `contactDeepen` | 1024 | extra depth (0..4096) for oblique pole/edge impacts (see §3 contact profile) |
| `falloffSmooth` | 1 | 0 = linear falloff, 1 = squared (smoother edge) |
| `cabinResistance` | 4096 | resistance at the cabin centre (rigid) |
| `noseResistance` | 128 | resistance at the AABB extremes (soft) |
| `resistanceInfluence` | 2048 | how much resistance limits movement (4096 = rigid parts barely move) |
| `cabinSize` | 2048 | t threshold (0..4096) marking the rigid midsection — the central ~half of the AABB stays rigid; outside it resistance eases to noseResistance |
| `globalScale` | 4096 | master deformation multiplier (4096 = 1.0x) — exaggerate everything for faster testing |
| `massFactorMin/Max` | 384 / 8192 | clamp on the other/victim mass ratio (≈0.09×..2×) |
| `worldImpactFactor` | 4096 | strength multiplier for hits against static geometry |
| `healthInfluence` | 2048 | how much `totalDamage` softens a car |
| `damageModelMix` | 192 | fraction (~4.7%) of the baked damaged-model delta mixed as surface noise |
| `wheelProximity` | 180 | distance (car-local) from a wheel that counts as "near the wheel" |
| `wheelMinHowHard` | 30000 | threshold before a wheel bends (tuned down so the d-pad simulator can bend wheels) |
| `wheelNormalize` | 90000 | `howHard` for a full bend kick |
| `wheelBendStep` | 10 | damage level added per full kick (level compounds, see §6) |
| `wheelBendMaxLevel` | 48 | max cumulative wheel-damage level (bend = level × latest-kick direction) |
| `wheelBendMaxX/Y/Z` | 48 / 24 / 48 | per-component bend clamps (lateral / visual sag / toe) |
| `impactCurve` | 2 | 1 = linear, 2 = squared strength response (hard hits deform much deeper) |
| `wheelScrubForce` | 192 | lateral drag coefficient per unit of lateral+toe bend (0..4096) |
| `wheelBendCooldown` | 30 | frames between wheel-bend kicks (sustained-contact guard) |
| `wheelCamberScale` | 6 | camber angle per unit of lateral bend (4096 = full circle); negate to flip the lean |
| `wheelToeScale` | 6 | toe angle per unit of longitudinal bend (4096 = full circle) |
| `simHowHard` | 1000000 | one-shot simulated-impact strength (kept for reference; the d-pad now grinds with `simTickHowHard`) |
| `simTickHowHard` | 90000 | per-frame howHard while a d-pad direction is HELD — full-strength ticks that the impact ring merges into a deep local crumple |
| `simJitter` | 40 | simulated-hit position jitter (world units) — hits land slightly off-centre |
| `simAngleJitter` | 2048 | simulated-hit normal tilt (4096-fixed, ~7°) — hits land off-perpendicular, more organic |
| `repairRate` | 128 | zone damage removed per frame during repair (4095 max → ~1s) |

---

## 9. Pause-menu debug features

Three features live under **Debug Options → Crumble Debug** (compiled in with
`DEBUG_OPTIONS`, i.e. the `Release_dev` configuration):

- **D-Pad Deform: OFF/ON** — a HELD directional pad grinds a simulated
  collision on the player's car at `simTickHowHard` per frame: up = front,
  down = rear, left = left side, right = right side. The impact ring merges
  the ticks into a deep local crumple; each tick gets a small random
  position/angle deviation (`simJitter` / `simAngleJitter`) so hits land
  slightly off-centre and off-perpendicular. The synthetic impact sits on
  the collision-box face (~1/3 up the body) with an outward normal, so
  `crumple_recordImpact`'s flip makes the dent push inward like a real hit.
- **Buddha Mode: OFF/ON** — the player car can still be damaged and
  crumpled, but `ApplyDamage` clamps `totalDamage` to one point below the
  totaled threshold, so it never catches fire / explodes — de-facto
  invulnerability while testing the crumple.
- **Repair Car** — resumes the game and smoothly repairs the player's car:
  each frame the six damage zones and `totalDamage` ease back toward 0 (at
  `repairRate`) while the persistent damaged mesh eases back toward the
  clean vertices, and the impact/wheel-bend state decays toward zero
  (`needsDenting` stays set so the UV damage panels clear with the body).
  On completion the exact clean mesh is copied in and one final denting
  pass settles the UVs — body, panels and lights end in sync.

---

## 10. Known limitations / future work

- Dents bake into a **persistent damaged mesh** (`gTempCarVertDump`) and
  are applied once per impact, so they never reset on a new collision; the
  mesh only returns to clean via car spawn/reset or **Repair Car**.
- The impact is transformed to car-local space **at record time**, so the
  deferred denting pass (up to a few frames later) uses the geometry the car
  had when it was hit, even if it rotated since.
- The impact ring holds up to `CRUMPLE_MAX_IMPACTS` (8) unapplied/merging
  impacts; once baked they persist in the mesh regardless of the ring.
- Wheel bends are local offsets plus a draw-side camber/toe rotation; the
  tyre contact patch is approximated by the compression floor + scrub force,
  not a full tire model.
- Detachable parts (mirrors, bumpers, tyres) are out of scope — the hubcap
  system in denting.c is the existing base if they're ever wanted.
