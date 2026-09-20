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

## Module pause menus

Modules add their own entries to the in-game pause screen with
`jer_pause_menu.h` — no game code needs editing:

```c
#include "jer_pause_menu.h"

static void MyToggleLabel(void* ud, char* out, int max) { snprintf(out, max, "My Toggle: %s", gMyToggle ? "ON" : "OFF"); }
static int  MyToggleAct(void* ud, int dir)              { gMyToggle ^= 1; return JER_PAUSE_QUIT_NONE; }

static const JER_PAUSE_MENU_ITEM items[] = {
    { NULL,        MyToggleLabel, MyToggleAct, NULL, NULL, 0 },  /* dynamic label + toggle */
    { "Open Sub",  NULL,          NULL,        NULL, &sub, 0 },  /* opens a submenu */
    { "Sensitivity", NULL,        MyAdjust,    NULL, NULL, 1 },  /* left/right adjusts */
};
static const JER_PAUSE_MENU myMenu = { "My Mod", items, 3 };

/* in the module entry: */
jer_pause_menu_register(&myMenu);
```

- The engine collects registered menus under a **"Modules"** submenu in the
  pause screen (`Continue` → `Modules` → your menu → items).
- `get_label` is re-queried every time the pause opens and after every
  activation, so toggles/adjustments show live state.
- `on_activate` returns a `JER_PAUSE_QUIT_*` code (e.g.
  `JER_PAUSE_QUIT_CONTINUE` to leave the pause, `JER_PAUSE_QUIT_NONE` to
  stay). With `adjust = 1` it receives `-1`/`1` on left/right.
- Depth: root → Modules → your menu → one submenu level.
- `crumple`, `d2pl` and `sandbox` are the worked examples — their old
  hardcoded shells were removed from `pause.c`.

## What a module can do

- **React to events** — see `events.md` for the full table. The engine
  call sites are inert when no module handles them, so a vanilla game (no
  modules) behaves exactly stock.
- **Add real frontend menus** — `jer_frontend.h` registers menus that the
  engine renders as native frontend screens (`jer_frontend_register_menu`),
  optionally routed from the main menu (`jer_frontend_set_main_entry`). Items
  can open submenus, run callbacks, adjust values with Left/Right, or return.
- **Talk over the network** — `jer_net.h` lets any module send/receive named
  channels over the active multiplayer session (`jer_net_register_channel` +
  `jer_net_send`, delivered back as `JER_EVENT_NET_RECV`). It is a safe no-op
  when there is no session, so a module can call it unconditionally. Use it for
  data a peer cannot derive locally (host-spawned entities, event logs, RNG a
  module author owns).
- **Own pause-menu items** — register menus/submenus with
  `jer_pause_menu_register()` from `jer_pause_menu.h` (see the "Module
  pause menus" section). The engine collects everything under a "Modules"
  submenu in the pause screen — no per-module code in `pause.c`.
- **Replace whole behaviors** — `ctx->jer_override(ctx, SLOT, fn)`
  swaps an engine function-pointer slot (`JER_OVERRIDE_SLOT_SIM` today);
  `jer_get_override(slot)` returns the previous one so a module can chain.
- **Persist settings** — `jer_config_get_int/set_int/...` from
  `jer_config.h`. Each module owns `JERICHO/CONFIG/<modid>.ini`. d2pl writes
  every setting there; hand-editing the file while the game is closed works.
- **Recolour one pedestrian instance** — `jer_ped_palette.h`:
  `jer_ped_palette_team(r,g,b,strength)` returns a handle for a team colour and
  `jer_ped_palette_select(handle)` applies it (from a `JER_EVENT_PED_DRAW`
  handler, for the ped being drawn). The engine swaps recoloured CLUT rows in for
  that one draw, so a module can put individual characters in team colours.
  Measured footprint and mechanism: `ped-palette.md`.
- **Suppress the frontend's idle demo** — `JER_EVENT_FRONTEND_IDLE`
  (`JER_ARGS_FRONTEND_IDLE`) fires from the timer that would boot the attract
  demo after ~30 s without input; set `suppress = 1` and the timer is pushed
  back. Anything that leaves a player sitting in a menu is "idle" by that
  timer, and the demo it starts loads a whole level -- which blocks the main
  thread while it happens.
- **Say something on screen** — `jer_hud.h`. `jer_hud_message` (and
  `jer_hud_message_segs` for a partly-coloured line) queues a message that is
  drawn centred, stacked from the top, and expires on its own — right for an
  event ("You killed VASQUEZ"). For a READOUT, where a line belongs in a corner
  for exactly as long as its subject exists, use `jer_hud_panel(slot, anchor,
  text, r, g, b)` — an anchored line drawn every frame until `jer_hud_panel_clear`
  or the next set. Both are drawn from the engine's overlay pass, so a module
  needs no draw hook of its own.
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
