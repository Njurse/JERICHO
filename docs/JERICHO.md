# JERICHO — The Complete Guide

**Just-in-Time Extensible Runtime Interface for Compiled Hooks & Overrides**

A developer's guide to the modding runtime built into REDRIVER2: what it is,
why it exists, how it hooks the game, every event it fires, and how to write
modules against it.

> **Who this is for:** a developer who knows C but is new to REDRIVER2 and
> JERICHO. You do not need prior knowledge of the game's codebase — the first
> sections give you the vocabulary, and every later section cites the exact
> files and lines to look at. Everything is verified against the current tree
> (branch `driver1`).

---

## Table of contents

1. [REDRIVER2 in one paragraph](#1-redriver2-in-one-paragraph)
2. [What JERICHO is, and why it exists](#2-what-jericho-is-and-why-it-exists)
3. [The core ideas](#3-the-core-ideas)
4. [System layout](#4-system-layout)
5. [The runtime: boot, dispatch, validation](#5-the-runtime-boot-dispatch-validation)
6. [The events, in depth](#6-the-events-in-depth)
7. [How JERICHO touches game code](#7-how-jericho-touches-game-code)
8. [Overrides: replacing whole behaviors](#8-overrides-replacing-whole-behaviors)
9. [Persistent config](#9-persistent-config)
10. [The module manager and modlist.json](#10-the-module-manager-and-modlistjson)
11. [Logging and diagnostics](#11-logging-and-diagnostics)
12. [Auxiliary libraries](#12-auxiliary-libraries)
13. [Writing a module — step by step](#13-writing-a-module--step-by-step)
14. [Example implementations](#14-example-implementations)
15. [Building with modules](#15-building-with-modules)
16. [Testing JERICHO](#16-testing-jericho)
17. [Conventions, gotchas, and design notes](#17-conventions-gotchas-and-design-notes)
18. [Glossary](#18-glossary)

---

## 1. REDRIVER2 in one paragraph

REDRIVER2 is a from-scratch, open-source reimplementation of the PlayStation
game *Driver 2* (2000). The game logic lives as C source in
`src_rebuild/Game/C/` (files like `cars.c`, `handling.c`, `denting.c`,
`camera.c`, `main.c`), built with a modern toolchain (MSVC / GCC via premake)
against the `PsyCross` library, which reimplements the PS1's drawing API
(`PsyX`). Because the code mirrors the original PS1 architecture, you will
see a lot of old-school idioms that matter when writing JERICHO code:

- **Fixed-point math** everywhere: `FIXED`/`FIXEDH()` macros, 16.16 or 12.4
  style values. Many values are `int` but represent fractions.
- **Angles in 0..4095** (12-bit PSX style), not degrees or radians. Helpers
  like `jer_angle_diff`, `jer_lerp_angle` in `jer_math.h` handle the wrap.
- **Global state** instead of objects: `car_data[]`, `camera_position`,
  `Pads[]`, `GameLevel`, `gDriver1Level`, etc. Modules read/write these
  globals directly through their events.
- The game is compiled as **C++** (so `extern "C"` matters for module entry
  points), but the style is C.

The vanilla game files are the "engine". JERICHO is a thin layer that lets
compiled-in modules observe and modify that engine without editing it.

---

## 2. What JERICHO is, and why it exists

JERICHO is a tiny, platform-neutral C/C++ API that turns REDRIVER2 into a
**host** for compiled-in modules ("packages"). A module is a folder under
`mods/` whose source is compiled **into the game binary** at build time.
There is no runtime loading, no DLLs, no `dlopen`/`LoadLibrary` — the module
source is linked straight into the executable.

Three properties fall out of this design (from the header's own rationale,
`src_rebuild/Game/C/JERICHO/include/jericho.h:13-19`):

1. **Deterministic security** — you audit the module source, build it with
   your own toolchain, and the bytes you run are the bytes you read. No
   binary blob you have to trust.
2. **Platform agnosticism** — no loader code to port to Windows, Linux,
   emscripten, or the PSX.
3. **Minimal intrusion** — the vanilla game files only get single inert
   `jer_fire(...)` lines tagged `// JERICHO-HOOK`. With no modules compiled
   in, those lines are no-ops, so upstream merges stay viable and the
   vanilla build is unchanged.

### Why it was added (the story)

JERICHO grew out of the **crumple** package (Nattdy's impact-driven car
damage). Damage/collision handling is deeply woven into the engine
(`bcollide.c`, `handling.c`, `denting.c`, `wheelforces.c`), and the original
crumple code edited those files directly. That approach has a problem: every
feature edit makes the engine diverge further from vanilla, merge conflicts
grow, and disabling a feature means reverting edits by hand.

JERICHO extracted the pattern into a formal mechanism:

- The engine keeps its vanilla behavior; it just **fires events** at the
  spots where a mod might want to intervene.
- Modules **subscribe** to events; with no subscribers the fires are no-ops
  and the game is stock.
- State that used to live in engine globals (wheel bends, Buddha mode, debug
  toggles) moved into the modules that own it. The engine **queries** the
  modules through dedicated "query events" when it needs those values.

The rest of the ecosystem followed the same pattern: the d2pl camera/weapon
mod (`CAMERA`, `CAMERA_LOOK`, `PED_INPUT`, `PED_SKELETON`, `PED_MOVE`,
`PED_POSE` events), the sandbox demo (overrides + custom events), the
Driver 1 asset library (`jer_d1` + `LEVELFILE`/`FRONTEND` events), and the
levelhacks frontend menu (`FRONTEND`, `FRONTEND_SCREEN`, `DRAW_OVERLAY`).

> **The one-line summary:** JERICHO exists so features can be compiled in
> and out of the game without forking the engine — the engine fires events,
> modules listen, and the vanilla code path is untouched when no module
> listens.

---

## 3. The core ideas

| Term | Meaning |
|---|---|
| **Module** | A package of code compiled into the game. Folder `mods/<id>/` with `mod.toml` metadata and `<id>.c` source. Exports `jer_module_<id>_entry()`. |
| **Event** | A named point in the engine's execution that modules can observe or act on (`JER_EVENT_*`). Events carry a `void* args` pointer to an argument struct. |
| **Hook handler** | A module function `int fn(void* userdata, void* args)` registered for an event. |
| **Priority** | An integer ordering hook handlers for the same event. Lower runs first. |
| **Dispatch** | The runtime walking registered handlers for a fired event, in `(event, priority)` order, until `JER_RESULT_STOP` or the end. |
| **Query event** | An event whose args struct has a *result slot* the engine reads back after firing. No handler = stock default. |
| **Override slot** | A swappable engine function-pointer slot (`JER_OVERRIDE_SLOT_SIM` = the world step). A module replaces a whole behavior and may chain to the previous pointer. |
| **Registry** | The generated table of compiled-in modules (`gen/jer_registry.c`), produced by premake from `--with-mods`. |
| **modlist.json** | Runtime enable/disable + load-order state, edited by the frontend Mods menu. |
| **Context** | The `JERICHO_CONTEXT*` handed to a module's entry; a table of function pointers (`jer_register_hook`, `jer_fire`, `jer_override`, `jer_log`, `jer_register_module`) plus `sdkVersion`. |

### The interaction patterns

Engine call sites come in four flavors; recognizing which one you're looking
at explains the contract:

| Pattern | How it works | Example |
|---|---|---|
| **Notification** (one-way) | Engine fires, module observes, no read-back. | `JER_EVENT_FRAME`, `JER_EVENT_COLLISION` |
| **Query** (result slot) | Engine pre-fills args with defaults, fires, reads the result slot back. Absent handler = stock. | `JER_EVENT_GET_WHEEL_BEND`, `JER_EVENT_GET_BUDDHA` |
| **Rewrite** (in/out) | Args carry engine state; module may modify it in place; engine uses the modified values. | `JER_EVENT_CAMERA`, `JER_EVENT_PED_INPUT`, `JER_EVENT_LEVELFILE` |
| **Claim** (STOP gates stock) | Handler returns `JER_RESULT_STOP` to make the engine skip its own behavior. | `JER_EVENT_PAUSE_MENU` with `JER_PAUSE_OPEN` (sandbox replaces the pause menu) |

Handlers return `JER_RESULT_CONTINUE` (0, keep dispatching) or
`JER_RESULT_STOP` (1, stop dispatching to later handlers). The return value
propagates back through `jer_fire()`, so the engine can test
`jer_fire(...) == JER_RESULT_STOP` to detect a claim.

---

## 4. System layout

```
src_rebuild/Game/C/JERICHO/          the SDK (ships inside the game dev area)
    include/jericho.h                public API: JERICHO_CONTEXT, JER_EVENT_*, host API
    include/jer_math.h               core math helpers (ex-Nattdy's nattdymath)
    include/jer_config.h             persistent config API (mods/config/*.ini)
    include/jer_anim.h               ped-animation API (limbs, bone rotations)
    include/jer_menu.h               tiny list-menu state machine
    include/jer_npc.h                pedestrian/NPC scaffolding library
    include/jer_d1.h                 Driver 1 asset support library
    src/jer_system.c                 runtime: registry mirror, dispatch, activation, validation, overrides, logging
    src/jer_manager.c                mods/modlist.json read/write (tiny JSON)
    src/jer_config.c                 persistent config store (PC/emscripten; no-op on PSX)
    src/jer_math.c                   external float helpers (soft_clamp etc.)
    src/jer_anim.c                   aim-diff helper (bone rotation lives in the game)
    src/jer_menu.c                   JerMenu list-menu state machine
    src/jer_internal.h               shared manager internals (not public SDK)
    gen/jer_registry.c               GENERATED by premake from --with-mods (do not edit)
    test/                            standalone smoke test (jer_test.c) — not in the game build
    docs/                            README.md, HOOKS.md, events.md, ped-animation.md (root JERICHO/docs/)

src_rebuild/Game/C/jer_events.h      game-side event argument structs (JER_ARGS_*)
src_rebuild/Game/C/jer_d1.c          Driver 1 library implementation (sees real engine types)
src_rebuild/Game/C/jer_npc.c         NPC scaffolding implementation
mods/<id>/                           installed packages, one folder each
    mod.toml                         metadata: id, name, version, author, description, dependencies, default-enabled
    <id>.c                           module source (+ .h for shared types, readme.md)
```

The root-level `JERICHO/` folder is a docs mirror (currently only
`JERICHO/docs/ped-animation.md`); the code lives under `src_rebuild/Game/C/`.

---

## 5. The runtime: boot, dispatch, validation

### Data structures (`jer_system.c`)

```c
typedef struct JER_HANDLER
{
    int event;
    JER_HOOK_FN fn;
    void* userdata;
    int priority;
    JER_MODULE* module;   /* owning module (NULL = engine-side handler) */
} JER_HANDLER;

typedef struct JER_MODULE
{
    const char* id;
    JER_MODULE_ENTRY entry;
    const char* name, *version, *author, *description, *deps;
    int enabled, activated, metadataSet, sdkVersion, valid, defaultEnabled;
} JER_MODULE;
```

Fixed capacities: `JER_MAX_MODULES 16`, `JER_MAX_HANDLERS 64`,
`JER_OVERRIDE_SLOTS 8`. The runtime keeps `gHandlers[]`, `gModules[]`,
`gOverrideSlots[]`, a `JERICHO_CONTEXT gCtx` (built once), the logger
pointer, and the mods dir.

### Boot sequence (`jer_init(modsDir)`, called from `main.c`)

1. Build the `JERICHO_CONTEXT` once (function pointers + `sdkVersion`).
2. Print the banner (the build string is `git describe` output stamped at
   premake time, e.g. `8.0-77-g709bcd51-dirty`):
   `== JERICHO v1 (build <git-describe>) == Just-in-Time Extensible Runtime Interface for Compiled Hooks & Overrides`
3. `jer_config_init(modsDir)` — ensure `mods/config/` exists.
4. `jerSnapshotModules()` — copy the generated registry into `gModules[]`.
5. `jerActivateModules(modsDir)`:
   - Read `mods/modlist.json` (missing/empty = everything follows its
     `default-enabled` from `mod.toml`).
   - Apply enable flags; load order = **modlist.json order first**, then
     unlisted built-ins in registry order.
   - For each enabled module in order: set `gCurrentModule`, call
     `m->entry(&gCtx)` (this is where the module registers metadata + hooks),
     mark `activated`.
   - **Validate** activated modules: SDK version mismatch
     (`sdkVersion != JERICHO_SDK_VERSION` → `valid = 0`, module disabled) and
     dependencies (each id in the comma-separated `deps` must exist, be
     enabled, *and* activated). Validation is order-independent: deps are
     checked after all entries ran.
   - Log the module/hook **inventory** (see §11).
6. Fire `JER_EVENT_BOOT` to all registered handlers.

### Dispatch semantics

- Handlers are kept sorted by `(event, priority)`; lower priority first,
  equal priorities in insertion order (FIFO — stable).
- `jerCtxFire` walks only matching events, **skips handlers owned by
  modules that failed validation**, calls each handler, and breaks on
  `JER_RESULT_STOP`. The last handler's return value is returned to the
  caller.
- So: "last result wins" for shared args; `JER_RESULT_STOP` both short-
  circuits remaining handlers *and* tells the engine a module claimed the
  event.

### Runtime toggling (no rebuild)

The frontend Mods menu calls `jer_manager_set_enabled` / `jer_manager_move`
(persisting to `modlist.json`) then `jer_manager_reload(modsDir)`, which:
clears all handlers, **zeroes all override slots** (so a disabled module
can't leave a swap behind), re-snapshots the registry, and re-runs the full
activation + validation + inventory. Modules' entry functions must therefore
be **re-entry safe** (they re-register their hooks every time; module state
should live in `static` and be re-initialized in the entry).

---

## 6. The events, in depth

The event IDs are declared in `jericho.h:53-89`; the argument structs live in
`src_rebuild/Game/C/jer_events.h`. Engine call sites are tagged
`// JERICHO-HOOK` in the game files.

### 6.1 Lifecycle

#### `JER_EVENT_BOOT` (no args)
- **Fired from:** `jer_init` (in the SDK, `jer_system.c:525`), once, after
  all modules activated.
- **Why added:** the "all modules are registered now" signal — modules
  initialize state that depends on other modules being present, and log a
  boot line. `mods/example` logs its boot message here.
- **Stock behavior:** no handler → nothing happens.

#### `JER_EVENT_FRAME` (no args)
- **Fired from:** `GlobalTimeStep()` (`handling.c:310-311`) every game frame,
  and `State_FrontEnd()` (`FEmain.c:1914-1917`) while the frontend is up so
  module menus can tick input/draw behind the frozen frontend.
- **Why added:** the every-frame heartbeat — the sandbox resets the player
  car's damage here, levelhacks reads input for its SP/MP prompt, example
  counts frames.
- **Stock behavior:** no-op.

#### `JER_EVENT_INIT` (no args)
- **Fired from:** `InitialiseDenting()` (`denting.c:66-67`) and
  `LoadCurrentGame()` (`loadsave.c:254-256`).
- **Why added:** the damage system (re)initialization signal. On a savegame
  load, pending impacts/wheel bends from the old session must not bleed into
  restored cars, so the crumple module resets its state here.
- **Stock behavior:** no-op.

#### `JER_EVENT_GAME_START` (no args)
- **Fired from:** `State_GameInit()` (`main.c:853-856`) — a level is
  starting (fresh launch, restart, next mission).
- **Why added:** modules reset transient state here (the sandbox menu must
  close so it doesn't reopen unprompted; driver1 refreshes its availability
  gate and logs the D1 mode).
- **Stock behavior:** no-op.

### 6.2 Damage & collisions (the crumple surface)

#### `JER_EVENT_COLLISION` — `JER_ARGS_COLLISION`
```c
typedef struct JER_ARGS_COLLISION {
    void* car0;      /* CAR_DATA* */
    void* car1;      /* CAR_DATA* (NULL for world hits) */
    void* point;     /* VECTOR* impact point */
    void* normal;    /* VECTOR* surface normal */
    int   howHard;   /* impact strength */
    int   wheelMask; /* wheels eligible to bend (0xF = all) */
} JER_ARGS_COLLISION;
```
- **Fired from:** car-car loop in `GlobalTimeStep()` (`handling.c:522-534`)
  and `CarBuildingCollision()` (`bcollide.c:975-987`, car-world, `car1 = NULL`).
- **Why added:** the crumple module records impacts here. Note the engine
  applies its own `DamageCar3D`/`DamageCar` damage *before* the fire — this
  event is observational, letting the module layer its own physics (pending
  impacts, wheel bend eligibility) on top of stock damage.
- **Stock behavior:** no-op; stock damage already applied.

#### `JER_EVENT_DENT_PASS` — `JER_ARGS_DENT_PASS`
```c
typedef struct JER_ARGS_DENT_PASS {
    void* car;    /* CAR_DATA* */
    void* damage; /* short tempDamage[256] — per-vertex damage accumulation */
} JER_ARGS_DENT_PASS;
```
> ⚠️ **Doc mismatch:** the header comment in `jer_events.h` says "the 6
> zone-damage values", but the engine passes the per-vertex
> `short tempDamage[256]` array (`denting.c:87,133`). The comment is stale;
> the code passes the vertex array.

- **Fired from:** `DentCarDirectional()` (`denting.c:127-135`) — the
  deferred vertex deformation pass, before the UV update.
- **Why added:** the crumple module consumes the car's pending collision
  impact and writes deformed vertices into `gTempCarVertDump[car->id]`,
  which the engine then renders. This is how *body* crumpling happens.
- **Stock behavior:** the vanilla vertex/UV denting below runs unchanged.

#### `JER_EVENT_RESET_CAR` — `JER_ARGS_RESET_CAR`
```c
typedef struct JER_ARGS_RESET_CAR { int carId; } JER_ARGS_RESET_CAR;
```
- **Fired from:** `CreateDentableCar()` (`denting.c:195-203`).
- **Why added:** clear a car's damage state (wheel bends, pending impacts).
  The engine's zone-damage reset below is preserved when
  `gDontResetCarDamage` is set, matching original behavior — the module gets
  the same signal so its state stays in sync.
- **Stock behavior:** no-op.

#### `JER_EVENT_DEBUG_TICK` (no args)
- **Fired from:** `GlobalTimeStep()` (`handling.c:756-757`), per frame.
- **Why added:** the per-frame debug/tuning tick; the crumple D-pad
  deformation simulation + repair animation moved into the crumple module
  and this tick feeds it (comment at `handling.c:759-760`).
- **Stock behavior:** no-op.

#### `JER_EVENT_GET_WHEEL_BEND` — `JER_ARGS_QUERY_PTR` (query)
```c
typedef struct JER_ARGS_QUERY_PTR { int carId; void* result; } JER_ARGS_QUERY_PTR;
```
- **Fired from:** wheel draw (`cars.c:805-813`), physics roll/pitch damping
  (`wheelforces.c:183-197`), and raycast origin offsetting
  (`wheelforces.c:288-298`).
- **Why added:** the engine needs each wheel's permanent bend (an
  `SVECTOR[4]` local-space offset) for drawing, leveling, and suspension —
  but the bend data is owned by the crumple module. `result` is a
  `SVECTOR*`; NULL = no module = stock straight wheels. The physics side
  uses it to damp roll/pitch (`cp->hd.aacc[i] -= cl->avel[i] / 12`) so a
  bent car settles into its slump, and to offset the raycast origin so a
  crooked wheel scrubs and resists steering/thrust.
- **Stock behavior:** `result` stays NULL → straight wheels.

#### `JER_EVENT_GET_WHEEL_DAMAGE` — `JER_ARGS_QUERY_INT` (query)
```c
typedef struct JER_ARGS_QUERY_INT { int carId; int result; } JER_ARGS_QUERY_INT;
```
- **Fired from:** surface-roughness amplification on grass
  (`wheelforces.c:342-350` and `wheelforces.c:756-769`).
- **Why added:** "screwed-up wheels transmit more of the surface roughness
  to the body (up to ~2× at full wheel damage); pristine cars are
  unaffected" (Nattdy comment). `result` is the cumulative wheel damage,
  `0..4096`; the engine computes `roughness = FIXEDH(roughness * (4096 + result))`.
- **Stock behavior:** `result` stays 0 → no amplification.

#### `JER_EVENT_GET_WHEEL_PARAMS` — `JER_ARGS_WHEEL_PARAMS` (query)
```c
typedef struct JER_ARGS_WHEEL_PARAMS {
    int carId;
    int frontScale;  /* steering deviation scale, front wheels */
    int rearScale;   /* rear wheels (much smaller) */
    int scrubForce;  /* lateral scrub force */
} JER_ARGS_WHEEL_PARAMS;
```
- **Fired from:** wheel draw (`cars.c:878-911`) and physics
  (`wheelforces.c:275-286`), which combine it with the bend array:
  `steerAngle += FIXEDH(bend[wheelnum].vx * steerScale)` and
  `scrub = (ABS(bend[i].vx)+ABS(bend[i].vz)) * wheelParams.scrubForce`.
- **Why added:** the deviation scales + scrub force are owned by the crumple
  module (engine comment: "the deviation scales are owned by the crumple
  module now"). All zeros = no wheel-damage influence = stock handling.
- **Stock behavior:** all zeros → stock.

#### `JER_EVENT_GET_BUDDHA` — `JER_ARGS_QUERY_FLAG` (query)
```c
typedef struct JER_ARGS_QUERY_FLAG { int result; } JER_ARGS_QUERY_FLAG;
```
- **Fired from:** `ApplyDamage()` (`bcollide.c:376-393`).
- **Why added:** "the flag is owned by the crumple module now" — when
  `result` is nonzero and the damaged car is the player's
  (`controlType == CONTROL_TYPE_PLAYER`), the engine clamps
  `cp->totalDamage = maxDamage - 1` (Buddha mode: the car never totals).
- **Stock behavior:** `result` stays 0 → damage accumulates normally.

#### `JER_EVENT_GET_IMPACT_INFO` — `JER_ARGS_IMPACT_INFO` (query)
```c
typedef struct JER_ARGS_IMPACT_INFO {
    int  carId;
    void* point;   /* VECTOR* out */
    void* normal;  /* VECTOR* out */
    void* howHard; /* int* out */
} JER_ARGS_IMPACT_INFO;
```
- **Fired from:** `DrawCrumpleDebugInfo()` (`overlay.c:715-734`), gated on
  `MainPlayer.playerCarId >= 0`.
- **Why added:** the debug overlay prints the newest impact (strength +
  normal) — data the crumple module owns.
- **Stock behavior:** zeros printed.

#### `JER_EVENT_DRAW_WHEEL` — `JER_ARGS_DRAW_WHEEL` (notification)
```c
typedef struct JER_ARGS_DRAW_WHEEL {
    int  carId;
    int  wheelnum;
    void* verts;    /* SVECTOR* — per-wheel copy of the model vertex buffer */
    int  numVerts;
} JER_ARGS_DRAW_WHEEL;
```
- **Fired from:** `DrawCarWheels()` (`cars.c:919-928`), per wheel.
- **Why added:** visual mesh distortion — a module may transform the wheel's
  vertex copy (camber/toe from the bend data) before
  `DrawWheelObject(model, wheelVerts, ...)` renders it.
- **Stock behavior:** undistorted copy drawn.

### 6.3 Camera

#### `JER_EVENT_CAMERA` — `JER_ARGS_CAMERA` (rewrite)
```c
typedef struct JER_ARGS_CAMERA {
    void* player;          /* PLAYER* */
    void* cameraPosition;  /* VECTOR* — modules may move it */
    void* cameraAngle;     /* SVECTOR* — modules may re-aim it */
    int   cameraView;      /* the player's camera view */
    int   override;        /* out: 1 = rebuild view matrices from our values */
    void* basePos;         /* LONGVECTOR4* — where the camera chases */
    int   baseDir;         /* chased object's facing, 0..4095 */
    int   carSpeed;        /* chased car's hd.wheel_speed (FIXEDH() it) */
    int   inCar;           /* 1 chasing a car, 0 on foot */
} JER_ARGS_CAMERA;
```
- **Fired from:** end of `InitCamera()` (`camera.c:195-216`), chase-cam path,
  after `camera_position`/`camera_angle` are computed.
- **Why added:** the d2pl module runs its own GTA4-style chase camera. It can
  either tweak the stock position/angle in place, or set `override = 1` —
  the engine then calls `BuildWorldMatrix()` so nothing downstream fights
  the module's transform. The `basePos`/`baseDir`/`carSpeed`/`inCar` fields
  give the module the raw inputs to run full chase-cam math from scratch.
- **Stock behavior:** `override` stays 0 → stock view matrix.

#### `JER_EVENT_CAMERA_LOOK` — `JER_ARGS_CAMERA_LOOK` (rewrite)
```c
typedef struct JER_ARGS_CAMERA_LOOK {
    void* player;     /* PLAYER* (write lp->headTarget / headTimer) */
    int   paddCamera; /* in: current camera pad bits (L2/R2/L3 look) */
    int   stickX;     /* in: right-stick X analog, -128..127 */
    int   stickY;     /* in: right-stick Y analog, -128..127 */
    int   suppress;   /* out: nonzero = skip stock L2/R2 look handling */
    int   gripOrbit;  /* out: 1 = module owns the camera orbit */
} JER_ARGS_CAMERA_LOOK;
```
- **Fired from:** top of `TurnHead()` (`camera.c:400-434`), every frame the
  chase/bumper camera is about to apply look input.
- **Why added:** d2pl's right-stick orbit/look. A module writes
  `lp->headTarget` (smoothed by the engine via `headPos`), sets `suppress`
  to skip stock L2/R2 look-left/right/back handling (e.g. while aiming), and
  `gripOrbit = 1` to take over the orbit — the engine then skips its
  "settle back behind the player" lerp on `lp->cameraAngle`
  (`PlaceCameraFollowCar`, `camera.c:555-563`), so the module's camera angle
  isn't fought. Note `suppress` is OR-ed across handlers
  (`jerLookSuppressed |= jerLook.suppress`), not last-writer-wins.
- **Stock behavior:** `suppress`/`gripOrbit` stay 0 → stock look branches
  run, `lp->headTarget` is zeroed (`camera.c:469-473`).

### 6.4 Pedestrian (on-foot)

#### `JER_EVENT_PED_INPUT` — `JER_ARGS_PED_INPUT` (rewrite)
```c
typedef struct JER_ARGS_PED_INPUT {
    void* player;   /* PLAYER* */
    int   pad;      /* in/out: tannerPad bits (TANNER_PAD_*) */
    int   stickX;   /* in: left-stick X analog, -128..127 */
    int   stickY;   /* in: left-stick Y analog, -128..127 */
    int   cameraYaw;/* in: current camera facing, 0..4095 */
} JER_ARGS_PED_INPUT;
```
- **Fired from:** the ped pad loop in `StepSim()` (`main.c:1188-1212`),
  right before `ProcessTannerPad(pl->pPed, t0, t1, t2)`.
- **Why added:** d2pl's camera-relative on-foot movement — combining
  `stickX`/`stickY` with `cameraYaw`, a module rewrites the pad bits (or
  writes `lp->dir` directly) so the left stick moves Tanner relative to
  where the camera looks. The engine comment explains why the sticks are
  read from `Pads[stream].mapanalog[2]/[3]`: `cjpRecord()` clobbers `t1`.
- **Stock behavior:** pad unchanged → stock pad handling.

#### `JER_EVENT_PED_MOVE` — `JER_ARGS_PED_MOVE` (rewrite)
```c
typedef struct JER_ARGS_PED_MOVE {
    void* ped;   /* LPPEDESTRIAN whose position is about to advance */
    int   padId; /* the pad driving this ped (>= 0 = player) */
} JER_ARGS_PED_MOVE;
```
- **Fired from:** inside `AnimatePed()` (`pedest.c:629-639`) for the player
  ped (`TANNER_MODEL && padId >= 0`), right before the position advances by
  `pPed->speed`.
- **Why added:** this is the **one spot** where a module write to
  `pPed->speed` survives — `ProcessTannerPad` and the runner state
  (`PedUserRunner`) re-arm `speed = MAXRUNSPEED` every frame, and the
  position advance uses the final value right after the fire. d2pl scales
  the run speed here (analog deflection magnitude, lerped for stand-start
  momentum).
- **Stock behavior:** speed unchanged → stock movement.

#### `JER_EVENT_PED_POSE` — `JER_ARGS_PED_POSE` (rewrite)
```c
typedef struct JER_ARGS_PED_POSE {
    void* ped;  /* LPPEDESTRIAN being drawn */
    void* skel; /* BONE* Skel[] (index with JER_LIMB_*) */
} JER_ARGS_PED_POSE;
```
- **Fired from:** `DrawTanner()` (`motion_c.c:1687-1697`), between
  `SetupTannerSkeleton` and `newRotateBones`.
- **Why added:** the ONLY window where a per-bone **rotation** write takes
  effect — each `Skel[i].pvRotation` points into the raw motion-frame bytes,
  and `newRotateBones` reads them immediately after the hook. Modules use
  `jer_anim_bone_rotation()` (defined in `motion_c.c:150`) to resolve bones.
  `JER_EVENT_PED_SKELETON` phase-0 remains the **position** channel.
- **Stock behavior:** rotations as authored in the motion data.

#### `JER_EVENT_PED_SKELETON` — `JER_ARGS_PED_SKELETON` (rewrite, 2 phases)
```c
typedef struct JER_ARGS_PED_SKELETON {
    void* ped;       /* LPPEDESTRIAN being drawn */
    void* skel;      /* BONE* Skel[NUM_BONES] — writeable */
    void* jointPos;  /* SVECTOR* vJPos[NUM_BONES] — absolute joint offsets */
    void* playerPos; /* VECTOR* model origin in world space */
    void* cameraPos; /* VECTOR* camera_position */
    int   phase;     /* 0 = pre-accumulation (pose), 1 = post (draw) */
    int   shadow;    /* 1 while the ped's shadow is drawn */
} JER_ARGS_PED_SKELETON;
```
- **Fired from:** `newShowTanner()` (`motion_c.c:1095-1111` phase 0,
  `1172-1188` phase 1), gated `TANNER_MODEL && padId >= 0`.
- **Why added:** phase 0 lets modules force a pose (write `Skel[i].vCurrPos`
  — e.g. an arm holding a weapon) before joint offsets are accumulated;
  phase 1 lets modules read the accumulated joint offsets (`vJPos[RHAND]` =
  hand position) and draw extra meshes attached to them (e.g. a weapon in
  Tanner's hand — d2pl's laser-sight/weapon mesh). `shadow` lets modules
  skip extra meshes during the shadow pass.
- **Stock behavior:** pose as authored.

### 6.5 Frontend & level loading

#### `JER_EVENT_FRONTEND` — `JER_ARGS_FRONTEND` (rewrite/claim)
```c
typedef struct JER_ARGS_FRONTEND {
    int gameLevel;  /* in/out: the pending level */
    int gameType;   /* in/out: the pending gametype */
    int numPlayers; /* in/out */
    int defer;      /* out: 1 = the module handles the start itself */
} JER_ARGS_FRONTEND;
```
- **Fired from:** `CutSceneCitySelectScreen()` CROSS confirm
  (`FEmain.c:3080-3099`), after `GameLevel = currCity`.
- **Why added:** the take-a-ride city confirm. A module may defer the start
  (its own menu runs over the frozen frontend — levelhacks asks
  Singleplayer/Multiplayer here) or rewrite the pending
  level/gametype/player count. The engine reads the modified values back.
- **Stock behavior:** values unchanged, `defer` 0 → stock start flow.

#### `JER_EVENT_FRONTEND_SCREEN` — `JER_ARGS_FRONTEND_SCREEN`
```c
typedef struct JER_ARGS_FRONTEND_SCREEN {
    int  bSetup;      /* 1 = screen setup, 0 = per-frame */
    void* screen;     /* PSXSCREEN* */
    int  numButtons;  /* in/out: button count (max 8) */
} JER_ARGS_FRONTEND_SCREEN;
```
> ⚠️ **Never fired.** This event is declared and documented (intended for
> button injection into the take-a-ride city screen) but **no call site
> exists in the tree** — the D1 city-page button injection is done by direct
> engine edits instead. It's a dangling contract; don't rely on it yet.

- **Stock behavior:** nothing (no call site).

#### `JER_EVENT_LEVELFILE` — `JER_ARGS_LEVELFILE` (rewrite/veto)
```c
typedef struct JER_ARGS_LEVELFILE {
    int  gameLevel;    /* in */
    int  cityType;     /* in: CITYTYPE_* */
    char* filename;    /* in/out: resolved path buffer */
    int  filenameSize; /* in: capacity of filename */
    int  result;       /* out: 0 = continue, -1 = module vetoed the load */
} JER_ARGS_LEVELFILE;
```
- **Fired from:** `SetCityType()` (`system.c:921-941`), right before the
  level file is opened, after the engine resolved its default path.
- **Why added:** the driver1 module rewrites `filename` to point at the D1
  data folder (`DRIVER\LEVELS\...`), and/or vetoes the load (`result = -1`)
  so `SetCityType` bails out gracefully instead of erroring when the D1
  install is missing (the `glaunch.c` boot path also checks
  `jer_d1_available()` and returns to the frontend).
- **Stock behavior:** `result` 0 → `fopen(filename, "rb")` with the resolved
  path.

### 6.6 Overlay & pause menu

#### `JER_EVENT_DRAW_OVERLAY` (no args)
- **Fired from:** `DrawGame()` (`main.c:1726-1728`), single-player, after
  `DrawPauseMenus()`.
- **Why added:** module overlays (sandbox menu, levelhacks prompt) draw
  here, into the display buffer like the pause menu.
- **Stock behavior:** no-op.

#### `JER_EVENT_PAUSE_MENU` — `JER_ARGS_PAUSE_MENU` (multiplexed shell bridge)
```c
typedef struct JER_ARGS_PAUSE_MENU {
    int  action;
    void* result; /* out: const char* for GET_*_TEXT actions */
    int  value;   /* item id for multiplexed actions */
} JER_ARGS_PAUSE_MENU;
```
- **Fired from:** 12 sites in `pause.c` — label refreshes
  (`CrumpleRefreshLabels`, `D2plRefreshLabels`), toggles (`CrumpleToggleDpad`,
  `CrumpleToggleBuddha`, `CrumpleToggleCol`, `CrumpleToggleOverlay`,
  `CrumpleRepairCar`), `SandboxMenuOpen`, and `CheckForPause` in `main.c`.
- **Actions** (`jer_events.h:104-130`):
  - `JER_PAUSE_CRUMPLE_GET_*_TEXT` (4) — query label strings;
  - `JER_PAUSE_CRUMPLE_TOGGLE_*` (4) and `JER_PAUSE_CRUMPLE_REPAIR` —
    notifications;
  - `JER_PAUSE_SANDBOX_GET_LABEL`, `JER_PAUSE_SANDBOX_OPEN` — the sandbox
    submenu entry;
  - `JER_PAUSE_OPEN` — **claim**: fired when the player presses START
    (single-player) *before* the engine pause opens. A module that replaces
    the pause menu (sandbox) returns `JER_RESULT_STOP`, and the engine pause
    never opens (`main.c:1586-1596`);
  - `JER_PAUSE_D2PL_GET_LABEL` / `JER_PAUSE_D2PL_ADJUST` — multiplexed by the
    item id in `value` (the `D2PL_ITEM_*` enum); `ADJUST` carries the
    direction (-1/0/+1) in `result`.
- **Why added:** the engine keeps the `MENU_ITEM` shells; modules own the
  state and the label text. This keeps module state out of `pause.c` while
  still letting the stock pause menu host module submenus.
- **Stock behavior:** `result` NULL → stock static labels; no claim →
  engine pause opens.

---

## 7. How JERICHO touches game code

### The `// JERICHO-HOOK` pattern

Every engine integration is a small tagged block, e.g. from `bcollide.c`:

```c
// JERICHO-HOOK: the flag is owned by the crumple module now
if (controlType == CONTROL_TYPE_PLAYER)
{
    JER_ARGS_QUERY_FLAG jerArgs;
    jerArgs.result = 0;
    jer_fire(JER_EVENT_GET_BUDDHA, &jerArgs);
    if (!fakeDamage && jerArgs.result && ...)
        cp->totalDamage = maxDamage - 1;
}
```

The rules the engine follows:

- **One thin `jer_fire(...)` line per hook point.** The engine never calls
  into a module by name.
- **Query args are pre-initialized to the stock default** (`NULL` / `0`) so
  an absent handler equals stock behavior.
- **Include lines are tagged too**: `#include "jericho.h" // JERICHO-HOOK: mod runtime (inert without modules)`.
- With zero handlers, `jer_fire` returns `JER_RESULT_CONTINUE` immediately —
  the hook is a no-op and the vanilla path runs.

### What the engine exposes to modules

- **Event args** carry engine pointers as `void*`; modules cast them back to
  the real types (`CAR_DATA*`, `VECTOR*`, `BONE*`, ...). The SDK headers
  stay free of game types.
- **Engine globals** are read/written directly by modules (they compile into
  the same binary): `Pads[]`, `GameLevel`, `GameType`, `NumPlayers`,
  `SetState`, `GAME_TAKEADRIVE`, `gMultiplayerLevels`, `gStopPadReads`,
  `gDriver1Level`, `camera_position`, `camera_angle`, `gCrumpleParams`, etc.
  This is the "zero vanilla edits" contract — modules may touch anything,
  the engine files only carry hook lines.

### Boot wiring (`main.c`)

`redriver2_main()` (`main.c:2058-2068`):

```c
// JERICHO-HOOK: boot the mod runtime...
jer_set_logger(JerichoLogBridge);           /* route logs into REDRIVER2.log */
sprintf(modsDir, "%smods", gDataFolder);
jer_init(modsDir);                          /* activate modules */
```

`JerichoLogBridge()` (`main.c:1942-1961`) classifies messages by keyword:
`"DISABLED"`/`"error"` → `printError`, `"warning"` → `printWarning`, else
`printInfo` — so JERICHO output lands in `REDRIVER2.log` with proper levels.

### Frontend wiring (`FEmain.c`)

- **Mods manager screen** (`JerichoModsScreen`, `FEmain.c:4012-4184`):
  lists modules via `jer_module_list`, toggle via `jer_manager_set_enabled`
  + `jer_manager_reload`, reorder via `jer_manager_move` + reload.
- **D1 city page** (`Driver1CitySelectScreen`, slot 42): named via
  `jer_d1_level_name(i)`, starts with `GameLevel = 4 + city`.
- **JERICHO Config button** injection in `MainScreen` (`FEmain.c:3697-3708`).

---

## 8. Overrides: replacing whole behaviors

Override slots are swappable engine function pointers (`jericho.h:116-123`).
Only one slot is consumed today:

- **`JER_OVERRIDE_SLOT_SIM = 0`** — the world step (`StepSim`). The engine
  call site is `StepGame()` (`main.c:1524-1533`):

```c
// JERICHO-HOOK: the world step is an override slot...
void (*stepSim)(void) = (void (*)(void))jer_get_override(JER_OVERRIDE_SLOT_SIM);
if (stepSim) stepSim(); else StepSim();
```

A module installs its own step and **chains** to the previous one
(sandbox, `sandbox.c:1904-1905`):

```c
gOriginalStepSim = (void (*)(void))jer_get_override(JER_OVERRIDE_SLOT_SIM);
ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SIM, (void*)SandboxStepSim);
```

and `SandboxStepSim` time-scales by running the original on a subset of
frames (or freezing the world entirely at 0×).

**Important semantics:** overrides are **wiped on every reload**
(`jer_manager_reload`), so a module must re-fetch `jer_get_override` and
re-install its own in its entry every time. The other 7 slots are reserved
for future whole-behavior replacement. There is a reference to
`JERICHO/docs/overrides.md` in the headers that does not exist yet — this
section is the current documentation.

---

## 9. Persistent config

Header: `src_rebuild/Game/C/JERICHO/include/jer_config.h`.
Implementation: `src/jer_config.c`.

Each module gets its own text file `mods/config/<modid>.ini`:

```
# Driver 2 Parallel Lines (d2pl)
sens_x = 64
invert_h = 1
```

API (all `extern "C"`):

| Function | Purpose |
|---|---|
| `jer_config_get_int(mod, key, def)` | int value (default when missing/PSX) |
| `jer_config_set_int(mod, key, value)` | persist int (rewrites the file) |
| `jer_config_get_bool` / `set_bool` | stored as `0`/`1` |
| `jer_config_get_str` / `set_str` | string value |
| `jer_config_clear(mod)` | delete the module's file + cache |

Semantics:

- **Lazy load:** a module's file is read at most once per init, on first
  access. **Every `set_*` rewrites the whole file immediately** — no flush
  step; values survive restarts.
- **PSX/emscripten:** on PSX everything is a no-op returning the provided
  default (no filesystem). PC persists to disk.
- **`get_str` caveat** (documented): the missing-key fallback is one shared
  static buffer — a `get_str` of a *different* missing key overwrites it.
  Copy the string if you keep it.
- d2pl persists its 17 settings here; sandbox persists its
  "replace pause menu" toggle; d2pl uses `jer_config_clear` when its
  `cfg_version` bumps to flush stale profiles.

---

## 10. The module manager and modlist.json

`src/jer_manager.c` implements a tiny JSON subset parser/writer for
`modlist.json` (buffer capped at 8192 bytes):

```json
{ "modules": [ { "id": "crumple", "enabled": true }, ... ] }
```

- Root must be `{ "modules": [ ... ] }`; each item `{ "id": "...", "enabled": true|false }`.
- Unknown keys are skipped tolerantly (forward-compatible).
- **Missing file** = every module follows its `default-enabled` from
  `mod.toml` (absent field defaults to `true`).
- **Load order** = modlist.json order first, then unlisted built-ins in
  registry order.
- `jer_manager_set_enabled` toggles an entry (appending if unlisted);
  `jer_manager_move` swaps with its neighbor in the order (appending the id
  first if unlisted); both persist and return 0 on success, -1 on failure.
- **Dependencies** in `mod.toml` (`dependencies = ["a","b"]`) are
  comma-separated ids honored only when the referenced module is itself
  enabled + activated.

---

## 11. Logging and diagnostics

- All JERICHO output goes through the logger set by `jer_set_logger` —
  default `printf`, game wires `JerichoLogBridge` → `REDRIVER2.log` (and the
  debug console in `_DEBUG` builds).
- Modules log via `ctx->jer_log(ctx, "[myid] ...\n", ...)` or the global
  `jer_log(...)`. Prefix lines with `[moduleid]` to keep them greppable.
- **Boot inventory** (logged at boot and on every Mods-menu reload):

```
[jericho] --- module inventory (5 built-in) ---
[jericho]   crumple    v1.0.0  enabled=1 state=active         author="Nattdy + DeepSeek" deps="-"
[jericho]   sandbox    v0.2.0  enabled=1 state=active         author="DeepSeek" deps="-"
[jericho]   example    v0.1.0  enabled=0 state=disabled       author="DeepSeek" deps="-"
[jericho] --- hook inventory (24 handler(s)) ---
[jericho]   event=COLLISION          module=crumple    priority=0
...
```

  `state` is one of: `disabled` (modlist), `not-activated`, `INVALID`
  (failed SDK/dependency validation — the specific reason is logged just
  above the inventory), `active`. The hook inventory lists every registered
  handler with event, owning module, priority — a quick way to see which
  hooks each installed mod touches.

---

## 12. Auxiliary libraries

### `jer_math.h` / `jer_math.c`
Core math helpers, originally Nattdy's `nattdymath.c`, moved into the SDK so
the game and every module share one home. Inline helpers: `soft_clamp`,
`interpolate_quad_ease_out`, `interpolate_quartic_ease_out`,
`CheckRadialCollision` (2D circle test — used for crumple impact proximity),
and fixed-point-friendly int helpers: `jer_clamp_int`, `jer_lerp_int`
(exponential approach, `1/div` per call), `jer_angle_diff` (PSX 0..4095
wrap, ±2048), `jer_lerp` (t in 0..4096), `jer_lerp_angle` (wrap-safe
shortest-path smoothing), `jer_smooth_step`. The game's vanilla files use
them too (`cars.c`, `denting.c`).

### `jer_anim.h` / `jer_anim.c` + `JERICHO/docs/ped-animation.md`
The ped-animation API. `JER_LIMB_*` enum mirrors the engine's limb indices
(root, lowerback, joint_1, neck, head, shoulders/elbows/hands/fingers,
hips). `JER_BONE_ROT` is a plain writable 3-short mirroring `SVECTOR` so a
module can write bone rotations without seeing engine structs.
`jer_anim_bone_rotation(void* skel, int limb)` resolves a limb to its
writable per-frame rotation — **defined in the game** (`motion_c.c:150`)
where the real `BONE` layout lives. `jer_anim_aim_diff(bodyHeading,
aimYaw)` returns the wrap-safe signed diff to write for aim-mode
torso/head alignment. See `JERICHO/docs/ped-animation.md` for the full
pipeline map and clobber table.

### `jer_menu.h` / `jer_menu.c`
A tiny list-menu state machine (`JerMenu`): `jer_menu_begin` (bind label
table, cursor 0, visible 1), `jer_menu_end`, `jer_menu_update`
(edge-triggered cursor move with wraparound, returns 1 on select press),
`jer_menu_label`. Rendering and pad reads stay with the module — the core
stays game-header-free. Used by levelhacks for its SP/MP prompt.

### `jer_npc.h` / `jer_npc.c`
Pedestrian/NPC scaffolding library (declared in the SDK, implemented in
`src_rebuild/Game/C/jer_npc.c`). Declared by the SDK core; currently a thin
surface for future NPC features.

### `jer_d1.h` / `jer_d1.c` — Driver 1 asset support
Why it exists (from the header): Driver 1 (1999) `.LEV` files share Driver
2's LUMPDESC container but differ inside — 32-bit lump counts, 3-int pallet
entries, spooled region ordering, D1 road formats, and terrain height from
the region road map. The fork loads the four D1 cities (Miami, San
Francisco, LA, New York — `JER_D1_CITY_COUNT 4`) as take-a-ride levels
(GameLevel 4-7), with every format delta isolated behind this API.

Key functions: `jer_d1_level_file(city)`, `jer_d1_level_name(city)`,
`jer_d1_folder()` (default `DRIVER\`, overridable via `config.ini [fs]
d1DataFolder`), `jer_d1_available()`, `jer_d1_lump_count`,
`jer_d1_process_pallet`, `jer_d1_process_junctions/roads/roadbounds/
juncbounds/roadsurf`, `jer_d1_find_surface` (D1 road-map height query used
by `dr2roads.c`), `jer_d1_region_begin/finish` + scratch accessors (D1
region spooling in `spool.c`), `jer_d1_convert_straddlers` (`map.c`),
`jer_d1_default_car_cosmetics` (`cosmetic.c` — D1 stored car cosmetics in
the executable, so a D2-format compat profile is synthesized), `jer_d1_log`.

Engine gating: the whole D1 path keys off `gDriver1Level = (GameLevel >= 4)`
set in `glaunch.c:153` before the level load. The driver1 module handles
`FRONTEND` (city page availability), `LEVELFILE` (path rewrite/veto), and
`GAME_START` (refresh availability). See `docs/driver1-cities.md` for the
full feature doc.

---

## 13. Writing a module — step by step

### 13.1 The entry point

Every module exports one function, declared with the SDK macro so it gets
the right linkage (the game is C++; the registry declares entries
`extern "C"`):

```c
#include "jericho.h"
#include "jer_events.h"   /* only if you use game event args */

JER_MODULE_ENTRY(jer_module_myid_entry)(JERICHO_CONTEXT* ctx)
{
    /* 1. metadata (id must match the build id "myid") */
    ctx->jer_register_module(ctx,
        "myid", "My Module", "1.0.0",
        "You", "What it does.", "dep1,dep2", JERICHO_SDK_VERSION);

    /* 2. hooks: (event, fn, userdata, priority) — lower priority first */
    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, MyOnFrame, NULL, 0);
}
```

`JER_MODULE_ENTRY(name)` expands to `extern "C" void name` in C++ and
`void name` in C (`jericho.h:170-174`). The registry (`gen/jer_registry.c`)
calls `jer_module_<id>_entry(ctx)` for enabled modules, in load order, at
boot.

### 13.2 The handler signature

```c
static int MyOnFrame(void* userdata, void* args)
{
    (void)userdata;
    (void)args;
    return JER_RESULT_CONTINUE;   /* or JER_RESULT_STOP to claim/short-circuit */
}
```

### 13.3 mod.toml

```toml
id = "myid"
name = "My Module"
version = "1.0.0"
author = "You"
description = "What it does."
dependencies = []
default-enabled = false
```

`default-enabled` is read by premake into the registry row (absent field =
`true`); `dependencies` are comma-separated module ids validated at boot.

### 13.4 What a module can do

- **React to events** (§6) — observe, query, rewrite, or claim.
- **Own pause-menu items** — via `JER_EVENT_PAUSE_MENU`; the engine keeps
  the `MENU_ITEM` shells, the module owns state + labels.
- **Replace whole behaviors** — `ctx->jer_override(ctx, SLOT, fn)`; chain
  with `jer_get_override` (§8).
- **Persist settings** — `jer_config_*` (§9).
- **Fire custom events** — ids `>= JER_EVENT_MODULE_CUSTOM` (1000) are free
  for module-to-module messaging (sandbox listens on +10 for its no-damage
  toggle).
- **Touch engine globals** directly (same binary, no encapsulation
  boundary) — e.g. `Pads[]`, `GameLevel`, `SetState`.

---

## 14. Example implementations

### 14.1 `mods/example` — the minimal smoke test (80 lines)

The smallest complete module; uses only the public SDK (no game headers), so
it proves the whole pipeline. Full source is at `mods/example/example.c`:

```c
#include "jericho.h"
#include <string.h>

/* one custom event defined by this module */
#define EXAMPLE_EVENT_TICK (JER_EVENT_MODULE_CUSTOM + 1)

typedef struct EXAMPLE_STATE
{
    JERICHO_CONTEXT* ctx;   /* saved so the module can fire custom events */
    int frames;
    int ticks;
} EXAMPLE_STATE;

static int exampleOnBoot(void* userdata, void* args)
{
    EXAMPLE_STATE* st = (EXAMPLE_STATE*)userdata;
    (void)args;
    st->ctx->jer_log(st->ctx, "[example] booted!\n");
    return JER_RESULT_CONTINUE;
}

static int exampleOnFrame(void* userdata, void* args)
{
    EXAMPLE_STATE* st = (EXAMPLE_STATE*)userdata;
    (void)args;
    st->frames++;
    /* fire a custom event once per 60 frames to prove custom ids work */
    if ((st->frames % 60) == 0)
    {
        int result = st->ctx->jer_fire(st->ctx, EXAMPLE_EVENT_TICK, NULL);
        if (result == JER_RESULT_STOP)
            st->ticks++;
        st->ctx->jer_log(st->ctx, "[example] fired custom event %d (frame %d)\n",
                         EXAMPLE_EVENT_TICK, st->frames);
    }
    return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_example_entry)(JERICHO_CONTEXT* ctx)
{
    static EXAMPLE_STATE state;
    memset(&state, 0, sizeof(state));
    state.ctx = ctx;

    ctx->jer_register_module(ctx,
        "example", "Example Module", "0.1.0", "DeepSeek",
        "JERICHO smoke-test: logs at boot and fires a custom event every 60 frames.",
        "", JERICHO_SDK_VERSION);

    ctx->jer_register_hook(ctx, JER_EVENT_BOOT, exampleOnBoot, &state, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, exampleOnFrame, &state, 0);
    ctx->jer_log(ctx, "[example] registered (SDK v%d)\n", ctx->sdkVersion);
}
```

Things to copy from it: the `static` state passed as `userdata` (re-entry
safe — the entry `memset`s it each activation), the custom-event fire with
`JER_RESULT_STOP` result check, and the `[moduleid]` log prefix.

### 14.2 `mods/sandbox` — the full-surface demo (~1900 lines)

Demonstrates **zero vanilla edits**: no-damage (FRAME hook zeroes player
damage), time-scale (**override** of the world step), and the Sandbox Menu
overlay (**claim** of `JER_PAUSE_OPEN`, `DRAW_OVERLAY` drawing,
`gStopPadReads`). Entry (`sandbox.c:1879-1913`):

```c
JER_MODULE_ENTRY(jer_module_sandbox_entry)(JERICHO_CONTEXT* ctx)
{
    gCtx = ctx;
    gNoDamage = 0;   /* must be OFF by default — see comment in source */

    /* persisted setting: does START open the sandbox overlay? */
    gSandboxReplacePause = jer_config_get_bool("sandbox", "replace_pause", 0);

    ctx->jer_register_module(ctx, "sandbox", "Sandbox", "0.2.0", "DeepSeek",
        "Demo package: no-damage, time-scale (world-step override), and the Sandbox Menu overlay.",
        "", JERICHO_SDK_VERSION);

    /* override the world step, chaining to the previous */
    gOriginalStepSim = (void (*)(void))jer_get_override(JER_OVERRIDE_SLOT_SIM);
    ctx->jer_override(ctx, JER_OVERRIDE_SLOT_SIM, (void*)SandboxStepSim);

    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, SandboxOnFrame, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_GAME_START, SandboxOnGameStart, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_DRAW_OVERLAY, SandboxOnDrawOverlay, NULL, 0);
    ctx->jer_register_hook(ctx, JER_EVENT_PAUSE_MENU, SandboxOnPauseMenu, NULL, 0);
    ctx->jer_register_hook(ctx, SANDBOX_CUSTOM_TOGGLE, SandboxOnToggle, NULL, 0);  /* +10 */
}
```

The time-scale helper:

```c
static void SandboxStepSim(void)
{
    if (gTimeScale <= 0) return;                       /* world frozen */
    if (gTimeScale >= 4096 || (gFrameCount % (4096 / gTimeScale)) == 0)
    {
        if (gOriginalStepSim != NULL) gOriginalStepSim();
        else StepSim();
    }
}
```

And the claim pattern for replacing the pause menu
(`SandboxOnPauseMenu`, action `JER_PAUSE_OPEN`):

```c
case JER_PAUSE_OPEN:
    if (/* sandbox overlay is enabled and opened */)
        return JER_RESULT_STOP;   /* engine pause never opens */
    break;
```

### 14.3 `mods/crumple` — the flagship (~1800 lines + 353-line design doc)

The package JERICHO was built for: impact-driven vertex deformation + wheel
damage. **The engine never calls CRUMPLE directly** — it fires events and
crumple's handlers (registered in `jer_module_crumple_entry`) do the work.
Remove `crumple` from `--with-mods` and the game is vanilla denting.

Representative handler (collision → record an impact):

```c
static int crumpleOnCollision(void* userdata, void* args)
{
    JER_ARGS_COLLISION* a = (JER_ARGS_COLLISION*)args;
    (void)userdata;
    if (a != NULL)
        crumpleRecordImpactInternal((CAR_DATA*)a->car0, (CAR_DATA*)a->car1,
            (const VECTOR*)a->point, (const VECTOR*)a->normal,
            a->howHard, a->wheelMask);
    return JER_RESULT_CONTINUE;
}
```

It registers all 12 of its hooks in the entry: `INIT`, `COLLISION`,
`DENT_PASS`, `RESET_CAR`, `DEBUG_TICK`, `GET_WHEEL_BEND`,
`GET_WHEEL_DAMAGE`, `GET_IMPACT_INFO`, `DRAW_WHEEL`, `GET_WHEEL_PARAMS`,
`GET_BUDDHA`, `PAUSE_MENU`. It owns the
`CRUMPLE_PARAMS gCrumpleParams` tuning struct and the pause submenu state,
and exposes the Crumple Debug overlay via `GET_IMPACT_INFO`. This module is
the reference for the full query/notification contract in §6.2.

### 14.4 `mods/d2pl` — the big camera + weapon mod (~2200 lines, 5 files)

- **Camera** (`d2pl_car.c`, `d2pl_ped.c`): GTA4-style dual-stick camera via
  `CAMERA` (override + full chase math from `basePos`/`baseDir`/`carSpeed`),
  `CAMERA_LOOK` (right-stick orbit, `gripOrbit`, `suppress` while aiming),
  `PED_INPUT` (camera-relative movement), `PED_MOVE` (run-speed scaling),
  `PED_SKELETON` phase 1 (weapon mesh attach at `vJPos[RHAND]`),
  `PED_POSE` (arm-hold pose).
- **Weapons** (`d2pl_weapon.c`): `WeaponBase` template (fire/reload/ammo/
  properties/arm pose/mesh attach/aim point), aim zoom, laser sight,
  impact markers.
- **Settings**: Pause → D2PL Settings via `JER_EVENT_PAUSE_MENU`
  (`GET_LABEL`/`ADJUST` multiplexed by `D2PL_ITEM_*` ids), persisted to
  `mods/config/d2pl.ini` via the config API with a `cfg_version` bump +
  `jer_config_clear` for stale profiles.
- **Diagnostics**: `[d2pl] cam: pos=(x,y,z) ang=(vx,vy) scr_z=290 collide=0`
  every 60 frames; the "Camera Mod" toggle is a kill-switch back to the
  stock camera.

### 14.5 `mods/levelhacks` — the frontend hack (~170 lines)

Take-a-Ride level hacks: after the city select, asks Singleplayer or
Multiplayer; Multiplayer starts the chosen city's multiplayer map in
singleplayer take-a-ride. Uses `FRONTEND` (defer the start + rewrite
`gameLevel`/`gameType`/`numPlayers`), `FRAME` (input for its prompt),
`DRAW_OVERLAY` (the prompt), `GAME_START`. Demonstrates the **defer**
pattern and the `jer_menu` library. Note it does **not** call
`jer_register_module` — metadata for the inventory comes from `mod.toml`.

### 14.6 `mods/driver1` — Driver 1 cities (~150 lines + the jer_d1 library)

Adds a "Driver 1 Cities…" page to the take-a-ride city select and loads the
four D1 PS1 cities. Uses `FRONTEND` (page availability),
`LEVELFILE` (rewrite the level path to the `DRIVER\` folder / veto when
data is missing), `GAME_START` (refresh availability + log). The heavy
lifting lives in the `jer_d1` library (§12). This module shows the
**rewrite/veto** pattern on `LEVELFILE`:

```c
static int Driver1OnLevelFile(void* userdata, void* args)
{
    JER_ARGS_LEVELFILE* a = (JER_ARGS_LEVELFILE*)args;
    if (a != NULL && a->gameLevel >= 4 && a->gameLevel <= 7)
    {
        const char* file = jer_d1_level_file(a->gameLevel - 4);
        if (file != NULL)
            snprintf(a->filename, a->filenameSize, "%s%s", jer_d1_folder(), file);
        /* a->result = -1 would veto the load */
    }
    return JER_RESULT_CONTINUE;
}
```

---

## 15. Building with modules

From `src_rebuild/`:

```
premake5.exe --with-mods="example,crumple,sandbox,d2pl,levelhacks,driver1" vs2019
```

- `--with-mods` is a comma-separated list of folder names under `mods/`
  (`premake5.lua:16-29`). No `--with-mods` = runtime only, zero modules.
- premake **generates** `Game/C/JERICHO/gen/jer_registry.c` from the list
  (`premake5.lua:31-92`): one `{ id, jer_module_<id>_entry, defaultEnabled }`
  row per module (defaultEnabled parsed from `mod.toml`'s
  `default-enabled`), plus a `jer_registry_module_count`.
- Each module becomes its own static-lib project `mod_<id>` (`premake5.lua:
  358-393`), all `.c` files compiled **as C++** (`compileas "C++"`), linked
  into `REDRIVER2`. The generated registry TU is also compiled as C++ but
  wraps everything in `extern "C"` so it links against the C-compiled
  `JERICHO.lib`.
- `JERICHO_BUILD_VERSION` is stamped into the build from
  `git describe --tags --always --dirty` on every premake run.

Workflow:

- **Install**: drop the folder into `mods/`, add its id to `--with-mods`,
  re-run premake, rebuild. Enable/disable at runtime via the Mods menu —
  **no rebuild needed for toggles**.
- **Remove**: delete the folder + remove the id, re-run premake, rebuild.
  The game reverts to vanilla (hook lines become no-ops).
- **Version**: bump `JERICHO_SDK_VERSION` only on ABI-breaking changes; the
  runtime refuses mismatched modules with a clear reason.

---

## 16. Testing JERICHO

`src_rebuild/Game/C/JERICHO/test/` is a standalone console smoke test, **not
part of the game build**:

- `jer_test.c` — calls `jer_init("mods")`, prints the module list, fires
  `JER_EVENT_FRAME`, fires a custom event (`MODULE_CUSTOM + 1`, expected
  ignored — no handler).
- `test_registry.c` — a hand-written registry declaring only the `example`
  module (keeps the test free of game globals).
- `build_test.bat` — compiles with MSVC `cl /EHsc /TP` into `jer_test.exe`
  (SDK + manager + test registry + `mods/example/example.c`), including
  SDL2/OpenAL/JPEG/PsyCross include dirs.

Run: `src_rebuild\Game\C\JERICHO\test\build_test.bat`.

There is also `src_rebuild/tests/d1/` — a headless CLI test that compiles the
real `jer_d1.c` with stubs and validates the D1 format decoding
(`d1test.bat` → `=== ALL PASS ===`).

---

## 17. Conventions, gotchas, and design notes

### Dispatch

- **Priority:** lower first; equal priorities FIFO (stable).
- **Last result wins** for the return value; `JER_RESULT_STOP` short-circuits
  and propagates to the engine.
- **Invalid modules' handlers are skipped at dispatch** (not removed from
  the table; the inventory still lists them).
- Handlers are global across modules — there is no per-module namespace for
  events (except custom event ids ≥ 1000).

### Re-entry safety

- `jer_manager_reload` **re-calls every enabled module's entry**. Module
  state must be `static` and re-initialized in the entry (`example.c`
  `memset`s its state; `crumple.c` keeps `gCrumpleParams` etc.).
- **Overrides are wiped on reload** — re-fetch + re-install in the entry
  (sandbox does this).

### Validation

- A module is validated only if it was enabled and its entry ran.
- `valid` gates dispatch; `state=INVALID` in the inventory with the reason
  logged above it.
- SDK mismatch: `sdkVersion != 0 && != JERICHO_SDK_VERSION` → disabled. A
  module that never calls `jer_register_module` keeps `sdkVersion == 0` and
  is treated as compatible (e.g. `levelhacks`).
- Deps are checked post-activation and order-independent; a dep must be
  enabled *and* activated. Dep ids must be < 40 chars (parser token
  buffer).

### Config

- Rewrites the whole file on every `set_*`; lazy-loads once per module per
  init. PSX = no-op.
- `get_str` missing-key fallback is a shared static buffer — copy it.
- `jer_config_clear` deletes the file; next `set_*` regenerates.

### Known doc/code mismatches (as of this writing)

- `JER_EVENT_FRONTEND_SCREEN` is declared + documented but **never fired**
  anywhere in the tree (dangling contract).
- `JER_ARGS_DENT_PASS.damage` comment says "the 6 zone-damage values", but
  the engine passes the per-vertex `short tempDamage[256]` array.
- The headers reference `JERICHO/docs/overrides.md`, which does not exist
  yet (this guide's §8 is the current override documentation).
- `jer_events.h` carries a "To do" note: *"Separate CRUMPLE functions from
  jericho events and try to use vanilla-bound function hooks"* — a future
  direction, not current behavior.
- `pedest.c` includes the JERICHO headers without the `JERICHO-HOOK` tag on
  the include lines (minor inconsistency; it does have a call site).
- Root `README.md` is currently mid-merge-conflict — do not copy from it.

### Style conventions

- Prefix all module log lines with `[moduleid]`.
- Tag any new engine edit with `// JERICHO-HOOK: <what>` so hook sites stay
  findable (`grep -rn "JERICHO-HOOK" src_rebuild/Game/C`).
- Pre-initialize query args to stock defaults before firing.
- Modules are compiled as C++ — use `JER_MODULE_ENTRY` for entry points and
  keep the C linkage for anything the registry or engine calls.

---

## 18. Glossary

| Term | Definition |
|---|---|
| **Context** | `JERICHO_CONTEXT*` — the module's API table (`sdkVersion`, `jer_register_hook`, `jer_fire`, `jer_override`, `jer_log`, `jer_register_module`). |
| **Event** | A named hook point (`JER_EVENT_*`) fired by the engine or a module. |
| **Handler** | A module function registered for an event; returns `JER_RESULT_CONTINUE`/`STOP`. |
| **Module** | A compiled-in package under `mods/<id>/` exporting `jer_module_<id>_entry()`. |
| **Registry** | Generated table of compiled-in modules (`gen/jer_registry.c`). |
| **modlist.json** | Runtime enable/disable + order state in the game's data folder. |
| **Override slot** | A swappable engine function pointer (`JER_OVERRIDE_SLOT_SIM` = world step). |
| **Query event** | Event whose args carry a result slot the engine reads back. |
| **Claim** | A handler returning `JER_RESULT_STOP` to suppress the engine's own behavior. |
| **FIXED/FIXEDH** | PS1 fixed-point macros; many args are fixed-point ints. |
| **0..4095** | PSX angle units (full circle); use `jer_angle_diff`/`jer_lerp_angle`. |
| **gDriver1Level** | Set when a Driver 1 city (GameLevel 4-7) is loading; gates the D1 paths. |
| **CAR_DATA / VECTOR / SVECTOR / BONE / PLAYER / LPPEDESTRIAN** | Engine struct types passed through event args as `void*`. |

---

*Generated from the current tree (branch `driver1`). Companion docs:
`src_rebuild/Game/C/JERICHO/docs/README.md` (SDK overview + building),
`src_rebuild/Game/C/JERICHO/docs/events.md` (event table — note it predates
some events added since, see §6),
`src_rebuild/Game/C/JERICHO/docs/HOOKS.md` (module-writing starter),
`JERICHO/docs/ped-animation.md` (ped pipeline map), `docs/driver1-cities.md`
(D1 feature doc), `mods/<id>/README.md` per module.*
