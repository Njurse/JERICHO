# COLLISIONDEVIL

An arcade handling overhaul for REDRIVER2, built as a JERICHO deep mod.
It re-shapes the stock Driver 2 physics into a "rocket-powered skateboard on
nitrous" — brake-tap powerslides, predictable oversteer, and exaggerated
visual drama — while obeying one rule: **derive, don't invent**.

## How it works

The mod listens to five engine hooks added for handling overhauls:

| Hook | What it does |
|---|---|
| `JER_EVENT_CAR_ENGINE` | scales `thrust` (engine force) and widens `wheel_angle` |
| `JER_EVENT_CAR_FRICTION` | drops the **rear** friction scale while drifting |
| `JER_EVENT_CAR_STEP` | per-car tick: drift detection + visual targets |
| `JER_EVENT_CAR_TORQUE` | injects a yaw kick into `aacc[1]` while drifting |
| `JER_EVENT_CAR_DRAW` | rotates a **render-only** copy of the body matrix |

Every value is derived from an existing chassis stat, so it works on every
car with zero per-car config:

- **Speed** scales `car_cosmetics[].powerRatio` (the engine force behind
  `thrust = powerRatio * 4915`). Because top speed is emergent from thrust
  vs. the game's linear drag, boosting thrust raises both acceleration *and*
  top speed.
- **Drift grip** drops `rearFS` (the rear friction scale, itself derived from
  `car_cosmetics[].traction` × `handlingType[].frictionScaleRatio`).
- **Drift angle** widens `cp->wheel_angle`.
- **Yaw kick** scales `car_cosmetics[].twistRateY` (the car's yaw inertia).
- **Visual drama** rotates only `cp->hd.drawCarMat` — the render matrix — so
  the collision box and physics never change.

## The pause menu (2 pages)

- **Page 1 — COLLISIONDEVIL**: Module Toggle, Arcade Aggression, Drift
  Eagerness, Boost Intensity, Handling Preset (Burnout 3 / Paradise / Custom),
  and the Visual Theater submenu.
- **Page 2 — Visual Theater**: Visual Drama, Camera FOV Pull, Reset to
  Defaults.

Settings persist to `JERICHO/CONFIG/collisiondevil.ini`.

## Tuning

All constants live at the top of `collisiondevil.h` as named `CD_*` macros —
engine boost, grip drop, yaw kick, and the drama magnitudes. The exact feel
is tuned by play; the values ship with sensible arcade defaults.
