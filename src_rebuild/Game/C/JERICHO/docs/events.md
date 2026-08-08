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
| `JER_EVENT_PAUSE_MENU` | `JER_ARGS_PAUSE_MENU` | pause Crumple Debug menu | module-owned menu state |
| `>= JER_EVENT_MODULE_CUSTOM` | module-defined | modules | custom events |

## Query events

Query events use the args struct as a result slot. The engine initializes
the fields to their "no module" defaults (NULL / 0), fires the event, and
reads the (possibly updated) fields back. No handler = stock behavior.

## Override slots

`MOD_CONTEXT::jer_override` swaps engine function-pointer slots. Currently
one slot is consumed by the engine: `JER_OVERRIDE_SLOT_SIM` (the world step,
`StepSim`) — the sandbox module uses it for time-scale. The remaining slots
are reserved for future whole-behavior replacement.
