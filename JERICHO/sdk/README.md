# JERICHO Addon SDK

A standalone, self-contained kit for writing JERICHO addons for REDRIVER2.
No game source, no PsyCross, no SDL2/OpenAL/JPEG — just this folder, a C
compiler, and the JERICHO API.

## What's inside

```
sdk/
  include/            the JERICHO API headers (jericho.h + helpers)
    jericho.h         the API: hooks, events, overrides, module entry
    jer_math.h        math helpers
    jer_config.h      persistent per-module settings (CONFIG/<id>.ini)
    jer_pause_menu.h  module-provided pause menus
    jer_anim.h        player-skeleton animation helpers
    jer_menu.h        module-provided frontend menus
    jer_npc.h         NPC helpers
    jer_hud.h         on-screen HUD messages (jer_hud_message) and readouts
                      (jer_hud_panel / jer_hud_panel_bar)
    jer_sound.h       sound-voice reservation (jer_sound_lock / jer_sound_ensure)
    jer_net.h         addon network bridge (jer_net_send / JER_EVENT_NET_RECV)
    jer_frontend.h    add real frontend menus (jer_frontend_register_menu)
    jer_events.h      event argument structs (game types as void*)
    jer_colour.h      canonical RGB colour type (pack/unpack for GTE words + CLUTs)
    jer_ped_palette.h per-instance pedestrian palettes (jer_ped_palette_select)
    jer_car_palette.h per-instance CAR colours (the car-path companion)
    jer_map.h         world region helper: where am I, and stream the map there
    jer_notify.h      the engine's own on-screen notice
    jer_prompt.h      a host-owned Yes/No prompt
    jer_screen.h      module-provided presentation screens
    jer_texture.h     custom texture injection (images and raw pages)
  lib/x64/Release/
    JERICHO.lib     import library for the game exe's exported symbols
  build_mods.bat      compiles one addon folder into a DLL
  example/            a minimal working addon (copy it to start)
```

The SDK headers and import lib mirror the ones shipped in the game's
development tree (`src_rebuild/`); they are refreshed whenever the game is
built. The import lib exports every symbol of the game's own code plus the
JERICHO API, so addons may call game functions and read game globals
(`player[]`, `car_data[]`, ...) directly — no exe modification.

## Writing an addon

An addon is a folder with a `mod.toml` and C/C++ source:

```
myaddon/
  mod.toml      metadata (see below)
  myaddon.c     the module: registers hooks via the JERICHO API
```

`mod.toml`:

```toml
id = "myaddon"            # must match the folder name AND the entry symbol
name = "My Addon"
version = "0.1.0"
author = "You"
description = "what it does"
default-enabled = true    # optional, used when modlist.ini omits it
dependencies = []         # optional: ["othermod"] or "othermod"
incompatible = []         # optional: ["othermod"] — this addon will not run
                          # BESIDE that one (see below)
runtime = "dll"           # REQUIRED: marks this as a runtime DLL addon
```

`dependencies` and `incompatible` both name other mod **ids**. A missing dependency
disables the addon; so does one that is active and listed in `incompatible`. The
difference is the meaning: a dependency is something you need, an incompatibility is
something you cannot share with. **You mark yourself** — the declaring mod is the one
that stands down, and the reason the player is shown names the other mod, both as an
error on the screen and as a line on the Mods screen. Two mods that each declare the
other both stand down and each say why; nothing is ever disabled by a third party's
manifest.

The source must export the entry point and register itself:

```c
#include "jericho.h"

static int myOnFrame(void* userdata, void* args)
{
    /* ... */
    return JER_RESULT_CONTINUE;
}

JER_MODULE_ENTRY(jer_module_myaddon_entry)(JERICHO_CONTEXT* ctx)
{
    ctx->jer_register_module(ctx, "myaddon", "My Addon", "0.1.0",
        "You", "what it does", "", JERICHO_SDK_VERSION);
    ctx->jer_register_hook(ctx, JER_EVENT_FRAME, myOnFrame, NULL, 0);
}
```

The entry symbol is always `jer_module_<id>_entry` — the folder name, the
`mod.toml` id and the symbol must agree.

## Building

```
build_mods.bat myaddon
```

Requires Visual Studio 2019/2022 with the C++ toolset (found automatically
via vswhere). Produces `myaddon\myaddon.dll`.

## Installing

Copy the built DLL next to the addon's `mod.toml` in the game's
`JERICHO\MODS\<id>\` folder, then enable it in the game under
**Options → JERICHO** (or add `id = 1` to `JERICHO\CONFIG\modlist.ini`).
No rebuild of the game — ever. Toggling, reordering and per-module settings
all happen at runtime and persist in `JERICHO\CONFIG\`.

## Platform notes

- Windows: runtime loading via `LoadLibrary` (this SDK builds x64 DLLs).
- Linux: the same pipeline uses `dlopen`; build with the game's mods build.
- Emscripten/Android: no runtime loading — addons are ignored (logged).
