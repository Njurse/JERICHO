# Writing a JERICHO module

A module is a folder with a `mod.toml` and a C/C++ source file. It joins the
game one of two ways: **compiled in** (a deep mod — no `runtime` key; it may
include engine headers and read/write game globals), or as a
**runtime-loaded shared library** (`runtime = "dll"`, API-only). Everything
below works for both unless a section says otherwise. The full list of
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

- The engine collects registered menus under a **"JERICHO Addons"** container
  in the pause screen (`Continue` → `JERICHO Addons` → your page → items).
- `get_label` is re-queried every time the pause opens and after every
  activation, so toggles/adjustments show live state.
- `on_activate` returns a `JER_PAUSE_QUIT_*` code (e.g.
  `JER_PAUSE_QUIT_CONTINUE` to leave the pause, `JER_PAUSE_QUIT_NONE` to
  stay). With `adjust = 1` it receives `-1`/`1` on left/right.
- **Nesting.** Your page may hold submenus up to `JER_PAUSE_MENU_MAX_DEPTH`
  (5) levels below it. The engine derives its navigation-stack size from that
  constant, so a tree that would nest deeper is refused and logged rather than
  built half-way. `jer_menu.h` exposes the same ceiling (`JER_MENU_MAX_DEPTH`)
  for a module that draws its own nested menu.
- **Contributing to someone else's menu.** Give your menu a stable `id`
  (`JER_PAUSE_MENU.id`) and another module can append rows to it with
  `jer_pause_menu_add_items(target, items, count)` — resolved when the menu is
  built, so order does not matter, and the owner's own rows are never
  displaced (carhacks contributes its car actions to mp's car page this way).
- `crumple`, `d2pl` and `sandbox` are the worked examples — their old
  hardcoded shells were removed from `pause.c`.

## Saying things on screen

A module has **five channels** for putting words in front of the player — the
HUD, the unified status console, the game's own notice slot, the red error
toast, and the full-screen frontend frames. Choosing between them is really two
questions: **who is speaking** — your mod, or the game — and **where should the
line live**: an event that passes, a readout that persists, or the game's own
single slot.

| Channel | API | Appears | Lasts | Reach for it when… |
| --- | --- | --- | --- | --- |
| HUD message | `jer_hud.h` — `jer_hud_message` / `…_segs` / `…_replace` | centred, stacked from the top of the screen | a set number of frames | an **event** happened: *"You killed VASQUEZ"* |
| HUD readout | `jer_hud.h` — `jer_hud_panel` / `jer_hud_panel_bar` | an anchored corner line or meter | until you clear it or set it again | a **value** whose subject exists: a lock-on name, a health bar under it |
| Player notice | `jer_notify.h` — `jer_notify` / `jer_notify_clear` | the game's own spot — golden, centred, near the top | a set number of seconds | the **game** is talking: *"You Drowned"* |
| Error toast | `jericho.h` — `jer_error` / `jer_error_count` / `jer_error_at` | gentle red, down the left edge — frontend **and** in game | ~5 s | something needs telling **now**: a bad argument, a failed join |
| Status console | `jer_console.h` — `jer_console_line` / `jer_console_log` / `jer_console_chat` | bottom-left, half size, newest row at the bottom | ~5 s (4 s hold + 1 s fade) | a running **status feed** in one place — the mp connection/handshake lines; see [`console.md`](console.md) |
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
  A menu that must STAND IN FOR a screen in a chain opens with
  `jer_frontend_open_replace` (not `jer_frontend_open`): it takes that screen's
  slot on the nav stack, so Back/Triangle return where the replaced screen would
  - the car-select menu replaces the stock car screen this way. Triangle is
  offered to the menu too, as it is on every stock screen: set
  `JER_FE_MENU.on_back` to answer for yourself, or leave it NULL and a row with
  `is_back` set (or the engine's own back) stands in. With neither, the press is
  left unclaimed. Triangle is a pad-only path — a headless run has no pad, so a
  module menu is never even reached.
- **Talk over the network** — `jer_net.h` lets any module send/receive named
  channels over the active multiplayer session (`jer_net_register_channel` +
  `jer_net_send`, delivered back as `JER_EVENT_NET_RECV`). It is a safe no-op
  when there is no session, so a module can call it unconditionally. Use it for
  data a peer cannot derive locally (host-spawned entities, event logs, RNG a
  module author owns).
- **Own pause-menu items** — register menus/submenus with
  `jer_pause_menu_register()` from `jer_pause_menu.h` (see the "Module
  pause menus" section). The engine collects everything under a "JERICHO
  Addons" container in the pause screen — no per-module code in `pause.c`.
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
- **Spawn and own NPCs** — `jer_npc.h`: `jer_npc_spawn` / `jer_npc_spawn_model`
  create a ped and hand it to your module (so the ped skeleton/pose hooks fire
  for it), `jer_npc_owned()` answers whether a ped is yours and still alive (the
  engine checks `pUsedPeds`, so a recycled slot never matches), and
  `jer_npc_despawn` releases it. `jer_npc_set_world` / `_move_to` / `_stop` /
  `_face` / `_tick` drive it; ambient pedestrians never fire those hooks.
- **Suppress the frontend's idle demo** — `JER_EVENT_FRONTEND_IDLE`
  (`JER_ARGS_FRONTEND_IDLE`) fires from the timer that would boot the attract
  demo after ~30 s without input; set `suppress = 1` and the timer is pushed
  back. Anything that leaves a player sitting in a menu is "idle" by that
  timer, and the demo it starts loads a whole level -- which blocks the main
  thread while it happens.
- **Say something on screen** — five channels for one job, HUD and game voice;
  the whole picture is in [Saying things on screen](#saying-things-on-screen).
  In short: `jer_hud_message` for module chatter, `jer_console_line` for a
  status feed, `jer_notify` for the game's own voice, and `jer_error` for
  "something needs telling, now".
- **Play a sound without starving the engine** — `jer_sound.h`. There are only 16
  SPU voices and the engine's own collision/explosion sounds play on whatever
  `GetFreeChannel()` hands out, so a module that LOCKS a voice per sound can leave
  it silent. `jer_sound_lock` locks only while `JER_SFX_RESERVE` voices stay free;
  `jer_sound_ensure(&channel)` is the one-call form for a cached `-1` channel
  (acquire, re-lock after a level change, or give up gracefully). A refused lock
  still yields a voice to play on — never treat it as "no sound".
- **Give the voice back when the player leaves the game** — a voice locked with
  `jer_sound_lock` survives a level change (`ResetSound` clears the locks at the next
  level's start) but NOT the return to the menus, so a module that caches a channel
  for the whole match keeps a voice out of the pool while the menus are up. The
  frontend entry now stops every channel (`StopAllChannels`) and calls
  `jer_sound_release_all()` — but that only clears the LOCK: a module must also drop
  its cached channel index, or the next match reuses a half-held voice. Do it in a
  `JER_EVENT_FRONTEND_ENTERED` hook (cainescrossfire's `cd2OnFrontendEntered` →
  `cd2DropHeldVoices` is the worked example).
- **Put a 2D label on a 3D thing** — `jer_screen.h`, `jer_screen_remap_x`. The 3D
  projection lands in the widescreen-MAPPED viewport, not in the 320x240 buffer that
  `PrintString` and friends draw into: the PSX 3D view is a centred 240-unit
  square-pixel band (viewport `[40, 280]` at 4:3, `[-53, 373]` at 16:9). Project a
  world point yourself and draw it unremapped and the label drifts off its object —
  a little at 4:3, a lot on a wide aspect. `jer_screen_remap_x` is the engine's own
  conversion (the one `sky.c` applies to the GTE-projected sun): pass the projected
  x, get the x to draw at. Y is not affected — the aspect matrix is PAL-only, not AR.
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

## Input debugging (environment levers)

"The look does nothing" is the kind of report that costs a day, because the chain
is four layers deep (`SDL trigger axis -> PSX button word -> Pads[n].mapped ->
paddCamera -> TurnHead`) and every layer fails the same way from the driver's seat.
Three test-only levers make it legible; nothing reads them unless the environment
sets them. `grep padprobe JERICHO.log` is the whole report:

- `JERICHO_PADPROBE=1` — on change only: the resolved L2/R2 mapping ints
  (`0x4000` = the AXIS flag, `-1` = bound to nothing), the RAW axis value the
  `> 16384` gate sees, and the PSX button word (`[Psy-X] [padprobe] slot N ...`);
  then, from `TurnHead`, `pads`/`lookbits` (after the CAMERA_LOOK handlers
  suppressed or not), the AI mode, and `headPos->headTarget` — which is the number
  the camera ANGLE is built from, so a working look walks it to ±1024.
- `JERICHO_PADPROBE_FORCE=<button>` — hold one PSX button down for the whole run
  (`cross square triangle circle l1 r1 l2 r2 l3 r3 select start`). It enters at the
  pad layer, so the bit travels the real path — the way to measure what a press
  DOES without a human on the pad.
- `JERICHO_PADPROBE_FORCE=left|right|back` — pin a look bit into `paddCamera`
  instead (handled in `camera.c`), which separates "the camera forgot how to look"
  from "the pad never sent it".
- `JERICHO_PADPROBE_AXIS=<0..255>` — hold the LEFT STICK X at a known deflection for
  the whole run **and put the pad in ANALOG mode**, so the car's *analogue* steering
  path runs (128 = centred, 0 = full left, 255 = full right). The two steering paths
  are different code: the digital one clamps `cp->wheel_angle` to 352, the analogue
  one computes it from the stick. A build with no controller attached never reaches
  the analogue path at all (a keyboard slot is forced digital), so a report of "it
  steers far too fast" was untestable without this.
- `JERICHO_HANDBRAKE_AIMODE=1` — restore the sandbox-era convenience where a
  handbrake edge cycles the player AI mode. It is OFF by default because sandbox
  keys its `CAMERA_LOOK` suppression off that mode, so one Triangle press used to
  kill look-left/right (and look-back on R3) for the rest of the level, silently.

## "My car drives way too fast": the handling probe

`JERICHO_HANDLING_PROBE=1` answers that report with numbers instead of adjectives.
The three causes look identical from the driver's seat — the CLOCK running fast, the
INPUT over-driving the car, or the car's own DATA — so the probe prints all three:

```
[JER-PROBE] t=4s speed=147 wheel=0/352 thrust=4915 hndType=0 slot=0 model=1 \
            power=4096 traction=4096 mass=4096 twistY=224 grip=896/1100 handbrake=0 \
            ff=0 lock=0 skip=0
JERICHO-PROBE: steps=239 ms=7986 steps_per_sec=29 max_speed=168 ff=0 lock=0 skip=0 verdict=RATE_OK
```

- One `[JER-PROBE]` line per **simulated** second, for the local player's car only —
  `MainPlayer`'s car by identity, because in a session the peer cars on this machine
  are player cars too and a looser test samples whichever one stepped last.
- `wheel=N/352` is the input check (the analogue path can exceed the lock; the stock
  digital path cannot). `power/traction/mass/twistY` are the car's `CAR_COSMETICS`
  record — the numbers everything else derives from, so a record belonging to the
  wrong car (cross-city import, resident-slot mix-up) shows up here first. `grip` is
  the front/rear friction scale the tyres actually resolved.
- `steps_per_sec` is the clock check, measured against **real milliseconds**
  (`PsyX_GetTicks`), never the vblank counter — the frame skip inflates that counter,
  which would make the fast case read *slow*.
- 30 = stock. ~210 = `FastForward` (7 steps/frame). Anything else above 30 with
  `ff=0` is the PsyX frame skip below (`skip=1`), which is what a latched BACKSPACE
  produced: **355 steps/second, ~12x**, with the car's per-sim-second speed curve
  unchanged (0, 67, 98, 130 ...) — i.e. the handling was never the problem.
- Drive it headlessly: `JERICHO_HANDLING_PROBE=1 JERICHO_PADPROBE_FORCE=cross
  JERICHO_dev.exe -level chicago -frames 240` (gas is `cross`; use `square` under the
  tmb_buttons scheme). The verdict line only prints on the `-frames` exit path.

What the probe paid for: the fork's physics is **stock-equivalent for an undamaged
car**. `handlingType[]` is byte-identical to upstream, the `wdx/sdx -> wdir` wheel
rewrite in `wheelforces.c` computes the same front/rear directions, every new
physics default (`GRAVITY_FORCE`, `angularDamping 128`, `springRate 230`,
`springDamping 100`) equals the stock constant, and `bcollide.c`'s wall-restitution
branch falls through to the stock impulse when no module answers. And the modules a
release ships (carhacks, crumple, mp, d1cars) never write `thrust`, `wheel_angle`,
`frontFS`/`rearFS`, `aacc` or a cosmetics field — so "the handling model is bugged"
can be ruled out by measurement before anything is tuned.

## The clock: who owns fast-forward

The engine offers fast-forward two ways, and both are the sim's business, so a
module that owns the sim rate (an mp session) must be able to turn the offer OFF:

- `BACKSPACE` — a debug/cutscene convenience (`redriver2_psxpc.cpp`): one press runs
  the sim fast from then on.
- the fullscreen map — `overlay.c` sets `FastForward` on **every frame** the map is
  open, so the world keeps moving behind it.

`int FastForwardLocked` (`director.c`, `extern` in `director.h`) is that switch. Set
it non-zero and the engine simply never offers fast-forward; clear it and the
stock behaviour returns.

**Lock it, don't clear `FastForward` per frame.** A module that cleared the flag each
frame while the map re-set it each frame made the two fight: for as long as the map
was open the sim toggled fast/normal every frame. mp raises the lock for the length
of a match (`MpOnFrame`) and logs the fact once — a fast-forwarded client runs at a
different rate from every other machine, which is a desync by definition.

### There is a SECOND fast-forward, and the lock has to cover it

`FastForwardLocked` gated the engine's own `BACKSPACE` handler but not PsyX's, and
PsyX has its own: on BACKSPACE key-down it sets `g_skipSwapInterval`, which makes
`PsyX_Sys_GetVBlankCount()` advance the counter an extra time per call, so the game's
2-vblank/30 Hz gate in `FilterFrameTime` passes on nearly every frame and the sim
runs at whatever rate the machine renders. Turning off "the fast-forward key" in a
session therefore turned off one of two, and the surviving one was worse: it is
latched on key-DOWN and cleared **only** by key-UP, so a key-up that never arrives
(focus stolen mid-press, a handler that swallows it) leaves the skip on for the rest
of the process.

That is a ~12x sim, measured at **355 steps/second against a stock 29-30**, and it
reads as "the player's car drives way too fast / the handling model is broken". Three
things close it:

- `PsyX_SetFastForwardLocked(int)` (`PsyX_public.h`) mirrors the engine's
  `FastForwardLocked`; the engine pushes it once per frame from `State_GameLoop`.
  While locked, BACKSPACE no longer sets the skip **and a latched skip is released**
  (logged: `[Psy-X] fast-forward locked: cleared a latched frame skip`).
- `SDL_WINDOWEVENT_FOCUS_LOST` releases the skip, because that is the event that
  usually eats the key-up (logged: `[Psy-X] focus lost: released the frame skip`).
- `overlay.c` clears `FastForward` **before** its early returns (cutscene,
  `NoPlayerControl`, player 0's split-screen frame). The clear used to sit below
  them, so those frames inherited whatever a map or a BACKSPACE tap had set and the
  sim stayed at 7 steps/frame afterwards.

`JERICHO_FF_LATCH=1` reproduces the latched state at boot for testing (it is the
skipped key-up, without the keyboard); it is test-only. A live 2-seat session with it
set is the end-to-end check: both seats log the release on the first frame, and
every probe sample afterwards reads `lock=1 skip=0`.

### The step gate itself is wall-clock, and every step path goes through it

The original gate was `VSync(-1) - frame < 2`: a step per two *simulated vblanks*. On
a PSX that was a hard 30 Hz; on PC the counter is faked by a spinning thread and can
be advanced extra times by the frame skip, so the step rate followed the counter's
mood rather than the clock. `FilterFrameTime()` now takes a step only when 33 ms of
`PsyX_GetTicks()` have elapsed (PSX keeps the vblank path), which makes the rate
independent of render load, of the frame skip, and of the fast-forward flags.

Two consequences worth knowing:

- **There is no fps to report.** The state machine is polled millions of times a
  second and returns immediately when the gate refuses, and the engine presents
  exactly one frame per *gated* step — so frames-presented and steps are the same
  number by construction. The probe therefore prints the step rate and the
  min..max inter-step gap instead; the gap is what shows a burst inside a second.
- **Every step path is gated.** The audit, so a future change can re-run it:

  | entry | where | gate |
  |---|---|---|
  | normal play, PC | `State_GameLoop`, `cnt = FastForward ? 7 : 1` loop (`main.c`) | after `FilterFrameTime()` |
  | normal play, PSX | the `gMultiStep`/`numFrames` loop (`main.c`) | after `FilterFrameTime()` |
  | cutscene skip | `gSkipInGameCutscene` branch (`main.c`) | **its own** `FilterFrameTime()` — it used to step once per state-machine iteration |
  | physics | `GlobalTimeStep()` has exactly one caller: `StepGame()` | inherits whichever gate above stepped it |
  | text entry | `pause.c` name entry | `FilterFrameTime()` polls only; never steps the sim |

`JERICHO_SKIP_CUTSCENE=1` holds the cutscene-skip branch on (the level-start cutscene
clears the flag, so it cannot be latched once) — test-only, and the rate must stay
~30 with it set.

Every run also logs which gate it was built with, next to the build id:

```
JERICHO 0.10.3-6-g7aba4e57-dirty Oct 17 2026 20:58:34 (Release_dev)
[JERICHO] step gate: wall clock, 33 ms per step (~30 steps/s, independent of render rate and frame skip)
```

That line exists because the banner alone cannot answer "is the fix in this binary?":
it is stamped at *premake* time, so it goes stale the moment another commit lands (one
session shipped a build whose banner still named the previous commit).

## Boot arguments (every PC build)

The game accepts frontend-bypass launch arguments (PC only) for fast testing.
They are accepted by EVERY configuration, Release included — they used to be
compiled out of a Release build, which made the binary we ship impossible to drive
with a rig (and every scripted launch of it died at startup):

```
JERICHO.exe -nointro -nofmv -level <city> -car <slot#> -gamemode <mode>
            -time <time> -weather <weather>
```

Only `-exportxasubtitles` is still `DEBUG_OPTIONS`-only (it does its work at parse
time rather than only steering the run). The stock `Debug Options` pause submenu is
the opposite case: a build defining `JERICHO_RELEASE_MODS` (i.e. a release) omits the
menu, while `DEBUG_OPTIONS` still enables the rest of the debug surface.

- `-level`: `chicago` / `havana` / `lasvegas` / `rio`
- `-car`: a slot number (0-9) or a car name
- `-gamemode`: `takeadrive` (default) / `survival` / `pursuit` / ...
- `-time`: `day` / `dusk` / `night`; `-weather`: `sunny` / `rain`

`-car` requires at least a `-level` (a message box explains otherwise).
