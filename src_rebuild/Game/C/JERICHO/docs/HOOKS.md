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
module and per hook into `JERICHO.log`.

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

## Saying things on screen

A module has **four channels** for putting words in front of the player — the
HUD, the game's own notice slot, the red error toast, and the full-screen
frontend frames. Choosing between them is really two questions: **who is
speaking** — your mod, or the game — and **where should the line live**: an
event that passes, a readout that persists, or the game's own single slot.

| Channel | API | Appears | Lasts | Reach for it when… |
| --- | --- | --- | --- | --- |
| HUD message | `jer_hud.h` — `jer_hud_message` / `…_segs` / `…_replace` | centred, stacked from the top of the screen | a set number of frames | an **event** happened: *"You killed VASQUEZ"* |
| HUD readout | `jer_hud.h` — `jer_hud_panel` / `jer_hud_panel_bar` | an anchored corner line or meter | until you clear it or set it again | a **value** whose subject exists: a lock-on name, a health bar under it |
| Player notice | `jer_notify.h` — `jer_notify` / `jer_notify_clear` | the game's own spot — golden, centred, near the top | a set number of seconds | the **game** is talking: *"You Drowned"* |
| Error toast | `jericho.h` — `jer_error` / `jer_error_count` / `jer_error_at` | gentle red, down the left edge — frontend **and** in game | ~5 s | something needs telling **now**: a bad argument, a failed join |
| Presentation screen | `jer_screen.h` / `jer_prompt.h` | a whole frontend frame | until dismissed | boot/progress text and Yes/No prompts — see [`screens.md`](screens.md) |

### The HUD channels — the module's voice

```c
#include "jer_hud.h"

jer_hud_message("You killed VASQUEZ", 0);        /* 0 frames = the 3 s default */
```

- A message **expires on its own** — `frames <= 0` means `JER_HUD_DEFAULT_FRAMES`
  (90, about 3 s at the 30 fps sim step). At most `JER_HUD_MAX` (4) are on screen
  at once; a burst recycles the slot with the least time left rather than
  growing, and `jer_hud_message_replace` clears the queue first for a repeater
  that would otherwise stack.
- `jer_hud_message_segs(segs, count, frames)` builds **one** line from up to
  `JER_HUD_SEG_MAX` (6) colour runs, so a name can carry its own colour inside a
  sentence. A run with `ambient = 1` is drawn in the engine's ambient text colour,
  which keeps the connective words matching every plain HUD line.
- Colour is **ambient global state** in the engine, so the drawer saves and
  restores it: a coloured message never tints whatever draws next.
- For a **readout** instead of an event, set a panel every frame while its
  subject exists and clear it when it does not:
  `jer_hud_panel(slot, anchor, text, r, g, b)` with `anchor` one of
  `JER_HUD_ANCHOR_TOP_LEFT` / `_TOP_CENTRE` / `_TOP_RIGHT`. Panels sharing an
  anchor stack downward in slot order (`JER_HUD_PANEL_MAX` = 4), and
  `jer_hud_panel_bar(slot, anchor, value, max, r, g, b)` puts a small filled
  **meter** in the same slot — a health bar under a lock-on name.
- All of it is drawn from the engine's overlay pass (`jer_hud_draw`, next to the
  pause menu), so a module needs **no draw hook of its own**.

### The game's own voice — `jer_notify` and `jer_error`

These two ride engine paths on purpose; they are not the HUD's, and the
difference is who is speaking.

- **`jer_notify(text, priority, seconds)`** takes the engine's OWN player-message
  path (`SetPlayerMessage` → `DrawMessage`), so the line lands exactly where
  *"You Drowned"* and *"You wrecked your vehicle"* do: golden, centred, near the
  top, for a fixed number of seconds. Priority `0..JER_NOTIFY_PRIORITY_MAX` (5)
  decides which line wins the single slot — engine mission lines use 2–3, and a
  line already showing with a higher priority is not replaced. `seconds <= 0`
  means 3. The text is **copied engine-side**, so a caller may pass a temporary
  buffer; `jer_notify_clear()` drops it.
- **`jer_error(fmt, ...)`** is the opposite: a short-lived gentle-red notice down
  the **left** of the screen, wrapped by the engine over several one-line rows
  (34 characters a row), for about 5 seconds, drawn in the frontend **and** in
  game. The engine raises one itself for a rejected command-line argument, and a
  module that refuses to load or fails to join should use it too. It is the
  "something needs to be told, now" channel, not a conversation.
- **Drawing them yourself.** `jer_error_count()` and `jer_error_at(i)` expose the
  live lines, which is how the engine prints them with its own text primitives
  (`State_FrontEnd` in the frontend, `DrawGame` in game). Give each row the pitch
  of the **font you are drawing in**: the frontend font is about three times the
  height of the in-game one, so the engine steps frontend rows by 36 px and
  in-game rows by 12 px.

## What a module can do

- **React to events** — see `events.md` for the full table. The engine
  call sites are inert when no module handles them, so a vanilla game (no
  modules) behaves exactly stock.
- **Add real frontend menus** — `jer_frontend.h` registers menus that the
  engine renders as native frontend screens (`jer_frontend_register_menu`),
  optionally routed from the main menu (`jer_frontend_set_main_entry`). Items
  can open submenus, run callbacks, adjust values with Left/Right, or return.
  Triangle is offered to the menu too, as it is on every stock screen: set
  `JER_FE_MENU.on_back` to answer for yourself (the car-select Back row returns
  to the Day/Night screen, deliberately not to the stock screen it replaced),
  or leave it NULL and a row with `is_back` set stands in. With neither, the
  press is left unclaimed. Triangle is a pad-only path — a headless run has no
  pad, so a module menu is never even reached.
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
- **Say something on screen** — four channels for one job, HUD and game voice;
  the whole picture is in [Saying things on screen](#saying-things-on-screen).
  In short: `jer_hud_message` for module chatter, `jer_notify` for the game's own
  voice, and `jer_error` for "something needs telling, now".
- **Play a sound without starving the engine** — `jer_sound.h`. There are only 16
  SPU voices and the engine's own collision/explosion sounds play on whatever
  `GetFreeChannel()` hands out, so a module that LOCKS a voice per sound can leave
  it silent. `jer_sound_lock` locks only while `JER_SFX_RESERVE` voices stay free;
  `jer_sound_ensure(&channel)` is the one-call form for a cached `-1` channel
  (acquire, re-lock after a level change, or give up gracefully). A refused lock
  still yields a voice to play on — never treat it as "no sound".
- **Move somewhere the world has not streamed to** — `jer_map.h`. The engine
  streams the map as a 2×2 window of REGIONS and only pre-loads neighbours, so a
  region you HOP into (a teleport, or a spawn far from the level start) is never
  loaded and the ground there reads as a void. `jer_map_spool_to(x, z)` makes the
  engine load the destination's region (and its texture areas) before you need
  them; `jer_map_region_of` / `jer_map_region_resident` / `jer_map_region_has_data`
  answer "where am I / is it loaded / is there anything there"; `jer_map_ready`
  is the level-header guard (the frontend runs with a zeroed header and every
  region division would trap). Full story: [`map-streaming.md`](map-streaming.md).
- **Custom events** — values `>= JER_EVENT_MODULE_CUSTOM` are free for
  module-to-module messaging (the sandbox uses one for its no-damage
  toggle).

## Logging

`ctx->jer_log(ctx, fmt, ...)` goes through the JERICHO logger, which the
game routes into `JERICHO.log` (and the console in `_DEBUG` builds).
Prefix module lines with `[myid]` so the boot inventory stays readable.

## Boot arguments (debug builds)

The game accepts frontend-bypass launch arguments (PC only, `DEBUG_OPTIONS`
builds) for fast testing:

```
JERICHO_dev.exe -nointro -nofmv -level <city> -car <slot#> -gamemode <mode>
                 -time <time> -weather <weather>
```

- `-level`: `chicago` / `havana` / `lasvegas` / `rio`
- `-car`: a slot number (0-9) or a car name
- `-gamemode`: `takeadrive` (default) / `survival` / `pursuit` / ...
- `-time`: `day` / `dusk` / `night`; `-weather`: `sunny` / `rain`

`-car` requires at least a `-level` (a message box explains otherwise).
