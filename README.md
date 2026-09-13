# JERICHO

**Just-in-Time Extensible Runtime Interface for Compiled Hooks & Overrides**

JERICHO is a small, platform-neutral C/C++ mod framework for the
reverse-engineered **REDRIVER2** (a source-level reimplementation of Driver 2).
It turns the game into a *host* for mods: the engine fires named events at the
exact points a mod needs to touch, and modules subscribe to those events through
one stable API. Every engine call site is an **inert no-op when nothing handles
it**, so a stock build with no modules behaves exactly like vanilla REDRIVER2.

This repository is a **REDRIVER2 fork built around JERICHO**. The framework, the
engine-side event bridges, the addon SDK and a set of showcase mods all live
under [`JERICHO/`](JERICHO/) — the mods in [`JERICHO/MODS/`](JERICHO/MODS/) and
the engine-side SDK in [`src_rebuild/Game/C/JERICHO/`](src_rebuild/Game/C/JERICHO/).
The mods exist to *demonstrate what the hook surface makes possible*: real car
deformation, a GTA-style camera, full arcade-handling overhauls, LAN
multiplayer, and more — each one built purely out of hooks plus (for the deep
mods) reads and writes of game globals.

## Two kinds of mods

JERICHO handles mods two ways, and the split is deliberate.

### 1. API addons — runtime DLLs (the primary model)

An **addon** is written against the JERICHO API only and marked `runtime = "dll"`
in its `mod.toml`. It compiles into a DLL **without rebuilding the game**: drop
it into `JERICHO/MODS/<id>/`, press **Compile Mods** in the game (Options →
JERICHO) or run `JERICHO/build_mods.bat`, and the runtime loads it at boot or on
reload. The game executable is never touched — the whole point of JERICHO is
that developers use the API instead of modifying the game.

- The loader scans `JERICHO/MODS`, reads each `mod.toml`, `LoadLibrary`s the
  compiled `<id>.dll` and resolves its entry point (`jer_module_<id>_entry`).
- Enable/disable/reorder at runtime in the Mods menu; the state persists to
  `JERICHO/CONFIG/modlist.ini` (line order = load order, value = enabled).
- Addons build against the standalone SDK in [`JERICHO/sdk/`](JERICHO/sdk/) —
  API headers + the game's import library + `build_mods.bat`. No game source,
  no PsyCross, no SDL/OpenAL/JPEG needed.

### 2. Deep mods — compiled into the game

A **deep mod** patches game internals at the source level: it includes game
headers and reads/writes game globals (`player[]`, `car_data[]`, ...). Because
MSVC cannot import game *data* globals into a DLL, these mods cannot be external
plugins — they are **compiled in** by premake's auto-scan (any folder with a
`mod.toml` that does *not* declare `runtime = "dll"`). The exe exports every
game symbol via [`exports.def`](src_rebuild/exports.def), but only functions are
usable from DLL addons.

Both kinds are managed identically at runtime through the same Mods menu, so the
distinction is about *what a mod is allowed to touch*, not how it is enabled.

![The in-game mod manager (Options → JERICHO)](readme_images/jericho_mod_menu_example.png)

*The Mods menu (**Options → JERICHO**): enable, disable and reorder every
installed module, and drop into each module's own page. In-game toggles are
written straight back to `JERICHO/CONFIG/modlist.ini`, and per-module settings
live in `JERICHO/CONFIG/<modid>.ini`.*

## Repository layout

```
JERICHO/                         the framework folder (also the runtime data dir)
├── MODS/<id>/                   installed modules, one folder each
│   ├── mod.toml                 metadata: id, name, version, author, runtime, ...
│   ├── <id>.c                   the module source (addon or deep mod)
│   └── <id>.dll                 compiled addon binary (built, not committed)
├── CONFIG/
│   ├── modlist.ini              module enabled state + load order
│   └── <id>.ini                 per-module settings (jer_config store)
├── build_mods.bat               compiles every runtime="dll" addon (in-game button)
└── sdk/                         standalone addon development kit (headers + import lib)

src_rebuild/Game/C/JERICHO/      the engine-side SDK + canonical docs
├── include/jericho.h            public API: hooks, events, overrides, module entry
├── include/jer_*.h              math / config / menu / pause-menu / net helpers
├── src/jer_loader.c             runtime DLL scanner + loader
├── src/jer_system.c             module table, activation, dispatch, logging
├── src/jer_manager.c            CONFIG/modlist.ini read/write
└── docs/                        the canonical JERICHO documentation
```

Documentation lives beside the code it describes. [`docs/README.md`](docs/README.md)
is the index for the whole set; the canonical JERICHO docs are in
[`src_rebuild/Game/C/JERICHO/docs/`](src_rebuild/Game/C/JERICHO/docs/) —
[`README.md`](src_rebuild/Game/C/JERICHO/docs/README.md) (overview, layout, build),
[`events.md`](src_rebuild/Game/C/JERICHO/docs/events.md) (the full event reference)
and [`HOOKS.md`](src_rebuild/Game/C/JERICHO/docs/HOOKS.md) (writing a module).

## The hook schema

A JERICHO module is a C/C++ source file. The engine discovers it at build time
(deep mods) or at load time (addons) and calls exactly one entry point, which
registers the module and its hooks.

### Registering a module

The entry point is declared with the SDK macro `JER_MODULE_ENTRY`. The game is
C++, so the entry and every symbol it exports must have **C linkage** — the macro
supplies that. The entry name is `jer_module_<id>_entry`, where `<id>` matches
both the folder name and the `mod.toml` id (all three must agree).

```c
#include "jericho.h"
#include "jer_events.h"

JER_MODULE_ENTRY(jer_module_myid_entry)(JERICHO_CONTEXT* ctx)
{
    /* register the module itself: id, name, version, author, description,
     * dependencies ("other,module,ids" or ""), SDK version */
    ctx->jer_register_module(ctx, "myid", "My Module", "1.0.0",
        "You", "What it does.", "", JERICHO_SDK_VERSION);

    /* register an event handler: (event, fn, userdata, priority) */
    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, MyOnFrame, NULL, 0);
}
```

### The hook contract

A handler has the signature `int fn(void* userdata, void* args)`:

- `userdata` is whatever was passed to `jer_register_hook` — use it to give one
  function several roles without global state.
- `args` points at the event's argument struct (`JER_ARGS_*` from
  [`jer_events.h`](src_rebuild/Game/C/jer_events.h) / the SDK copy in
  [`JERICHO/sdk/include/`](JERICHO/sdk/include/)), or is `NULL` for events with
  no payload. Cast it to the type the event documents.
- `priority` orders handlers registered for the same event: **lower runs first**,
  and for shared args the **last return value wins**. Layering a hand
  transformation is just running at a lower/higher priority than another module.
- The return value is one of two `JER_RESULT_*` codes:
  - `JER_RESULT_CONTINUE` — let the remaining handlers (and the engine) run.
  - `JER_RESULT_STOP` — claim/consume the event. For the events that document it,
    the engine then skips its own handling of that input/draw for the frame.

`JERICHO_SDK_VERSION` is the version the module was compiled against; the runtime
checks it and **logs a mismatch**, so a stale addon is obvious rather than silent.
The SDK's [`example/`](JERICHO/MODS/example/) module is a complete, minimal addon
to copy from.

### Notification vs query events

There are two shapes of event, and they change how a handler reads `args`:

- **Notifications** tell a module that something happened and let it mutate the
  args in place (last writer wins) — e.g. `JER_EVENT_FRAME` (once per frame),
  `JER_EVENT_COLLISION` (an impact occurred), or `JER_EVENT_GAME_START` (a level
  is starting, so reset transient state).
- **Queries** let the engine *ask* a module for a value. The engine seeds the
  args struct with the "no module" default (0 / `NULL` / the stock value), fires
  the event, and reads the fields back. A handler that writes a field changes
  engine behaviour; a handler that leaves it alone leaves stock behaviour.

The upshot is the framework's core safety property: because a query's args start
at stock and the engine ignores events nobody handles, **a build with no modules
is byte-for-byte stock behaviour** — the hook call sites are inert.

### The event surface

Events are grouped by the subsystem they bridge. This is the map; the exhaustive
catalogue — every event's argument fields and engine call site — is in
[`events.md`](src_rebuild/Game/C/JERICHO/docs/events.md).

| Area | Events |
|---|---|
| Lifecycle & frame | `JER_EVENT_BOOT`, `JER_EVENT_FRAME`, `JER_EVENT_DEBUG_TICK`, `JER_EVENT_GAME_START`, `JER_EVENT_LEVEL_LAUNCH`, `JER_EVENT_SHUTDOWN` |
| Input | `JER_EVENT_PRE_SIM`, `JER_EVENT_CAR_PAD`, `JER_EVENT_PED_INPUT`, `JER_EVENT_CAMERA_LOOK`, `JER_EVENT_MAP` |
| Car handling | `JER_EVENT_CAR_ENGINE`, `JER_EVENT_CAR_FRICTION`, `JER_EVENT_CAR_STEP`, `JER_EVENT_CAR_TORQUE`, `JER_EVENT_CAR_DRAW`, `JER_EVENT_CAR_DRAW_COLOR`, `JER_EVENT_GET_PHYSICS_PARAMS` |
| Damage & collision | `JER_EVENT_COLLISION`, `JER_EVENT_DENT_PASS`, `JER_EVENT_RESET_CAR`, `JER_EVENT_CAR_VS_CAR`, `JER_EVENT_GET_DAMAGE_SCALE`, `JER_EVENT_GET_WALL_RESTITUTION`, `JER_EVENT_GET_BUDDHA` |
| Wheels | `JER_EVENT_GET_WHEEL_BEND`, `JER_EVENT_GET_WHEEL_DAMAGE`, `JER_EVENT_GET_WHEEL_PARAMS`, `JER_EVENT_DRAW_WHEEL` |
| Engine sound | `JER_EVENT_CAR_GEARBOX`, `JER_EVENT_CAR_REVS`, `JER_EVENT_CAR_ENGINE_SOUND` |
| Explosions & overlay | `JER_EVENT_EXPLOSION_SPAWN`, `JER_EVENT_EXPLOSION_DRAW`, `JER_EVENT_EXPLOSION_COLLIDE`, `JER_EVENT_DRAW_OVERLAY`, `JER_EVENT_DRAW_WORLD` |
| Camera | `JER_EVENT_CAMERA` |
| Ped & animation | `JER_EVENT_PED_MOVE`, `JER_EVENT_PED_POSE`, `JER_EVENT_PED_SKELETON` |
| Frontend & menus | `JER_EVENT_FRONTEND`, `JER_EVENT_PAUSE_MENU`, `JER_EVENT_MP_FRONTEND` |
| Map | `JER_EVENT_MAP`, `JER_EVENT_DRAW_MAP` |
| Levels & vehicles | `JER_EVENT_CAR_AVAILABILITY`, `JER_EVENT_CAR_DATA_SOURCE` |
| Networking | `JER_EVENT_NET_INPUT`, `JER_EVENT_NET_CAR_STATE`, `JER_EVENT_NET_PLAYERS`, `JER_EVENT_NET_RECV`, `JER_EVENT_NET_SPAWN` |
| Custom | `>= JER_EVENT_MODULE_CUSTOM` (free for module-to-module messaging) |
