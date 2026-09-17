# Presentation screens

A **presentation screen** is a full-screen, non-interactive frontend screen: a
heading plus one live body line.

```
Compiling JERICHO addons...
ZOOMMOD [3/7]
```

It is the counterpart of the [module frontend menus](HOOKS.md) for work that
has nothing to choose. It exists so that long work which happens before the
frontend menu is up — compiling deep modules, say — shows progress instead of a
frozen screen, **without any new engine code per screen**.

## The API — `JERICHO/include/jer_screen.h`

```c
typedef struct JER_SCREEN
{
    const char* id;                                 /* stable id */
    const char* title;                              /* static heading line */

    void (*get_body)(void* ud, char* out, int max); /* live body line (optional) */
    int  (*on_update)(void* ud);                    /* non-zero = finished, dismiss */
    void (*on_enter)(void* ud);                     /* became visible */
    void (*on_exit)(void* ud);                      /* dismissed */
    void* userdata;
} JER_SCREEN;
```

| call | who calls it | what it does |
| --- | --- | --- |
| `jer_screen_register(&scr)` | module / host | add or replace a screen; returns its index or `-1` |
| `jer_screen_reset()` | engine | clear the registry (runs on every module reload) |
| `jer_screen_show("compile")` | module / host | make a screen visible; non-zero if found |
| `jer_screen_dismiss()` | module / host | dismiss the visible screen (idempotent) |
| `jer_screen_tick()` | engine | advance one frame; runs `on_update` and auto-dismisses on finished |
| `jer_screen_active()` | engine | non-zero while a screen should be drawn |
| `jer_screen_title()` / `jer_screen_body(out, max)` | engine | the heading / the live body line |

```c
static char gName[32];
static int  gStep, gTotal;

static void body(void* ud, char* out, int max)
{
    snprintf(out, max, "%s [%d/%d]", gName, gStep, gTotal);
}

static int step(void* ud)
{
    /* do a slice of the work, then report whether we are done */
    return ++gStep > gTotal;
}

static const JER_SCREEN scr = { "compile", "Compiling JERICHO addons...", body, step, NULL, NULL, NULL };

/* from your module entry */
jer_screen_register(&scr);
jer_screen_show("compile");
```

## When a screen is drawn

A screen is drawn **only while the game is in the frontend**, in two places:

1. **At boot, before the frontend menu is built** — `JerichoRunBootScreens()`
   is called from `LoadFrontendScreens()` right after the loading screen is
   up and before the rest of the frontend is built. It pumps the screen until
   `on_update` reports finished. A screen shown from your module's initialiser
   therefore appears *before* the menu.
2. **On any later frontend frame** — `jer_screen_tick()` runs from the
   frontend frame (`JER_EVENT_FRAME`) and the screen is drawn with the engine
   error notices, so a screen raised during gameplay takes effect the next
   time the frontend runs.

## Yes/No prompts — `jer_prompt.h`

A prompt is a presentation screen with a fixed shape, so the engine needs no
dialog UI either:

```c
jer_prompt_begin("Compile the deep mods in your folder? JERICHO must restart to do this.",
                 jer_compile_request);
```

The question becomes the heading and the body shows the highlight
(`> Yes   No` / `  Yes > No`). `jer_prompt_tick(prev, next, confirm, cancel)` is
fed one frame of input — already-decoded booleans, so the engine keeps its own
pad makes — and runs the `on_yes` callback when the player confirms.
Left/right move the highlight, Cross confirms it, Circle dismisses.

The frontend feeds it from its own frame loop, **after** the current screen has
had its turn, so a Cross that answers the prompt cannot also press the button
underneath it; and while a prompt is up the frontend must not act on the pad
(see the guard in `JerichoModsScreen`). "Compile Mods" is the first user.

## Rules and gotchas

- **A boot screen needs `on_update`.** The boot loop ends when `on_update`
  reports finished; the loop is capped at `JER_SCREEN_BOOT_MAX_FRAMES`
  (100000) so a screen that never finishes can never hang the frontend, but it
  would sit there. A screen with no `on_update` is meant for the frontend-frame
  path, where it stays until something calls `jer_screen_dismiss()`.
- **Do the work in slices, not all at once.** `on_update` is called once per
  frame, so a build or a scan should advance a little each frame — that is what
  keeps the body line live and the screen responsive.
- **The registry is cleared on reload.** `jer_screen_reset()` runs inside module
  activation (like `jer_pause_menu_reset()`), so register in your entry
  function, not in a global constructor.
- **Engine side.** The only engine code is `JerichoDrawScreen()` (heading +
  body, centred, over a black backdrop) and `JerichoRunBootScreens()` in
  `Frontend/FEmain.c`. `JerichoRunBootScreens()` deliberately does **not** use
  `EndFrame()`: `EndFrame()` VSyncs before it presents, which deadlocks while
  swap-interval blocking is on, and at that point in the boot the display has
  just been brought up. It mirrors `ShowLoadingScreen()` instead
  (draw → `DrawSync` → `VSync` unless fast-loading → `PsyX_EndScene` →
  re-install the env) and **clears the draw OT itself** — `EndFrame()` normally
  does that as part of its buffer swap, and an un-cleared OT makes `DrawOTag`
  walk garbage and hang.
- **Text is the frontend font** (`FEPrintString`), centred on the 640×512
  frontend screen at x = 320.
