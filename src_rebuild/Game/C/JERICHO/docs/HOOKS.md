# Writing a JERICHO module

A module is a C/C++ source file compiled into the game. The full list of
events, their argument structs, and the engine-side bridges is in
[`events.md`](events.md) and `src_rebuild/Game/C/jer_events.h`.

## Module anatomy

Every module folder under `JERICHO/MODS/` has:

```
JERICHO/MODS/<id>/
    mod.toml      metadata: id, name, version, author, description
    <id>.c        the module source (and .h for shared types)
    readme.md     what it does, how to configure it
```

The entry point is declared with the SDK macro (the game is C++, so the
entry and every function it exposes must have C linkage):

```c
#include "jericho.h"
#include "jer_events.h"

JER_MODULE_ENTRY(jer_module_myid_entry)(JERICHO_CONTEXT* ctx)
{
    /* register the module itself: id, name, version, author, desc,
     * dependencies ("other,module,ids"), SDK version */
    ctx->jer_register_module(ctx, "myid", "My Module", "1.0.0",
        "You", "What it does.", "", JERICHO_SDK_VERSION);

    /* register event handlers: (event, fn, userdata, priority).
     * Handlers run in priority order (lower first); the last return
     * value wins for shared args. */
    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, MyOnFrame, NULL, 0);
}

static int MyOnFrame(void* userdata, void* args)
{
    (void)userdata;
    (void)args;
    /* return JER_RESULT_STOP to claim/consume the event,
     * JER_RESULT_CONTINUE to let later handlers run */
    return JER_RESULT_CONTINUE;
}
```

Compile it: premake auto-scans `JERICHO/MODS` and builds every installed
module — no list to maintain. `--with-mods="myid,crumple"` is an optional
filter for building only a subset:

```
premake5.exe vs2019
premake5.exe --with-mods="myid,crumple" vs2019   # optional filter
```

The runtime reads `JERICHO/CONFIG/modlist.ini` (regenerated on first boot)
and activates the enabled modules in load order, logging one line per
module and per hook into `REDRIVER2.log`.

## What a module can do

- **React to events** — see `events.md` for the full table. The engine
  call sites are inert when no module handles them, so a vanilla game (no
  modules) behaves exactly stock.
- **Own pause-menu items** — bridge via `JER_EVENT_PAUSE_MENU` (see
  `pause.c`'s crumple/d2pl shells for the pattern). The engine keeps the
  `MENU_ITEM` shells; the module owns the state and the label text.
- **Replace whole behaviors** — `ctx->jer_override(ctx, SLOT, fn)`
  swaps an engine function-pointer slot (`JER_OVERRIDE_SLOT_SIM` today);
  `jer_get_override(slot)` returns the previous one so a module can chain.
- **Persist settings** — `jer_config_get_int/set_int/...` from
  `jer_config.h`. Each module owns `JERICHO/CONFIG/<modid>.ini`. d2pl writes
  every setting there; hand-editing the file while the game is closed works.
- **Custom events** — values `>= JER_EVENT_MODULE_CUSTOM` are free for
  module-to-module messaging (the sandbox uses one for its no-damage
  toggle).

## Logging

`ctx->jer_log(ctx, fmt, ...)` goes through the JERICHO logger, which the
game routes into `REDRIVER2.log` (and the console in `_DEBUG` builds).
Prefix module lines with `[myid]` so the boot inventory stays readable.

## Boot arguments (debug builds)

The game accepts frontend-bypass launch arguments (PC only, `DEBUG_OPTIONS`
builds) for fast testing:

```
REDRIVER2_dev.exe -nointro -nofmv -level <city> -car <slot#> -gamemode <mode>
                 -time <time> -weather <weather>
```

- `-level`: `chicago` / `havana` / `lasvegas` / `rio`
- `-car`: a slot number (0-9) or a car name
- `-gamemode`: `takeadrive` (default) / `survival` / `pursuit` / ...
- `-time`: `day` / `dusk` / `night`; `-weather`: `sunny` / `rain`

`-car` requires at least a `-level` (a message box explains otherwise).
