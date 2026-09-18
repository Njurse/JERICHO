# Module activation — and when car handling is vanilla

This documents how JERICHO decides which modules run, why "not listed in
`modlist.ini`" used to mean "silently ON", and what has to be true for car
handling to be **vanilla** (stock REDRIVER2) — which is the case whenever no
handling-overriding module is running.

## How a module gets enabled

Resolution order (jer_system.c `jerActivateModules`):

1. **`JERICHO/CONFIG/modlist.ini`** — if the id is listed, its `1/0` wins.
2. **`mod.toml` `default-enabled`** — if the id is *not* listed, the module's
   `mod.toml` decides.
3. **Absent key ⇒ disabled.** Both the runtime loader
   (`jerParseModToml`, jer_loader.c:138) and the compiled-in registry generator
   (`premake5.lua:91`) default a module to **OFF** when its `mod.toml` says
   nothing. A module must *opt in*; it can never inherit "on".

Step 3 is fail-closed on purpose. It used to be fail-open (absent ⇒ enabled),
which is how `collisiondevil`, `combatd2` and `d2pl` ran unannounced for months
without appearing in `modlist.ini` at all. The value is parsed as an
**allow-list** in both parsers — only `true` / `1` / `enabled` turns a module on;
an empty value, a typo or an unwrapped quoted string leaves it **off**.

> **Compiled-in modules read the registry, not the file.** For a module that is
> compiled into the game (no `runtime = "dll"` in its `mod.toml`), the effective
> default comes from the generated `JERICHO/gen/jer_registry.c`, which premake
> produces from `mod.toml` at build time (`jer_system.c` fills the table from
> the registry; `jerParseModToml` only runs for runtime DLL addons). So editing a
> compiled-in module's `default-enabled` has no runtime effect until premake is
> re-run (`premake5.exe vs2019`) and the game is rebuilt.

Every boot writes the resolution to `REDRIVER2.log`:

```
[jericho] --- module inventory (11 loaded) ---
[jericho]   combatd2   v0.6.0  enabled=1 src=default state=active   ...
[jericho]   crumple    v1.0.0  enabled=1 src=modlist state=active   ...
```

`src=` is the source of the enable decision:

| `src=` | meaning |
|---|---|
| `modlist` | `modlist.ini` listed it and decided |
| `default` | **not listed** — its `mod.toml` default was used (treat as a bug: list it) |
| `forced` | a diagnostic flag forced this one module (see below) |
| `nomods` | the whole runtime was force-disabled with `-nomods` |

"If `src=default` and `enabled=1`, a module is running that nobody asked for" is
the single check that catches this class of surprise. `JERICHO/CONFIG/modlist.ini`
therefore lists **every** installed module explicitly.

## The `-nomods` switch

`-nomods` (main.c:2270, scanned before `jer_init`) calls
`jer_disable_all_modules()` (jer_system.c:774): every module is forced off
regardless of the modlist and of any `mod.toml`, and the boot log says so
(`-nomods: N module(s) force-disabled`). This is the only way to be *sure* the
sim has zero modules in it — useful both for a clean baseline and for
attributing a behaviour to a module.

The flag is set for the whole boot and is never cleared, so it also applies to
`jer_manager_reload` and to frontend toggles: with `-nomods` active, toggling a
module in Options → JERICHO rewrites `modlist.ini` but activates nothing until
the game is restarted without the flag.

## Diagnostic flags that force a module on

A module does not have to be enabled before it can be used for a test: the engine
can force one on for the boot with `jer_force_module(id, on)` (jer_system.c),
called before `jer_init`. It outranks both `modlist.ini` and the module's
`default-enabled`, adds the module to the activation order, and shows up in the
inventory as `src=forced`. `-nomods` still outranks it.

`-testmode` / `-testcar <slot|n>` / `-testped` (main.c, next to the `-nomods`
scan) use it for the `testmode` module, so a session can be set up with flags and
no ini editing. Modules can read their own arguments through `JER_EVENT_CMDLINE`
(`events.md`), but the engine must *recognise* a flag in its own parser or it
raises a player-visible "invalid command line argument" toast - which is why the
mp mod's `-host`/`-join` and these have explicit branches in `main.c` even though
the module reads the values itself.

## Which modules override car handling

Only these two replace the stock car physics; with both off the engine's
handling path is the untouched stock one.

| Module | What it overrides | Where |
|---|---|---|
| `collisiondevil` | angular damping, drift yaw kick, rear-grip drop, crash spin | `CAR_TORQUE`, `CAR_FRICTION`, `CAR_STEP`, `CAR_ENGINE`, `COLLISION` — collisiondevil.c |
| `combatd2` | gravity / angular damping / spring rate + a point-mass integrator | `GET_PHYSICS_PARAMS`, `CAR_TORQUE` — combatd2sim.c:708, :284 |

`crumple` changes behaviour too, but only for **damaged** cars: its physics lives
in bend-gated branches of the engine (see below), so an undamaged car is stock.

**Guaranteeing vanilla handling:** `-nomods`, or a `modlist.ini` with
`collisiondevil = 0` and `combatd2 = 0`. The shipped `modlist.ini` pins both to
`0`, so a default install runs no handling override. Verify with the boot log —
`grep 'state=active' REDRIVER2.log` must not name either module.

## Why the engine is stock when no module is loaded

The engine carries leftovers from the CRUMPLE era. Each one is classified as
**harmless no-op** (no effect on the no-module path), **parity risk** (a
behaviour delta vs stock that does not change car physics), or **move-into-crumple
candidate** (physics that lives in the engine but only belongs there because a
module supplies the input):

| Artefact | Where | Class | Why |
|---|---|---|---|
| Bend-gated suspension/roll physics | wheelforces.c:218 (extra roll/pitch damping), :429 (wheel droop), :527 (steering deviation), :612 (scrub) | **move-into-crumple candidate** | Every branch needs a nonzero `bend`, which only `crumple` supplies (`GET_WHEEL_BEND` returns NULL with no module). The maths sits in the engine solver rather than the module. |
| Physics-param refactor | wheelforces.c:216 / :490 (`cl->angularDamping`, `cl->springRate`, `cl->springDamping`) | **harmless no-op** | Defaults are `128 / 230 / 100` — exactly upstream's hard-coded constants; `cl->gravity` = `GRAVITY_FORCE` (-7456). |
| `leverPos` lever arm | wheelforces.c (`AddWheelForcesDriver1`) | **harmless no-op** | Equals `wheelPos` whenever `bend == NULL` (`FindSurfaceD2` does not mutate its input), so the torque it feeds is stock. |
| Front/rear force heading (`wdir`) | wheelforces.c | **harmless no-op** | Reproduces stock's `-cdx/cdz` (rear) and `-sdx/sdz` (front) exactly. |
| `twistRateZ` doubling in `FixCarCos` | handling.c:190 (note) | **harmless no-op** (already removed) | The pre-JERICHO `carCos->twistRateZ <<= 1` is gone; the note exists so it is not reintroduced (doubling it halves roll inertia and tips every car). |
| `gCrumpleLastCollisionPoint` + `DentCarDirectional` | handling.c:37, :498, :778 | **parity risk** (cosmetic) | The dent *direction* comes from a global written elsewhere, so dents can point differently from stock; it does not touch physics. |
| `gBouncePhase` | handling.c:316 | **harmless no-op** | Written each frame and never read by physics (its only reader is the disabled bounce model in cars.c). |
| Handbrake → `g_PlayerControlMode` cycle | handling.c:1244 | **parity risk** | Unconditional gameplay mutation in `ProcessCarPad`; inert unless a module reads it (`sandbox`/`aidriver`), which then changes the player car's control mode. |
| `CheckCarEffects` wreck guards | handling.c:1594, :1601, :1632, :1685 | **harmless no-op** (effects only) | Only gate skid marks / tyre noise / smoke for a totaled car; no physics. Also note it uses `MaxPlayerDamage[0]` for every car rather than the per-`player_id` value. |
| `civ_ai.c` AI-driving rewrite | civ_ai.c:3645 (and the follow-distance braking above it) | **parity risk** | Changes AI *input* (throttle/braking/steering penalty), not the handling model; the car still uses stock physics. |

So: **no handling-overriding module ⇒ vanilla handling.**

> Do not reintroduce a `twistRateZ` scale in `FixCarCos`. `twistRateZ` is the
> roll rate consumed by `ConvertTorqueToAngularAcceleration`; doubling it halves
> a car's effective roll inertia and makes every car tip onto its side after a
> hit. Roll damping for damaged wheels belongs in the bend-gated branch, not a
> global scale (handling.c:190).

## Known issue left in place (out of scope)

`collisiondevil` has two physics bugs that make cars cartwheel. They are
**recorded, not fixed** here:

- collisiondevil.c:292-294 adds `st.n.angularVelocity[i] >> CD_DAMP_SHIFT` with
  `CD_DAMP_SHIFT = 2` (collisiondevil.h:68) = `+avel/4`, while the stock damping
  in `ConvertTorqueToAngularAcceleration` is only `-avel/32`. Net `+7/32*avel`
  is **positive feedback** (anti-damping): any car that starts rotating spins up
  until the angular velocity saturates and it tumbles. The constant contradicts
  its own comment, which says `6`.
- collisiondevil.c:370 `spin = a->howHard >> CD_CRASH_SPIN_SHIFT` with
  `CD_CRASH_SPIN_SHIFT = 32` (collisiondevil.h:64) is **undefined behaviour**
  (MSVC `warning C4293`, evaluated as `>> 0`), so the crash spin kick is a raw
  `~419000` per hit.

Measured on the two-per-city arena (`-level chicago -mp 0 -frames 500 -seed 7`,
identical car population): `collisiondevil` on = 78 roll-overs, off = 0. Because
the shipped `modlist.ini` pins it off, this does not affect a default install.
