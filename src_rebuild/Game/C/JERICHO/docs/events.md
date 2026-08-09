# JERICHO events

Argument structs live in `src_rebuild/Game/C/jer_events.h` (game-side; the
SDK stays generic). Engine call sites are tagged `// JERICHO-HOOK` and are
inert no-ops when no module handles them.

| Event | Args | Fired from | Meaning |
|---|---|---|---|
| `JER_EVENT_BOOT` | — | `jer_init` | once, after all modules activated |
| `JER_EVENT_FRAME` | — | `GlobalTimeStep` | every game frame |
| `JER_EVENT_INIT` | — | `InitialiseDenting`, game-load | damage system (re)init |
| `JER_EVENT_COLLISION` | `JER_ARGS_COLLISION` | car-car (`handling.c`) + car-world (`bcollide.c`) | record an impact |
| `JER_EVENT_DENT_PASS` | `JER_ARGS_DENT_PASS` | deferred denting pass (`denting.c`) | deform car verts |
| `JER_EVENT_RESET_CAR` | `JER_ARGS_RESET_CAR` | `denting.c` | clear a car's damage state |
| `JER_EVENT_DEBUG_TICK` | — | `GlobalTimeStep` | per-frame debug/tuning |
| `JER_EVENT_GET_WHEEL_BEND` | `JER_ARGS_QUERY_PTR` | wheel draw + physics | query: per-wheel bend array |
| `JER_EVENT_GET_WHEEL_DAMAGE` | `JER_ARGS_QUERY_INT` | surface roughness | query: cumulative wheel damage 0..4096 |
| `JER_EVENT_GET_WHEEL_PARAMS` | `JER_ARGS_WHEEL_PARAMS` | wheel yaw matrix + physics | query: deviation scales + scrub force |
| `JER_EVENT_GET_BUDDHA` | `JER_ARGS_QUERY_FLAG` | `bcollide.c` | query: damage-total clamp flag |
| `JER_EVENT_GET_IMPACT_INFO` | `JER_ARGS_IMPACT_INFO` | debug overlay | query: newest impact |
| `JER_EVENT_DRAW_WHEEL` | `JER_ARGS_DRAW_WHEEL` | `DrawCarWheels` | distort a wheel's vertex copy |
| `JER_EVENT_CAMERA` | `JER_ARGS_CAMERA` | `InitCamera` (chase cam) | adjust/override the camera transform |
| `JER_EVENT_CAMERA_LOOK` | `JER_ARGS_CAMERA_LOOK` | `TurnHead` | right-stick look input, before the stock look handling |
| `JER_EVENT_PED_INPUT` | `JER_ARGS_PED_INPUT` | ped control loop | rewrite the on-foot pad before it drives Tanner |
| `JER_EVENT_PED_SKELETON` | `JER_ARGS_PED_SKELETON` | `newShowTanner` | pose Tanner's skeleton (phase 0) + draw extras (phase 1) |
| `JER_EVENT_DRAW_OVERLAY` | — | `DrawDebugOverlays` / HUD path | 2D overlay / HUD drawing |
| `JER_EVENT_PAUSE_MENU` | `JER_ARGS_PAUSE_MENU` | pause menu shell | module-owned menu state (labels + actions) |
| `>= JER_EVENT_MODULE_CUSTOM` | module-defined | modules | custom events |

## Query events

Query events use the args struct as a result slot. The engine initializes
the fields to their "no module" defaults (NULL / 0), fires the event, and
reads the (possibly updated) fields back. No handler = stock behavior.

## The camera events (d2pl uses these)

- **`JER_EVENT_CAMERA`** fires at the end of the chase-cam update once the
  engine has computed `camera_position` / `camera_angle`. A module may move
  the position, re-aim the angle, and set `args->override = 1` — the engine
  then rebuilds its view matrices from the module's values so nothing else
  fights the transform. The args also carry `basePos` (the chased point),
  `baseDir` (the chased facing), `carSpeed` and `inCar` so a module can run
  its own full chase-cam math instead of poking at the stock result.
- **`JER_EVENT_CAMERA_LOOK`** fires at the top of `TurnHead` with the
  player, the camera pad bits, and the raw right-stick analog
  (`stickX`/`stickY`). A module drives the look (e.g. GTA-style orbit by
  writing `lp->cameraAngle` + `gripOrbit = 1`, which makes the engine skip
  its settle-back lerp) and may set `suppress` to skip the stock L2/R2
  look-left/right/back handling.
- **`JER_EVENT_PED_INPUT`** fires right before `ProcessTannerPad` with the
  pad that is about to drive the on-foot player. The module can rewrite the
  pad bits (or write `lp->dir` directly, as d2pl does for camera-relative
  movement). No handler = stock pad handling.
- **`JER_EVENT_PED_SKELETON`** fires twice per player-ped draw from
  `newShowTanner`: phase 0 before the bone accumulation (override
  `Skel[i].vCurrPos` to pose the arm), phase 1 after (read `vJPos` for
  world-space joint positions and draw extra meshes). `shadow` is set when
  the pass is for the shadow, so modules can skip there.

## The pause menu bridge

`JER_EVENT_PAUSE_MENU` is the pause shell ↔ module contract. The engine's
`pause.c` owns the menu items; modules own the state. Actions are
multiplexed: the crumple module has one action per toggle; the d2pl module
uses two multiplexed actions — `JER_PAUSE_D2PL_GET_LABEL` (with the item id
in `args->value`, the label returned via `args->result`) and
`JER_PAUSE_D2PL_ADJUST` (item id in `value`, direction -1/0/+1 in `result`).

`JER_PAUSE_OPEN` is special: it fires when the player presses START
(single-player) *before* the engine pause opens. A module that replaces the
pause menu (the sandbox overlay) claims the press with `JER_RESULT_STOP` and
the engine pause never opens.

## Diagnostics

At boot (and on every Mods-menu reload) the runtime logs into
**`REDRIVER2.log`** (via the PsyX logger, wired in `main.c`):
- one line per compiled-in module (id, version, enabled, state, author,
  deps) — `state=INVALID` means SDK/dependency validation failed and the
  reason is logged right above it;
- one line per registered hook handler (event, owning module, priority).

So a quick scan of `REDRIVER2.log` right after boot tells you which of the
hooks in this table are actually live, and which modules are running them.

## Override slots

`MOD_CONTEXT::jer_override` swaps engine function-pointer slots. Currently
one slot is consumed by the engine: `JER_OVERRIDE_SLOT_SIM` (the world step,
`StepSim`) — the sandbox module uses it for time-scale. The remaining slots
are reserved for future whole-behavior replacement.

## Persistent config

Modules get a persistent key/value store via the JERICHO config API
(`jer_config.h`): each module owns a text file in
**`mods/config/<modid>.ini`** (the folder is created at boot). Files are
read lazily on first access and rewritten on every `set`, so values survive
restarts with no flush step. On PSX the API is a no-op returning the
provided defaults. d2pl persists its camera/weapon settings there; the
sandbox persists its "replace pause menu" toggle.
